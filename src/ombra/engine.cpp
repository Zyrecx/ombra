// RunEngine: startup, mode dispatch, the resident engine's wiring and its message loop.

#include "ombra/engine.h"
#include "ombra/brand.h"
#include "ombra/engine_pipe.h"

#include "ombra/audio.h"
#include "ombra/capture.h"
#include "ombra/capturebench.h"
#include "ombra/clip_ops.h"
#include "ombra/clip_reencode.h"
#include "ombra/config.h"
#include "ombra/controller.h"
#include "ombra/crash.h"
#include "ombra/crash_upload.h"
#include "ombra/die.h"
#include "ombra/version.h"
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
#include "ombra/protocol.h"
#include "ombra/screenshot.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"
#include "ombra/reconfig.h"
#include "ombra/save.h"
#include "ombra/save_toast.h"
#include "ombra/util.h"
#include "ombra/telemetry.h"
#include "ombra/update.h"

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
#include <ctime>
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

using namespace ombra;

namespace {

// A labelled memory snapshot at each stage of startup, so the startup cost is attributed rather
// than a single number. GetProcessRssMb lives in telemetry.cpp, shared with get_status and the
// periodic sampler.
void LogRssStage(const char* stage) {
    double ws_mb = 0, private_mb = 0;
    if (!GetProcessRssMb(ws_mb, private_mb)) return;
    std::printf("  rss            %-28s  WS %7.1f MB  private %7.1f MB\n", stage, ws_mb, private_mb);
    Log::Info("rss stage=%-28s ws_mb=%.1f private_mb=%.1f", stage, ws_mb, private_mb);
}

// Previews are keyed by monitor and by HWND, so window ones pile up as windows come and go. Runs at
// engine start: drops anything not grabbed for a week, leftover temp files, and the older
// nonce-named files (every grab used to get a new name, and nothing ever deleted them).
void PruneThumbs() {
    const std::wstring dir = ThumbsDir();
    if (dir.empty()) return;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = (static_cast<uint64_t>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    constexpr uint64_t kMaxAge = 7ull * 24 * 3600 * 10'000'000;  // FILETIME ticks are 100 ns
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        const uint64_t written =
            (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const bool current_name = (name.rfind(L"mon_", 0) == 0 || name.rfind(L"win_", 0) == 0) &&
                                  name.size() > 4 && name.compare(name.size() - 4, 4, L".png") == 0;
        if (!current_name || (now > written && now - written > kMaxAge)) {
            if (DeleteFileW((dir + L"\\" + name).c_str())) ++removed;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (removed > 0) Log::Info("thumbs: removed %d old preview files", removed);
}

// GetVersionEx lies (app-manifest-gated compatibility shims) on anything past Win8; the registry
// value is what Settings > About shows and isn't subject to that. Missing outright only on
// pre-Win10 systems Ombra doesn't support anyway, so a fallback of 0 there is fine — it will show
// up as an obviously-wrong build number in a bug report, not a crash.
std::wstring OsBuildString() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                       KEY_READ, &key) != ERROR_SUCCESS) {
        return L"unknown";
    }
    wchar_t build[32] = L"0";
    DWORD   build_size = sizeof(build);
    RegGetValueW(key, nullptr, L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &build_size);
    DWORD ubr = 0, ubr_size = sizeof(ubr);
    RegGetValueW(key, nullptr, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &ubr_size);
    RegCloseKey(key);
    return std::wstring(build) + L"." + std::to_wstring(ubr);
}

// The first line of every run, test modes included. Non-default settings follow once the config is
// loaded; adapters with their drivers, encoder, capture target and audio endpoints follow from their
// own Open/probe calls.
void LogStartupHeader() {
    Log::Info("ombra %s (%s) starting, pid %lu, OS build %s", AppVersion(), BuildCommit(),
              GetCurrentProcessId(), NarrowUtf8(OsBuildString()).c_str());
}

// Not a crash: another engine owns the single-instance lock (a UI spawn racing a starting engine
// lands here). No dump, no "previous run crashed"; TerminateProcess like the fatal path, since
// threads may be running.
[[noreturn]] void StopAsSecondInstance(const char* what) {
    std::printf("\n  %s\n", what);
    Log::Info("%s", what);
    Log::Shutdown();
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) Sleep(INFINITE);
}

// A real (not message-only) hidden top-level window on the pump thread. Needed
// now, because Shell_NotifyIconW's callback and a tray context menu's WM_COMMAND
// both route to an HWND, so the tray shares this window rather than creating a second one. Never shown (no WS_VISIBLE) — this process has
// no UI surface of its own until ombra-ui connects over the pipe.
constexpr wchar_t kEngineWindowClassName[] = L"OmbraEngineWindow";

// The tray is constructed after the window (it needs the HWND to attach
// Shell_NotifyIconW to), so EngineWndProc reaches it through a global rather than GWLP_USERDATA —
// there is exactly one engine window per process, same single-instance precedent as
// g_pump_thread_id_for_ctrl_handler below.
Tray* g_tray_for_wndproc = nullptr;
// WM_APP-based, not a raw application-defined constant, per the standard Shell_NotifyIconW
// callback-message guidance — avoids colliding with any other message this window might someday
// need to handle.
constexpr UINT kTrayCallbackMessage = WM_APP + 1;

// True while Windows reports every display off (GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed;
// dimmed counts as on). Written by EngineWndProc on the pump thread, read by the audio target
// watcher, which closes every capture while it holds (AudioStandbyGate), and handed to the
// BufferController, which stops recording. Display-off is what the engine can observe ahead of
// Modern Standby; it is a proxy, not "the system is asleep".
std::atomic<bool> g_display_off{false};
std::atomic<BufferController*> g_controller_for_display{nullptr};
HPOWERNOTIFY      g_display_notify = nullptr;
// {6FE69556-704A-47A0-8F24-C28D936FDA47}; spelled out so this file doesn't need INITGUID.
constexpr GUID kConsoleDisplayStateGuid = {0x6fe69556, 0x704a, 0x47a0,
                                           {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

LRESULT CALLBACK EngineWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Windows is shutting down, restarting or signing out and may end the process once this returns.
    // First, so no other handler can swallow it.
    if (msg == WM_ENDSESSION && wparam) {
        crash_upload::EndRun();
        return 0;
    }
    if (msg == WM_POWERBROADCAST && wparam == PBT_POWERSETTINGCHANGE && lparam) {
        const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lparam);
        if (s->PowerSetting == kConsoleDisplayStateGuid && s->DataLength == sizeof(DWORD)) {
            const bool off = *reinterpret_cast<const DWORD*>(s->Data) == 0;
            g_display_off.store(off, std::memory_order_relaxed);
            if (BufferController* controller = g_controller_for_display.load(std::memory_order_acquire))
                controller->SetDisplayOff(off);
        }
        return TRUE;
    }
    if (HotkeyRegistry* hk = g_hotkeys_for_engine.load();
        hk && hk->HandleWindowMessage(hwnd, msg, wparam, lparam)) {
        return 0;
    }
    if (g_tray_for_wndproc && g_tray_for_wndproc->HandleWindowMessage(hwnd, msg, wparam, lparam)) {
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (g_display_notify) UnregisterPowerSettingNotification(g_display_notify);
        g_display_notify = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Another Ombra process besides this engine: the overlay or a toast (ombra-ui), or an ombra.exe
// child such as a trim export. An update waits for them, since setup would close them.
bool OtherOmbraProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;  // can't tell: assume busy
    const DWORD    self = GetCurrentProcessId();
    PROCESSENTRY32W pe{sizeof(pe)};
    bool            found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self) continue;
        found = _wcsicmp(pe.szExeFile, L"" OMBRA_UI_EXE) == 0 || _wcsicmp(pe.szExeFile, L"" OMBRA_ENGINE_EXE) == 0;
    }
    CloseHandle(snap);
    return found;
}

HWND CreateEngineWindow(HINSTANCE hinst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = EngineWndProc;
    wc.hInstance     = hinst;
    wc.hIcon         = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kEngineWindowClassName;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log::Error("RegisterClassW failed for the resident engine window (err=%lu)", GetLastError());
        return nullptr;
    }
    HWND hwnd = CreateWindowW(kEngineWindowClassName, L"" OMBRA_PRODUCT_NAME, WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                              nullptr, hinst, nullptr);
    if (!hwnd) {
        Log::Error("CreateWindowW failed for the resident engine window (err=%lu)", GetLastError());
        return hwnd;
    }
    // Delivers the current state at once, then each change. Failing is not fatal: without it the
    // audio captures stay open through standby (the old behaviour).
    g_display_notify = RegisterPowerSettingNotification(hwnd, &kConsoleDisplayStateGuid,
                                                        DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!g_display_notify)
        Log::Warn("RegisterPowerSettingNotification(display state) failed (err=%lu); audio stays open while the display is off",
                  GetLastError());
    return hwnd;
}

// CTRL_CLOSE_EVENT deliberately excluded: Windows gives that handler only ~5 seconds before
// force-terminating the process, and is conventionally expected to block until cleanup is done —
// returning TRUE immediately (as this handler does, since teardown happens back on the pump
// thread on its own schedule) risks the process being killed mid-teardown (supervisor.Stop(),
// audio_capture.Stop(), the screenshot worker join). CTRL_C_EVENT/CTRL_BREAK_EVENT carry no such
// deadline, so they're what a dev session at a terminal actually needs; closing the console window
// outright is not a supported way to stop the resident engine (use the tray's future "Exit", or
// just close ombra normally).
BOOL WINAPI EngineConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            PostThreadMessage(g_pump_thread_id_for_ctrl_handler.load(), WM_QUIT, 0, 0);
            return TRUE;
        default:
            return FALSE;
    }
}

// Toast notifications: a short-lived `ombra-ui.exe --toast <event>` process, spawned
// fresh per event rather than a resident process, so a disabled setting costs literally nothing.
//
// Single-slot, newest-wins: a rapid run of hotkey presses would otherwise spawn overlapping toast
// processes (each standing up its own D3D11/DComp device), and a superseded toast could linger on
// screen showing stale state. Guarded by a mutex because this is called from two different threads
// — the buffer-toggle hotkey's pump thread and a detached save-completion thread.
std::mutex g_toast_mutex;
HANDLE     g_last_toast_process = nullptr;

