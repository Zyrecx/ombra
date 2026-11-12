// HandlePipeRequest: every request type the engine answers over \\.\pipe\OmbraEngine.

#include "ombra/engine_pipe.h"
#include "ombra/brand.h"

#include "ombra/audio.h"
#include "ombra/capture.h"
#include "ombra/capturebench.h"
#include "ombra/config.h"
#include "ombra/crash.h"
#include "ombra/crash_upload.h"
#include "ombra/controller.h"
#include "ombra/die.h"
#include "ombra/amf_probe.h"
#include "ombra/encode_target.h"
#include "ombra/encoder.h"
#include "ombra/gputopo.h"
#include "ombra/hotkeys.h"
#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/mux.h"
#include "ombra/pacer.h"
#include "ombra/paths.h"
#include "ombra/audio_tracks.h"
#include "ombra/pipe.h"
#include "ombra/resource.h"
#include "ombra/tray.h"
#include "ombra/update.h"
#include "ombra/version.h"
#include "ombra/protocol.h"
#include "ombra/screenshot.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"
#include "ombra/reconfig.h"
#include "ombra/save.h"
#include "ombra/util.h"
#include "ombra/telemetry.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>

#include <knownfolders.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ombra {

namespace {

// One preview grab for capture_*_thumbnail, written to `final_path` (paths.h's MonitorThumbPath/
// WindowThumbPath) through a temp file and a rename: the UI decodes that file at page build without
// asking, so it must never see a half-written PNG. A failed grab leaves the previous file in place;
// a stale preview beats a placeholder.
JsonValue WriteThumbnailReply(const std::wstring& final_path,
                              const std::function<ThumbnailResult(const std::wstring&)>& grab,
                              const std::string& what) {
    JsonValue resp = JsonValue::MakeObject();
    const std::wstring tmp_path = final_path + L".tmp";
    std::string error;
    ThumbnailResult tr;
    if (final_path.empty()) {
        error = "could not resolve thumbs directory";
    } else if (!EnsureParentDir(final_path)) {
        error = "could not create thumbs directory";
    } else {
        ComInit com;  // the ServeClient thread running this never initializes COM
        tr = grab(tmp_path);
        if (!tr.ok) {
            error = tr.error;
        } else if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = "rename failed, err=" + std::to_string(GetLastError());
        }
    }
    if (!error.empty()) {
        DeleteFileW(tmp_path.c_str());
        Log::Warn("%s thumbnail failed: %s", what.c_str(), error.c_str());
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr(error));
        return resp;
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("path", JsonValue::MakeStr(NarrowUtf8(final_path)));
    resp.Set("width", JsonValue::MakeNum(static_cast<double>(tr.width)));
    resp.Set("height", JsonValue::MakeNum(static_cast<double>(tr.height)));
    return resp;
}

// The audio "Add app" picker (list_processes pipe query, main.cpp's HandlePipeRequest). Distinct
// process image names currently running, so the Settings UI can offer them as a picker instead of
// a free-text field -- deduped by name, since per-app audio targets match by exe name, not pid.
//
// Filtered to processes that own at least one capturable top-level window (the same universe
// EnumerateCapturableWindows offers for the window-target picker), not every process in the
// Toolhelp32 snapshot -- a raw snapshot is mostly services/background processes (svchost.exe and
// friends) that game-mode's foreground detection could never mistake for a game in the first
// place, so listing them just makes the real, pickable entries hard to find.
std::vector<std::pair<std::string, std::string>> ListRunningProcessesUtf8() {
    std::vector<std::pair<std::string, std::string>> out;  // {name, full path}

    std::set<std::wstring> windowed_names_lower;
    for (const WindowInfo& w : EnumerateCapturableWindows()) {
        if (w.process.empty()) continue;
        std::wstring name_lower = WideFromUtf8(w.process);
        for (auto& c : name_lower) c = static_cast<wchar_t>(towlower(c));
        windowed_names_lower.insert(std::move(name_lower));
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    std::set<std::wstring> seen_lower;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;  // System Idle / System
            std::wstring name = pe.szExeFile;
            std::wstring name_lower = name;
            for (auto& c : name_lower) c = static_cast<wchar_t>(towlower(c));
            if (name_lower == L"" OMBRA_ENGINE_EXE || name_lower == L"" OMBRA_UI_EXE) continue;
            if (!windowed_names_lower.count(name_lower)) continue;
            if (!seen_lower.insert(name_lower).second) continue;

            std::wstring path;
            if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t image[MAX_PATH]{};
                DWORD   len = MAX_PATH;
                if (QueryFullProcessImageNameW(proc, 0, image, &len)) path.assign(image, len);
                CloseHandle(proc);
            }
            out.emplace_back(NarrowUtf8(name), NarrowUtf8(path));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    std::sort(out.begin(), out.end());
    return out;
}

// A documented gap ("no live-rebuild path for capture.monitors -- the pipelines
// vector is built once in wmain") is deliberately not closed by making that vector mutable at
// runtime: the same area of this codebase already produced one real concurrent-access crash (a
// concurrent audio-ring pin) once pipelines started being touched from more than one
// thread. A full self-restart is the safe alternative --
// relaunch a fresh ombra.exe (which re-reads config.json, including the just-persisted
// capture.monitors), then shut this instance down through the exact same path Ctrl+C/tray-Exit
// already use, so nothing about shutdown itself is new or under-tested.
void EngineSelfRestart() {
    wchar_t exe_path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        Log::Warn("EngineSelfRestart: GetModuleFileNameW failed (%lu) -- not restarting", GetLastError());
        return;
    }
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    // The new process's AcquireEngineInstanceLock() retries for a few seconds (wmain) precisely so
    // it can win the named mutex once this instance actually releases it during shutdown below --
    // spawn first, then quit, not the other way around, so there's a live successor as early as
    // possible.
    if (CreateProcessW(exe_path, nullptr, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log::Warn("EngineSelfRestart: CreateProcessW failed (%lu) -- not restarting", GetLastError());
        return;
    }
    // g_pump_thread_id_for_ctrl_handler is only nonzero once wmain's resident path has installed
    // the pump loop (main.cpp:4746-ish) -- the same precondition self-restart itself requires,
    // since it's only reachable through a live pipe server, which only exists in that path.
    Telemetry::Event("self_restart", "reason=config_change");
    PostThreadMessage(g_pump_thread_id_for_ctrl_handler.load(), WM_QUIT, 0, 0);
}

}  // namespace

// The live hotkeys, for EngineWndProc (notify/lease messages) and the pipe handler (set_config,
// hotkeys_suspend, get_status). Null outside the resident engine. Same single-instance reasoning as
// g_tray_for_wndproc below.
std::atomic<HotkeyRegistry*> g_hotkeys_for_engine{nullptr};

// The Games list (game_library.h), for the pipe handler (set_config games.list, list_games,
// list_windows' is_game). Null outside the resident engine and before wmain builds it.
std::atomic<GameLibrary*> g_games_for_engine{nullptr};

std::map<std::string, std::string> HotkeySpecsFromConfig(const Config& cfg) {
    return {{"hotkeys.save_replay", cfg.hotkeys_save_replay},
            {"hotkeys.screenshot", cfg.hotkeys_screenshot},
            {"hotkeys.toggle_buffer", cfg.hotkeys_toggle_buffer},
            {"hotkeys.overlay", cfg.hotkeys_overlay}};
}

// Ctrl+C/Ctrl+Break/console-close all need to unblock GetMessage on the pump thread the same way
// the tray's "Exit" eventually will (DestroyWindow -> WM_DESTROY -> PostQuitMessage) — a console
// control handler runs on its own OS-created thread, not the pump thread, so it can only post into
// the pump thread's queue, not call PostQuitMessage directly (that only affects the calling
// thread's own queue, same reason the pre-existing timedtest timeout thread already used
// PostThreadMessage instead). Only meaningful for the resident mode (timedtest has its own timeout
// thread); left registered afterward is harmless since resident mode does not return except at
// process exit. Atomic: the console handler thread and pipe worker threads (the shutdown request)
// read it while the pump thread sets it.
std::atomic<DWORD> g_pump_thread_id_for_ctrl_handler{0};

