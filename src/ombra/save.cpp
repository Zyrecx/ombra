#include "ombra/save.h"


#include "ombra/controller.h"
#include "ombra/crash_upload.h"
#include "ombra/log.h"
#include "ombra/mux.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"
#include "ombra/telemetry.h"
#include "ombra/util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ombra {

namespace {

// Local wall-clock time of `qpc` (the hotkey press), so a clip is named for when it was asked for,
// not for when the save reached naming it (after the audio encode, seconds later). Outside the
// last ten minutes (a synthetic test timeline) it falls back to now.
SYSTEMTIME LocalTimeAtQpc(int64_t qpc) {
    LARGE_INTEGER now_qpc{}, freq{};
    QueryPerformanceCounter(&now_qpc);
    QueryPerformanceFrequency(&freq);
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER t{};
    t.LowPart  = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    const int64_t ago_ticks = now_qpc.QuadPart - qpc;
    if (ago_ticks >= 0 && ago_ticks <= 600 * freq.QuadPart)
        t.QuadPart -= static_cast<uint64_t>(ago_ticks * 10000000 / freq.QuadPart);
    FILETIME utc{t.LowPart, t.HighPart};
    SYSTEMTIME st_utc{}, st{};
    FileTimeToSystemTime(&utc, &st_utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &st_utc, &st);
    return st;
}

// Filled in by DoSaveImpl when non-null and folded into DoSave's telemetry event, so a real save
// leaves numbers behind instead of a guess about where its time and memory go.
struct SaveTiming {
    int64_t audio_ms             = 0;  // collecting the hub's encoded frames (and the last few ms)
    int64_t mux_ms               = 0;  // ring -> file, one AU at a time: a large mux_ms with a
                                         // small output_mb points at the disk
    int64_t pin_ms               = 0;  // Pin to Unpin: how long eviction was held back
    double  output_mb            = 0;
    bool    have_output_mb       = false;
    double  private_mb_delta_mux = 0;  // private bytes halfway through the mux minus before it
    const char* codec            = "";  // the pinned segment's, i.e. what the file holds
    bool    fell_back            = false;
    double  clip_seconds         = 0;  // ticks / fps of the pinned window
};

// Releases the ring pin on every path out of a save. The outer DoSave retains save_in_flight
// through telemetry and result collection, so shutdown/reconfiguration cannot retire ctx early.
class SavePin {
public:
    explicit SavePin(SaveContext* ctx) : ctx_(ctx), pinned_at_(std::chrono::steady_clock::now()) {}
    ~SavePin() {
        Release();
    }
    SavePin(const SavePin&) = delete;
    SavePin& operator=(const SavePin&) = delete;

    // Milliseconds the pin was held.
    int64_t Release() {
        if (pinned_) {
            ctx_->ring->Unpin();
            pinned_  = false;
            held_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - pinned_at_).count();
        }
        return held_ms_;
    }

private:
    SaveContext*                          ctx_;
    std::chrono::steady_clock::time_point pinned_at_;
    bool                                  pinned_  = true;
    int64_t                               held_ms_ = 0;
};