// `inherit` handles (already inheritable) pass to the toast and nothing else does: the explicit
// handle list keeps every other inheritable handle in the engine out of the child.
void SpawnToastProcess(const std::wstring& args, const std::vector<HANDLE>& inherit) {
    // An isolated instance (OMBRA_HOME: tests, ombra-ui --drive) puts nothing on the user's screen.
    if (Instance().isolated) return;
    const std::wstring ui_path = ResolveOmbraUiExePath();
    if (ui_path.empty()) return;

    std::wstring cmdline = L"\"" + ui_path + L"\" --toast " + args;

    std::lock_guard<std::mutex> lock(g_toast_mutex);
    if (g_last_toast_process) {
        if (WaitForSingleObject(g_last_toast_process, 0) == WAIT_TIMEOUT) {
            // Still showing a previous toast — best-effort kill so it doesn't linger with stale
            // state once this newer one replaces it. Ignore failure: it may have just exited on
            // its own between the wait above and this call.
            TerminateProcess(g_last_toast_process, 0);
        }
        CloseHandle(g_last_toast_process);
        g_last_toast_process = nullptr;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    std::vector<uint8_t> attr_storage;
    DWORD flags = 0;
    if (!inherit.empty()) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attr_storage.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       const_cast<HANDLE*>(inherit.data()),
                                       inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            Log::Warn("SpawnToast: handle list setup failed (err=%lu)", GetLastError());
            return;
        }
        flags = EXTENDED_STARTUPINFO_PRESENT;
    }
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(ui_path.c_str(), cmdline.data(), nullptr, nullptr,
                                        inherit.empty() ? FALSE : TRUE, flags, nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!created) {
        Log::Warn("SpawnToast: CreateProcessW failed for %ls (err=%lu)", ui_path.c_str(), GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    g_last_toast_process = pi.hProcess;
}

void SpawnToast(const wchar_t* event) { SpawnToastProcess(event, {}); }

// A save toast spawned at the press, not after the save: it waits on these events (inherited by
// the toast process), so it appears within its own startup time and resolves in place when the
// save ends. The engine's own process handle goes along too, so a crash mid-save still resolves it.
struct SaveToastSignal {
    HANDLE ok = nullptr, nothing_new = nullptr, failed = nullptr, engine = nullptr, elsewhere = nullptr;
    HANDLE info = nullptr;  // a SaveToastInfo (save_toast.h), written before the result's event
    ~SaveToastSignal() {
        for (HANDLE h : {ok, nothing_new, failed, engine, elsewhere, info})
            if (h) CloseHandle(h);
    }
    void Resolve(const ReplaySaveResults& r) {
        SaveToastInfo summary;
        bool fell_back = false;
        for (const auto& m : r.monitors) {
            fell_back |= m.fell_back;
            if (m.outcome != SaveOutcome::kSaved) continue;
            ++summary.clips;
            summary.seconds = (std::max)(summary.seconds, static_cast<float>(m.seconds));
        }
        if (void* view = MapViewOfFile(info, FILE_MAP_WRITE, 0, 0, sizeof(SaveToastInfo))) {
            *static_cast<SaveToastInfo*>(view) = summary;
            UnmapViewOfFile(view);
        }
        // SetEvent orders the write above before the toast's wait returns.
        SetEvent(r.outcome == SaveOutcome::kSaved        ? (fell_back ? elsewhere : ok)
                 : r.outcome == SaveOutcome::kNothingNew ? nothing_new
                                                         : failed);
    }
};

std::shared_ptr<SaveToastSignal> SpawnSaveToast() {
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    auto sig = std::make_shared<SaveToastSignal>();
    sig->ok          = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    sig->nothing_new = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    sig->failed      = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    sig->engine      = OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId());
    sig->elsewhere   = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    sig->info        = CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0,
                                          sizeof(SaveToastInfo), nullptr);
    if (!sig->ok || !sig->nothing_new || !sig->failed || !sig->engine || !sig->elsewhere || !sig->info) {
        Log::Warn("SpawnSaveToast: handle setup failed (err=%lu)", GetLastError());
        return nullptr;
    }
    // The toast's argv: the five wait handles (in WaitForMultipleObjects order), then the info.
    std::wstring args = L"save --wait";
    for (HANDLE h : {sig->ok, sig->nothing_new, sig->failed, sig->engine, sig->elsewhere, sig->info})
        args += L" " + std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    SpawnToastProcess(args, {sig->ok, sig->nothing_new, sig->failed, sig->engine, sig->elsewhere, sig->info});
    return sig;
}

// Both entry points use the same ring claims, timestamp, grouping and completion result.
StartSaveFeedback ReplayFeedback(bool enabled) {
    if (!enabled) return {};
    return []() -> SaveFeedback {
        auto toast = SpawnSaveToast();
        return [toast](const ReplaySaveResults& r) { if (toast) toast->Resolve(r); };
    };
}

// Every running process's PID, parent PID and image name, for FindTargetRootPid (audio.h).
std::vector<ProcessEntry> SnapshotProcesses() {
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            out.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// `ombra.exe export SRC DST START END [--tracks 1,0,...]`: a frame-exact trim (clip_reencode.h),
// run by ombra-ui as a child so it outlives the overlay and a failure can't take the engine's
// buffer with it. Takes no instance lock, tray or hotkeys. Reports on stdout, one line each,
// flushed: "progress DONE TOTAL", then "done START END" or "error MESSAGE". Exit code 0 on success.
// `--overlay HANDLE` is the overlay's process: if it has exited by the end, nobody saw the result,
// so a toast says it (an isolated instance shows none, SpawnToastProcess).
int RunExport(const Config& cfg, int argc, wchar_t** argv) {
    if (argc < 6) {
        std::printf("error usage: export SRC DST START END [--tracks 1,0,...] [--replace] [--cancel HANDLE] [--overlay HANDLE]\n");
        return 2;
    }
    const std::wstring src = argv[2], dst = argv[3];
    const double start_s = _wtof(argv[4]), end_s = _wtof(argv[5]);
    std::vector<bool> tracks;
    bool replace = false;
    HANDLE cancel = nullptr;  // an inherited event: set means stop (closing the overlay doesn't)
    HANDLE overlay = nullptr;
    for (int i = 6; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--replace") replace = true;
        if (i + 1 >= argc) continue;
        if (arg == L"--tracks") {
            for (const wchar_t* p = argv[i + 1]; *p; ++p)
                if (*p == L'0' || *p == L'1') tracks.push_back(*p == L'1');
        } else if (arg == L"--cancel") {
            cancel = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[i + 1], nullptr, 10)));
        } else if (arg == L"--overlay") {
            overlay = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[i + 1], nullptr, 10)));
        }
    }
    const auto toast_if_unseen = [&](const wchar_t* event) {
        if (!overlay || WaitForSingleObject(overlay, 0) != WAIT_OBJECT_0) return;  // open: it shows the result
        if (!cfg.ui_toast_enabled || !cfg.ui_toast_on_save) return;
        Log::Info("export: overlay closed, toast %ls", event);
        SpawnToast(event);
    };
    // An overlay opened meanwhile leaves the source alone while this holds it (clip_ops.h).
    const ClipBusyLock busy(src);
    std::printf("phase encode\n");
    std::fflush(stdout);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        const AmfProbeResult probe = ProbeAmfOutOfProcess();
        SetAmfAdapterCaps(probe.adapters);
    }
    ReencodeOptions options;
    options.adapter = cfg.encode_adapter;
    options.encoder = EncoderOptionsFor(cfg);
    options.keyframe_sec = static_cast<uint32_t>(cfg.encode_keyframe_sec);
    options.av1_unaligned = cfg.encode_amf_av1_unaligned;
    Log::Info("export: %ls %.3f-%.3f s -> %ls", src.c_str(), start_s, end_s, dst.c_str());
    // A progress line every ~2% is plenty for a bar; the reader may be gone (overlay closed), so
    // write failures are ignored.
    uint32_t last_pct = 101;
    const TrimResult r = ReencodeClip(src, dst, start_s, end_s, tracks.empty() ? nullptr : &tracks, options,
                                      [&](uint32_t done, uint32_t total) {
                                          const uint32_t pct = done * 50 / (std::max)(total, 1u);
                                          if (pct != last_pct || done == total) {
                                              last_pct = pct;
                                              std::printf("progress %u %u\n", done, total);
                                              std::fflush(stdout);
                                          }
                                          if (done == total) {
                                              // The mux and the replace follow: too late to cancel.
                                              std::printf("phase finish\n");
                                              std::fflush(stdout);
                                          }
                                          return !cancel || WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0;
                                      });
    if (!r.ok) {
        Log::Warn("export failed: %s", r.error.c_str());
        std::printf("error %s\n", r.error.c_str());
        std::fflush(stdout);
        if (r.error != "cancelled") toast_if_unseen(L"trim_failed");
        return 1;
    }
    // Replace: the source file is closed by now (the reader is gone with ReencodeClip).
    const char* outcome = "saved";
    if (replace) outcome = ReplaceWithEdited(src, dst) ? "replaced" : "kept";
    Log::Info("export: %s", outcome);
    std::printf("done %.6f %.6f %s\n", r.start_s, r.end_s, outcome);
    std::fflush(stdout);
    toast_if_unseen(!replace ? L"trim_saved" : std::strcmp(outcome, "replaced") == 0 ? L"trim_replaced" : L"trim_kept");
    return 0;
}

}  // namespace