// Runs on a detached thread started right after CreateProcessW spawns capturebench (main.cpp's
// set_config handler) — waits for it to exit, reads back the JSON result file it wrote, and
// updates ctx for the next get_status poll to report. Detached rather than joined: this thread
// only touches ctx's own atomic/mutex-guarded state and a HANDLE it exclusively owns, so letting
// the OS tear it down at process exit (same as every other thread here) loses nothing.
void WaitForAutotuneCompletion(AutotuneContext* ctx) {
    WaitForSingleObject(ctx->process, INFINITE);
    DWORD exit_code = 1;
    const bool completed = GetExitCodeProcess(ctx->process, &exit_code) && exit_code == 0;
    CloseHandle(ctx->process);
    ctx->process = nullptr;

    JsonValue result;
    bool ok = false;
    const std::wstring path = AutotuneResultFilePath();
    // A refused/failed run may leave the previous run's JSON behind. It is not fresh advice.
    if (completed && !path.empty()) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            try {
                result = ParseJson(text);
                ok = result.type == JsonValue::Type::Object;
            } catch (const std::string& err) {
                Log::Warn("autotune: result file parse failed: %s", err.c_str());
            }
        }
    }
    if (!ok) {
        result = JsonValue::MakeObject();
        result.Set("reason", JsonValue::MakeStr(completed
            ? "capturebench exited without a readable result file"
            : "capturebench failed or was refused; no recommendation is available"));
    }

    {
        std::lock_guard<std::mutex> lock(ctx->result_mutex);
        ctx->last_result = std::move(result);
    }
    // The last touch of ctx, after the unlock: shutdown waits on this state and then destroys ctx,
    // so publishing it with the mutex still held would unlock a destroyed mutex.
    ctx->state.store(ok ? AutotuneContext::State::Done : AutotuneContext::State::Failed,
                      std::memory_order_release);
}

// The tray tooltip says whether the Instant Replay switch is on, not whether a frame is being
// recorded this second: with Only in games or One window the buffer waits, switched on.
std::wstring TrayTooltip(bool enabled) {
    return enabled ? L"" OMBRA_PRODUCT_NAME " — Instant Replay on" : L"" OMBRA_PRODUCT_NAME " — Instant Replay off";
}

ScreenshotPaths ScreenshotPathsFor(const std::wstring& root, const std::wstring& fallback_root,
                                   const std::wstring& game_name, bool per_game_folders, int n) {
    std::wstring name = FormatFilename("{game} {date} {time}{monitor_suffix}.png", game_name, L"");
    if (per_game_folders && !game_name.empty()) name = game_name + L"\\" + name;
    const auto in_root = [&](const std::wstring& folder) {
        std::wstring full = folder + L"\\" + name;
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
            size_t dot = full.rfind(L'.');
            std::wstring base = (dot == std::wstring::npos) ? full : full.substr(0, dot);
            std::wstring ext = (dot == std::wstring::npos) ? L"" : full.substr(dot);
            wchar_t suffixed[MAX_PATH];
            swprintf_s(suffixed, L"%s_%03d%s", base.c_str(), n, ext.c_str());
            full = suffixed;
        }
        return full;
    };
    return {in_root(root), fallback_root.empty() ? std::wstring() : in_root(fallback_root)};
}

void ScreenshotWorkerLoop(ScreenshotContext* ctx) {
    ComInit com;
    const auto notify = [ctx](const wchar_t* event) {
        if (ctx->notify) ctx->notify(event);
    };
    while (true) {
        std::unique_lock<std::mutex> lock(ctx->job_mutex);
        ctx->job_cv.wait(lock, [&] {
            return ctx->job_ready || !ctx->running.load(std::memory_order_acquire);
        });
        if (!ctx->job_ready) break;
        std::wstring          wanted    = std::move(ctx->job_path);
        const std::wstring     fallback  = std::move(ctx->job_fallback_path);
        const uint32_t         width     = ctx->job_width;
        const uint32_t         height    = ctx->job_height;
        const uint32_t         row_pitch = ctx->job_row_pitch;
        std::vector<uint8_t>   bgra      = std::move(ctx->job_bgra);
        ctx->job_ready = false;
        lock.unlock();

        if (bgra.empty()) {
            Log::Error("screenshot: the GPU refused the readback (device reset?) -- nothing written");
            notify(L"shot_failed");
            continue;
        }
        // The raw pixels bound the PNG closely enough: only room for the file matters here.
        const std::wstring path = PickWritablePath(wanted, fallback, bgra.size());
        if (path.empty()) {
            Log::Error("screenshot: \"%ls\" can't take it (drive missing, disk full or read-only); not saved",
                       wanted.c_str());
            std::printf("\n  screenshot     FAILED — could not write to \"%ls\"\n", wanted.c_str());
            notify(L"shot_failed");
            continue;
        }
        if (path != wanted)
            Log::Warn("screenshot: \"%ls\" can't take it (drive missing, disk full or read-only); writing \"%ls\"",
                      wanted.c_str(), path.c_str());
        const bool ok = WriteBgraPng(path, width, height, row_pitch, bgra);
        if (!ok) Log::Error("screenshot: could not write \"%ls\"", path.c_str());
        std::printf("\n  screenshot     %s\n", ok ? "ok" : "FAILED — could not write PNG");
        std::wprintf(L"  output         %s\n\n", path.c_str());
        if (!ok) notify(L"shot_failed");
        else if (path != wanted) notify(L"shot_elsewhere");
    }
}

void RequestScreenshot(ScreenshotContext* ctx, PipelineSupervisor* supervisor,
                        BufferController* controller, const std::string& output_root,
                        bool per_game_folders, const std::wstring& out_path) {
    if (!supervisor->Armed()) {
        Log::Info("hotkeys.screenshot: pressed while the pipeline is disarmed — nothing to "
                  "capture, press dropped");
        return;
    }
    std::wstring path = out_path;
    std::wstring fallback;
    const int    n    = ctx->shot_count.fetch_add(1);
    if (path == L"out\\ombra_shot.png") {
        const std::wstring game_name = (controller) ? controller->CurrentGameFolder() : L"";
        ScreenshotPaths paths = ScreenshotPathsFor(ResolveOutputRoot(output_root),
                                                   ctx->fallback_root, game_name, per_game_folders, n);
        path = std::move(paths.path);
        fallback = std::move(paths.fallback);
    }
    {
        std::lock_guard<std::mutex> lock(ctx->request_mutex);
        ctx->pending_path = std::move(path);
        ctx->pending_fallback_path = std::move(fallback);
    }
    ctx->requested.store(true, std::memory_order_release);
}

// The sources the engine records, in capture (and hub) order: SelectActiveAudioSources minus any
// kind this build can't capture.
std::vector<AudioSourceSpec> RecordableAudioSources(const std::vector<AudioSourceSpec>& configured,
                                                    bool per_app_mode) {
    std::vector<AudioSourceSpec> out;
    for (const auto& spec : SelectActiveAudioSources(configured, per_app_mode)) {
        if (spec.kind != "output_loopback" && spec.kind != "input") {
            Log::Warn("audio source \"%s\": unrecognized kind \"%s\", skipping", spec.id.c_str(), spec.kind.c_str());
            continue;
        }
        out.push_back(spec);
    }
    return out;
}

std::vector<AudioTrackSource> TrackSourcesFrom(const std::vector<AudioSourceSpec>& specs) {
    std::vector<AudioTrackSource> out;
    for (const auto& spec : specs)
        out.push_back({AudioTrackName(spec), spec.group_id, spec.gain_db,
                       spec.kind == "input" ? AudioTrackSource::kAudibleInput : AudioTrackSource::kAudibleLoopback,
                       spec.id});
    return out;
}

// capture.framerate as a pipeline runs it: 0 ("match display") becomes `device`'s refresh rate.
uint32_t EngineFps(const Config& cfg, const std::string& device) {
    return static_cast<uint32_t>(ResolveFramerate(cfg.capture_framerate, DisplayRefreshHz(device)));
}

namespace {

// One request against the engine: HandlePipeRequest's arguments, and one member per request type.
// The members carry the parameter names, so each handler reads like the function body it was.
struct PipeRequest {
    const JsonValue&                                     req;
    Config&                                              cfg;
    PipelineSupervisor&                                  supervisor;
    BufferController&                                    controller;
    SaveContext&                                         save_ctx;
    const StartupGpuSummary&                             gpu_summary;
    Tray*                                                tray;
    ScreenshotContext*                                   screenshot_ctx;
    AutotuneContext*                                     autotune_ctx;
    const std::vector<std::unique_ptr<MonitorPipeline>>* all_pipelines;
    RawConfigMap*                                        raw_store;
    MemoryLayout*                                        memory;
    AudioSourceControl*                                  audio_sources;
    const StartSaveFeedback&                             save_feedback;
    // Held already when the command's table entry says so; save_now releases it before waiting.
    std::unique_lock<std::mutex>&                        config_lock;

