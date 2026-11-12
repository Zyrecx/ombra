#include "ombra/reconfig.h"
#include "ombra/brand.h"
#include "ombra/audio.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/update.h"
#include "ombra/util.h"

#include <windows.h>

namespace ombra {

std::mutex& LiveConfigMutex() {
    static std::mutex m;
    return m;
}

EncoderOptions EncoderOptionsFor(const Config& cfg) {
    EncoderOptions o;
    o.preset           = cfg.encode_preset;
    o.rate_control     = cfg.encode_rate_control;
    o.cq               = static_cast<uint32_t>(cfg.encode_cq);
    o.max_bitrate_kbps = static_cast<uint32_t>(cfg.encode_max_bitrate_kbps);
    o.adapter          = cfg.encode_adapter;
    o.codec            = ParseVideoCodec(cfg.encode_video_codec);
    o.av1_unaligned    = cfg.encode_amf_av1_unaligned;
    return o;
}

std::wstring FormatFilename(const std::string& template_str, const std::wstring& game_name,
                            const std::wstring& monitor_suffix, const SYSTEMTIME* at) {
    SYSTEMTIME st;
    if (at) st = *at;
    else GetLocalTime(&st);
    wchar_t date_buf[32], time_buf[32];
    swprintf_s(date_buf, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    swprintf_s(time_buf, L"%02d-%02d-%02d", st.wHour, st.wMinute, st.wSecond);

    std::wstring result = WideFromUtf8(template_str);
    auto replace_all = [](std::wstring& str, const std::wstring& from, const std::wstring& to) {
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::wstring::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    };

    std::wstring clean_game = game_name.empty() ? L"Desktop" : game_name;
    replace_all(result, L"{game}", clean_game);
    replace_all(result, L"{date}", date_buf);
    replace_all(result, L"{time}", time_buf);
    replace_all(result, L"{monitor_suffix}", monitor_suffix);
    return result;
}

std::wstring ResolveRingFilePath(const Config& cfg, const std::string& suffix) {
    const std::wstring expanded = cfg.buffer_file_path.empty()
                                      ? Instance().data_dir + L"\\ring.dat"
                                      : ExpandPathString(cfg.buffer_file_path);
    if (suffix.empty()) return expanded;

    const std::wstring wsuffix = WideFromUtf8(suffix);
    const size_t last_slash = expanded.find_last_of(L"\\/");
    const size_t dot        = expanded.find_last_of(L'.');
    if (dot == std::wstring::npos || (last_slash != std::wstring::npos && dot < last_slash)) {
        return expanded + wsuffix;
    }
    return expanded.substr(0, dot) + wsuffix + expanded.substr(dot);
}

bool SetWindowsAutostart(bool enable) {
    // An isolated instance (OMBRA_HOME, i.e. a test) must not rewrite the real install's Run key.
    if (Instance().isolated) return true;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    if (enable) {
        wchar_t self_path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring val = L"\"" + ScoopStablePath(self_path) + L"\"";
            RegSetValueExW(key, L"" OMBRA_INSTALL_ID, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(val.c_str()),
                            static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(key, L"" OMBRA_INSTALL_ID);
    }
    RegCloseKey(key);
    return true;
}

int64_t RingMaxAgeQpc(double buffer_seconds) {
    return static_cast<int64_t>((buffer_seconds + 2.0) * static_cast<double>(QpcFrequency()));
}

int64_t AudioRingMaxAgeQpc(double buffer_seconds) {
    constexpr double kMaxKeyframeSeconds = 10.0;  // encode.keyframe_sec's upper bound (config.h)
    return RingMaxAgeQpc(buffer_seconds + kMaxKeyframeSeconds);
}

LiveApplyResult ApplyLiveConfig(Config& live_cfg, const JsonValue& patch_json,
                                const std::vector<LivePipeline>& pipelines,
                                BufferController& controller,
                                Tray* tray,
                                const std::vector<RecordingGpuCaps>& gpus) {
    LiveApplyResult result;
    const LivePipeline& first = pipelines.front();

    const auto cur_params = first.supervisor.CurrentParams();
    // The captured size: PreviewConfig scales it by capture.resolution itself.
    const int dw = cur_params.source_width > 0 ? static_cast<int>(cur_params.source_width) : 1920;
    const int dh = cur_params.source_height > 0 ? static_cast<int>(cur_params.source_height) : 1080;
    ConfigPreview preview = PreviewConfig(live_cfg, patch_json, dw, dh, DisplayRefreshHz(first.save_ctx.monitor_device), gpus);

    Config working = live_cfg;
    ConfigPatchResult patch = ApplyConfigPatch(working, patch_json);
    // A preset without a bitrate commits the derived bitrate (and a bare bitrate its preset), the
    // same values preview_config reported -- so a Save doesn't depend on the UI having synced them.
    if (!preview.derived.empty()) {
        JsonValue derived = JsonValue::MakeObject();
        for (const auto& [key, val] : preview.derived) derived.Set(key, val);
        ApplyConfigPatch(working, derived);
    }

    result.tier = preview.apply_tier;
    result.accepted = patch.accepted;
    result.rejected = patch.rejected;

    if (patch.accepted.empty() && !patch.rejected.empty()) {
        result.ok = false;
        result.error = "no valid keys to apply";
        return result;
    }

    // Same sizing as startup and preview_config, so the three can't drift.
    const RingSizing sizing = ComputeRingSizing(working, working.buffer_seconds);
    const size_t new_ring_capacity = static_cast<size_t>(sizing.video_capacity);
    const size_t new_overshoot_bytes = static_cast<size_t>(sizing.video_overshoot);
    const bool file_backed = working.buffer_storage == "file";

    // A reallocation holds every pipeline's save slot, so a save pressed from here on is busy instead
    // of pinning a ring after this check, which would leave one ring reallocated and another refused.
    struct SaveSlots {
        std::vector<SaveContext*> held;
        ~SaveSlots() {
            for (SaveContext* ctx : held) ctx->save_in_flight.store(false, std::memory_order_release);
        }
    } save_slots;

    // Everything that can refuse comes before anything changes, on every pipeline.
    if (preview.apply_tier == ApplyTier::Realloc) {
        for (const LivePipeline& p : pipelines) {
            bool expected = false;
            const bool claimed = p.save_ctx.save_in_flight.compare_exchange_strong(expected, true);
            if (claimed) save_slots.held.push_back(&p.save_ctx);
            if (!claimed || p.ring.IsPinned()) {
                result.ok = false;
                result.buffer_dropped = false;
                result.error = "cannot reallocate buffer while a save is in progress";
                return result;
            }
        }
        if (file_backed) {
            // Each pipeline maps its own file on the one disk, so each asks for room for them all.
            const size_t all_rings = (new_ring_capacity + new_overshoot_bytes) * pipelines.size();
            for (const LivePipeline& p : pipelines) {
                std::string why;
                if (!CheckRingFile(ResolveRingFilePath(working, p.suffix), all_rings, &why)) {
                    result.ok = false;
                    result.buffer_dropped = false;
                    result.error = "can't keep the buffer on disk there: " + why;
                    return result;
                }
            }
        }
    }

    for (const LivePipeline& p : pipelines) {
        const uint32_t fps = static_cast<uint32_t>(
            ResolveFramerate(working.capture_framerate, DisplayRefreshHz(p.save_ctx.monitor_device)));

        PipelineSupervisor::SupervisorReconfigParams req;
        req.tier = preview.apply_tier;
        req.capture_cursor = working.capture_capture_cursor;
        if (preview.apply_tier != ApplyTier::Free) {
            req.fps = fps;
            req.bitrate_kbps = static_cast<uint32_t>(working.encode_bitrate_kbps);
            req.keyframe_sec = static_cast<uint32_t>(working.encode_keyframe_sec);
            req.encoder_opts = EncoderOptionsFor(working);
            req.encoder_opts.preset_bitrate_codec = PresetBitrateCodec(working, dw, dh, gpus);
            req.capture_target_mode   = working.capture_target;
            req.capture_target_window = working.capture_target_window;
            req.capture_resolution    = working.capture_resolution;
        }
        if (preview.apply_tier == ApplyTier::Realloc) {
            req.ring_capacity = new_ring_capacity;
            req.ring_overshoot = new_overshoot_bytes;
            // Populated on every Realloc-tier request, not just ones that touch
            // buffer.storage/buffer.file_path directly: RequestReconfigure's Realloc branch always
            // re-Inits the ring, so an unset/defaulted req here would silently drop a file-backed
            // ring back to RAM on an unrelated change (e.g. buffer.seconds alone).
            req.ring_file_backed = file_backed;
            req.ring_file_path   = file_backed ? ResolveRingFilePath(working, p.suffix) : std::wstring{};
        }

        const bool reconfigured = p.supervisor.RequestReconfigure(req);

        if (preview.apply_tier == ApplyTier::Realloc) {
            if (reconfigured) {
                result.realloc_sizing = sizing;
                result.realloc_file_backed = file_backed;
            } else {
                // Unreachable while the save slots are held, unless something else pinned the ring.
                result.ok = false;
                result.buffer_dropped = false;
                result.error = "cannot reallocate buffer while a save is in progress";
                return result;
            }
            p.save_ctx.buffer_seconds = working.buffer_seconds;
            p.ring.SetMaxAge(RingMaxAgeQpc(working.buffer_seconds));  // or a longer save gets cut short
        }
        if (preview.apply_tier != ApplyTier::Free) p.save_ctx.fps = fps;
        std::lock_guard<std::mutex> lock(LiveConfigMutex());
        p.save_ctx.output_root = working.output_root;
        p.save_ctx.output_filename = working.output_filename;
        p.save_ctx.per_game_folders = working.output_per_game_folders;
        p.save_ctx.save_overlap_policy = working.save_overlap_policy;
    }

    // The audio hub is shared, so it hears each change once.
    if (AudioTrackHub* audio = first.save_ctx.audio) {
        if (preview.apply_tier == ApplyTier::Realloc) {
            audio->Resize(AudioTrackHub::SizeFor(working.buffer_seconds,
                                                 static_cast<uint32_t>(working.audio_bitrate_kbps),
                                                 AudioRingMaxAgeQpc(working.buffer_seconds)));
        }
        audio->SetBitrate(static_cast<uint32_t>(working.audio_bitrate_kbps));
        audio->SetFullMix(working.audio_full_mix_track);
    }

    controller.SetMode(working.capture_mode);
    controller.SetTarget(working.capture_target, working.capture_target_window);

    const std::string prev_accent = live_cfg.ui_accent, prev_theme = live_cfg.ui_theme;

    SetWindowsAutostart(working.engine_autostart);
    Log::SetLevel(ParseLogLevel(working.log_level));
    Log::EnablePruning(static_cast<size_t>(working.log_max_bytes));
    // The change is live either way, so ok stays true; the error says it won't survive a restart.
    if (!SaveConfig(working))
        result.error = "applied, but config.json could not be written: this reverts when " OMBRA_PRODUCT_NAME " restarts";
    {
        std::lock_guard<std::mutex> lock(LiveConfigMutex());
        live_cfg = working;
    }

    if (preview.apply_tier == ApplyTier::Free && tray) {
        tray->SetTooltip(L"" OMBRA_PRODUCT_NAME " — " + std::wstring(first.supervisor.Armed() ? L"Recording" : L"Paused") +
                          L" (" + std::wstring(live_cfg.capture_mode.begin(), live_cfg.capture_mode.end()) +
                          L")");
        if (working.ui_accent != prev_accent || working.ui_theme != prev_theme) {  // "theme" follows ui.theme
            tray->SetAccent(working.ui_accent, working.ui_theme);
        }
    }

    result.ok = true;
    result.buffer_dropped = preview.apply_tier == ApplyTier::Realloc;
    result.note = preview.apply_tier == ApplyTier::Realloc ? "reallocated buffer and applied config"
                : preview.apply_tier == ApplyTier::Rebuild ? "rebuilt pipeline and applied config"
                                                           : "applied config live";
    return result;
}

LiveApplyResult ApplyLiveConfig(Config& live_cfg, const JsonValue& patch_json,
                                PipelineSupervisor& supervisor,
                                BufferController& controller,
                                SaveContext& save_ctx,
                                Ring& ring,
                                Tray* tray,
                                const std::vector<RecordingGpuCaps>& gpus) {
    return ApplyLiveConfig(live_cfg, patch_json, {LivePipeline{supervisor, save_ctx, ring, ""}}, controller, tray, gpus);
}

}  // namespace ombra