// `group_folder_name`, when non-null, names a subfolder (already fully formatted -- game/date/time,
// no extension) that this save's file belongs inside, alongside every other monitor's file from the
// same action. SaveReplay formats it once so workers scheduled across a second boundary still
// write into the same folder.
SaveOutcome DoSaveImpl(SaveContext* ctx, int64_t t_end, const std::wstring* group_folder_name,
                       SaveTiming* timing, std::wstring* saved_path) {
    ComInit com;
    const int64_t qpc_freq    = QpcFrequency();
    const int64_t t_start_min = t_end - static_cast<int64_t>(ctx->buffer_seconds * qpc_freq);

    auto descriptors = ctx->ring->PinCovering(t_start_min, t_end);
    SavePin pin(ctx);
    std::printf("\n  save            %zu access units pinned\n", descriptors.size());

    // Checked before the truncate barrier below: a skipped save must not cut the next one short.
    const auto too_short = [&] {
        std::printf("  save            skipped — %zu access units is under %.1f s of footage\n",
                    descriptors.size(), kMinClipSeconds);
        return SaveOutcome::kNothingNew;
    };
    if (descriptors.empty()) return too_short();

    std::shared_ptr<const SegmentParams> segment = ctx->ring->PinnedSegmentParams();
    if (!segment) {
        auto live = ctx->supervisor->CurrentParams();
        auto fallback = std::make_shared<SegmentParams>();
        fallback->codec_header = std::move(live.codec_header);
        fallback->codec        = live.codec;
        fallback->width   = live.width;
        fallback->height  = live.height;
        segment = fallback;
    }
    // The pinned footage's rate, not the live one: a frame-rate change with Instant Replay off keeps
    // the old footage, which at the new rate would play at the wrong speed and lose its audio sync.
    const uint32_t fps = segment->fps ? segment->fps : ctx->fps.load();
    // By ticks, not samples: an overloaded encoder's skipped ticks are held frames, so 40 samples can
    // span 2 s at 60 fps.
    const uint64_t ticks = descriptors.back().index - descriptors.front().index + 1;
    if (ticks < static_cast<uint64_t>(std::ceil(kMinClipSeconds * fps))) return too_short();
    if (timing) timing->clip_seconds = static_cast<double>(ticks) / fps;

    const int64_t video_floor_qpc = descriptors.front().qpc;

    // Finished AAC frames from the hub (audio_tracks.h), trimmed to this window. Starts at the
    // first video frame so audio and video share time zero.
    const auto audio_started = std::chrono::steady_clock::now();
    std::vector<MuxAudioTrack> audio_tracks;
    if (ctx->audio) audio_tracks = ctx->audio->Collect(video_floor_qpc, t_end);
    const auto audio_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - audio_started).count();

    ctx->save_count.fetch_add(1);
    // Copied once: a settings Save can replace these while this save runs on its own thread.
    std::string output_root, output_filename, overlap_policy;
    bool per_game_folders = false;
    {
        std::lock_guard<std::mutex> lock(LiveConfigMutex());
        output_root = ctx->output_root;
        output_filename = ctx->output_filename;
        overlap_policy = ctx->save_overlap_policy;
        per_game_folders = ctx->per_game_folders;
    }
    const bool templated = (ctx->out_path == L"out\\ombra.mp4");
    // Always .mp4, whether or not the filename template already ends in it.
    const std::wstring ext = L".mp4";
    // The clip's full path under `root_dir`. A templated name never overwrites an earlier clip: the
    // check runs on the final name (it used to run before the extension, so a save overwrote its
    // predecessor) and takes the first free suffix, so suffixes left by an earlier run are skipped.
    auto templated_path = [&](std::wstring root_dir) {
        std::wstring game_name = (ctx->controller) ? ctx->controller->CurrentGameFolder() : L"";
        if (per_game_folders && !game_name.empty()) {
            root_dir += L"\\" + game_name;
        }

        std::wstring fn;
        if (group_folder_name && !group_folder_name->empty()) {
            // N>1 monitors: one subfolder per save event (named like a normal single-monitor
            // save filename would be, minus the extension), each monitor's file inside named by
            // its label ("DISPLAY1.mp4", "Primary.mp4", ...) instead of a flat filename with
            // {monitor_suffix} appended -- easier to browse "one save, N files" as a unit than N
            // look-alike filenames sharing one folder.
            root_dir += L"\\" + *group_folder_name;
            fn = (ctx->monitor_label.empty() ? L"capture" : ctx->monitor_label) + L".mp4";
        } else {
            const SYSTEMTIME pressed = LocalTimeAtQpc(t_end);
            fn = SanitizeFileName(FormatFilename(
                output_filename.empty() ? "{game} {date} {time}{monitor_suffix}" : output_filename,
                game_name, ctx->monitor_suffix, &pressed));
        }
        std::wstring full = WithContainerExtension(root_dir + L"\\" + fn, ext);
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring base = full.substr(0, full.size() - ext.size());
            for (int i = 1;; ++i) {
                wchar_t suffix[16];
                swprintf_s(suffix, L"_%03d", i);
                const std::wstring candidate = base + suffix + ext;
                if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    full = candidate;
                    break;
                }
            }
        }
        return full;
    };
    const std::wstring root = ResolveOutputRoot(output_root);
    std::wstring path = templated ? templated_path(root) : WithContainerExtension(ctx->out_path, ext);

    // Each AU is read out of the pinned ring and written before the next one is read, and the pin
    // is released behind the writer (Ring::ReleaseThrough), so a save holds one AU in memory and
    // holds back eviction only by as much as the disk lags the encoder. A disk that stalls past
    // the ring's headroom breaks the pin and fails this save, never the engine (Ring::Append).
    size_t   save_bytes     = 0;
    uint32_t save_keyframes = 0;
    std::vector<MuxVideoSample> samples(descriptors.size());
    for (size_t i = 0; i < descriptors.size(); ++i) {
        samples[i] = {descriptors[i].is_keyframe, descriptors[i].index};
        save_bytes += descriptors[i].length;
        if (descriptors[i].is_keyframe) ++save_keyframes;
    }

    // Decided here, before the mux: past this point the pin has let go of what it wrote, so a retry
    // elsewhere couldn't be whole. The margin covers the audio and the MP4's index.
    const uint64_t room_needed = save_bytes + save_bytes / 8 + (16u << 20);
    if (!FolderTakesFile(path, room_needed)) {
        const bool can_fall_back = templated && !ctx->fallback_root.empty() &&
                                   _wcsicmp(ctx->fallback_root.c_str(), root.c_str()) != 0;
        std::wstring elsewhere;
        if (can_fall_back) {
            elsewhere = templated_path(ctx->fallback_root);
            if (!FolderTakesFile(elsewhere, room_needed)) elsewhere.clear();
        }
        if (elsewhere.empty()) {
            Log::Error("save: \"%ls\" can't take this clip (drive missing, disk full or read-only); not saved, the "
                       "footage stays in the buffer for a retry", path.c_str());
            crash_upload::ReportError(crash_upload::ErrorKind::Save, "no_room", "", "save folder can't take the clip");
            return SaveOutcome::kFailed;
        }
        Log::Warn("save: \"%ls\" can't take this clip (drive missing, disk full or read-only); writing \"%ls\"",
                  path.c_str(), elsewhere.c_str());
        path = std::move(elsewhere);
        if (timing) timing->fell_back = true;
    }
    double private_mb_before_mux = 0, private_mb_mid_mux = 0, ws_mb_unused = 0;
    GetProcessRssMb(ws_mb_unused, private_mb_before_mux);
    bool pin_broken = false;
    auto read = [&](size_t i, std::vector<uint8_t>& out) {
        if (i > 0) ctx->ring->ReleaseThrough(descriptors[i - 1]);
        if (!ctx->ring->ReadInto(descriptors[i], out)) {
            pin_broken = true;
            return false;
        }
        if (i == descriptors.size() / 2) GetProcessRssMb(ws_mb_unused, private_mb_mid_mux);
        return true;
    };

    // `segment` came from the ring at Pin() time — see the comment where it is read.
    const auto mux_started = std::chrono::steady_clock::now();
    if (timing) timing->codec = VideoCodecName(segment->codec);
    const bool ok = MuxWriteMp4(path, segment->codec_header, segment->width, segment->height, fps,
                                samples, read, audio_tracks, segment->codec, 1,
                                {segment->picture_width, segment->picture_height});
    const auto mux_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - mux_started).count();
    // Only a written clip cuts the next one short: after a failure (a full disk) the footage is still
    // in the ring for a retry.
    if (ok && overlap_policy == "truncate") {
        ctx->ring->Barrier(t_end);  // audio follows the video floor, so it needs no barrier
        Log::Info("save.overlap_policy=truncate: dropped barrier at t_end=%lld", static_cast<long long>(t_end));
    }
    const int64_t pin_ms = pin.Release();
    if (pin_broken)
        Log::Error("save: the footage this save was writing is gone: the disk fell behind the encoder by more "
                   "than the ring's headroom, or the buffer's drive was removed (pinned %lld ms)",
                   static_cast<long long>(pin_ms));

    WIN32_FILE_ATTRIBUTE_DATA file_info{};
    const bool have_file_size = ok && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file_info);
    const double output_mb = have_file_size
        ? static_cast<double>((static_cast<uint64_t>(file_info.nFileSizeHigh) << 32) | file_info.nFileSizeLow) /
              (1024.0 * 1024.0)
        : 0.0;

    std::printf("  access units     %zu  (target ~%.0f for a %.1fs buffer)\n", descriptors.size(),
                ctx->buffer_seconds * fps, ctx->buffer_seconds);
    std::printf("  bytes            %.2f MB (Annex-B, pre-mux)\n",
                static_cast<double>(save_bytes) / (1024.0 * 1024.0));
    std::printf("  keyframes        %u\n", save_keyframes);
    std::printf("  segment          %ux%u  (%zu-byte SPS/PPS from the pinned segment)\n",
                segment->width, segment->height, segment->codec_header.size());
    std::printf("  audio frames     %zu%s\n", audio_tracks.empty() ? size_t{0} : audio_tracks[0].frames.size(),
                audio_tracks.empty() ? " (no audio track written)" : "");
    std::printf("  audio collect    %lld ms\n", static_cast<long long>(audio_ms));
    std::printf("  mux+write        %lld ms%s (pinned %lld ms)\n", static_cast<long long>(mux_ms),
                have_file_size ? "" : " (output size unavailable)", static_cast<long long>(pin_ms));
    if (have_file_size) std::printf("  output size      %.2f MB\n", output_mb);
    std::printf("  private bytes    %.1f MB before mux, %+.1f MB halfway through\n",
                private_mb_before_mux, private_mb_mid_mux - private_mb_before_mux);
    std::printf("  mux              %s\n", ok           ? "ok"
                                             : pin_broken ? "FAILED — the ring overwrote the footage"
                                                          : "FAILED — could not write output file");
    std::wprintf(L"  output           %s\n\n", path.c_str());

    if (timing) {
        timing->audio_ms             = audio_ms;
        timing->mux_ms               = mux_ms;
        timing->pin_ms               = pin_ms;
        timing->output_mb            = output_mb;
        timing->have_output_mb       = have_file_size;
        timing->private_mb_delta_mux = private_mb_mid_mux - private_mb_before_mux;
    }

    if (!ok)
        crash_upload::ReportError(crash_upload::ErrorKind::Save, pin_broken ? "footage_evicted" : "write_failed", "",
                                  pin_broken ? "footage gone before the save finished" : "couldn't write the clip");
    if (ok) {
        if (saved_path) *saved_path = path;
        std::lock_guard<std::mutex> lock(ctx->last_save_mutex);
        ctx->last_save_path = path;
        ctx->last_save_qpc  = t_end;
    }
    return ok ? SaveOutcome::kSaved : SaveOutcome::kFailed;
}


}  // namespace