    JsonValue OnShutdown();
    JsonValue OnHotkeysSuspend();
    JsonValue OnGetSchema();
    JsonValue OnGetConfig();
    JsonValue OnSetConfig();
    JsonValue OnGetStatus();
    JsonValue OnSaveNow();
    JsonValue OnScreenshot();
    JsonValue OnSetArmed();
    JsonValue OnPreviewConfig();
    JsonValue OnNotify();
    JsonValue OnListMonitors();
    JsonValue OnListWindows();
    JsonValue OnCaptureWindowThumbnail();
    JsonValue OnCaptureMonitorThumbnail();
    JsonValue OnListGames();
    JsonValue OnListProcesses();
    JsonValue OnListAudioDevices();
    JsonValue OnAudioStatus();
    JsonValue OnListAudioSessions();
    JsonValue OnUpdateStatus();
    JsonValue OnUpdateCheck();
    JsonValue OnUploadSoon();
    JsonValue OnUpdateInstall();
};

}  // namespace

JsonValue PipeRequest::OnShutdown() {
    // Same exit path as tray Exit and Ctrl+C. The pump thread id is zero until the resident
    // message loop runs (and always in harnesses that call this function directly).
    JsonValue   resp     = JsonValue::MakeObject();
    const DWORD pump_tid = g_pump_thread_id_for_ctrl_handler.load();
    if (pump_tid == 0 || !PostThreadMessage(pump_tid, WM_QUIT, 0, 0)) {
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("the engine's message loop isn't running"));
        return resp;
    }
    Telemetry::Event("shutdown_request", "source=pipe");
    return resp;
}

JsonValue PipeRequest::OnHotkeysSuspend() {
    // Settings' key-capture control, while it listens for a new combo. A lease: the UI renews
    // it every few seconds and it lapses on its own (HotkeyRegistry::kSuspendLeaseMs).
    JsonValue resp = JsonValue::MakeObject();
    const JsonValue* on = req.Find("suspend");
    HotkeyRegistry* hk = g_hotkeys_for_engine.load();
    if (!hk || !on || on->type != JsonValue::Type::Bool) {
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr(hk ? "expected {\"suspend\": bool}" : "hotkeys aren't live"));
        return resp;
    }
    hk->RequestSuspend(on->b);
    resp.Set("ok", JsonValue::MakeBool(true));
    return resp;
}

JsonValue PipeRequest::OnGetSchema() {
    return SerializeSettingsSchema();
}

JsonValue PipeRequest::OnGetConfig() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue flat = SerializeConfigFlat(cfg);
    if (raw_store) {
        for (const auto& [key, value] : *raw_store) {
            if (value.type != JsonValue::Type::Null) flat.Set(key, value);
        }
    }
    resp.Set("config", std::move(flat));
    return resp;
}