int RunEngine(int argc, wchar_t** argv) {
    // Must return before any GPU/capture/audio setup below — the verify line
    // ("--help renders every key") has to work on a machine with no NVIDIA GPU at all.
    if (argc > 1 && std::wstring(argv[1]) == L"--help") {
        RenderConfigHelp(stdout);
        return 0;
    }
    // The AMF probe child (amf_probe.h): before logging and the instance lock, since it runs
    // alongside a starting engine and must touch nothing of the engine's.
    if (argc > 1 && std::wstring(argv[1]) == L"amfprobe") return RunAmfProbeChild();
    // The update check (update.h): same reasons, it runs beside the engine and owns nothing of it.
    if (argc > 1 && std::wstring(argv[1]) == L"update-check") return RunUpdateCheckChild(argc, argv);
    // Crash reports (crash_upload.h): the same.
    if (argc > 1 && std::wstring(argv[1]) == L"crash-upload") return crash_upload::RunUploadChild();
    // Send logs (Settings > Diagnostics): the UI writes the zip and runs this, so it works without
    // the engine and keeps WinHTTP out of both resident processes.
    if (argc > 3 && std::wstring(argv[1]) == L"send-logs")
        return static_cast<int>(crash_upload::RunSendLogs(argv[2], NarrowUtf8(argv[3])));

    Log::SetThreadName("main");
    const std::wstring log_path = Log::Init();
    LogStartupHeader();
    crash::Install("ombra");
    if (!log_path.empty()) std::wprintf(L"  log            %s\n", log_path.c_str());

    // Loaded once here regardless of mode (ringtest etc. don't read it) so every invocation logs
    // the same config issues a real capture run would. defaults -> config.json -> positional CLI
    // args below is the precedence order applied here.
    const ConfigLoadResult cfg_result = LoadConfig();
    Config                 cfg        = cfg_result.config;
    // Mutable, process-lifetime copy — HandlePipeRequest's set_config writes
    // new values here (and to config.json via SaveRawConfigValues) so a later get_config in the
    // same run reflects what was actually set, not just what config.json held at startup.
    RawConfigMap raw_config = cfg_result.raw;
    // Only the engine prunes the shared logs directory (log.h).
    Log::EnablePruning(static_cast<size_t>(cfg.log_max_bytes));
    Log::SetLevel(ParseLogLevel(cfg.log_level));
    {
        const std::string changed = DescribeNonDefaultSettings(cfg, raw_config);
        Log::Info("settings: %s", changed.empty() ? "all defaults" : changed.c_str());
    }
    for (const ConfigIssue& issue : cfg_result.issues) {
        Log::Warn("config %s: %s", issue.key.empty() ? "(root)" : issue.key.c_str(),
                  issue.message.c_str());
        std::printf("  config warn    %s: %s\n", issue.key.empty() ? "(root)" : issue.key.c_str(),
                    issue.message.c_str());
    }

    if (argc > 1 && std::wstring(argv[1]) == L"gputest") {
        DWORD pid = 0;
        for (int i = 2; i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--pid") pid = static_cast<DWORD>(_wtoi(argv[i + 1]));
        {
            const AmfProbeResult probe = ProbeAmfOutOfProcess();
            SetAmfAdapterCaps(probe.adapters);
        }
        return RunGpuTest(stdout, pid);
    }
    if (argc > 1 && std::wstring(argv[1]) == L"export") return RunExport(cfg, argc, argv);
    if (argc > 1 && std::wstring(argv[1]) == L"capturebench") {
        // Before the bench window exists: the probe is a child process and takes ~150 ms.
        {
            const AmfProbeResult probe = ProbeAmfOutOfProcess();
            SetAmfAdapterCaps(probe.adapters);
        }
        return RunCaptureBench(cfg, ResolveCaptureMonitors(cfg_result.raw.at("capture.monitors")).front().device);
    }

    const bool timedtest = argc > 1 && std::wstring(argv[1]) == L"timedtest";
    // No argv at all is the real default (resident forever). Any other unrecognized argv[1] (not
    // one of the dispatches above, not timedtest) is a usage error rather than being silently
    // interpreted as a total_seconds number.
    const bool resident = argc == 1;
    if (!resident && !timedtest) {
        std::fwprintf(stderr, L"unrecognized argument: %s (run with no arguments for the resident "
                               L"engine, or \"timedtest\" for the scriptable test harness)\n", argv[1]);
        return 1;
    }
    const double       total_seconds  = timedtest ? ((argc > 2) ? _wtof(argv[2]) : 15.0) : 15.0;
    const double       buffer_seconds = (timedtest && argc > 3) ? _wtof(argv[3]) : cfg.buffer_seconds;
    const std::wstring out_path       = (timedtest && argc > 4) ? argv[4] : L"out\\ombra.mp4";
    const uint32_t     fps            = EngineFps(cfg, ResolveCaptureMonitors(cfg_result.raw.at("capture.monitors")).front().device);
    uint32_t           bitrate_kbps   = static_cast<uint32_t>(cfg.encode_bitrate_kbps);  // re-derived below

    Telemetry::SetEnabled(cfg.engine_telemetry && (resident || timedtest));
    if (const auto prev = crash::TakePreviousCrash("ombra")) {
        // Also the log line (Telemetry::Event logs every event); the crash itself was the ERROR.
        Telemetry::Event("previous_crash", "dump=%s reason=\"%s\"",
                         prev->dump_name.empty() ? "none" : NarrowUtf8(prev->dump_name).c_str(), prev->reason.c_str());
    }
    crash::SetSharingEnabled(cfg.diagnostics_sharing != "off", cfg.diagnostics_sharing == "usage");
    crash::PruneDumps(5);

    std::printf("" OMBRA_PRODUCT_NAME " engine — WGC -> pacer -> NVENC -> ring -> MP4%s\n\n",
                resident ? " (resident)" : "");

    // Capability enumeration is milliseconds and belongs in every real
    // run's startup log, ahead of Capture::Open()/Encoder::Open() so a probe NVENC session can
    // never collide with the real one (gputopo.cpp tears its own probe session down before
    // returning). Skipped for the synthetic test modes above (ringtest etc.) the same way their
    // adapter-specific logging already is — they never touch a GPU. A machine with no hardware
    // encoder still starts: it records on the CPU (x264_encoder.h).
    // AMF counts as an encoder only once a child process has shown its init doesn't hang or crash.
    const AmfProbeResult amf_probe = ProbeAmfOutOfProcess();
    SetAmfAdapterCaps(amf_probe.adapters);
    const StartupGpuSummary gpu_summary = LogGpuTopologyStartup(
        cfg.encode_adapter, amf_probe,
        MonitorForDevice(ResolveCaptureMonitors(cfg_result.raw.at("capture.monitors")).front().device));

    // While the quality preset isn't custom it decides the bitrate: re-derive it for this monitor,
    // display rate and recording GPU, so a value stored under another monitor, codec or an older
    // table (AV1's kAv1BitrateFactor, 2026-10-31) matches what Settings shows before the ring is
    // sized below. Only for a monitor target: with "The game" or a window the source is that
    // window's size, which Settings and a Save derive from (PreviewConfig gets the pipeline's
    // source size) and which the monitor's mode can put in another kQualityRows row; re-deriving
    // here would undo every Save at the next start.
    if (cfg.encode_quality_preset != "custom" && cfg.capture_target == "monitor") {
        const std::string device = ResolveCaptureMonitors(cfg_result.raw.at("capture.monitors")).front().device;
        int src_w = 1920, src_h = 1080;
        for (const MonitorInfo& m : EnumerateAllMonitors())
            if (m.device == device && m.width > 0 && m.height > 0) {
                src_w = static_cast<int>(m.width);
                src_h = static_cast<int>(m.height);
            }
        const int derived = PresetBitrateFor(cfg, src_w, src_h, DisplayRefreshHz(device), RecordingGpuCapsOf(gpu_summary));
        if (derived > 0 && derived != cfg.encode_bitrate_kbps) {
            Log::Info("quality preset %s: bitrate %lld -> %d kbps for %dx%d on this GPU", cfg.encode_quality_preset.c_str(),
                      static_cast<long long>(cfg.encode_bitrate_kbps), derived, src_w, src_h);
            cfg.encode_bitrate_kbps = derived;
            bitrate_kbps            = static_cast<uint32_t>(derived);
            if (resident) SaveConfig(cfg);
        }
    }

    // Acquire the single-instance mutex here, before any GPU/capture/audio
    // setup, not down where pipe_server->Start() used to be the only place that checked it. A
    // second `ombra.exe` launched while the first is still a few seconds into startup (e.g.
    // ombra-ui spawning one because its own connect attempt raced the first instance's own
    // PipeServer::Start(), which lands well after the pipeline is up) would otherwise build a full
    // second pipeline — second NVENC session, second WGC session, a 300MB+ ring — before dying on
    // this same check down at pipe_server->Start(). Failing fast here, before any of that, is the
    // whole fix. PipeServer::Start() adopts this handle (external_lock) instead of acquiring its
    // own — a second CreateMutexW from the same process wouldn't take ownership and would
    // misreport "already running" against ourselves.
    HANDLE instance_lock = nullptr;
    if (resident) {
        // A capture.monitors change (Settings' monitor picker) makes EngineSelfRestart spawn a
        // fresh ombra.exe and then quit the current one -- the new process can easily win this
        // race against a still-shutting-down predecessor still holding the mutex, so retry briefly
        // instead of a single attempt. Harmless for a normal single-launch start: the first try
        // succeeds immediately whenever nothing else is running.
        for (int attempt = 0; !instance_lock && attempt < 20; ++attempt) {
            instance_lock = AcquireEngineInstanceLock();
            if (!instance_lock) Sleep(250);
        }
        if (!instance_lock) {
            StopAsSecondInstance("another " OMBRA_PRODUCT_NAME " engine instance is already running (see the log "
                                 "for the exact reason)");
        }
        // Only the instance holding the lock: a second launch must not touch the live run's marker.
        crash_upload::BeginRun();
    }

    // The resident window exists for the whole run — created early since
    // nothing depends on pipeline state. Neither it nor the pipe server (started further down,
    // after the pipeline is actually up) exists for timedtest — it's an unmodified
    // scriptable test harness, not the always-resident engine this step introduces.
    HWND hwnd = nullptr;
    if (resident) {
        hwnd = CreateEngineWindow(GetModuleHandleW(nullptr));
        if (!hwnd) die("could not create the resident engine's window");
        PruneThumbs();
    }
    MemoryLayout memory_layout;  // declared before pipe_server so it outlives the handler reading it
    std::unique_ptr<PipeServer> pipe_server;
    LogRssStage("process start");

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    LogRssStage("after init_apartment");

    // Budget in bytes, not seconds, since bitrate is variable (ComputeRingSizing: seconds *
    // max_bitrate * 1.3, plus pin-time overshoot). Shared with PreviewConfig so the projected and
    // actual memory numbers agree.
    const RingSizing ring_sizing    = ComputeRingSizing(cfg, buffer_seconds);
    const size_t     ring_capacity  = static_cast<size_t>(ring_sizing.video_capacity);
    const size_t     overshoot_bytes = static_cast<size_t>(ring_sizing.video_overshoot);

    // One MonitorPipeline per capture.monitors entry (or the sole "primary"
    // entry ResolveCaptureMonitors() returns for an empty/absent key — today's only behavior,
    // unchanged). This loop only ever runs at startup — capture.monitors is now implemented=true
    // (Settings' per-monitor toggle list) but a change made through the panel doesn't
    // reach here live; it persists to config.json and triggers EngineSelfRestart (see
    // HandlePipeRequest), so this loop picks it up fresh on the next launch, same as a hand-edit
    // always could. Every pipeline's ring is sized identically (ring size doesn't vary
    // by monitor).
    std::vector<std::unique_ptr<MonitorPipeline>> pipelines;
    for (MonitorSpec& spec : ResolveCaptureMonitors(cfg_result.raw.at("capture.monitors"))) {
        auto mp = std::make_unique<MonitorPipeline>();
        // buffer.storage=file maps a real file per pipeline (ResolveRingFilePath
        // inserts spec.suffix before the extension, "" for the sole/first pipeline -- same file
        // path today's only, single-pipeline behavior already used before this key existed).
        if (cfg.buffer_storage == "file") {
            // A path that stopped working (a removed drive) falls back to RAM inside Init: dying
            // here would stop the engine at every boot until someone edited the config by hand.
            mp->ring.Init(ring_capacity, overshoot_bytes, ResolveRingFilePath(cfg, spec.suffix));
        } else {
            mp->ring.Init(ring_capacity, overshoot_bytes);
        }
        mp->ring.SetMaxAge(RingMaxAgeQpc(cfg.buffer_seconds));
        mp->spec = std::move(spec);
        pipelines.push_back(std::move(mp));
    }
    std::printf("  ring capacity  %.1f MB per monitor for a %.1fs buffer (+%.1f MB pin-overshoot "
                "headroom)\n",
                static_cast<double>(ring_capacity) / (1024.0 * 1024.0), buffer_seconds,
                static_cast<double>(overshoot_bytes) / (1024.0 * 1024.0));
    if (pipelines.size() > 1) {
        std::printf("  monitors       %zu pipelines:", pipelines.size());
        for (const auto& mp : pipelines) std::printf(" %s", mp->spec.device.c_str());
        std::printf("\n");
        // (Advisor review): capture.target=game/window resolves
        // one HWND independent of which monitor a pipeline was built for — with N>1
        // capture.monitors entries, every pipeline resolves the *same* window and writes the same
        // content to N differently-suffixed files. No collapse-to-one-pipeline logic exists for
        // this combination, so it's logged once here rather than silently producing duplicate
        // files — reachable from the panel now that both keys are implemented=true,
        // not just via a hand-edited config.json.
        if (cfg.capture_target != "monitor") {
            Log::Warn("capture.target=%s with %zu capture.monitors entries -- every pipeline "
                      "resolves the same window; expect duplicate output files",
                      cfg.capture_target.c_str(), pipelines.size());
        }
    }

    // Audio: one AudioTrackHub (audio_tracks.h) for every source and every pipeline — there is one
    // set of audio devices whatever the monitor count. Captures hand it PCM; it mixes and
    // AAC-encodes each track continuously; a save takes the finished frames.
    const auto all_sources = RecordableAudioSources(ResolveAudioSources(cfg_result.raw.at("audio.sources")),
                                                    cfg.audio_per_app_mode);
    AudioTrackHub audio_hub(QpcNow(), QpcFrequency(), static_cast<uint32_t>(cfg.audio_bitrate_kbps),
                            AudioTrackHub::SizeFor(buffer_seconds, static_cast<uint32_t>(cfg.audio_bitrate_kbps),
                                                   AudioRingMaxAgeQpc(buffer_seconds)));
    audio_hub.SetSources(TrackSourcesFrom(all_sources), cfg.audio_full_mix_track);
    audio_hub.Start();
    std::printf("  audio          %zu sources -> %zu tracks, %.1f MB encoded ring per track\n",
                all_sources.size(), audio_hub.TrackCount(),
                static_cast<double>(ring_sizing.audio_capacity - static_cast<int64_t>(AudioTrackHub::StageBytes())) /
                    (1024.0 * 1024.0));
    LogRssStage("after rings allocate");

    // Every source is started, stopped and retargeted by the audio target watcher (after
    // controller.Start() below), so nothing captures before its first poll and no audio.sources
    // edit needs a restart. `device` is a specific
    // endpoint's IMMDevice id (audio.h's AudioDeviceInfo::id, chosen in the Settings device picker:
    // a render endpoint for Desktop, a capture endpoint for Mic) or "default", which follows the
    // system default as it changes. An id that no longer exists falls back to the default
    // (OpenStream).
    auto start_audio_source = [](AudioCapture& capture, const AudioSourceSpec& spec,
                                  const AudioCapture::PcmCallback& cb) {
        const bool is_mic = spec.kind == "input";
        const std::wstring device_id =
            (spec.device.empty() || spec.device == "default") ? L"" : WideFromUtf8(spec.device);
        capture.Start(cb, AudioEndpoint{is_mic ? AudioEndpointKind::kMicrophone
                                               : AudioEndpointKind::kDesktopLoopback,
                                        device_id});
        const auto& fmt = capture.Negotiated();
        if (fmt.sample_rate_hz == 0) return;  // no endpoint yet: AudioCapture keeps retrying the open
        Log::Info("audio source \"%s\" (%s) negotiated %u Hz/%u ch/%u-bit %s -> 48000 Hz stereo s16%s",
                  spec.id.c_str(), spec.kind.c_str(), fmt.sample_rate_hz, fmt.channels,
                  fmt.bits_per_sample, fmt.is_float ? "float" : "int", fmt.was_resampled ? " (resampled)" : "");
    };

    // One running source. Its callback hands PCM to the hub under the slot SetSources gave it; a
    // source keeps its slot (and runtime) for as long as its id stays in audio.sources.
    struct AudioSourceRuntime {
        AudioSourceSpec           spec;      // watcher thread only
        std::string               id;        // fixed at creation; the callback reads it
        size_t                    slot = 0;  // fixed at creation
        AudioCapture              capture;
        AudioCapture::PcmCallback cb;
        TargetWatcherState        watch;        // watcher thread only
        DWORD                     exe_pid = 0;  // watcher thread only
        std::atomic<uint64_t>     packets{0};
        int64_t                   last_cb_qpc = 0;  // capture thread only
        struct {
            int64_t frames = 0, gap_ms = 0, age_ms = 0;
            int     packets = 0;
            bool    discontinuity = false;
        } late_burst;
    };
    auto make_audio_runtime = [&audio_hub](const AudioSourceSpec& spec, size_t slot) {
        auto owned = std::make_unique<AudioSourceRuntime>();
        AudioSourceRuntime& rt = *owned;
        rt.spec = spec;
        rt.id   = spec.id;
        rt.slot = slot;
        rt.cb = [&audio_hub, &rt](const PcmPacket& p) {
            const int64_t late = audio_hub.Append(rt.slot, p.samples.data(), p.samples.size(), p.qpc_ticks);
            rt.packets.fetch_add(1, std::memory_order_relaxed);
            // audio_late_ms steps by ~200 ms (the WASAPI buffer) a few times a day. One line per
            // burst of late packets says which source, how long the callbacks stopped before it
            // and how stale its timestamps were. Capture thread only, so plain fields are fine.
            const int64_t now = QpcNow();
            auto& b = rt.late_burst;
            if (late > 0) {
                if (b.frames == 0) {
                    b.gap_ms = rt.last_cb_qpc ? (now - rt.last_cb_qpc) * 1000 / QpcFrequency() : -1;
                    b.age_ms = (now - p.qpc_ticks) * 1000 / QpcFrequency();
                }
                b.frames += late;
                b.packets += 1;
                b.discontinuity |= p.discontinuity;
            } else if (b.frames > 0) {
                Log::Warn("audio source \"%s\": %lld ms arrived behind the encoder in %d packets "
                          "(callback gap before it %lld ms, first packet %lld ms old, discontinuity=%d)",
                          rt.id.c_str(), static_cast<long long>(b.frames * 1000 / 48000), b.packets,
                          static_cast<long long>(b.gap_ms), static_cast<long long>(b.age_ms),
                          b.discontinuity ? 1 : 0);
                b = {};
            }
            rt.last_cb_qpc = now;
        };
        return owned;
    };
    // The audio target watcher's alone while it runs (it creates, reopens and drops them as
    // audio.sources changes); shutdown reads them only after joining it. unique_ptr: each callback
    // holds its runtime's address.
    std::vector<std::unique_ptr<AudioSourceRuntime>> audio_sources;
    // What the watcher runs (audio_status) and the next list for it to apply (set_config). The
    // startup list goes through the same path: the watcher's first poll creates every runtime.
    AudioSourceControl audio_control;
    audio_control.Post(all_sources);
    // Both timelines are absolute QPC, so a different start-of-capture instant between them is
    // harmless for the pin math — but printed once here rather than left to be inferred later
    // (advisor review): a large unexpected gap between these two numbers would be the first thing
    // to check if a save's audio/video windows come out oddly misaligned.
    std::printf("  audio origin   qpc=%lld (video pacer's first tick timestamps itself independently)\n",
                static_cast<long long>(QpcNow()));
    // Media Foundation (MFStartup, aac_encoder.cpp) is not part of this stack — it's opened lazily
    // inside DoSave, not at startup, so it never shows up in this attribution at all until the
    // first save happens. Worth stating explicitly: an earlier design assumed an "after Media Foundation
    // initialises" startup stage that this architecture doesn't have.
    LogRssStage("after audio hub starts");

    std::printf("\n  capturing up to %.1fs at %u fps into a %.1fs ring — WGC only delivers on\n"
                "  change; the pacer duplicates the last frame when nothing new has arrived...\n\n",
                total_seconds, fps, buffer_seconds);

    // Counters are atomics, not plain locals: the pacer thread writes them from inside the
    // callback, and the main thread polls them below to print progress. Printing (or any syscall)
    // from inside the callback was the actual source of the pacing jitter measured in an early
    // idle test — a console write can take hundreds of ms when the host is busy, which the
    // synchronous pacer loop has no slack to absorb. Progress reporting now happens entirely on
    // the main thread, off the pacer's critical path.
    std::atomic<size_t>   total_bytes{0};
    std::atomic<uint32_t> frames{0};
    std::atomic<uint32_t> keyframes{0};
    // At most one pacer thread is ever alive at a time — PipelineSupervisor's TeardownGeneration
    // always joins the current generation's Pacer before a rebuild starts the next one — so this
    // stays effectively single-writer across the whole run, rebuilds included,
    // and reading it after the final Stop() below is still safe without a mutex.
    std::vector<std::chrono::steady_clock::time_point> tick_times;
    tick_times.reserve(static_cast<size_t>(total_seconds * fps) + 64);

    // PipelineSupervisor owns capture+encoder+pacer from
    // here on and is the only thing allowed to construct or destroy them — a recoverable fault
    // (device removed, WGC item closed, NVENC session lost) tears the generation down, drops a
    // barrier on both rings, backs off, and rebuilds, instead of die()ing this thread the way
    // main.cpp used to.
    EncoderOptions enc_opts = EncoderOptionsFor(cfg);
    {
        const std::string device = ResolveCaptureMonitors(cfg_result.raw.at("capture.monitors")).front().device;
        int src_w = 1920, src_h = 1080;
        for (const MonitorInfo& m : EnumerateAllMonitors())
            if (m.device == device && m.width > 0 && m.height > 0) {
                src_w = static_cast<int>(m.width);
                src_h = static_cast<int>(m.height);
            }
        enc_opts.preset_bitrate_codec = PresetBitrateCodec(cfg, src_w, src_h, RecordingGpuCapsOf(gpu_summary));
    }
    // The Games list: launcher scans + games.list, shared by the controller (Game mode) and every
    // pipeline ("The game" target). Scanned once here, then again by Game mode as it runs.
    GameLibrary games(DefaultLauncherRoots(),
                      Instance().data_dir.empty() ? std::wstring{} : Instance().data_dir + L"\\games_seen.json");
    games.SetDecisions(ParseGamesList(cfg_result.raw.at("games.list")));
    SetTrayUserThemes(ParseUserThemes(cfg_result.raw.at("ui.themes")));
    games.Rescan();
    g_games_for_engine.store(&games, std::memory_order_release);

    for (size_t i = 0; i < pipelines.size(); ++i) {
        MonitorPipeline& mp = *pipelines[i];

        // monitor_device comes from this pipeline's own MonitorSpec (fixed at
        // construction, per RequestReconfigure's comment); mode/window_match come from
        // the global capture.target/capture.target_window keys, same for every pipeline.
        CaptureTarget capture_target;
        capture_target.monitor_device = mp.spec.device;
        capture_target.window.match = WideFromUtf8(cfg.capture_target_window);
        capture_target.mode         = CaptureTarget::ParseMode(cfg.capture_target);

        mp.supervisor = std::make_unique<PipelineSupervisor>(
            fps, bitrate_kbps, static_cast<uint32_t>(cfg.encode_keyframe_sec), &mp.ring, enc_opts,
            cfg.capture_capture_cursor, capture_target);
        mp.supervisor->SetGameLibrary(&games);
        mp.supervisor->SetCaptureResolution(cfg.capture_resolution);

        mp.save_ctx.ring             = &mp.ring;
        mp.save_ctx.audio            = &audio_hub;
        mp.save_ctx.buffer_seconds   = buffer_seconds;
        // Every pipeline gets the same literal out_path — harmless for resident mode, where this
        // is always the sentinel "out\ombra.mp4" that makes DoSave take the templated
        // root/output_filename branch (monitor_suffix differentiates the real filename there). It
        // is NOT harmless for a caller that passes an explicit non-sentinel path (e.g. `timedtest
        // total buffer out.mp4` with N>1 pipelines, not possible today since no test harness feeds
        // capture.monitors) — DoSave's `path == sentinel` gate would be false for every pipeline,
        // the suffix would never apply, and all N would race the same output file. Not fixed here:
        // no caller can hit it yet, and guessing at the right non-sentinel per-pipeline naming
        // scheme without a real use case would be speculative.
        mp.save_ctx.out_path         = out_path;
        mp.save_ctx.fps              = fps;
        mp.save_ctx.supervisor       = mp.supervisor.get();
        mp.save_ctx.per_game_folders   = cfg.output_per_game_folders;
        mp.save_ctx.output_root        = cfg.output_root;
        mp.save_ctx.fallback_root      = FallbackClipsDir();
        mp.save_ctx.output_filename    = cfg.output_filename;
        mp.save_ctx.save_overlap_policy = cfg.save_overlap_policy;
        mp.save_ctx.monitor_suffix      = WideFromUtf8(mp.spec.suffix);
        mp.save_ctx.monitor_label       = WideFromUtf8(mp.spec.label);
        mp.save_ctx.monitor_device      = mp.spec.device;
    }

    // Everything below this point was written, and stays written, in terms of one pipeline's
    // ring/supervisor/save_ctx — these three aliases are pipeline[0]'s (screenshots, the tray
    // tooltip, and the pipe's get_config/set_config stay scoped to pipeline[0].
    // See HandlePipeRequest's all_pipelines parameter.
    // Only the handful of call sites that must fan out to every monitor (BufferController's
    // supervisor list, the save hotkey, toggle_buffer, and shutdown) reach into `pipelines`
    // directly instead of through these aliases.
    Ring&               ring       = pipelines[0]->ring;
    PipelineSupervisor& supervisor = *pipelines[0]->supervisor;
    SaveContext&        save_ctx   = pipelines[0]->save_ctx;

    if (resident) {
        SetWindowsAutostart(cfg.engine_autostart);
    }

    // The screenshot worker starts before the pipeline so it's ready the
    // instant a PreEncodeCallback can fire, and the callback itself is installed before
    // supervisor.Start() — SetPreEncodeCallback's contract (supervisor.h) is that it must be set
    // before the first generation exists, and it then survives every later rebuild/reopen since
    // StartSegment() (the one place a Pacer is constructed) reads it fresh each time.
    ScreenshotContext screenshot_ctx;
    screenshot_ctx.fallback_root = FallbackClipsDir();
    screenshot_ctx.notify = [&cfg](const wchar_t* event) {
        if (ReadLiveConfig(cfg, [](const Config& c) { return c.ui_toast_enabled; })) SpawnToast(event);
    };
    screenshot_ctx.worker = std::thread([&screenshot_ctx] {
        Log::SetThreadName("screenshot");
        ScreenshotWorkerLoop(&screenshot_ctx);
    });
    AutotuneContext autotune_ctx;
    supervisor.SetPreEncodeCallback([&](ICaptureSource& capture, uint32_t slot, const Tick&) {
        if (!screenshot_ctx.requested.exchange(false, std::memory_order_acq_rel)) return;
        std::wstring path, fallback;
        {
            std::lock_guard<std::mutex> lock(screenshot_ctx.request_mutex);
            path = screenshot_ctx.pending_path;
            fallback = screenshot_ctx.pending_fallback_path;
        }
        std::vector<uint8_t> bgra;
        uint32_t              pitch = 0;
        capture.ReadSlotRegionBGRA(slot, 0, 0, capture.Width(), capture.Height(), bgra, pitch);
        capture.ReleaseReadbackStaging();
        // An empty readback goes on to the worker, which reports it: this is the pacer's thread.
        {
            std::lock_guard<std::mutex> lock(screenshot_ctx.job_mutex);
            screenshot_ctx.job_path      = std::move(path);
            screenshot_ctx.job_fallback_path = std::move(fallback);
            screenshot_ctx.job_width     = capture.Width();
            screenshot_ctx.job_height    = capture.Height();
            screenshot_ctx.job_row_pitch = pitch;
            screenshot_ctx.job_bgra      = std::move(bgra);
            screenshot_ctx.job_ready     = true;
        }
        screenshot_ctx.job_cv.notify_one();
    });

    // "game" boots the pipeline disarmed (arms once a fullscreen
    // game is in the foreground) — Start()'s fatal-open validation still runs either way
    // (supervisor.h's own comment), so a machine that can't record still refuses to start loudly
    // under every mode. "desktop" is always armed. The "off" mode was removed as redundant
    // with the Instant Replay toggle / set_armed pipe command.
    const bool initial_armed = BufferController::InitialArmed(cfg.capture_mode, cfg.capture_target);

    // The audio hub is shared, so it sleeps only once every pipeline has released its ring (disarmed
    // a whole buffer length: nothing it holds is saveable) and wakes on the first arm. One mutex
    // orders the two: SetArmed sets the armed flag before on_arm takes it, so a suspend that runs
    // after an arm sees that pipeline armed, and one that ran before it is undone by the resume.
    struct AudioSleepGate {
        std::mutex                       mutex;
        AudioTrackHub*                   hub = nullptr;
        std::vector<PipelineSupervisor*> supervisors;
    };
    auto audio_gate = std::make_shared<AudioSleepGate>();
    audio_gate->hub = &audio_hub;
    for (auto& mp : pipelines) audio_gate->supervisors.push_back(mp->supervisor.get());
    for (auto& mp : pipelines) {
        if (resident) {
            mp->supervisor->SetEncoderFallbackCallback([](const std::string&) { SpawnToast(L"cpu_encoder"); });
            mp->supervisor->SetRingStorageLostCallback([] { SpawnToast(L"buffer_storage_lost"); });
        }
        mp->supervisor->SetRingReleaseCallbacks(
            [audio_gate] {
                std::lock_guard<std::mutex> lock(audio_gate->mutex);
                for (PipelineSupervisor* s : audio_gate->supervisors)
                    if (s->Armed() || !s->RingReleased()) return;
                if (!audio_gate->hub->Suspended()) Log::Info("audio  every pipeline released -- audio encoding suspended");
                audio_gate->hub->SetSuspended(true);
            },
            [audio_gate] {
                std::lock_guard<std::mutex> lock(audio_gate->mutex);
                audio_gate->hub->SetSuspended(false);
            });
    }

    supervisor.Start(
        [&](const IVideoEncoder::AccessUnit& au, const Tick& tick) {
            // timedtest only: the resident engine never stops, so recording every tick there grew
            // without bound and reallocated on the tick thread.
            if (timedtest) tick_times.push_back(std::chrono::steady_clock::now());
            ring.Append(au.data, au.size, tick.qpc, tick.index, au.is_keyframe);  // before UnlockLast
            total_bytes.fetch_add(au.size, std::memory_order_relaxed);
            if (au.is_keyframe) keyframes.fetch_add(1, std::memory_order_relaxed);
            frames.fetch_add(1, std::memory_order_relaxed);
        },
        initial_armed);
    // Every other monitor's pipeline runs the same fatal-at-startup validation (PipelineSupervisor::
    // Start's own contract) but isn't part of the tick_times/total_bytes/frames/keyframes
    // diagnostics above — those are pipeline[0]-only counters printed at shutdown, not a per-run
    // correctness signal (advisor review).
    for (size_t i = 1; i < pipelines.size(); ++i) {
        MonitorPipeline& mp = *pipelines[i];
        mp.supervisor->Start(
            [&mp](const IVideoEncoder::AccessUnit& au, const Tick& tick) {
                mp.ring.Append(au.data, au.size, tick.qpc, tick.index, au.is_keyframe);
            },
            initial_armed);
    }

    // Game-mode arming is process-scoped, not monitor-scoped — one
    // BufferController fans SetArmed() out to every pipeline's supervisor together.
    std::vector<PipelineSupervisor*> all_supervisors;
    all_supervisors.reserve(pipelines.size());
    for (auto& mp : pipelines) all_supervisors.push_back(mp->supervisor.get());

    // Usage Logs (crash_upload.h): a game session from the game being named to its end. The
    // controller's worker calls this; the first pipeline says how it was recorded. Declared before
    // the controller, whose destructor can still end a session.
    struct OpenGame {
        std::string exe, group;
        int64_t     start = 0;
        int         saves = 0;
    } open_game;
    const auto total_saves = [&pipelines] {
        int n = 0;
        for (const auto& mp : pipelines) n += mp->save_ctx.save_count.load();
        return n;
    };
    BufferController controller(cfg.capture_mode, cfg.capture_target, cfg.capture_target_window,
                                all_supervisors, &games);
    if (resident)
        controller.SetGameSessionSink([&, total_saves](const std::wstring& exe_path) {
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            if (!open_game.exe.empty()) {
                crash_upload::GameSession s;
                s.start = open_game.start;
                s.end   = now;
                s.saves = total_saves() - open_game.saves;
                s.exe   = open_game.exe;
                s.group = open_game.group;
                const PipelineSupervisor::PipelineParams p = supervisor.CurrentParams();
                if (p.fps) {  // built at least once
                    s.encoder = BackendName(supervisor.CurrentEncodeAdapter().backend);
                    s.codec   = VideoCodecName(p.codec);
                    s.width   = p.picture_width;
                    s.height  = p.picture_height;
                    s.fps     = p.fps;
                }
                crash_upload::RecordGameSession(s);
            }
            const size_t slash = exe_path.find_last_of(L"\\/");
            std::wstring entry_key;
            if (!exe_path.empty()) games.Classify(exe_path, nullptr, &entry_key);
            open_game = {exe_path.empty() ? std::string() : NarrowUtf8(exe_path.substr(slash == std::wstring::npos ? 0 : slash + 1)),
                         crash_upload::GameSessionGroup(entry_key), now, total_saves()};
        });
    controller.Start();
    // On the pump thread, the one the window procedure runs on, so no change is missed between these.
    controller.SetDisplayOff(g_display_off.load(std::memory_order_relaxed));
    g_controller_for_display.store(&controller, std::memory_order_release);
    for (auto& mp : pipelines) mp->save_ctx.controller = &controller;

    // Audio target watcher: the only thread that starts, stops or retargets any source's
    // AudioCapture, so a WASAPI client is never torn down from another thread (the cross-thread
    // audio teardown shape of the concurrent-audio-pin crash). It also applies a
    // new audio.sources list (set_config posts it to audio_control): DiffAudioSources says which
    // captures close, reopen or open; the rest keep running and the hub keeps their tracks. It polls
    // rather than subscribing to BufferController or process events. Each poll a source's wanted
    // capture is WantedSourceCapture (audio.h): nothing while switched off, its own endpoint for
    // plain Desktop and the mic, and for a target source whatever its target allows. The detected
    // game is checked every second; a process snapshot for exe targets is taken every 3 s (OBS's
    // application-audio capture retries on the same RECONNECT_INTERVAL). A target that isn't
    // running records nothing (only/game_only) or the whole desktop (all_except), never a
    // duplicate desktop mix. The holes this leaves stay in sync because the hub places audio by QPC
    // (AudioTrackHub::Append).
    std::atomic<bool> audio_target_watcher_running{true};
    std::thread audio_target_watcher([&audio_sources, &audio_hub, &audio_control, &controller,
                                      &audio_target_watcher_running, start_audio_source,
                                      make_audio_runtime]() {
            Log::SetThreadName("audio-watch");
            using C = AudioTargetCapture;
            // The decision/next-state logic itself (WantedSourceCapture, DecideTargetWatcherAction,
            // NextTargetWatcherState, DiffAudioSources) is pure and unit-tested headless
            // (tests/audio_tests.cpp, config_tests.cpp) -- this loop only owns the actual WASAPI
            // Stop()/Start()/StartProcessLoopback() I/O and logging around it.
            auto exe_targeted = [](const AudioSourceSpec& spec) {
                return AudioSourceHasTarget(spec.kind, spec.mode) && spec.mode != "game_only";
            };
            auto publish = [&] {
                std::vector<AudioSourceBoard::Entry> entries;
                std::vector<TargetWatcherState>      watch;
                for (const auto& rt : audio_sources) {
                    entries.push_back({rt->spec.id, rt->spec.mode, AudioSourceHasTarget(rt->spec.kind, rt->spec.mode),
                                       rt->spec.enabled, rt->slot});
                    watch.push_back(rt->watch);
                }
                audio_control.board.Init(std::move(entries), std::move(watch));
            };
            // A new list: close what went away or changed target, tell the hub, open what's new.
            // Returns whether a process snapshot is needed now (an exe target with no pid yet).
            auto apply_sources = [&](std::vector<AudioSourceSpec> next) {
                std::vector<AudioSourceSpec> before;
                for (const auto& rt : audio_sources) before.push_back(rt->spec);
                const AudioSourcesDiff diff = DiffAudioSources(before, next);
                for (size_t i : diff.removed) audio_sources[i]->capture.Stop();
                for (size_t k = 0; k < next.size(); ++k) {
                    if (!diff.reopen[k]) continue;
                    AudioSourceRuntime& rt = *audio_sources[static_cast<size_t>(diff.old_index[k])];
                    rt.capture.Stop();
                    rt.watch   = {};  // as at startup: the next poll opens it fresh
                    rt.exe_pid = 0;
                }
                const std::vector<size_t> slots = audio_hub.SetSources(TrackSourcesFrom(next));
                std::vector<std::unique_ptr<AudioSourceRuntime>> runtimes;
                size_t added = 0, reopened = 0;
                for (size_t k = 0; k < next.size(); ++k) {
                    if (diff.old_index[k] < 0) {
                        runtimes.push_back(make_audio_runtime(next[k], slots[k]));
                        ++added;
                    } else {
                        runtimes.push_back(std::move(audio_sources[static_cast<size_t>(diff.old_index[k])]));
                        runtimes.back()->spec = next[k];
                        reopened += diff.reopen[k] ? 1 : 0;
                    }
                }
                const bool first = audio_sources.empty();
                audio_sources = std::move(runtimes);  // removed runtimes die here, their captures stopped
                publish();
                if (!first) {
                    Log::Info("audio  sources applied live: %zu running, %zu added, %zu removed, %zu reopened, %zu tracks",
                              audio_sources.size(), added, diff.removed.size(), reopened, audio_hub.TrackCount());
                }
                return std::any_of(audio_sources.begin(), audio_sources.end(),
                                   [&](const auto& rt) { return exe_targeted(rt->spec) && rt->exe_pid == 0; });
            };

            constexpr int  kSnapshotEveryPolls = 3;
            constexpr auto kPollInterval       = std::chrono::milliseconds(1000);
            // Every capture is closed while the display is off and every output silent
            // (AudioStandbyGate): an open WASAPI stream blocks Modern Standby's low-power idle. The
            // peak-meter probe needs COM on this thread and opens no stream, so it can tell when sound
            // returns while nothing is captured. Audio is placed by QPC, so the hole this leaves is the
            // same kind a missing target leaves.
            struct ComScope {
                ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
                ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
                HRESULT hr;
            } com;
            AudioStandbyGate    standby_gate;
            OutputActivityProbe output_probe;
            bool                standby = false;
            for (int poll = 0; audio_target_watcher_running.load(std::memory_order_acquire); ++poll) {
                bool snapshot_now = poll % kSnapshotEveryPolls == 0;
                if (auto next = audio_control.Take()) snapshot_now = apply_sources(std::move(*next)) || snapshot_now;
                const bool display_off = g_display_off.load(std::memory_order_relaxed);
                // Only read the meters when the answer matters (display off); an on display resets the gate.
                standby = standby_gate.Update(display_off, display_off && (FAILED(com.hr) || output_probe.Audible()));
                // Every ring released (Game mode waiting, or a long time disarmed): nothing keeps this audio, so
                // no stream stays open, the mic's in-use indicator included. Arming clears it and the wait
                // below notices, so capture is back about when the first video frame is.
                const bool unwanted = audio_hub.Suspended();
                audio_control.board.SetPaused(unwanted);
                const bool any_exe_target = std::any_of(audio_sources.begin(), audio_sources.end(),
                                                        [&](const auto& rt) { return exe_targeted(rt->spec); });
                if (any_exe_target && snapshot_now) {
                    const std::vector<ProcessEntry> processes = SnapshotProcesses();
                    for (auto& rt : audio_sources) {
                        if (exe_targeted(rt->spec)) rt->exe_pid = FindTargetRootPid(processes, WideFromUtf8(rt->spec.target));
                    }
                }

                for (size_t i = 0; i < audio_sources.size(); ++i) {
                    AudioSourceRuntime&    rt   = *audio_sources[i];
                    const AudioSourceSpec& spec = rt.spec;
                    const bool  enabled = spec.enabled;
                    const DWORD pid = !AudioSourceHasTarget(spec.kind, spec.mode) ? 0
                                      : spec.mode == "game_only"                   ? controller.CurrentGamePid()
                                                                                   : rt.exe_pid;
                    const C     want = WantedSourceCapture(enabled && !standby && !unwanted, spec.kind, spec.mode, pid);
                    const DWORD want_pid = (want == C::kTargetOnly || want == C::kAllExceptTarget) ? pid : 0;
                    TargetWatcherState& cur = rt.watch;
                    if (cur.started && rt.capture.StreamLost()) {
                        Log::Info("audio source \"%s\": process-loopback stream lost, starting it again", spec.id.c_str());
                        cur.started = false;  // Decide acts; a failed restart falls into its usual retry
                    }
                    const TargetWatcherDecision decision =
                        DecideTargetWatcherAction(cur, want, want_pid, poll, kSnapshotEveryPolls);
                    if (!decision.act) continue;

                    rt.capture.Stop();
                    bool activation_failed = false;
                    if (want == C::kTargetOnly || want == C::kAllExceptTarget) {
                        const bool exclude = want == C::kAllExceptTarget;
                        activation_failed = !rt.capture.StartProcessLoopback(rt.cb, ProcessLoopbackTarget{pid, exclude});
                        if (activation_failed && !decision.retrying) {
                            Log::Warn("audio source \"%s\": process-loopback activation failed for pid "
                                      "%lu, retrying every 3 s", spec.id.c_str(),
                                      static_cast<unsigned long>(pid));
                        }
                    }
                    const TargetWatcherState next = NextTargetWatcherState(want, want_pid, activation_failed);
                    // kAllDesktop is the source's own endpoint: its configured render device's
                    // loopback, or the microphone (start_audio_source).
                    if (next.capture == C::kAllDesktop) start_audio_source(rt.capture, spec, rt.cb);
                    audio_control.board.SetWatch(i, next);
                    if (decision.retrying && next.failed_pid != 0) {
                        // Still failing: the fallback is already what's running, so don't log it again.
                        cur = next;
                        continue;
                    }
                    const char* capturing = (spec.kind == "input" && next.capture == C::kAllDesktop)
                                                ? "microphone" : AudioTargetCaptureName(next.capture);
                    Telemetry::Event("audio_source", "id=%s kind=%s mode=%s target=%s pid=%lu enabled=%d display_off=%d rings_released=%d capturing=%s",
                                     spec.id.c_str(), spec.kind.c_str(), spec.mode.c_str(),
                                     spec.target.c_str(), static_cast<unsigned long>(pid), enabled ? 1 : 0,
                                     standby ? 1 : 0, unwanted ? 1 : 0, capturing);
                    cur = next;
                }

                // Short slices so shutdown and a new source list are picked up promptly (same
                // pattern as GpuLoadWatcher).
                auto slept = std::chrono::milliseconds(0);
                while (slept < kPollInterval && audio_target_watcher_running.load(std::memory_order_acquire) &&
                       !audio_control.HasPending()) {
                    auto slice = (std::min)(std::chrono::milliseconds(200), kPollInterval - slept);
                    std::this_thread::sleep_for(slice);
                    slept += slice;
                    // Held: sound or the display coming back reopens the captures now, not up to a second
                    // later, so the start of what you put your earphones on to hear is still recorded.
                    if (standby && (!g_display_off.load(std::memory_order_relaxed) ||
                                    (FAILED(com.hr) || output_probe.Audible())))
                        break;
                    if (audio_hub.Suspended() != unwanted) break;
                }
            }
        });

    // Declared after everything the probe reads, so it is destroyed first on any exit path.
    std::unique_ptr<TelemetrySampler> telemetry_sampler;
    if (resident || timedtest) {
        telemetry_sampler = std::make_unique<TelemetrySampler>([&]() {
            EngineSample s;
            s.armed     = supervisor.Armed();
            // Only while armed for a game: the controller keeps the name after a disarm for saves,
            // but a sample naming a game for hours after it closed would mislead every reader.
            s.game_pid  = controller.CurrentGamePid();
            s.game      = s.game_pid ? NarrowUtf8(controller.CurrentGameFolder()) : std::string{};
            s.pipelines = static_cast<uint32_t>(pipelines.size());
            for (const auto& mp : pipelines) {
                s.video_ring_bytes    += mp->ring.LiveBytes();
                s.video_ring_committed += mp->ring.CommittedBytes();
                s.video_ring_capacity += mp->ring.Capacity();
                s.ticks.Add(mp->supervisor->LifetimeCounters());
                s.rebuilds           += mp->supervisor->RebuildCount();
                s.resolution_changes += mp->supervisor->ResolutionChangeCount();
                s.saves              += static_cast<uint64_t>(mp->save_ctx.save_count.load());
            }
            s.audio_sources    = static_cast<uint32_t>(audio_hub.SourceCount());
            s.audio_ring_bytes = audio_hub.LiveBytes();
            const AudioTrackHub::Losses lost = audio_hub.GetLosses();
            s.audio_dropped_frames = lost.overrun_frames + lost.failed_frames;
            s.audio_late_frames    = lost.late_frames;
            s.audio_encoder_errors = lost.encoder_errors;
            return s;
        });
    }
    Telemetry::Event("engine_start",
                     "mode=%s monitors=%zu audio_sources=%zu fps=%u bitrate_kbps=%u buffer_s=%.0f "
                     "storage=%s codec=%s resident=%d",
                     cfg.capture_mode.c_str(), pipelines.size(), all_sources.size(), fps,
                     bitrate_kbps, buffer_seconds, cfg.buffer_storage.c_str(),
                     cfg.encode_video_codec.c_str(), resident ? 1 : 0);

    if (initial_armed) {
        const auto params = supervisor.CurrentParams();
        std::printf("  seq params     %zu bytes, %ux%u\n", params.codec_header.size(), params.width,
                    params.height);
    } else {
        std::printf("  capture        when=%s what=%s — booted disarmed, waiting for the target\n",
                    cfg.capture_mode.c_str(), cfg.capture_target.c_str());
    }

    // Tray icon + "Open Settings"/"Exit" menu, resident mode only (timedtest
    // has no window to host Shell_NotifyIconW on). Built after the controller so the
    // startup tooltip can report the real initial armed state rather than a guess.
    std::unique_ptr<Tray> tray;
    if (resident) {
        tray = std::make_unique<Tray>(hwnd, kTrayCallbackMessage, [] { PostQuitMessage(0); },
                                      cfg.ui_accent, cfg.ui_theme);
        g_tray_for_wndproc = tray.get();
        tray->SetTooltip(TrayTooltip(controller.UserEnabled()));
    }

    // Started only now, after supervisor/controller are up: an earlier get_status request would
    // have been answered against a PipelineSupervisor that doesn't exist yet. A client that races
    // ombra.exe startup (ombra-ui spawning it) already has to retry ERROR_FILE_NOT_FOUND —
    // pipetest proved that path — so starting the pipe later doesn't add a new failure mode, only
    // narrows the "listening but pipeline not ready" window to nothing.
    if (resident) {
        // get_schema/get_config/set_config/get_status/save_now — the actual
        // logic lives in HandlePipeRequest (engine_pipe.h), which tests/engine_pipe_tests.cpp drives
        // directly. cfg/supervisor/controller/save_ctx all outlive the pipe server (it's stopped
        // before any of them starts tearing down, see the shutdown sequence below).
        memory_layout.sizing          = ring_sizing;
        memory_layout.video_pipelines = pipelines.size();
        memory_layout.audio_sources   = all_sources.size();
        memory_layout.file_backed     = (cfg.buffer_storage == "file");
        pipe_server = std::make_unique<PipeServer>([&](const JsonValue& req) {
            return HandlePipeRequest(req, cfg, supervisor, controller, save_ctx, gpu_summary,
                                      tray.get(), &screenshot_ctx, &autotune_ctx, &pipelines,
                                      &raw_config, &memory_layout, &audio_control,
                                      ReplayFeedback(true));
        });
        // A failed Start() here means another ombra.exe instance already owns the pipe's single-
        // instance mutex (pipe.cpp) — i.e. this IS the "Ombra is already running" case, not merely
        // "no pipe available". The message says so explicitly rather than the more generic wording
        // a first read might expect.
        if (!pipe_server->Start(instance_lock)) {
            StopAsSecondInstance("could not start the pipe server — another " OMBRA_PRODUCT_NAME
                                 " engine instance is very likely already running (see the log for the exact reason)");
        }
        // GPU drivers and anything injected have loaded by now: make sure the crash filter is still ours.
        crash::RecheckFilter();
        std::printf("  pipe           listening on %ls\n", Instance().pipe_name.c_str());
        Log::Info("pipe listening on %ls%s", Instance().pipe_name.c_str(),
                  Instance().isolated ? " (isolated instance, OMBRA_HOME)" : "");
    }

    // Every hotkey goes through one HotkeyRegistry so Settings can rebind them, change the backend
    // and suspend them live (hotkeys.h). An unknown backend or an unparseable spec falls back to
    // the default with a warning: a config typo shouldn't take down an always-resident recorder.
    // Alt+F10 is also ShadowPlay's save hotkey and Alt+Z the NVIDIA overlay's, so under auto
    // RegisterHotKey losing those combos and the binding falling back to the LL hook is the common
    // case, not a failure. A binding that registers under no backend is logged and reported in
    // get_status ("hotkeys"), never fatal: the recorder still works without it.
    bool backend_known = true;
    const HotkeyPolicy backend_policy = ParseHotkeyPolicy(cfg.hotkeys_backend, &backend_known);
    if (!backend_known) {
        Log::Warn("config hotkeys.backend: unknown value \"%s\" — falling back to poll",
                  cfg.hotkeys_backend.c_str());
    }
    // One press saves every monitor's buffer (N entries, N files).
    std::vector<SaveContext*> all_save_ctxs;
    all_save_ctxs.reserve(pipelines.size());
    for (auto& mp : pipelines) all_save_ctxs.push_back(&mp->save_ctx);

    // Shared by the hotkeys and the tray menu.
    const auto save_clip = [&] {
        const bool toast = ReadLiveConfig(cfg, [](const Config& c) { return c.ui_toast_enabled && c.ui_toast_on_save; });
        SaveReplay(all_save_ctxs, ReplayFeedback(toast));
    };
    const auto toggle_replay = [&] {
        // The Instant Replay switch, not the pipelines' armed state: while waiting for a game the
        // pipelines are disarmed but the switch is on, and this must turn it off.
        const bool next_armed = controller.ToggleUserEnabled();
        if (tray) tray->SetTooltip(TrayTooltip(next_armed));
        if (ReadLiveConfig(cfg, [](const Config& c) { return c.ui_toast_enabled && c.ui_toast_on_buffer_toggle; })) {
            SpawnToast(next_armed ? L"buffer_on" : L"buffer_off");
        }
    };
    if (tray) {
        TrayMenuActions menu;
        menu.state = [&] {
            TrayMenuState s;
            s.replay_on = controller.UserEnabled();
            s.recording = supervisor.Armed();
            s.game      = s.recording ? controller.CurrentGameFolder() : std::wstring();
            if (Updater* u = g_updater_for_engine.load()) {
                const Updater::Status us = u->GetStatus();
                if (us.state == "ready") s.update_ready = WideFromUtf8(us.latest);
            }
            ReadLiveConfig(cfg, [&](const Config& c) {
                s.buffer_seconds = c.buffer_seconds;
                s.theme          = c.ui_theme;
                s.save_hotkey    = WideFromUtf8(c.hotkeys_save_replay);
                s.toggle_hotkey  = WideFromUtf8(c.hotkeys_toggle_buffer);
                s.overlay_hotkey = WideFromUtf8(c.hotkeys_overlay);
                return 0;
            });
            return s;
        };
        menu.install_update = [] {
            if (Updater* u = g_updater_for_engine.load()) u->RequestInstall(nullptr);
        };
        menu.save_clip     = save_clip;
        menu.toggle_replay = toggle_replay;
        menu.clips_folder  = [&] {
            return ResolveOutputRoot(ReadLiveConfig(cfg, [](const Config& c) { return c.output_root; }));
        };
        tray->SetMenuActions(std::move(menu));
    }

    crash_upload::Worker crash_uploader;
    // The daily usage Log's counts (crash_upload.h), from the same sources as the telemetry probe
    // but kept whether or not resource tracking is on.
    crash_uploader.SetUsageProbe({[&] {
                                      crash_upload::UsageCounters c;
                                      c.armed = supervisor.Armed();
                                      for (const auto& mp : pipelines) {
                                          c.ticks.Add(mp->supervisor->LifetimeCounters());
                                          c.rebuilds += mp->supervisor->RebuildCount();
                                          c.resolution_changes += mp->supervisor->ResolutionChangeCount();
                                          c.saves += static_cast<uint64_t>(mp->save_ctx.save_count.load());
                                          c.ring_committed += mp->ring.CommittedBytes();
                                      }
                                      const AudioTrackHub::Losses lost = audio_hub.GetLosses();
                                      c.audio_dropped_frames = lost.overrun_frames + lost.failed_frames;
                                      c.audio_late_frames    = lost.late_frames;
                                      c.audio_encoder_errors = lost.encoder_errors;
                                      double ws_mb = 0;
                                      GetProcessRssMb(ws_mb, c.private_mb);
                                      return c;
                                  },
                                  [&](crash_upload::DayRun& r) {
                                      r.settings = crash_upload::UsageSettings(
                                          ReadLiveConfig(cfg, [](const Config& c) { return SerializeConfigFlat(c); }));
                                      r.monitors      = static_cast<uint32_t>(pipelines.size());
                                      r.audio_sources = static_cast<uint32_t>(audio_hub.SourceCount());
                                  }});
    if (resident && hwnd) crash_uploader.Start();  // refuses an isolated instance

    // Updates (update.h): resident, non-isolated engines only; Start() refuses an isolated one.
    std::unique_ptr<Updater> updater;
    if (resident && hwnd && !Instance().isolated) {
        Updater::Hooks hooks;
        hooks.check_enabled = [&] { return ReadLiveConfig(cfg, [](const Config& c) { return c.updates_check; }); };
        hooks.auto_install  = [&] { return ReadLiveConfig(cfg, [](const Config& c) { return c.updates_auto_install; }); };
        hooks.gate          = [&] {
            ApplyGate g;
            LASTINPUTINFO lii{sizeof(lii)};
            if (GetLastInputInfo(&lii)) g.idle_ms = GetTickCount() - lii.dwTime;
            QUERY_USER_NOTIFICATION_STATE quns{};
            if (SUCCEEDED(SHQueryUserNotificationState(&quns)))
                g.busy_fullscreen = quns == QUNS_BUSY || quns == QUNS_RUNNING_D3D_FULL_SCREEN ||
                                    quns == QUNS_PRESENTATION_MODE;
            // The controller's PID covers a game it records; Instant Replay off or a One-window target
            // records none, so the open windows are also checked against the Games list.
            g.game_running = controller.CurrentGamePid() != 0;
            if (!g.game_running) {
                const auto is_game = games.GameFilter();
                for (const WindowCandidate& w : SnapshotTargetWindows())
                    if (!w.image_path.empty() && is_game(w.image_path)) g.game_running = true;
            }
            for (const auto& mp : pipelines)
                if (mp->save_ctx.save_in_flight.load()) g.save_in_flight = true;
            g.other_ombra_processes = OtherOmbraProcessRunning();
            return g;
        };
        // The shutdown command's path: quit the pump, then the normal teardown (pipe, tray, hotkeys).
        hooks.request_exit = [hwnd] {
            const DWORD tid = g_pump_thread_id_for_ctrl_handler.load();
            if (!tid || !PostThreadMessageW(tid, WM_QUIT, 0, 0)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        };
        updater = std::make_unique<Updater>(std::move(hooks));
        g_updater_for_engine = updater.get();
        updater->Start();
    }

    std::vector<HotkeyActionSpec> hotkey_actions;
    hotkey_actions.push_back({1, "hotkeys.save_replay", "Alt+F10", save_clip});
    hotkey_actions.push_back({2, "hotkeys.screenshot", "Alt+F1", [&] {
        const auto [root, per_game] = ReadLiveConfig(cfg, [](const Config& c) {
            return std::pair(c.output_root, c.output_per_game_folders);
        });
        RequestScreenshot(&screenshot_ctx, &supervisor, &controller, root, per_game, L"out\\ombra_shot.png");
    }});
    hotkey_actions.push_back({3, "hotkeys.toggle_buffer", "Alt+F9", toggle_replay});
    // Opens ombra-ui's overlay (spawning ombra-ui.exe if it isn't running) or closes it.
    hotkey_actions.push_back({4, "hotkeys.overlay", "Alt+Z", [&] {
        if (tray) tray->ToggleOverlay();
    }});
    HotkeyRegistry hotkeys(std::move(hotkey_actions), backend_policy);
    // Before registering: WM_HOTKEY goes to this window, so a press during the tray menu isn't lost.
    if (hwnd && !Instance().isolated) hotkeys.SetNotifyWindow(hwnd);
    // An isolated instance (OMBRA_HOME) binds no global hotkeys: it would answer
    // Alt+Z/Alt+F10 alongside the real engine. Unpublished below, so set_config can't bind them either.
    if (!Instance().isolated) {
        hotkeys.RequestBindings(backend_policy, HotkeySpecsFromConfig(cfg));
        hotkeys.ApplyPending();
    }
    for (const auto& b : hotkeys.Status().bindings) {
        if (b.ok) std::printf("  hotkey         %-22s %s via %s\n", b.key.c_str(), b.spec.c_str(), b.backend.c_str());
        else      std::printf("  hotkey         %-22s %s FAILED to register\n", b.key.c_str(), b.spec.c_str());
    }
    if (hwnd && !Instance().isolated) g_hotkeys_for_engine = &hotkeys;

    // timedtest only: a timer thread ends the run after total_seconds, keeping [total_seconds]
    // usable as a scriptable test harness even though the pump thread is blocked in GetMessage
    // rather than polling. PostQuitMessage only affects the calling thread's own queue, so from
    // this separate thread the quit has to be posted explicitly at the pump thread's id.
    // resident's equivalent is EngineConsoleCtrlHandler (Ctrl+C) and the tray's
    // "Exit" — both eventually funnel into the same PostThreadMessage(..., WM_QUIT, ...)/
    // DestroyWindow -> WM_DESTROY -> PostQuitMessage path.
    const DWORD pump_tid = GetCurrentThreadId();
    std::thread timeout_thread;
    if (timedtest) {
        timeout_thread = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::duration<double>(total_seconds));
            PostThreadMessage(pump_tid, WM_QUIT, 0, 0);
        });
    }
    if (resident) {
        g_pump_thread_id_for_ctrl_handler = pump_tid;
        SetConsoleCtrlHandler(EngineConsoleCtrlHandler, TRUE);
    }

    // 250ms progress print (timedtest only), moved off the pacer thread (see the comment on the
    // atomics above) and now off a polling sleep loop too, onto a timer delivered through the same
    // pump.
    if (timedtest) SetTimer(nullptr, 0, 250, nullptr);

    MSG msg;
    // A new install: the setup is how anyone learns this is running. ombra-ui writes the marker.
    if (tray && !Instance().isolated && !Instance().config_dir.empty() &&
        GetFileAttributesW((Instance().config_dir + L"\\first_run_done").c_str()) == INVALID_FILE_ATTRIBUTES) {
        tray->OpenOverlay();
    }
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            hotkeys.DispatchWmHotkey(msg.wParam);
        } else if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
            // Only timedtest's thread timer; window timers (the hotkey lease) go to their window.
            std::printf("\r  %u frames, %.1f MB", frames.load(),
                        static_cast<double>(total_bytes.load()) / (1024.0 * 1024.0));
            std::fflush(stdout);
        } else {
            // Only reached in resident mode (timedtest has no window, hwnd is NULL
            // throughout its run) — routes WM_DESTROY etc. to EngineWndProc.
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    if (timeout_thread.joinable()) timeout_thread.join();
    // A clean exit from here on (a hang in teardown is not a crash), and before the pipe server
    // releases the instance lock a self-restarted successor is waiting for.
    if (resident) crash_upload::EndRun();
    // Pipe server first: stops accepting requests and joins every client worker before anything a
    // handler can reach (the tray, the hotkeys, then the supervisor's accessors) starts tearing down.
    if (pipe_server) pipe_server->Stop();
    g_updater_for_engine = nullptr;
    if (updater) updater->Stop();
    g_hotkeys_for_engine = nullptr;
    hotkeys.Shutdown();
    if (resident) SetConsoleCtrlHandler(EngineConsoleCtrlHandler, FALSE);
    // Dropped here, not left to tray's own destructor further down: the message pump has already
    // exited by this point, but clearing the global before tearing anything else down removes any
    // chance of EngineWndProc reaching a half-destroyed Tray if a stray message is still in flight.
    g_tray_for_wndproc = nullptr;
    tray.reset();

    // If the hotkey was never pressed, save once now so timedtest remains a scriptable end-to-end
    // test — every pipeline, same as a real hotkey press would.
    // Resident mode has no such auto-save-on-exit — a user closing the app is not a request to
    // save. If a save is already running, let it finish rather than racing Ring::Pin against it.
    if (timedtest && save_ctx.save_count.load() == 0) {
        const int64_t t_end = QpcNow();
        for (auto& mp : pipelines) {
            bool expected = false;
            if (mp->save_ctx.save_in_flight.compare_exchange_strong(expected, true)) {
                DoSave(&mp->save_ctx, t_end);
            }
        }
    }
    for (auto& mp : pipelines) {
        while (mp->save_ctx.save_in_flight.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LogRssStage("end of run, before shutdown");
    telemetry_sampler.reset();
    crash_uploader.Stop();  // writes the run's last usage line while everything it reads still runs
    Telemetry::Event("engine_stop", "saves=%d rebuilds=%llu", save_ctx.save_count.load(),
                     static_cast<unsigned long long>(supervisor.RebuildCount()));

    g_games_for_engine.store(nullptr, std::memory_order_release);

    // Controller first: joins its worker thread so nothing can call supervisor.SetArmed() after
    // supervisor.Stop() starts tearing the final generation down. PipelineSupervisor::Stop()
    // itself handles ordering internally (pacer joined, then encoder, then capture) — see
    // supervisor.cpp's TeardownGeneration comment for why that's not the same ordering wmain used
    // pre-step-8.
    // Audio target watcher first: it calls controller.CurrentGamePid() and is the only thread
    // touching its sources' AudioCaptures, so it must be joined before anything below does.
    audio_target_watcher_running.store(false, std::memory_order_release);
    if (audio_target_watcher.joinable()) audio_target_watcher.join();

    g_controller_for_display.store(nullptr, std::memory_order_release);
    controller.Stop();
    for (auto& mp : pipelines) mp->supervisor->Stop();
    // AudioCapture has no destructor that calls Stop(), and std::thread's destructor terminates on a
    // still-joinable thread: every capture stops here, before the hub it feeds.
    for (auto& rt : audio_sources) rt->capture.Stop();
    audio_hub.Stop();

    // supervisor.Stop() has joined every pacer thread, so no new screenshot job can be queued from
    // here on — safe to drain and stop the worker now rather than at process exit. `running` is
    // set under job_mutex, not as a bare atomic store (advisor review): the worker's wait predicate
    // reads both `job_ready` and `running`, and setting `running` outside the lock leaves a window
    // between the worker evaluating the predicate false and actually going to sleep where this
    // notify can be missed entirely — a lost wakeup that hangs the join forever, exactly the wrong
    // failure shape for an always-resident process's shutdown path.
    {
        std::lock_guard<std::mutex> lock(screenshot_ctx.job_mutex);
        screenshot_ctx.running.store(false, std::memory_order_release);
    }
    screenshot_ctx.job_cv.notify_one();
    screenshot_ctx.worker.join();

    // WaitForAutotuneCompletion runs detached (AutotuneContext's own comment: it only touches
    // autotune_ctx and a HANDLE it exclusively owns, so letting the OS reclaim it at process exit
    // is fine) -- but autotune_ctx is a *local* here, not heap-owned, so if it's still running when
    // this function returns and the stack unwinds, that thread would go on writing through a
    // dangling pointer. capturebench.cpp's kMaxSeconds+kControlSeconds wall-clock cap (~14s) bounds
    // this to a short, rare stall on shutdown rather than an unbounded one.
    while (autotune_ctx.state.load(std::memory_order_acquire) == AutotuneContext::State::Running) {
        Sleep(100);
    }

    const PipelineSupervisor::RunStats pacer_stats = supervisor.LastStats();

    double avg_ms = 0.0, max_ms = 0.0, var_ms2 = 0.0;
    if (tick_times.size() > 1) {
        std::vector<double> gaps_ms;
        gaps_ms.reserve(tick_times.size() - 1);
        for (size_t i = 1; i < tick_times.size(); ++i)
            gaps_ms.push_back(
                std::chrono::duration<double, std::milli>(tick_times[i] - tick_times[i - 1]).count());
        for (double g : gaps_ms) { avg_ms += g; max_ms = (std::max)(max_ms, g); }
        avg_ms /= gaps_ms.size();
        for (double g : gaps_ms) var_ms2 += (g - avg_ms) * (g - avg_ms);
        var_ms2 /= gaps_ms.size();
    }

    std::printf("\n  --- capture ---\n");
    std::printf("  frames           %u  (target %.0f)\n", frames.load(), total_seconds * fps);
    std::printf("  bytes            %.2f MB\n",
                static_cast<double>(total_bytes.load()) / (1024.0 * 1024.0));
    std::printf("  keyframes        %u (NV_ENC_PIC_TYPE_IDR)\n", keyframes.load());
    std::printf("  pacer ticks      %llu  duplicates %llu  late %llu  resyncs %llu"
                "  (last generation only — %llu rebuild%s)\n",
                static_cast<unsigned long long>(pacer_stats.ticks_submitted),
                static_cast<unsigned long long>(pacer_stats.duplicate_ticks),
                static_cast<unsigned long long>(pacer_stats.late_ticks),
                static_cast<unsigned long long>(pacer_stats.resync_count),
                static_cast<unsigned long long>(supervisor.RebuildCount()),
                supervisor.RebuildCount() == 1 ? "" : "s");
    std::printf("  tick spacing     avg %.3f ms (target %.3f)  max %.3f ms  stdev %.3f ms\n",
                avg_ms, 1000.0 / fps, max_ms, std::sqrt(var_ms2));
    const PacerLatencyStats& lat = pacer_stats.latency;
    const char* timer_backend_str =
        pacer_stats.timer_backend == PacerTimerBackend::kHighResWaitableTimer
            ? "high-res waitable timer"
            : "timeBeginPeriod(1) fallback";
    std::printf("  pacer timer      %s\n", timer_backend_str);
    std::printf("  wait overshoot   p50 %.3f ms  p99 %.3f ms\n", lat.wait_overshoot_p50_ms,
                lat.wait_overshoot_p99_ms);
    std::printf("  encode duration  p50 %.3f ms  p99 %.3f ms\n", lat.encode_p50_ms,
                lat.encode_p99_ms);
    std::printf("  saves            %d\n", save_ctx.save_count.load());
    Log::Info("pacer summary     ticks=%llu duplicates=%llu late=%llu resyncs=%llu rebuilds=%llu "
              "spacing_avg_ms=%.3f spacing_max_ms=%.3f spacing_stdev_ms=%.3f "
              "timer_backend=%s wait_overshoot_p50_ms=%.3f wait_overshoot_p99_ms=%.3f "
              "encode_p50_ms=%.3f encode_p99_ms=%.3f saves=%d",
              static_cast<unsigned long long>(pacer_stats.ticks_submitted),
              static_cast<unsigned long long>(pacer_stats.duplicate_ticks),
              static_cast<unsigned long long>(pacer_stats.late_ticks),
              static_cast<unsigned long long>(pacer_stats.resync_count),
              static_cast<unsigned long long>(supervisor.RebuildCount()),
              avg_ms, max_ms, std::sqrt(var_ms2), timer_backend_str,
              lat.wait_overshoot_p50_ms, lat.wait_overshoot_p99_ms,
              lat.encode_p50_ms, lat.encode_p99_ms, save_ctx.save_count.load());
    std::printf("\n  --- audio ---\n");
    for (const auto& rt : audio_sources) {
        std::printf("  %-16s %llu packets\n", rt->id.c_str(),
                    static_cast<unsigned long long>(rt->packets.load()));
    }

    if (hwnd) DestroyWindow(hwnd);
    Log::Shutdown();  // prints pending repeat-suppression counts

    // Resident mode exiting cleanly (a closed window / Ctrl+C) is success regardless of whether a
    // save ever happened during the session — timedtest's "0 saves is exit code 2" convention is
    // specific to it being a scriptable end-to-end test that's supposed to produce a file.
    if (resident) return 0;
    return save_ctx.save_count.load() > 0 ? 0 : 2;
}