std::wstring WithContainerExtension(const std::wstring& path, const std::wstring& ext) {
    const size_t name_start = path.find_last_of(L"\\/") == std::wstring::npos ? 0 : path.find_last_of(L"\\/") + 1;
    if (path.size() >= name_start + 4) {
        std::wstring tail = path.substr(path.size() - 4);
        for (auto& c : tail) c = static_cast<wchar_t>(towlower(c));
        if (tail == L".mp4") return path.substr(0, path.size() - 4) + ext;
    }
    return path + ext;
}

std::wstring SanitizeFileName(std::wstring name) {
    for (auto& c : name) {
        if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    return name;
}

const char* SaveOutcomeName(SaveOutcome outcome) {
    switch (outcome) {
        case SaveOutcome::kSaved:      return "saved";
        case SaveOutcome::kNothingNew: return "nothing_new";
        case SaveOutcome::kFailed:     return "failed";
        case SaveOutcome::kBusy:       return "busy";
    }
    return "failed";
}

// Every save goes through here so each one leaves a telemetry event with its duration and result.
SaveOutcome DoSave(SaveContext* ctx, int64_t t_end, const std::wstring* group_folder_name,
                   std::wstring* saved_path, bool* fell_back, double* clip_seconds) {
    struct ReleaseFlight {
        SaveContext* ctx;
        ~ReleaseFlight() { ctx->save_in_flight.store(false, std::memory_order_release); }
    } release{ctx};
    const auto        started = std::chrono::steady_clock::now();
    SaveTiming         timing;
    std::wstring path;
    const SaveOutcome outcome = DoSaveImpl(ctx, t_end, group_folder_name, &timing, &path);
    if (saved_path) *saved_path = path;
    if (fell_back) *fell_back = timing.fell_back;
    if (clip_seconds) *clip_seconds = outcome == SaveOutcome::kSaved ? timing.clip_seconds : 0.0;
    if (Telemetry::Enabled()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started).count();
        const char* event = outcome == SaveOutcome::kSaved      ? "save"
                            : outcome == SaveOutcome::kNothingNew ? "save_skipped"
                                                                  : "save_failed";
        Telemetry::Event(event,
                         "duration_ms=%lld audio_ms=%lld mux_ms=%lld pin_ms=%lld output_mb=%.2f "
                         "private_mb_delta_mux=%+.1f monitor=%s codec=%s buffer_s=%.0f clip_s=%.1f fell_back=%d path=%s",
                         static_cast<long long>(ms), static_cast<long long>(timing.audio_ms),
                         static_cast<long long>(timing.mux_ms), static_cast<long long>(timing.pin_ms),
                         // -1.00 means the output size lookup failed (e.g. the save itself failed),
                         // not a zero-byte file.
                         timing.have_output_mb ? timing.output_mb : -1.0,
                         timing.private_mb_delta_mux,
                         NarrowUtf8(ctx->monitor_label).c_str(), timing.codec,
                         ctx->buffer_seconds, timing.clip_seconds, timing.fell_back ? 1 : 0, NarrowUtf8(path).c_str());
    }
    return outcome;
}