JsonValue PipeRequest::OnSetConfig() {
    const JsonValue* patch_v = req.Find("config");
    if (!patch_v || patch_v->type != JsonValue::Type::Object) {
        JsonValue resp = JsonValue::MakeObject();
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("missing or malformed \"config\" object"));
        return resp;
    }

    // Raw-kind keys are validated and persisted here, straight into
    // config.json — ApplyConfigPatch's MarkRaw (called below, inside ApplyLiveConfig) only
    // marks them "visited" (there is no Config field to hold a Raw value), which used to mean
    // a client's new value was silently discarded despite the response saying "accepted." A
    // key that fails shape validation is removed from the patch before it reaches
    // ApplyLiveConfig (so it can't silently "succeed" there too) and reported as rejected here
    // instead, with the real reason.
    JsonValue filtered_patch = *patch_v;
    const bool old_per_app_mode = cfg.audio_per_app_mode;  // see the audio sources apply below
    std::vector<std::string> raw_written;
    std::vector<ConfigIssue> raw_rejected;
    bool restart_needed = false;  // a startup-only key changed; see the restart block below
    bool audio_sources_written = false;
    bool raw_save_failed = false;  // SaveRawConfigValues below failed; see its own comment
    // A dotted key at the top level (the only shape SerializeConfigFlat/ApplyConfigPatch ever
    // produce or expect) or nested ({"capture":{"monitors":[...]}}) -- ApplyConfigPatch's own
    // Flatten() normalizes either shape before MarkRaw ever sees it, so this lookup has to
    // match that or a nested set_config patch would silently discard here (the same class of
    // a reported bug asked to close generally, not just for the flat case).
    auto find_raw = [](const JsonValue& patch, const std::string& key) -> const JsonValue* {
        if (const JsonValue* flat = patch.Find(key)) return flat;
        const size_t dot = key.find('.');
        if (dot == std::string::npos) return nullptr;
        if (const JsonValue* group = patch.Find(key.substr(0, dot))) {
            return group->Find(key.substr(dot + 1));
        }
        return nullptr;
    };
    // Both shapes again, for what ApplyLiveConfig must not see: its Flatten would find a nested
    // value left behind, and a Realloc-tier raw key then drops the buffer for nothing.
    auto drop_raw = [](JsonValue& patch, const std::string& key) {
        auto erase_key = [](JsonValue& obj, const std::string& k) {
            obj.obj.erase(std::remove_if(obj.obj.begin(), obj.obj.end(),
                                         [&](const auto& kv) { return kv.first == k; }),
                          obj.obj.end());
        };
        erase_key(patch, key);
        const size_t dot = key.find('.');
        if (dot == std::string::npos) return;
        const std::string group = key.substr(0, dot);
        for (auto& [k, v] : patch.obj)
            if (k == group && v.type == JsonValue::Type::Object) erase_key(v, key.substr(dot + 1));
        patch.obj.erase(std::remove_if(patch.obj.begin(), patch.obj.end(),
                                       [&](const auto& kv) {
                                           return kv.first == group && kv.second.type == JsonValue::Type::Object &&
                                                  kv.second.obj.empty();
                                       }),
                        patch.obj.end());
    };
    if (raw_store) {
        for (const std::string& key : RawConfigKeys()) {
            const JsonValue* v = find_raw(*patch_v, key);
            if (!v) continue;
            std::string err;
            if (ValidateRawConfigValue(key, *v, err)) {
                (*raw_store)[key] = *v;
                raw_written.push_back(key);
            } else {
                raw_rejected.push_back({key, err});
                drop_raw(filtered_patch, key);
            }
        }
        if (!raw_written.empty()) {
            RawConfigMap to_save;
            for (const auto& key : raw_written) to_save[key] = (*raw_store)[key];
            // Fix: this used to be fire-and-forget -- a failed atomic write (e.g.
            // WriteConfigFileAtomic's rename losing to a still-open read handle) left
            // config.json untouched with no trace, while raw_written being non-empty still
            // made the response below report "ok":true regardless of what ApplyLiveConfig did.
            raw_save_failed = !SaveRawConfigValues(to_save);
            if (raw_save_failed) {
                Log::Error("SaveRawConfigValues failed for keys: %s -- config.json not updated",
                          [&] {
                              std::string joined;
                              for (const auto& key : raw_written) joined += (joined.empty() ? "" : " ") + key;
                              return joined;
                          }().c_str());
            }
        }

        // Raw keys are stripped out of filtered_patch before ApplyLiveConfig, so a Free raw key
        // that the running engine reads is handed over here.
        if (std::find(raw_written.begin(), raw_written.end(), "games.list") != raw_written.end()) {
            if (GameLibrary* games = g_games_for_engine.load(std::memory_order_acquire))
                games->SetDecisions(ParseGamesList((*raw_store)["games.list"]));
        }
        if (std::find(raw_written.begin(), raw_written.end(), "ui.themes") != raw_written.end())
            SetTrayUserThemes(ParseUserThemes((*raw_store)["ui.themes"]));

        // capture.monitors has no live pipeline-rebuild path (wmain builds the MonitorPipeline
        // vector once, at startup), so a changed selection self-restarts the engine and the next
        // launch picks up the new list. Delayed on a detached
        // thread so this response reaches the client before the pipe server goes down.
        //
        // Gated on `tray` being non-null (below): only the real resident engine has one; a test
        // harness calling HandlePipeRequest directly must not spawn a second ombra.exe.
        audio_sources_written =
            std::find(raw_written.begin(), raw_written.end(), "audio.sources") != raw_written.end();
        restart_needed =
            !raw_save_failed &&
            std::find(raw_written.begin(), raw_written.end(), "capture.monitors") != raw_written.end();
        // audio.sources is persisted above and applied live below (the audio target watcher).
        // Left in the typed patch, its schema tier (Realloc) made ApplyLiveConfig reallocate the
        // ring and drop the buffer on every gain, grouping or on/off edit (seen live 2026-09-27).
        if (audio_sources_written) drop_raw(filtered_patch, "audio.sources");
    }

    if (const JsonValue* at_v = patch_v->Find("engine.autotune"); at_v && at_v->type == JsonValue::Type::String) {
        if (at_v->str == "run") {
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("tier", JsonValue::MakeStr("free"));
            resp.Set("buffer_dropped", JsonValue::MakeBool(false));

            if (!autotune_ctx) {
                resp.Set("ok", JsonValue::MakeBool(false));
                resp.Set("error", JsonValue::MakeStr("autotune not available in this context"));
                return resp;
            }
            // capturebench draws a window on the desktop; an isolated instance (OMBRA_HOME:
            // tests, ombra-ui --drive) puts nothing on the user's screen.
            if (Instance().isolated) {
                resp.Set("ok", JsonValue::MakeBool(false));
                resp.Set("error", JsonValue::MakeStr("autotune is not run by an isolated instance"));
                return resp;
            }
            AutotuneContext::State expected = AutotuneContext::State::Idle;
            if (!autotune_ctx->state.compare_exchange_strong(expected, AutotuneContext::State::Running)) {
                // Done/Failed also refuse a second "run" -- the UI is expected to "ack" a
                // latched result (see AutotuneContext's comment) before starting another.
                resp.Set("ok", JsonValue::MakeBool(false));
                resp.Set("error", JsonValue::MakeStr(
                    expected == AutotuneContext::State::Running
                        ? "a benchmark is already running"
                        : "a previous benchmark result hasn't been acknowledged yet"));
                return resp;
            }

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            wchar_t cmd[] = L"" OMBRA_ENGINE_EXE " capturebench";
            if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
                autotune_ctx->state.store(AutotuneContext::State::Idle, std::memory_order_release);
                resp.Set("ok", JsonValue::MakeBool(false));
                resp.Set("error", JsonValue::MakeStr("failed to launch capturebench"));
                return resp;
            }
            CloseHandle(pi.hThread);
            // Without this, capturebench.cpp's own SetForegroundWindow(window.Hwnd()) call
            // (after its countdown) is a different, unrelated process from Windows' point of
            // view -- this engine process is a headless background service that was never
            // itself the foreground window, so it has no foreground-setting right of its own to
            // implicitly hand to a child it spawns. Confirmed live: closing
            // the Settings overlay before firing "run" (screens.cpp) was necessary but not
            // sufficient -- capturebench still logged frames=0 aborted_focus=1, instantly, even
            // with the overlay gone. AllowSetForegroundWindow explicitly grants capturebench's
            // PID the right regardless of which process currently owns the foreground, which is
            // the standard fix for exactly this "spawned helper needs to grab focus itself"
            // pattern.
            AllowSetForegroundWindow(pi.dwProcessId);
            autotune_ctx->process = pi.hProcess;  // owned by the watcher thread from here on
            std::thread(WaitForAutotuneCompletion, autotune_ctx).detach();

            resp.Set("ok", JsonValue::MakeBool(true));
            return resp;
        }
        if (at_v->str == "ack") {
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("tier", JsonValue::MakeStr("free"));
            resp.Set("buffer_dropped", JsonValue::MakeBool(false));
            // Only a latched result: a run's watcher thread owns the context until it finishes, and
            // shutdown waits on Running to know that.
            if (autotune_ctx) {
                auto state = autotune_ctx->state.load(std::memory_order_acquire);
                if (state == AutotuneContext::State::Running) {
                    resp.Set("ok", JsonValue::MakeBool(false));
                    resp.Set("error", JsonValue::MakeStr("a benchmark is still running"));
                    return resp;
                }
                autotune_ctx->state.compare_exchange_strong(state, AutotuneContext::State::Idle);
            }
            resp.Set("ok", JsonValue::MakeBool(true));
            return resp;
        }
    }
    // Matches ApplyConfigPatch's own "nothing valid in this patch" convention: a patch that,
    // after raw-key filtering, has nothing left for ApplyLiveConfig to accept and nothing this
    // block itself wrote is a wholesale failure, not a silent no-op success.
    const bool filtered_patch_empty = filtered_patch.obj.empty();
    std::vector<LivePipeline> live_pipelines;
    if (all_pipelines) {
        for (const auto& mp : *all_pipelines)
            live_pipelines.push_back({*mp->supervisor, mp->save_ctx, mp->ring, mp->spec.suffix});
    } else {
        live_pipelines.push_back({supervisor, save_ctx, *save_ctx.ring, ""});
    }
    LiveApplyResult live_res = ApplyLiveConfig(cfg, filtered_patch, live_pipelines, controller, tray,
                                               RecordingGpuCapsOf(gpu_summary));
    // `tray` is only non-null in the real resident engine, so test harnesses driving this
    // function never switch telemetry on.
    if (tray) Telemetry::SetEnabled(cfg.engine_telemetry);
    crash::SetSharingEnabled(cfg.diagnostics_sharing != "off", cfg.diagnostics_sharing == "usage");
    if (memory && live_res.ok && live_res.realloc_sizing) {
        memory->sizing      = *live_res.realloc_sizing;
        memory->file_backed = live_res.realloc_file_backed;
    }

    if (HotkeyRegistry* hk = g_hotkeys_for_engine.load(); hk && live_res.ok &&
        std::any_of(live_res.accepted.begin(), live_res.accepted.end(),
                    [](const std::string& key) { return key.rfind("hotkeys.", 0) == 0; })) {
        hk->RequestBindings(ParseHotkeyPolicy(cfg.hotkeys_backend), HotkeySpecsFromConfig(cfg));
    }

    // A new audio.sources list or a Simple/Per-app switch goes to the audio target watcher, which
    // opens, closes and reopens only the captures that changed (DiffAudioSources) and keeps the
    // buffer. Read after ApplyLiveConfig, so cfg holds the mode that actually applied.
    if (audio_sources && raw_store && (audio_sources_written || cfg.audio_per_app_mode != old_per_app_mode)) {
        auto recordable = RecordableAudioSources(ResolveAudioSources((*raw_store)["audio.sources"]),
                                                 cfg.audio_per_app_mode);
        if (memory) memory->audio_sources = recordable.size();
        audio_sources->Post(std::move(recordable));
    }
    if (tray && restart_needed) {
        // Debounced: two startup-only key changes within 300 ms used to each spawn
        // a detached thread calling EngineSelfRestart(), two CreateProcessW calls racing for
        // the single-instance mutex with the loser dying loudly. A generation counter makes
        // only the last change in a burst restart.
        static std::atomic<uint64_t> restart_generation{0};
        const uint64_t my_generation = ++restart_generation;
        std::thread([my_generation]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (restart_generation.load(std::memory_order_acquire) == my_generation) {
                EngineSelfRestart();
            }
        }).detach();
    }
    if (!live_res.accepted.empty() || !raw_written.empty()) {
        std::string keys;
        for (const auto& key : live_res.accepted) keys += (keys.empty() ? "" : " ") + key;
        for (const auto& key : raw_written) keys += (keys.empty() ? "" : " ") + key;
        Telemetry::Event("settings_change", "tier=%s keys=%s", ApplyTierStr(live_res.tier),
                         keys.c_str());
    }
    JsonValue resp = JsonValue::MakeObject();
    const bool nothing_applied = filtered_patch_empty && raw_written.empty() && !raw_rejected.empty();
    // Fix: this used to be `live_res.ok || !raw_written.empty()` -- a raw key
    // (e.g. audio.sources, always bundled with audio.per_app_mode by the audio panel's Save)
    // being written made the response report "ok":true even when ApplyLiveConfig itself
    // rejected the typed portion of the same patch (e.g. its Realloc-tier guard). The typed
    // portion's own success now has to hold whenever there was a typed portion at all, and a
    // raw write that silently failed to reach disk (raw_save_failed, config.cpp's
    // WriteConfigFileAtomic) can no longer be reported as success either.
    const bool ok = !nothing_applied && !raw_save_failed &&
                    (filtered_patch_empty ? !raw_written.empty() : live_res.ok);
    resp.Set("ok", JsonValue::MakeBool(ok));
    if (nothing_applied && live_res.error.empty()) live_res.error = "no valid keys to apply";
    if (raw_save_failed && live_res.error.empty()) {
        live_res.error = "config.json could not be updated (write failed) -- setting not saved";
    }
    resp.Set("tier", JsonValue::MakeStr(ApplyTierStr(live_res.tier)));
    resp.Set("buffer_dropped", JsonValue::MakeBool(live_res.buffer_dropped));
    if (!live_res.error.empty()) {
        resp.Set("error", JsonValue::MakeStr(live_res.error));
    }
    JsonValue accepted = JsonValue::MakeArray();
    for (const auto& key : live_res.accepted) accepted.arr.push_back(JsonValue::MakeStr(key));
    JsonValue rejected = JsonValue::MakeArray();
    for (const auto& issue : live_res.rejected) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("key", JsonValue::MakeStr(issue.key));
        entry.Set("reason", JsonValue::MakeStr(issue.message));
        rejected.arr.push_back(std::move(entry));
    }
    // ApplyConfigPatch's MarkRaw already marks a present-and-well-formed Raw key "visited" (it
    // has no shape validation of its own), so a raw key that made it through filtering above is
    // typically already in live_res.accepted — add it here only if it somehow isn't.
    for (const auto& key : raw_written) {
        if (std::find(live_res.accepted.begin(), live_res.accepted.end(), key) ==
            live_res.accepted.end()) {
            accepted.arr.push_back(JsonValue::MakeStr(key));
        }
    }
    for (const auto& issue : raw_rejected) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("key", JsonValue::MakeStr(issue.key));
        entry.Set("reason", JsonValue::MakeStr(issue.message));
        rejected.arr.push_back(std::move(entry));
    }
    resp.Set("accepted", std::move(accepted));
    resp.Set("rejected", std::move(rejected));
    std::string note = live_res.note;
    if (!raw_written.empty()) {
        std::string names;
        for (const auto& key : raw_written) { if (!names.empty()) names += ", "; names += key; }
        if (!note.empty()) note += "; ";
        // Exclusions, game overrides and a same-topology audio.sources edit are applied live
        // above; capture.monitors and an audio add/remove/retarget self-restart the engine.
        note += names + (restart_needed ? ": written to config.json, restarting the engine"
                                        : ": written to config.json and applied");
    }
    if (!note.empty()) resp.Set("note", JsonValue::MakeStr(note));
    return resp;
}

JsonValue PipeRequest::OnGetStatus() {
    double ws_mb = 0, private_mb = 0;
    GetProcessRssMb(ws_mb, private_mb);
    // Recording a game now, not the name kept for saves after a disarm.
    const std::wstring game = controller.CurrentGamePid() ? controller.CurrentGameFolder() : std::wstring{};
    JsonValue resp = JsonValue::MakeObject();
    // `enabled` is the Instant Replay switch; `armed` is whether pipeline[0] records right now.
    // Switched on and not armed: waiting for a game, or for the One window program.
    resp.Set("enabled", JsonValue::MakeBool(controller.UserEnabled()));
    resp.Set("armed", JsonValue::MakeBool(supervisor.Armed()));
    resp.Set("mode", JsonValue::MakeStr(cfg.capture_mode));
    resp.Set("target", JsonValue::MakeStr(cfg.capture_target));
    resp.Set("current_game", game.empty() ? JsonValue{} : JsonValue::MakeStr(NarrowUtf8(game)));
    resp.Set("buffer_seconds", JsonValue::MakeNum(cfg.buffer_seconds));
    // The rate the pipeline was built at: "match display" resolved at the last build/rebuild.
    resp.Set("fps", JsonValue::MakeNum(save_ctx.fps.load()));
    {
        std::lock_guard<std::mutex> lock(save_ctx.last_save_mutex);
        resp.Set("last_save_path", save_ctx.last_save_path.empty()
                     ? JsonValue{} : JsonValue::MakeStr(NarrowUtf8(save_ctx.last_save_path)));
        resp.Set("last_save_time_qpc",
                 save_ctx.last_save_qpc < 0
                     ? JsonValue{}
                     : JsonValue::MakeNum(static_cast<double>(save_ctx.last_save_qpc)));
    }
    resp.Set("rss_ws_mb", JsonValue::MakeNum(ws_mb));
    resp.Set("rss_private_mb", JsonValue::MakeNum(private_mb));
    {
        // Device RAM, so the memory card can say what share of the machine Ombra holds.
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            resp.Set("sys_mem_total_mb", JsonValue::MakeNum(static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0)));
            resp.Set("sys_mem_load_pct", JsonValue::MakeNum(static_cast<double>(mem.dwMemoryLoad)));
        }
    }
    // VRAM (capture textures + NVENC surfaces). The telemetry sampler's last read rather than
    // a second PDH query here; left out when telemetry is off.
    if (const double vram_mb = Telemetry::LastGpuDedicatedMb(); vram_mb >= 0.0) {
        resp.Set("gpu_dedicated_mb", JsonValue::MakeNum(vram_mb));
    }
    double fill_pct = 0.0;
    double live_bytes = 0.0;
    if (Ring* r = supervisor.GetRing()) {
        live_bytes = static_cast<double>(r->LiveBytes());
        fill_pct = r->Capacity() > 0 ? (live_bytes / r->Capacity()) * 100.0 : 0.0;
    }
    resp.Set("buffer_fill_pct", JsonValue::MakeNum(fill_pct));
    // Real measured bytes, not the bitrate*seconds estimate preview_config computes -- Settings
    // prefers this once the engine is actually buffering.
    resp.Set("buffer_live_bytes", JsonValue::MakeNum(live_bytes));
    // The engine's own LogGpuTopologyStartup() classification, cached at
    // startup and reported here rather than ombra-ui probing topology a second time — see
    // gputopo.h's StartupGpuSummary comment for why (two independent probes have already been
    // observed disagreeing across sessions on a test machine).
    // Classified against the adapter the pipeline is actually on, not the startup guess:
    // encode.adapter can change without a restart.
    const EncodeAdapter encode_adapter = supervisor.CurrentEncodeAdapter();
    const Topology      topology       = ClassifyForEncoder(gpu_summary, encode_adapter.luid);
    resp.Set("gpu_topology", JsonValue::MakeStr(TopologyLabel(topology)));
    resp.Set("gpu_topology_desc", JsonValue::MakeStr(TopologyDescription(topology)));
    resp.Set("gpu_display_owner", gpu_summary.display_owner_description.empty()
                 ? JsonValue{} : JsonValue::MakeStr(NarrowUtf8(gpu_summary.display_owner_description)));
    resp.Set("encoder_backend", JsonValue::MakeStr(BackendName(encode_adapter.backend)));
    // What the running pipeline opened (AV1 falls back to H.264 inside the encoder); absent
    // before the first pipeline build.
    if (const auto params = supervisor.CurrentParams(); params.width > 0)
        resp.Set("encoder_codec", JsonValue::MakeStr(VideoCodecName(params.codec)));
    if (HotkeyRegistry* hk = g_hotkeys_for_engine.load()) {
        const HotkeyStatus hs = hk->Status();
        JsonValue o = JsonValue::MakeObject();
        o.Set("policy", JsonValue::MakeStr(hs.policy));
        o.Set("suspended", JsonValue::MakeBool(hs.suspended));
        JsonValue list = JsonValue::MakeArray();
        for (const auto& b : hs.bindings) {
            JsonValue e = JsonValue::MakeObject();
            e.Set("key", JsonValue::MakeStr(b.key));
            e.Set("spec", JsonValue::MakeStr(b.spec));
            e.Set("ok", JsonValue::MakeBool(b.ok));
            e.Set("backend", b.backend.empty() ? JsonValue{} : JsonValue::MakeStr(b.backend));
            list.arr.push_back(std::move(e));
        }
        o.Set("bindings", std::move(list));
        resp.Set("hotkeys", std::move(o));
    }
    {
        JsonValue gpus = JsonValue::MakeArray();
        for (const auto& g : gpu_summary.recording_gpus) {
            JsonValue o = JsonValue::MakeObject();
            o.Set("vendor", JsonValue::MakeStr(g.vendor));
            o.Set("backend", JsonValue::MakeStr(g.backend));
            o.Set("name", JsonValue::MakeStr(NarrowUtf8(g.description)));
            o.Set("hevc", JsonValue::MakeBool(g.hevc));
            o.Set("av1", JsonValue::MakeBool(g.av1));
            o.Set("av1_align_w", JsonValue::MakeNum(g.av1_align_w));
            o.Set("av1_align_h", JsonValue::MakeNum(g.av1_align_h));
            o.Set("drives_display", JsonValue::MakeBool(g.drives_display));
            gpus.arr.push_back(std::move(o));
        }
        resp.Set("recording_gpus", std::move(gpus));
    }
    resp.Set("encoder_adapter", encode_adapter.description.empty()
                 ? JsonValue{} : JsonValue::MakeStr(NarrowUtf8(encode_adapter.description)));
    if (autotune_ctx) {
        const char* state_str = "idle";
        switch (autotune_ctx->state.load(std::memory_order_acquire)) {
            case AutotuneContext::State::Running: state_str = "running"; break;
            case AutotuneContext::State::Done:     state_str = "done"; break;
            case AutotuneContext::State::Failed:   state_str = "failed"; break;
            case AutotuneContext::State::Idle:     default: break;
        }
        resp.Set("autotune_state", JsonValue::MakeStr(state_str));
        std::lock_guard<std::mutex> lock(autotune_ctx->result_mutex);
        resp.Set("autotune_result", autotune_ctx->last_result);
    }
    // Additive per-pipeline array. Every field above stays sourced from
    // pipeline[0] and keeps meaning what it means today — ombra-ui's existing 2 Hz get_status
    // poll parses this response unmodified against an N-monitor engine. No panel row reads
    // this yet (Advanced-tier surfacing is a later UI change, out of scope here).
    if (all_pipelines) {
        JsonValue pipelines_json = JsonValue::MakeArray();
        for (const auto& mp : *all_pipelines) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("device", JsonValue::MakeStr(mp->spec.device));
            entry.Set("suffix", JsonValue::MakeStr(mp->spec.suffix));
            entry.Set("armed", JsonValue::MakeBool(mp->supervisor->Armed()));
            entry.Set("rebuild_count",
                      JsonValue::MakeNum(static_cast<double>(mp->supervisor->RebuildCount())));
            double pipeline_live_bytes = static_cast<double>(mp->ring.LiveBytes());
            double pipeline_fill_pct = 0.0;
            if (mp->ring.Capacity() > 0) {
                pipeline_fill_pct = (pipeline_live_bytes / mp->ring.Capacity()) * 100.0;
            }
            entry.Set("buffer_fill_pct", JsonValue::MakeNum(pipeline_fill_pct));
            entry.Set("buffer_live_bytes", JsonValue::MakeNum(pipeline_live_bytes));
            pipelines_json.arr.push_back(std::move(entry));
        }
        resp.Set("pipelines", std::move(pipelines_json));
    }
    if (memory) {
        // Ring sizes (capacity + overshoot per ring, the most they take), not fill -- MemoryLayout.
        JsonValue m = JsonValue::MakeObject();
        m.Set("video_pipelines", JsonValue::MakeNum(static_cast<double>(memory->video_pipelines)));
        m.Set("video_capacity_bytes", JsonValue::MakeNum(static_cast<double>(memory->sizing.video_capacity)));
        m.Set("video_overshoot_bytes", JsonValue::MakeNum(static_cast<double>(memory->sizing.video_overshoot)));
        m.Set("audio_sources", JsonValue::MakeNum(static_cast<double>(memory->audio_sources)));
        m.Set("audio_capacity_bytes", JsonValue::MakeNum(static_cast<double>(memory->sizing.audio_capacity)));
        m.Set("audio_overshoot_bytes", JsonValue::MakeNum(static_cast<double>(memory->sizing.audio_overshoot)));
        m.Set("file_backed", JsonValue::MakeBool(memory->file_backed));
        // What the rings hold in memory right now: they commit as footage arrives and are
        // freed a buffer length after a disarm, so this is below capacity + overshoot until
        // the buffer has filled once. The UI's engine baseline is private bytes minus these.
        if (all_pipelines) {
            size_t video_committed = 0;
            if (!memory->file_backed)
                for (const auto& mp : *all_pipelines) video_committed += mp->ring.CommittedBytes();
            m.Set("video_committed_bytes", JsonValue::MakeNum(static_cast<double>(video_committed)));
        }
        if (save_ctx.audio)
            m.Set("audio_committed_bytes", JsonValue::MakeNum(static_cast<double>(save_ctx.audio->CommittedBytes())));
        resp.Set("memory", std::move(m));
    }
    return resp;
}