std::future<ReplaySaveResults> SaveReplay(const std::vector<SaveContext*>& contexts,
                                        const StartSaveFeedback& feedback) {
    struct Operation {
        std::promise<ReplaySaveResults> promise;
        ReplaySaveResults result{SaveOutcome::kBusy, {}};
        std::atomic<size_t> remaining{0};
        SaveFeedback feedback;
    };
    auto op = std::make_shared<Operation>();
    auto future = op->promise.get_future();
    const int64_t t_end = QpcNow();
    std::wstring folder;
    if (contexts.size() > 1) {
        const auto* controller = contexts.front()->controller;
        folder = FormatFilename("{game} {date} {time}",
                                controller ? controller->CurrentGameFolder() : L"", L"");
    }
    op->result.monitors.resize(contexts.size());
    std::vector<size_t> claimed;
    for (size_t i = 0; i < contexts.size(); ++i) {
        bool expected = false;
        if (contexts[i]->save_in_flight.compare_exchange_strong(expected, true)) claimed.push_back(i);
    }
    if (claimed.empty()) {
        op->promise.set_value(std::move(op->result));
        return future;  // retain the existing in-progress toast
    }
    op->remaining = claimed.size();  // fixed before any worker can finish
    if (feedback) op->feedback = feedback();
    for (size_t i : claimed) {
        std::thread([op, ctx = contexts[i], i, t_end, folder] {
            Log::SetThreadName("save");
            auto& result = op->result.monitors[i];
            result.outcome = DoSave(ctx, t_end, folder.empty() ? nullptr : &folder, &result.path,
                                    &result.fell_back, &result.seconds);
            if (op->remaining.fetch_sub(1) != 1) return;
            bool saved = false, incomplete = false;
            for (const auto& monitor : op->result.monitors) {
                saved |= monitor.outcome == SaveOutcome::kSaved;
                incomplete |= monitor.outcome == SaveOutcome::kFailed || monitor.outcome == SaveOutcome::kBusy;
            }
            op->result.outcome = incomplete ? SaveOutcome::kFailed
                                 : saved ? SaveOutcome::kSaved : SaveOutcome::kNothingNew;
            if (op->feedback) op->feedback(op->result);
            op->promise.set_value(std::move(op->result));
        }).detach();
    }
    return future;
}

}  // namespace ombra