JsonValue PipeRequest::OnSaveNow() {
    const StartSaveFeedback feedback = cfg.ui_toast_enabled && cfg.ui_toast_on_save ? save_feedback : StartSaveFeedback{};
    config_lock.unlock();
    std::vector<SaveContext*> contexts;
    if (all_pipelines) {
        for (const auto& pipeline : *all_pipelines) contexts.push_back(&pipeline->save_ctx);
    } else {
        contexts.push_back(&save_ctx);
    }
    // Wait only on this pipe worker. Hotkeys dispatch the same operation without waiting.
    const auto result = SaveReplay(contexts, feedback).get();
    JsonValue resp = JsonValue::MakeObject();
    JsonValue monitors = JsonValue::MakeArray();
    std::wstring first_path;
    bool any_saved = false;
    for (size_t i = 0; i < result.monitors.size(); ++i) {
        const auto& monitor = result.monitors[i];
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("monitor", JsonValue::MakeStr(contexts[i]->monitor_device));
        entry.Set("reason", JsonValue::MakeStr(SaveOutcomeName(monitor.outcome)));
        entry.Set("path", monitor.path.empty() ? JsonValue{} : JsonValue::MakeStr(NarrowUtf8(monitor.path)));
        entry.Set("fell_back", JsonValue::MakeBool(monitor.fell_back));
        if (monitor.outcome == SaveOutcome::kSaved) {
            any_saved = true;
            if (first_path.empty()) first_path = monitor.path;
        }
        monitors.arr.push_back(std::move(entry));
    }
    resp.Set("monitors", std::move(monitors));
    resp.Set("saved", JsonValue::MakeBool(any_saved));
    resp.Set("reason", JsonValue::MakeStr(SaveOutcomeName(result.outcome)));
    resp.Set("path", first_path.empty() ? JsonValue{} : JsonValue::MakeStr(NarrowUtf8(first_path)));
    if (result.outcome == SaveOutcome::kSaved) resp.Set("error", JsonValue{});
    else resp.Set("error", JsonValue::MakeStr(
        result.outcome == SaveOutcome::kBusy ? "a save is already in progress"
        : result.outcome == SaveOutcome::kNothingNew ? "nothing new to save: under 1 s of footage"
        : "one or more monitors could not be saved completely - see monitor results and the log"));
    return resp;
}

JsonValue PipeRequest::OnScreenshot() {
    JsonValue resp = JsonValue::MakeObject();
    if (!screenshot_ctx) {
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("screenshot worker not available"));
        return resp;
    }
    if (!supervisor.Armed()) {
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("not currently recording — nothing to capture"));
        return resp;
    }
    const auto [root, per_game] = ReadLiveConfig(cfg, [](const Config& c) {
        return std::pair(c.output_root, c.output_per_game_folders);
    });
    RequestScreenshot(screenshot_ctx, &supervisor, &controller, root, per_game, L"out\\ombra_shot.png");
    resp.Set("ok", JsonValue::MakeBool(true));
    return resp;
}

// Buffer On/Off, driven from the overlay's Instant Replay tile — replaces the old Home-screen
// pattern of set_config'ing capture.mode to fake a pause (that conflated "desktop vs game
// mode" with "armed vs disarmed"). Fans out across every monitor pipeline, same as
// hotkeys.toggle_buffer's handler in wmain, since arm/disarm is a mode decision,
// not a per-monitor one.
JsonValue PipeRequest::OnSetArmed() {
    JsonValue resp = JsonValue::MakeObject();
    const JsonValue* armed_v = req.Find("armed");
    if (!armed_v || armed_v->type != JsonValue::Type::Bool) {
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("missing or non-bool \"armed\" parameter"));
        return resp;
    }
    const bool armed = armed_v->b;
    // The switch, not the pipelines: the controller fans it out and decides, with When/What,
    // whether anything records right now (a switched-on buffer can be waiting for a game).
    controller.SetUserEnabled(armed);
    if (tray) tray->SetTooltip(TrayTooltip(armed));
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("armed", JsonValue::MakeBool(armed));
    return resp;
}

JsonValue PipeRequest::OnPreviewConfig() {
    const JsonValue* patch_v = req.Find("config");
    if (!patch_v || patch_v->type != JsonValue::Type::Object) {
        JsonValue resp = JsonValue::MakeObject();
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("missing or malformed \"config\" object"));
        return resp;
    }
    PipelineSupervisor::PipelineParams params = supervisor.CurrentParams();
    int disp_w = params.source_width ? static_cast<int>(params.source_width) : 1920;
    int disp_h = params.source_height ? static_cast<int>(params.source_height) : 1080;
    ConfigPreview prev = PreviewConfig(cfg, *patch_v, disp_w, disp_h, DisplayRefreshHz(save_ctx.monitor_device),
                                       RecordingGpuCapsOf(gpu_summary));

    JsonValue resp = JsonValue::MakeObject();
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("apply_tier", JsonValue::MakeStr(ApplyTierStr(prev.apply_tier)));
    resp.Set("ring_bytes", JsonValue::MakeNum(static_cast<double>(prev.ring_bytes)));
    resp.Set("video_ring_total_bytes", JsonValue::MakeNum(static_cast<double>(prev.sizing.VideoBytes())));
    resp.Set("audio_ring_total_bytes", JsonValue::MakeNum(static_cast<double>(prev.sizing.AudioBytes())));
    resp.Set("mb_per_minute", JsonValue::MakeNum(prev.mb_per_min));
    resp.Set("bitrate_kbps", JsonValue::MakeNum(static_cast<double>(prev.bitrate_kbps)));
    resp.Set("fps", JsonValue::MakeNum(prev.fps));
    resp.Set("display_fps", JsonValue::MakeNum(prev.display_fps));
    // What capture.resolution scales from; the UI resolves the output size itself (ResolveOutputSize).
    if (params.source_width && params.source_height) {  // 0 until a generation has been built
        resp.Set("source_width", JsonValue::MakeNum(params.source_width));
        resp.Set("source_height", JsonValue::MakeNum(params.source_height));
    }

    JsonValue derived_obj = JsonValue::MakeObject();
    for (const auto& [k, v] : prev.derived) {
        derived_obj.Set(k, v);
    }
    resp.Set("derived", std::move(derived_obj));

    JsonValue warn_arr = JsonValue::MakeArray();
    for (const auto& w : prev.warnings) {
        warn_arr.arr.push_back(JsonValue::MakeStr(w));
    }
    resp.Set("warnings", std::move(warn_arr));
    return resp;
}

JsonValue PipeRequest::OnNotify() {
    const JsonValue* title_v   = req.Find("title");
    const JsonValue* message_v = req.Find("message");
    const std::string title    = (title_v && title_v->type == JsonValue::Type::String)
                                      ? title_v->str : OMBRA_PRODUCT_NAME;
    const std::string message  = (message_v && message_v->type == JsonValue::Type::String)
                                      ? message_v->str : "";
    if (tray) tray->ShowBalloon(WideFromUtf8(title), WideFromUtf8(message));
    JsonValue resp = JsonValue::MakeObject();
    resp.Set("ok", JsonValue::MakeBool(true));
    return resp;
}

// Settings pickers -- three pure OS
// queries, no engine state touched. Synchronous and local (named-pipe round trip), same as
// every other request this function serves.
JsonValue PipeRequest::OnListMonitors() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr = JsonValue::MakeArray();
    for (const MonitorInfo& m : EnumerateAllMonitors()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("device", JsonValue::MakeStr(m.device));
        entry.Set("label", JsonValue::MakeStr(m.label));
        entry.Set("left", JsonValue::MakeNum(static_cast<double>(m.left)));
        entry.Set("top", JsonValue::MakeNum(static_cast<double>(m.top)));
        entry.Set("width", JsonValue::MakeNum(static_cast<double>(m.width)));
        entry.Set("height", JsonValue::MakeNum(static_cast<double>(m.height)));
        entry.Set("primary", JsonValue::MakeBool(m.primary));
        arr.arr.push_back(std::move(entry));
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("monitors", std::move(arr));
    return resp;
}

JsonValue PipeRequest::OnListWindows() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr = JsonValue::MakeArray();
    for (const WindowInfo& w : EnumerateCapturableWindows()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("title", JsonValue::MakeStr(w.title));
        entry.Set("process", JsonValue::MakeStr(w.process));
        // What ChooseTargetWindow reads, so the picker can say which window the engine would record.
        entry.Set("image_path", JsonValue::MakeStr(w.image_path));
        entry.Set("minimized", JsonValue::MakeBool(w.minimized));
        entry.Set("foreground", JsonValue::MakeBool(w.foreground));
        entry.Set("covers_monitor", JsonValue::MakeBool(w.covers_monitor));
        entry.Set("area", JsonValue::MakeNum(static_cast<double>(w.area)));
        // The picker's controller icon and "Mark as game": what the Games list says about it.
        if (const GameLibrary* games = g_games_for_engine.load(std::memory_order_acquire)) {
            const GameVerdict v = games->Classify(WideFromUtf8(w.image_path));
            entry.Set("is_game", JsonValue::MakeBool(v == GameVerdict::Game));
            entry.Set("not_game", JsonValue::MakeBool(v == GameVerdict::NotGame));
        }
        // A same-session key for the thumbnail-grid picker's capture_window_thumbnail request
        // below -- never persisted.
        entry.Set("hwnd", JsonValue::MakeNum(static_cast<double>(w.hwnd)));
        arr.arr.push_back(std::move(entry));
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("windows", std::move(arr));
    return resp;
}

// One-shot preview grabs for Settings' window grid and display tiles. Each runs on this
// request's own ServeClient thread (pipe.cpp) with its own D3D11 device + WGC session
// (capture.cpp), concurrently with the recording pipeline, and never die()s: a window that closed
// since list_windows is routine. The reply carries a file path, not image bytes -- PipeClient is
// one connection, one request in flight, unfit for even one compressed image over the wire.
JsonValue PipeRequest::OnCaptureWindowThumbnail() {
    const JsonValue* hwnd_v = req.Find("hwnd");
    if (!hwnd_v || hwnd_v->type != JsonValue::Type::Number) {
        JsonValue resp = JsonValue::MakeObject();
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("missing hwnd"));
        return resp;
    }
    const uint64_t hwnd_u = static_cast<uint64_t>(hwnd_v->num);
    return WriteThumbnailReply(WindowThumbPath(hwnd_u), [hwnd_u](const std::wstring& path) {
        return CaptureWindowThumbnailToPng(reinterpret_cast<HWND>(hwnd_u), /*max_dim=*/320, path);
    }, "window");
}

JsonValue PipeRequest::OnCaptureMonitorThumbnail() {
    const JsonValue* device_v = req.Find("device");
    if (!device_v || device_v->type != JsonValue::Type::String) {
        JsonValue resp = JsonValue::MakeObject();
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("missing device"));
        return resp;
    }
    // 960, not the window grid's 320: the display tiles are much bigger, and 320 read as
    // visibly blurry there.
    const std::string device = device_v->str;
    return WriteThumbnailReply(MonitorThumbPath(device), [&device](const std::wstring& path) {
        return CaptureMonitorThumbnailToPng(device, /*max_dim=*/960, path);
    }, "monitor " + device);
}

// The Games page: the merged list (launcher-detected entries with the user's decisions applied,
// then the user's own) and the "looked like a game" apps Game mode armed for on its heuristic.
JsonValue PipeRequest::OnListGames() {
    JsonValue resp = JsonValue::MakeObject();
    GameLibrary* games = g_games_for_engine.load(std::memory_order_acquire);
    if (!games) {
        resp.Set("ok", JsonValue::MakeBool(false));
        resp.Set("error", JsonValue::MakeStr("no games list in this engine"));
        return resp;
    }
    if (const JsonValue* rescan = req.Find("rescan"); rescan && rescan->type == JsonValue::Type::Bool && rescan->b)
        games->Rescan();
    JsonValue arr = JsonValue::MakeArray();
    for (const GameEntry& g : games->Games()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue::MakeStr(NarrowUtf8(g.name)));
        entry.Set("path", JsonValue::MakeStr(NarrowUtf8(g.path)));
        entry.Set("source", JsonValue::MakeStr(GameSourceName(g.source)));
        entry.Set("game", JsonValue::MakeBool(g.is_game));
        entry.Set("detected", JsonValue::MakeBool(g.detected));
        if (!g.icon.empty()) entry.Set("icon", JsonValue::MakeStr(NarrowUtf8(g.icon)));
        arr.arr.push_back(std::move(entry));
    }
    JsonValue seen = JsonValue::MakeArray();
    for (const SeenApp& a : games->Seen()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue::MakeStr(NarrowUtf8(a.name)));
        entry.Set("path", JsonValue::MakeStr(NarrowUtf8(a.path)));
        entry.Set("last_seen", JsonValue::MakeNum(static_cast<double>(a.last_seen)));
        seen.arr.push_back(std::move(entry));
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("games", std::move(arr));
    resp.Set("seen", std::move(seen));
    return resp;
}

JsonValue PipeRequest::OnListProcesses() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr = JsonValue::MakeArray();
    for (const auto& [name, path] : ListRunningProcessesUtf8()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue::MakeStr(name));
        entry.Set("path", JsonValue::MakeStr(path));
        // Same FileDescription/ProductName lookup DoSave's per-game folder naming uses
        // (controller.h) -- lets the audio picker show "Discord"/"a game" the way
        // Windows' own Volume Mixer does, instead of the bare .exe name. Falls back to the exe
        // name for anything with no version resource.
        const std::wstring friendly = path.empty() ? L"" : ReadVersionResourceName(WideFromUtf8(path));
        entry.Set("display_name", JsonValue::MakeStr(friendly.empty() ? name : NarrowUtf8(friendly)));
        arr.arr.push_back(std::move(entry));
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("processes", std::move(arr));
    return resp;
}

// "which processes are making sound right now", for the
// drag-to-group audio panel's auto-populated card list -- distinct from list_processes above,
// which lists windowed processes regardless of whether they're producing audio.
// ListActiveAudioSessions (audio.h) is a WASAPI query, not engine state, same "pure OS query"
// category as list_monitors/list_windows/list_processes.
JsonValue PipeRequest::OnListAudioDevices() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr = JsonValue::MakeArray();
    {
        ComInit com;  // this handler's own ServeClient thread never initializes COM
        // Outputs (Desktop's picker) and inputs (Mic's), told apart by "input".
        for (const bool input : {false, true}) {
            for (const AudioDeviceInfo& d : ListAudioDevices(input)) {
                JsonValue entry = JsonValue::MakeObject();
                entry.Set("id", JsonValue::MakeStr(d.id));
                entry.Set("name", JsonValue::MakeStr(d.name));
                entry.Set("is_default", JsonValue::MakeBool(d.is_default));
                entry.Set("input", JsonValue::MakeBool(input));
                arr.arr.push_back(std::move(entry));
            }
        }
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("devices", std::move(arr));
    return resp;
}

// Polled about once a second while the Audio page is open, so it takes no config_lock: the
// board and the hub lock for themselves.
JsonValue PipeRequest::OnAudioStatus() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr  = JsonValue::MakeArray();
    if (audio_sources && save_ctx.audio) {
        const AudioTrackHub& hub = *save_ctx.audio;
        for (const auto& st : audio_sources->board.Snapshot([&hub](size_t i) { return hub.LastAudibleQpc(i); },
                                                     QpcNow(), QpcFrequency())) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("id", JsonValue::MakeStr(st.id));
            entry.Set("state", JsonValue::MakeStr(AudioSourceStateName(st.state)));
            arr.arr.push_back(std::move(entry));
        }
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("sources", std::move(arr));
    return resp;
}

JsonValue PipeRequest::OnListAudioSessions() {
    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr = JsonValue::MakeArray();
    {
        ComInit com;  // this handler's own ServeClient thread never initializes COM
        for (const AudioSessionInfo& s : ListActiveAudioSessions()) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("pid", JsonValue::MakeNum(static_cast<double>(s.pid)));
            entry.Set("name", JsonValue::MakeStr(s.exe_name));
            entry.Set("path", JsonValue::MakeStr(s.path));
            entry.Set("display_name", JsonValue::MakeStr(s.display_name));
            arr.arr.push_back(std::move(entry));
        }
    }
    resp.Set("ok", JsonValue::MakeBool(true));
    resp.Set("sessions", std::move(arr));
    return resp;
}

namespace {

// Every request type the engine answers. `config_lock`: taken before the handler runs (see
// HandlePipeRequest); audio_status, the pickers and the thumbnails don't need it.
// Updates (update.h). An engine without an Updater (isolated, timedtest) still answers
// update_status, with state "off", so Settings can draw the card.
JsonValue PipeRequest::OnUpdateStatus() {
    JsonValue resp = JsonValue::MakeObject();
    resp.Set("ok", JsonValue::MakeBool(true));
    Updater* u = g_updater_for_engine.load();
    const Updater::Status s = u ? u->GetStatus() : Updater::Status{};
    resp.Set("state", JsonValue::MakeStr(u ? s.state : "off"));
    resp.Set("current", JsonValue::MakeStr(AppVersion()));
    resp.Set("latest", JsonValue::MakeStr(s.latest));
    resp.Set("page_url", JsonValue::MakeStr(s.page_url));
    resp.Set("error", JsonValue::MakeStr(s.error));
    resp.Set("install_kind", JsonValue::MakeStr(u ? s.install_kind : ""));
    resp.Set("last_check_unix", JsonValue::MakeNum(static_cast<double>(s.last_check_unix)));
    return resp;
}

JsonValue PipeRequest::OnUpdateCheck() {
    JsonValue resp = JsonValue::MakeObject();
    Updater* u = g_updater_for_engine.load();
    if (u) u->RequestCheck();
    resp.Set("ok", JsonValue::MakeBool(u != nullptr));
    if (!u) resp.Set("error", JsonValue::MakeStr("updates are off in this instance"));
    return resp;
}

JsonValue PipeRequest::OnUpdateInstall() {
    JsonValue resp = JsonValue::MakeObject();
    Updater* u = g_updater_for_engine.load();
    std::string error = "updates are off in this instance";
    const bool ok = u && u->RequestInstall(&error);
    resp.Set("ok", JsonValue::MakeBool(ok));
    if (!ok) resp.Set("error", JsonValue::MakeStr(error));
    return resp;
}

JsonValue PipeRequest::OnUploadSoon() {
    crash_upload::UploadSoon();
    JsonValue resp = JsonValue::MakeObject();
    resp.Set("ok", JsonValue::MakeBool(true));
    return resp;
}

struct PipeCommand {
    const char* type;
    bool        config_lock;
    JsonValue (PipeRequest::*handle)();
};

constexpr PipeCommand kPipeCommands[] = {
    {"shutdown",                  false, &PipeRequest::OnShutdown},
    {"hotkeys_suspend",           false, &PipeRequest::OnHotkeysSuspend},
    {"get_schema",                false, &PipeRequest::OnGetSchema},
    {"get_config",                true,  &PipeRequest::OnGetConfig},
    {"set_config",                true,  &PipeRequest::OnSetConfig},
    {"get_status",                true,  &PipeRequest::OnGetStatus},
    {"save_now",                  true,  &PipeRequest::OnSaveNow},
    {"screenshot",                false, &PipeRequest::OnScreenshot},
    {"set_armed",                 false, &PipeRequest::OnSetArmed},
    {"preview_config",            true,  &PipeRequest::OnPreviewConfig},
    {"notify",                    false, &PipeRequest::OnNotify},
    {"list_monitors",             false, &PipeRequest::OnListMonitors},
    {"list_windows",              false, &PipeRequest::OnListWindows},
    {"capture_window_thumbnail",  false, &PipeRequest::OnCaptureWindowThumbnail},
    {"capture_monitor_thumbnail", false, &PipeRequest::OnCaptureMonitorThumbnail},
    {"list_games",                false, &PipeRequest::OnListGames},
    {"list_processes",            false, &PipeRequest::OnListProcesses},
    {"list_audio_devices",        false, &PipeRequest::OnListAudioDevices},
    {"audio_status",              false, &PipeRequest::OnAudioStatus},
    {"list_audio_sessions",       false, &PipeRequest::OnListAudioSessions},
    {"update_status",             false, &PipeRequest::OnUpdateStatus},
    {"update_check",              false, &PipeRequest::OnUpdateCheck},
    {"update_install",            false, &PipeRequest::OnUpdateInstall},
    {"upload_soon",               false, &PipeRequest::OnUploadSoon},
};

}  // namespace

JsonValue HandlePipeRequest(const JsonValue& req, Config& cfg, PipelineSupervisor& supervisor,
                             BufferController& controller, SaveContext& save_ctx,
                             const StartupGpuSummary& gpu_summary, Tray* tray,
                             ScreenshotContext* screenshot_ctx, AutotuneContext* autotune_ctx,
                             const std::vector<std::unique_ptr<MonitorPipeline>>* all_pipelines,
                             RawConfigMap* raw_store, MemoryLayout* memory,
                             AudioSourceControl* audio_sources, const StartSaveFeedback& save_feedback) {
    const JsonValue* type_v = req.Find("type");
    const std::string type  = (type_v && type_v->type == JsonValue::Type::String)
                                   ? type_v->str : "";

    // `cfg` is a single process-lifetime Config shared across every connected pipe client's own
    // worker thread (pipe.h: one thread per client). get_config/preview_config read it and
    // set_config's ApplyLiveConfig does an unsynchronized `Config working = cfg; ...; cfg =
    // working` read-modify-write -- two clients (or a client racing this function's own test call
    // sites) touching it at the same moment is a genuine data race, not just a logic bug. A single
    // client already serializes its own requests (PipeClient::Request blocks), so this only bites
    // multi-client or scripted-concurrent access, but it's real UB regardless of how fast Settings
    // is clicked. Coarser than strictly necessary (a Rebuild/Realloc set_config holds this for the
    // whole reconfigure, not just the Config read/write) -- PipelineSupervisor::reconfig_mutex_
    // already serializes the expensive part against itself, so the only added cost is a second,
    // concurrent client's get_config/preview_config/get_status occasionally waiting out a rebuild
    // it would have raced anyway.
    static std::mutex config_mutex;
    std::unique_lock<std::mutex> config_lock(config_mutex, std::defer_lock);
    PipeRequest request{req, cfg, supervisor, controller, save_ctx, gpu_summary, tray, screenshot_ctx,
                        autotune_ctx, all_pipelines, raw_store, memory, audio_sources, save_feedback,
                        config_lock};
    for (const PipeCommand& command : kPipeCommands) {
        if (type != command.type) continue;
        if (command.config_lock) config_lock.lock();
        return (request.*command.handle)();
    }

    JsonValue resp = JsonValue::MakeObject();
    resp.Set("ok", JsonValue::MakeBool(false));
    resp.Set("error", JsonValue::MakeStr("unknown type"));
    return resp;
}

}  // namespace ombra
