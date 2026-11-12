// ombra-ui.exe — process shell + pipe client: find or start the engine, talk to it over the pipe,
// show the overlay. The first-run marker below is kept for the first-run setup; the engine
// derives the bitrate from the quality preset itself, so no first run has to write one.
//
// Entry points:
//   ombra-ui.exe             real flow: connect (spawning ombra.exe if needed), then the overlay.
//   ombra-ui.exe --drive     headless, scriptable overlay for checking UI changes (driver.cpp).
//   ombra-ui.exe --selftest  headless: exercises the same connect/spawn/get_schema/get_config/
//                             first-run-marker logic against a real running ombra.exe,
//                             asserts against ground truth, prints PASS/FAIL, no window shown.
#include "ombra-ui/overlay_window.h"
#include "ombra/brand.h"
#include "ombra-ui/pipe_client.h"
#include "ombra-ui/screens.h"
#include "ombra-ui/toast_window.h"
#include "ombra-ui/ui/motion.h"
#include "ombra-ui/ui/widget.h"
#include "ombra-ui/driver.h"

#include "ombra-ui/setup_screen.h"
#include "ombra/config.h"
#include "ombra/json.h"
#include "ombra/crash.h"
#include "ombra/log.h"
#include "ombra/version.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef OMBRA_GALLERY_INC
#include OMBRA_GALLERY_INC  // Gallery build only
#endif

using ombra::AllSettings;
using ombra::ConfigDirW;
using ombra::JsonValue;
using ombra::Log;
using ombra::ParseJson;
using ombra::SettingKind;
using ombra_ui::PipeClient;

namespace {

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

// A JsonValue string field, or "" if absent/wrong type/null — every get_status field this file
// reads is optional-shaped (engine reports null for "no game", "no save yet", etc.).
std::string StrField(const JsonValue& obj, const char* key) {
    const JsonValue* v = obj.Find(key);
    return (v && v->type == JsonValue::Type::String) ? v->str : "";
}

// ---------------------------------------------------------------------------------------------
// Single-instance probe + spawn, on paths.h's Instance().instance_mutex:
// the engine now acquires this mutex before any GPU/capture/audio setup, so its mere existence
// (regardless of who currently holds pipe-serving duties) is a reliable "an ombra.exe process
// object is alive" signal — no probe-then-race the way a connect attempt alone would be.
// ---------------------------------------------------------------------------------------------

bool IsEngineInstanceRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, ombra::Instance().instance_mutex.c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// Locates ombra.exe next to this executable and launches it detached from ombra-ui's own console
// (CREATE_NO_WINDOW — ombra.exe is a console-subsystem app; without this it would inherit
// ombra-ui's console, and closing ombra-ui or a Ctrl+C there could tear the engine down with it,
// i.e. closing ombra-ui.exe must not affect the running engine).
//
// `out_pi`, when non-null, receives the child's handles and this function does NOT close them —
// the caller then owns them and must CloseHandle() both. This exists so a caller that needs to
// clean up a process it spawned itself (RunSelfTest below: it must never touch an engine that was
// already running before the test started) has something to wait on/terminate. The normal (null)
// path keeps the original behavior: the child is intentionally left resident, handles closed
// immediately since nothing needs them again.
bool SpawnEngine(PROCESS_INFORMATION* out_pi = nullptr) {
    wchar_t self_path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log::Error("ombra-ui: GetModuleFileNameW failed (err=%lu)", GetLastError());
        return false;
    }
    std::wstring dir(self_path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    const std::wstring engine_path = dir + L"" OMBRA_ENGINE_EXE;

    STARTUPINFOW         si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION  pi{};
    std::wstring         cmdline = L"\"" + engine_path + L"\"";
    if (!CreateProcessW(engine_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log::Error("ombra-ui: CreateProcessW failed for %ls (err=%lu)", engine_path.c_str(),
                   GetLastError());
        return false;
    }
    if (out_pi) {
        *out_pi = pi;
    } else {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return true;
}

// Connects to the engine, spawning it first if nothing answers and nothing is already running.
// Returns true with `client` connected and past its hello handshake. On failure, *fatal_reason
// (if non-null) is set only for an explicit protocol-level rejection (a version mismatch) — the
// one failure mode where retrying can't help, so it's worth telling the caller apart from "no
// engine yet" or "timed out".
//
// `spawned_pi_out`, when non-null, is left zeroed unless THIS call is the one that spawned a new
// engine instance (the "no engine instance found" branch below) — a caller uses that to tell "I
// own this process and must clean it up" apart from "something else already had this running"
// without ever touching a pre-existing instance.
bool EnsureEngineRunningAndConnect(PipeClient& client, std::string* fatal_reason = nullptr,
                                    PROCESS_INFORMATION* spawned_pi_out = nullptr) {
    std::string last_reason;
    // 1 = handshake ok, 0 = no/garbled response (retryable), -1 = explicit rejection (not
    // retryable — last_reason is set, and copied to *fatal_reason if the caller wants it).
    auto try_hello = [&](DWORD timeout_ms) -> int {
        if (client.Hello(timeout_ms, &last_reason)) return 1;
        return last_reason.empty() ? 0 : -1;
    };
    auto fail_fast = [&]() {
        Log::Error("ombra-ui: engine rejected the handshake: %s", last_reason.c_str());
        if (fatal_reason) *fatal_reason = last_reason;
        client.Close();
    };

    // Fast path: something is already listening (a prior ombra-ui launch, the tray, a manual
    // start) — this covers the large majority of real launches without ever touching
    // CreateProcessW.
    if (client.Connect(800)) {
        const int r = try_hello(3000);
        if (r == 1) {
            std::printf("  ombra-ui       connected to an already-running engine\n");
            return true;
        }
        if (r == -1) { fail_fast(); return false; }
        client.Close();
    }

    if (IsEngineInstanceRunning()) {
        // An instance exists (mutex held) but isn't answering yet — it's still inside its own
        // startup (GPU/capture/audio setup takes a few seconds).
        // Don't spawn a second one; just wait longer for the first.
        Log::Info("ombra-ui: an engine instance is starting up, waiting for its pipe");
        if (client.Connect(15000)) {
            const int r = try_hello(5000);
            if (r == 1) {
                std::printf("  ombra-ui       waited for an already-starting engine, then connected\n");
                return true;
            }
            if (r == -1) { fail_fast(); return false; }
        }
        client.Close();
        Log::Error("ombra-ui: an engine instance is running but never answered the pipe");
        return false;
    }

    Log::Info("ombra-ui: no engine instance found, spawning " OMBRA_ENGINE_EXE);
    if (!SpawnEngine(spawned_pi_out)) return false;
    if (client.Connect(15000)) {
        const int r = try_hello(5000);
        if (r == 1) {
            std::printf("  ombra-ui       spawned a new engine instance and connected\n");
            return true;
        }
        if (r == -1) { fail_fast(); return false; }
    }
    client.Close();
    Log::Error("ombra-ui: spawned " OMBRA_ENGINE_EXE " but it never answered the pipe");
    return false;
}

// ---------------------------------------------------------------------------------------------
// --selftest: headless, asserts against ground truth, no window: a printing-only test doesn't
// survive to catch a later regression.
// ---------------------------------------------------------------------------------------------

// Terminates and joins a self-spawned throwaway engine on scope exit, iff this run is the one
// that spawned it (EnsureEngineRunningAndConnect only fills `pi` on that exact branch — an engine
// that was already running before the test started is never touched). RAII rather than a single
// cleanup call at the bottom of RunSelfTest so an early return (e.g. the "cannot continue without
// a connection" bail-out right after the connect check) still cleans up. There is no graceful
// shutdown request over the pipe (HandlePipeRequest has no "exit"/"shutdown" type) and this
// process has no console to deliver a CTRL event to (CREATE_NO_WINDOW, fully detached) — so this
// is the same terminate Task Manager's "End Task" would do, acceptable here because it's always a
// throwaway instance with nothing but a default RAM buffer in flight, never one the user started.
struct SpawnedEngineGuard {
    PROCESS_INFORMATION pi{};
    ~SpawnedEngineGuard() {
        if (!pi.hProcess) return;
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
};

// Backs up and restores the real first-run marker around the test's own create/delete probing.
struct FirstRunMarkerBackup {
    std::wstring path;
    bool         had_marker = false;
    explicit FirstRunMarkerBackup(std::wstring p) : path(std::move(p)) {
        had_marker = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    ~FirstRunMarkerBackup() {
        if (path.empty()) return;
        if (had_marker) {
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        } else {
            DeleteFileW(path.c_str());
        }
    }
};

// Every visual-review PNG under out\snapshots. Needs a D3D device but no engine, so `--snapshots`
// can run it alone.
void RenderSnapshots(const std::function<void(bool, const char*)>& check) {
    // Visual-review snapshots: one PNG per view. Rail pages are named by SettingsRail (screens.h).
    {
        const int home_rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\home.png", ombra_ui::ActiveScreen::Home);
        check(home_rc == 0, "snapshot: Home view rendered");

        struct RailSnapshot { const wchar_t* name; int index; };
        const RailSnapshot rails[] = {
            { L"capture", ombra_ui::kRailCapture }, { L"replay", ombra_ui::kRailReplay },
            { L"games", ombra_ui::kRailGames }, { L"appearance", ombra_ui::kRailAppearance },
            { L"shortcuts", ombra_ui::kRailShortcuts }, { L"advanced", ombra_ui::kRailAdvanced },
        };
        for (const auto& r : rails) {
            const std::wstring path = L"out\\snapshots\\settings_" + std::wstring(r.name) + L".png";
            const int rc = ombra_ui::RunOverlayTest(path, ombra_ui::ActiveScreen::Settings, r.index);
            char what[96];
            std::snprintf(what, sizeof(what), "snapshot: Settings/%ls rail rendered", r.name);
            check(rc == 0, what);
        }

        // Replay & Output's second sub-tab. The loop above only ever renders sub-tab 0, so the
        // Output page (folder picker, filename template, container) had no snapshot at all.
        {
            const int rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\settings_output.png",
                                                    ombra_ui::ActiveScreen::Settings, ombra_ui::kRailReplay, 1920, 1080,
                                                    ombra_ui::PanelPosition::Center,
                                                    ombra_ui::PanelAttach::Floating, 1);
            check(rc == 0, "snapshot: Settings/Output sub-tab rendered");
        }
        // Sub-tabs the loop above never reaches.
        {
            struct SubTab { const wchar_t* path; int rail; int sub_tab; };
            const SubTab tabs[] = {
                { L"out\\snapshots\\settings_advanced_encoder.png", ombra_ui::kRailAdvanced, 0 },
                { L"out\\snapshots\\settings_advanced_system.png", ombra_ui::kRailAdvanced, 1 },
            };
            for (const auto& t : tabs) {
                const int rc = ombra_ui::RunOverlayTest(t.path, ombra_ui::ActiveScreen::Settings, t.rail, 1920, 1080,
                                                        ombra_ui::PanelPosition::Center,
                                                        ombra_ui::PanelAttach::Floating, t.sub_tab);
                check(rc == 0, "snapshot: Settings sub-tab rendered");
            }
        }
        // States a card only shows under another setting: Custom bitrate + disk storage, constant
        // quality, and the process lists with entries in them.
        {
            using ombra::JsonValue;
            struct Staged { const wchar_t* path; int rail; int sub_tab; std::function<void(ombra_ui::SettingsScreen&)> stage; };
            const Staged staged[] = {
                { L"out\\snapshots\\settings_replay_custom_disk.png", ombra_ui::kRailReplay, 0, [](ombra_ui::SettingsScreen& s) {
                      s.ApplySettingChangeForTest("encode.quality_preset", JsonValue::MakeStr("custom"));
                      s.ApplySettingChangeForTest("buffer.storage", JsonValue::MakeStr("file"));
                  } },
                { L"out\\snapshots\\settings_replay_match_display.png", ombra_ui::kRailReplay, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedDisplayFpsForSnapshot(144);
                      s.ApplySettingChangeForTest("capture.framerate", JsonValue::MakeNum(0));
                  } },
                { L"out\\snapshots\\settings_replay_scaled.png", ombra_ui::kRailReplay, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedSourceSizeForSnapshot(2560, 1600);
                      s.ApplySettingChangeForTest("capture.resolution", JsonValue::MakeStr("1080p"));
                  } },
                { L"out\\snapshots\\settings_advanced_encoder_cq.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.ApplySettingChangeForTest("encode.rate_control", JsonValue::MakeStr("cqp"));
                  } },
                { L"out\\snapshots\\settings_advanced_encoder_gpus.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedRecordingGpusForSnapshot();
                  } },
                { L"out\\snapshots\\settings_advanced_encoder_cpu.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedRecordingGpusForSnapshot();
                      s.ApplySettingChangeForTest("encode.adapter", JsonValue::MakeStr("cpu"));
                  } },
                // No GPU encoder would open: auto is recording on the CPU.
                { L"out\\snapshots\\settings_advanced_encoder_cpu_fallback.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedRecordingGpusForSnapshot(/*cpu_fallback=*/true);
                  } },
                { L"out\\snapshots\\settings_advanced_encoder_amd.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedRecordingGpusForSnapshot();
                      s.SeedSourceSizeForSnapshot(1920, 1080);
                      s.ApplySettingChangeForTest("encode.adapter", JsonValue::MakeStr("amd"));
                  } },
                { L"out\\snapshots\\settings_advanced_encoder_amd_h264.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedRecordingGpusForSnapshot();
                      s.SeedSourceSizeForSnapshot(1920, 1080);
                      s.ApplySettingChangeForTest("encode.adapter", JsonValue::MakeStr("amd"));
                      s.ApplySettingChangeForTest("encode.amf_av1_unaligned", JsonValue::MakeStr("h264"));
                  } },
                { L"out\\snapshots\\settings_advanced_encoder_amd_scale.png", ombra_ui::kRailAdvanced, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedRecordingGpusForSnapshot();
                      s.SeedSourceSizeForSnapshot(1920, 1080);
                      s.ApplySettingChangeForTest("encode.adapter", JsonValue::MakeStr("amd"));
                      s.ApplySettingChangeForTest("encode.amf_av1_unaligned", JsonValue::MakeStr("scale"));
                  } },
                // General -> Hotkeys as the engine would report it: all live, a customised shortcut
                // (reset button), one that failed to register, recording, and a refused combo.
                { L"out\\snapshots\\settings_hotkeys_live.png", ombra_ui::kRailShortcuts, 0, [](ombra_ui::SettingsScreen& s) {
                      s.ApplySettingChangeForTest("hotkeys.screenshot", JsonValue::MakeStr("Ctrl+Alt+S"));
                      s.SeedHotkeyStatusForSnapshot("register_hotkey");
                  } },
                { L"out\\snapshots\\settings_hotkeys_failed.png", ombra_ui::kRailShortcuts, 0, [](ombra_ui::SettingsScreen& s) {
                      s.ApplySettingChangeForTest("hotkeys.backend", JsonValue::MakeStr("register_hotkey"));
                      s.SeedHotkeyStatusForSnapshot("register_hotkey", "hotkeys.overlay");
                  } },
                { L"out\\snapshots\\settings_hotkeys_recording.png", ombra_ui::kRailShortcuts, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedHotkeyStatusForSnapshot("ll_hook");
                      s.SeedShortcutRecordingForSnapshot("hotkeys.save_replay", MOD_CONTROL | MOD_SHIFT, L"");
                  } },
                { L"out\\snapshots\\settings_hotkeys_conflict.png", ombra_ui::kRailShortcuts, 0, [](ombra_ui::SettingsScreen& s) {
                      s.SeedHotkeyStatusForSnapshot("register_hotkey");
                      s.SeedShortcutRecordingForSnapshot("hotkeys.save_replay", 0,
                                                         L"Alt+F1: already used by Screenshot.");
                  } },
            };
            for (const auto& t : staged) {
                const auto stage = t.stage;
                const int rc = ombra_ui::RunOverlayTest(t.path, ombra_ui::ActiveScreen::Settings, t.rail, 1920, 1080,
                                                        ombra_ui::PanelPosition::Center,
                                                        ombra_ui::PanelAttach::Floating, t.sub_tab,
                                                        [stage](ombra_ui::OverlayHost& h) {
                                                            if (auto* s = h.GetSettingsScreen()) stage(*s);
                                                        });
                check(rc == 0, "snapshot: Settings staged state rendered");
            }
        }
        // The new cards at 150%: fixed label widths must still fit once fonts scale.
        {
            struct Scaled { const wchar_t* path; int rail; int sub_tab; };
            const Scaled scaled[] = {
                { L"out\\snapshots\\settings_output_150pct.png", ombra_ui::kRailReplay, 1 },
                { L"out\\snapshots\\settings_advanced_encoder_150pct.png", ombra_ui::kRailAdvanced, 0 },
                { L"out\\snapshots\\settings_advanced_system_150pct.png", ombra_ui::kRailAdvanced, 1 },
                { L"out\\snapshots\\settings_shortcuts_150pct.png", ombra_ui::kRailShortcuts, 0 },
            };
            for (const auto& t : scaled) {
                const int rc = ombra_ui::RunOverlayTest(t.path, ombra_ui::ActiveScreen::Settings, t.rail, 2880, 1620,
                                                        ombra_ui::PanelPosition::Center,
                                                        ombra_ui::PanelAttach::Floating, t.sub_tab,
                                                        [](ombra_ui::OverlayHost& h) { h.SetTestDpiScale(1.5f); });
                check(rc == 0, "snapshot: Settings sub-tab at 150% rendered");
            }
        }

        // 150% scaling at the same logical size (2880x1620 px = 1920x1080 dip). Fonts used to stay at
        // 100% while everything else scaled, so text shrank inside its controls.
        for (const auto& r : rails) {
            const std::wstring path = L"out\\snapshots\\settings_" + std::wstring(r.name) + L"_150pct.png";
            const int rc = ombra_ui::RunOverlayTest(path, ombra_ui::ActiveScreen::Settings, r.index, 2880, 1620,
                                                    ombra_ui::PanelPosition::Center,
                                                    ombra_ui::PanelAttach::Floating, 0,
                                                    [](ombra_ui::OverlayHost& h) { h.SetTestDpiScale(1.5f); });
            char what[96];
            std::snprintf(what, sizeof(what), "snapshot: Settings/%ls at 150%% rendered", r.name);
            check(rc == 0, what);
        }
        {
            const int rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\home_150pct.png", ombra_ui::ActiveScreen::Home, 0,
                                                    2880, 1620, ombra_ui::PanelPosition::Center,
                                                    ombra_ui::PanelAttach::Floating, 0,
                                                    [](ombra_ui::OverlayHost& h) { h.SetTestDpiScale(1.5f); });
            check(rc == 0, "snapshot: Home at 150% rendered");
        }

        // Capture > Audio (sub-tab 1) with the default sources; playing apps need an engine.
        const int audio_rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\settings_monitors_and_audio_audio.png",
                                                       ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture, 1920, 1080,
                                                       ombra_ui::PanelPosition::Center,
                                                       ombra_ui::PanelAttach::Floating, 1);
        check(audio_rc == 0, "snapshot: Monitors & Audio/Audio sub-tab rendered");

        // Settings Save/Discard bar: the fixed bottom-right bar only
        // renders once something's staged (SettingsScreen::HasPendingChanges()) -- stage one Realloc
        // key here so this snapshot actually exercises the bar's real layout/consequence-text path,
        // not just its (invisible) absence like every other snapshot above.
        const int bar_rc = ombra_ui::RunOverlayTest(
            L"out\\snapshots\\settings_save_discard_bar.png", ombra_ui::ActiveScreen::Settings, ombra_ui::kRailReplay,
            1920, 1080, ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
            [](ombra_ui::OverlayHost& host) {
                if (auto* screen = host.GetSettingsScreen()) {
                    screen->ApplySettingChangeForTest("buffer.seconds", ombra::JsonValue::MakeNum(90.0));
                }
            });
        check(bar_rc == 0, "snapshot: Settings Save/Discard bar rendered with a staged Realloc edit");
    }

    // Settings position consistency: ui.panel_position (Top|Center) now drives both
    // screens' content-row placement identically (OverlayHost::ComputeRowTop) -- previously Home
    // always centered and Settings always hugged the header regardless of this setting. Render both
    // screens at the non-default Top position explicitly, so a regression that decouples one screen
    // from the setting again shows up as a visual diff here, not just at the (Center) default above.
    {
        const int home_top_rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\home_top.png", ombra_ui::ActiveScreen::Home,
                                                          0, 1920, 1080, ombra_ui::PanelPosition::Top);
        check(home_top_rc == 0, "snapshot: Home @ Position::Top rendered");
        const int settings_top_rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\settings_top.png", ombra_ui::ActiveScreen::Settings,
                                                              ombra_ui::kRailShortcuts, 1920, 1080, ombra_ui::PanelPosition::Top);
        check(settings_top_rc == 0, "snapshot: Settings @ Position::Top rendered");
    }

    // Attached panel on every edge, plus the screen frame. Geometry is checked
    // headlessly (tests/ui_tests.cpp); these exist so a human can look at the silhouette.
    {
        struct EdgeCase { const wchar_t* file; ombra_ui::PanelPosition pos; const char* name; };
        const EdgeCase edges[] = {
            {L"out\\snapshots\\attached_top.png",    ombra_ui::PanelPosition::Top,    "top"},
            {L"out\\snapshots\\attached_bottom.png", ombra_ui::PanelPosition::Bottom, "bottom"},
            {L"out\\snapshots\\attached_left.png",   ombra_ui::PanelPosition::Left,   "left"},
            {L"out\\snapshots\\attached_right.png",  ombra_ui::PanelPosition::Right,  "right"},
        };
        for (const auto& e : edges) {
            const int rc = ombra_ui::RunOverlayTest(
                e.file, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailShortcuts, 1920, 1080, e.pos,
                ombra_ui::PanelAttach::Attached, 0, [](ombra_ui::OverlayHost& host) {
                    host.GetTheme().screen_frame = true;
                    host.GetTheme().screen_frame_radius = 32.0f;
                    host.GetTheme().screen_frame_thickness = 0.0f;
                });
            check(rc == 0, (std::string("snapshot: Settings attached to ") + e.name + " edge rendered").c_str());
        }
        const int home_rc = ombra_ui::RunOverlayTest(
            L"out\\snapshots\\attached_home_top.png", ombra_ui::ActiveScreen::Home, 0, 1920, 1080,
            ombra_ui::PanelPosition::Top, ombra_ui::PanelAttach::Attached);
        check(home_rc == 0, "snapshot: Home attached to top edge rendered");
        const int border_rc = ombra_ui::RunOverlayTest(
            L"out\\snapshots\\attached_top_border.png", ombra_ui::ActiveScreen::Settings, ombra_ui::kRailShortcuts, 1920, 1080,
            ombra_ui::PanelPosition::Top, ombra_ui::PanelAttach::Attached, 0,
            [](ombra_ui::OverlayHost& host) {
                host.GetTheme().screen_frame = true;
                host.GetTheme().screen_frame_radius = 32.0f;
                host.GetTheme().screen_frame_thickness = 10.0f;
            });
        check(border_rc == 0, "snapshot: attached top with a 10px frame border rendered");
        // Tall tabs (they scroll) inside the attached panel, with the default 12/8 frame: this is where
        // scrollbar/clip problems show up, the General tab above is too short to scroll.
        for (int rail : {ombra_ui::kRailCapture, ombra_ui::kRailAppearance, ombra_ui::kRailAdvanced}) {
            const std::wstring f = L"out\\snapshots\\attached_top_tab" + std::to_wstring(rail) + L".png";
            const int rc = ombra_ui::RunOverlayTest(
                f, ombra_ui::ActiveScreen::Settings, rail, 1920, 1080, ombra_ui::PanelPosition::Top,
                ombra_ui::PanelAttach::Attached, 0, [](ombra_ui::OverlayHost& host) {
                    host.GetTheme().screen_frame = true;
                });
            check(rc == 0, "snapshot: attached top, tall tab rendered");
        }
    }

    // Light theme (ui.theme = light): every screen family once, mono accent (black) plus one hue,
    // and the contrast frame. Look at these -- contrast/legibility is what a token swap gets wrong.
    {
        struct LightCase { const wchar_t* file; ombra_ui::ActiveScreen screen; int tab; ombra_ui::AccentId accent; bool attached; int sub_tab = 0; };
        const LightCase cases[] = {
            {L"out\\snapshots\\light_home.png",       ombra_ui::ActiveScreen::Home,     0, ombra_ui::AccentId::Mono,   false},
            {L"out\\snapshots\\light_shortcuts.png",  ombra_ui::ActiveScreen::Settings, ombra_ui::kRailShortcuts, ombra_ui::AccentId::Mono,   false},
            {L"out\\snapshots\\light_appearance.png", ombra_ui::ActiveScreen::Settings, ombra_ui::kRailAppearance, ombra_ui::AccentId::Blue,   false},
            {L"out\\snapshots\\light_audio.png",      ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture, ombra_ui::AccentId::Mono,   false},
            {L"out\\snapshots\\light_attached_top.png", ombra_ui::ActiveScreen::Settings, ombra_ui::kRailShortcuts, ombra_ui::AccentId::Mono, true},
        };
        for (const auto& c : cases) {
            const int rc = ombra_ui::RunOverlayTest(
                c.file, c.screen, c.tab, 1920, 1080,
                c.attached ? ombra_ui::PanelPosition::Top : ombra_ui::PanelPosition::Center,
                c.attached ? ombra_ui::PanelAttach::Attached : ombra_ui::PanelAttach::Floating, c.sub_tab,
                [&c](ombra_ui::OverlayHost& host) {
                    host.GetTheme().ApplyMode(ombra_ui::ThemeMode::Light);
                    host.GetTheme().ApplyAccent(c.accent);
                    if (c.attached) {
                        host.GetTheme().screen_frame = true;
                        host.GetTheme().screen_frame_color = ombra_ui::ScreenFrameColor::Contrast;
                    }
                });
            check(rc == 0, "snapshot: light theme rendered");
        }
    }

    // ui.rail_position: the tab rail on each of the four sides. Left is the default (already in
    // settings_general.png); right/top/bottom are the ones worth eyeballing, since top/bottom flip
    // the rail to a horizontal strip with the icon beside the label instead of above it.
    {
        struct RailCase { const wchar_t* file; ombra_ui::RailPosition pos; };
        const RailCase cases[] = {
            {L"out\\snapshots\\rail_right.png",  ombra_ui::RailPosition::Right},
            {L"out\\snapshots\\rail_top.png",    ombra_ui::RailPosition::Top},
            {L"out\\snapshots\\rail_bottom.png", ombra_ui::RailPosition::Bottom},
        };
        for (const auto& c : cases) {
            const int rc = ombra_ui::RunOverlayTest(
                c.file, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailShortcuts, 1920, 1080,
                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
                [&c](ombra_ui::OverlayHost& host) { host.GetTheme().rail_position = c.pos; });
            check(rc == 0, "snapshot: rail position rendered");
        }
    }

    // Games: a list of 17 entries (scrolls inside its card), the "Looked like a game"
    // rows, a staged edit (the Save bar shrinks the list), the list scrolled to the not-games, 150%,
    // and light.
    {
        struct GamesCase { const wchar_t* file; bool light; float dpi; bool stage; float list_scroll; };
        const GamesCase cases[] = {
            {L"out\\snapshots\\games.png",          false, 1.0f, false, 0.0f},
            {L"out\\snapshots\\games_staged.png",   false, 1.0f, true,  0.0f},
            {L"out\\snapshots\\games_scrolled.png", false, 1.0f, false, 2000.0f},
            {L"out\\snapshots\\games_150pct.png",   false, 1.5f, false, 0.0f},
            {L"out\\snapshots\\light_games.png",    true,  1.0f, false, 0.0f},
        };
        for (const auto& c : cases) {
            const int w = c.dpi > 1.0f ? 2880 : 1920, h = c.dpi > 1.0f ? 1620 : 1080;
            const int rc = ombra_ui::RunOverlayTest(
                c.file, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailGames, w, h,
                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
                [&c](ombra_ui::OverlayHost& host) {
                    if (c.light) host.GetTheme().ApplyMode(ombra_ui::ThemeMode::Light);
                    if (c.dpi > 1.0f) host.SetTestDpiScale(c.dpi);
                    auto* s = host.GetSettingsScreen();
                    if (!s) return;
                    s->SeedGamesForSnapshot();
                    if (c.stage) {  // switch off the first game in the list (after the undecided rows)
                        {
                            for (const auto& r : s->GameRowsForTest()) {
                                if (r.decide || !r.on_toggle) continue;
                                auto toggle = r.on_toggle;
                                toggle(!r.is_game);
                                break;
                            }
                        }
                    }
                    if (c.list_scroll > 0.0f) s->ScrollGamesListForTest(c.list_scroll);
                });
            check(rc == 0, "snapshot: Games page rendered");
        }
    }

    // Monitors tiles (fake monitors + synthetic previews): the eye/check overlay buttons, both themes.
    for (const bool light : {false, true}) {
        const std::wstring f = light ? L"out\\snapshots\\light_monitors.png" : L"out\\snapshots\\dark_monitors.png";
        const int rc = ombra_ui::RunOverlayTest(f, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture, 1920, 1080,
                                                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
                                                [light](ombra_ui::OverlayHost& host) {
                                                    if (light) host.GetTheme().ApplyMode(ombra_ui::ThemeMode::Light);
                                                    if (auto* s = host.GetSettingsScreen()) s->SeedMonitorsForSnapshot(3);
                                                });
        check(rc == 0, "snapshot: monitors tiles rendered");
    }

    // When = Only in games with What = The game: no display tiles, nothing records between games.
    {
        const int rc = ombra_ui::RunOverlayTest(
            L"out\\snapshots\\monitors_only_games_game.png", ombra_ui::ActiveScreen::Settings,
            ombra_ui::kRailCapture, 1920, 1080, ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
            [](ombra_ui::OverlayHost& host) {
                auto* s = host.GetSettingsScreen();
                if (!s) return;
                s->SeedMonitorsForSnapshot(2);
                s->ApplySettingChangeForTest("capture.mode", ombra::JsonValue::MakeStr("game"));
                s->ApplySettingChangeForTest("capture.target", ombra::JsonValue::MakeStr("game"));
            });
        check(rc == 0, "snapshot: Monitors, Only in games + The game rendered");
    }

    // Home with Instant Replay switched on but nothing recording: waiting for a game.
    {
        const int rc = ombra_ui::RunOverlayTest(
            L"out\\snapshots\\home_waiting.png", ombra_ui::ActiveScreen::Home, 0, 1920, 1080,
            ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0, [](ombra_ui::OverlayHost& host) {
                auto* home = host.GetHomeScreen();
                if (!home) return;
                ombra::JsonValue status = ombra::JsonValue::MakeObject();
                status.Set("enabled", ombra::JsonValue::MakeBool(true));
                status.Set("armed", ombra::JsonValue::MakeBool(false));
                status.Set("target", ombra::JsonValue::MakeStr("monitor"));
                home->UpdateState(&status, nullptr);
            });
        check(rc == 0, "snapshot: Home waiting for a game rendered");
    }

    // Monitors card in window mode: saved window (running), "Running now" grid, "Show all".
    for (const bool light : {false, true}) {
        const std::wstring f = light ? L"out\\snapshots\\light_windows.png" : L"out\\snapshots\\dark_windows.png";
        const int rc = ombra_ui::RunOverlayTest(f, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture, 1920, 1080,
                                                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
                                                [light](ombra_ui::OverlayHost& host) {
                                                    if (light) host.GetTheme().ApplyMode(ombra_ui::ThemeMode::Light);
                                                    if (auto* s = host.GetSettingsScreen()) s->SeedWindowsForSnapshot(8, "chrome.exe");
                                                });
        check(rc == 0, "snapshot: window picker rendered");
    }

    // Appearance with its dependents showing: frame on (radius/border/color appear), and the panel at
    // an edge (Style appears). Scrolled to the bottom as well, for the Notifications card.
    for (const float scroll : {0.0f, 1000.0f}) {
        const std::wstring f = scroll > 0.0f ? L"out\\snapshots\\appearance_expanded_bottom.png"
                                             : L"out\\snapshots\\appearance_expanded.png";
        const int rc = ombra_ui::RunOverlayTest(f, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailAppearance, 1920, 1080,
                                                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
                                                [scroll](ombra_ui::OverlayHost& host) {
                                                    auto* s = host.GetSettingsScreen();
                                                    if (!s) return;
                                                    s->ApplySettingChangeForTest("ui.screen_frame", ombra::JsonValue::MakeBool(true));
                                                    s->ApplySettingChangeForTest("ui.panel_position", ombra::JsonValue::MakeStr("bottom"));
                                                    s->ScrollForTest(scroll);
                                                });
        check(rc == 0, "snapshot: appearance with dependents rendered");
    }

    // Styling knobs at their far ends: round corners and no card outlines, then sharp corners.
    for (const char* corners : {"round", "sharp"}) {
        const std::wstring f = std::wstring(L"out\\snapshots\\knobs_") + (corners[0] == 'r' ? L"round_noborder" : L"sharp") + L".png";
        const bool borders = corners[0] != 'r';
        const int rc = ombra_ui::RunOverlayTest(f, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailAppearance, 1920, 1080,
                                                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 0,
                                                [corners, borders](ombra_ui::OverlayHost& host) {
                                                    const ombra::JsonValue c = ombra::JsonValue::MakeStr(corners);
                                                    const ombra::JsonValue b = ombra::JsonValue::MakeBool(borders);
                                                    host.GetTheme().ApplyUiConfig([&](const char* key) -> const ombra::JsonValue* {
                                                        if (std::string(key) == "ui.corners") return &c;
                                                        if (std::string(key) == "ui.card_borders") return &b;
                                                        return nullptr;
                                                    });
                                                });
        check(rc == 0, "snapshot: styling knobs rendered");
    }

    // Audio tab in Simple mode with a mic.
    {
        const int rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\dark_audio_simple.png", ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture,
                                                1920, 1080, ombra_ui::PanelPosition::Center,
                                                ombra_ui::PanelAttach::Floating, 1,
                                                [](ombra_ui::OverlayHost& host) {
                                                    if (auto* s = host.GetSettingsScreen()) s->SeedAudioForSnapshot(0.0f, false);
                                                });
        check(rc == 0, "snapshot: simple audio rendered");
    }

    // Audio tab with app tracks and a mixed track.
    for (const bool light : {false, true}) {
        const std::wstring f = light ? L"out\\snapshots\\light_audio_cards.png" : L"out\\snapshots\\dark_audio_cards.png";
        const int rc = ombra_ui::RunOverlayTest(f, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture, 1920, 1080,
                                                ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating, 1,
                                                [light](ombra_ui::OverlayHost& host) {
                                                    if (light) host.GetTheme().ApplyMode(ombra_ui::ThemeMode::Light);
                                                    if (auto* s = host.GetSettingsScreen()) s->SeedAudioForSnapshot(0.0f);
                                                });
        check(rc == 0, "snapshot: audio cards rendered");
    }

    // Same, scrolled to the bottom: Playing now and Saved audio.
    {
        const int rc = ombra_ui::RunOverlayTest(L"out\\snapshots\\dark_audio_live.png", ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture,
                                                1920, 1080, ombra_ui::PanelPosition::Center,
                                                ombra_ui::PanelAttach::Floating, 1,
                                                [](ombra_ui::OverlayHost& host) {
                                                    if (auto* s = host.GetSettingsScreen()) s->SeedAudioForSnapshot(330.0f);
                                                });
        check(rc == 0, "snapshot: per-app live sessions rendered");
    }

    // Memory card (seeded numbers): dark/light with a staged edit, plus the two placements that move
    // it -- rail on the right puts the card on the left, and an attached panel flush to a screen edge.
    {
        struct MemCase { const wchar_t* file; bool light; bool projection; ombra_ui::RailPosition rail;
                         ombra_ui::PanelPosition pos; ombra_ui::PanelAttach attach; };
        const MemCase cases[] = {
            {L"out\\snapshots\\dark_memory_card.png",        false, false, ombra_ui::RailPosition::Left,  ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating},
            {L"out\\snapshots\\dark_memory_card_staged.png", false, true,  ombra_ui::RailPosition::Left,  ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating},
            {L"out\\snapshots\\light_memory_card_staged.png", true, true,  ombra_ui::RailPosition::Left,  ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating},
            {L"out\\snapshots\\memory_card_rail_right.png",  false, true,  ombra_ui::RailPosition::Right, ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating},
            {L"out\\snapshots\\memory_card_rail_top.png",    false, true,  ombra_ui::RailPosition::Top,   ombra_ui::PanelPosition::Center, ombra_ui::PanelAttach::Floating},
            {L"out\\snapshots\\memory_card_attached_left.png", false, true, ombra_ui::RailPosition::Left, ombra_ui::PanelPosition::Left,   ombra_ui::PanelAttach::Attached},
        };
        for (const auto& c : cases) {
            const int rc = ombra_ui::RunOverlayTest(c.file, ombra_ui::ActiveScreen::Settings, ombra_ui::kRailReplay, 1920, 1080,
                                                    c.pos, c.attach, 0,
                                                    [&c](ombra_ui::OverlayHost& host) {
                                                        if (c.light) host.GetTheme().ApplyMode(ombra_ui::ThemeMode::Light);
                                                        host.GetTheme().rail_position = c.rail;
                                                        if (auto* s = host.GetSettingsScreen()) s->SeedMemoryForSnapshot(c.projection);
                                                    });
            check(rc == 0, "snapshot: memory card rendered");
        }
    }

    // Confirm the percentage-of-view sizing formula still looks right at both reference
    // resolutions, at the default (Center) position.
    {
        struct Resolution { const wchar_t* suffix; int w; int h; };
        const Resolution resolutions[] = { {L"1080p", 1920, 1080}, {L"4k", 3840, 2160} };
        for (const auto& res : resolutions) {
            const std::wstring path = L"out\\snapshots\\home_" + std::wstring(res.suffix) + L".png";
            const int rc = ombra_ui::RunOverlayTest(path, ombra_ui::ActiveScreen::Home, 0, res.w, res.h);
            char what[128];
            std::snprintf(what, sizeof(what), "snapshot: Home @ %ls rendered", res.suffix);
            check(rc == 0, what);
        }
    }
}

int RunSelfTest() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    };

    PipeClient client;
    SpawnedEngineGuard spawned_guard;  // terminates the engine on scope exit iff this run spawned it
    check(EnsureEngineRunningAndConnect(client, nullptr, &spawned_guard.pi),
          "connected to the engine (spawned if needed) and handshake succeeded");
    if (!client.Connected()) {
        std::printf("selftest: cannot continue without a connection\n");
        return 1;
    }

    JsonValue schema_req = JsonValue::MakeObject();
    schema_req.Set("type", JsonValue::MakeStr("get_schema"));
    const auto schema_resp = client.Request(schema_req, 3000);
    if (check(schema_resp.has_value(), "get_schema responded"); schema_resp) {
        const JsonValue* settings = schema_resp->Find("settings");
        check(settings && settings->type == JsonValue::Type::Array &&
                  settings->arr.size() == AllSettings().size(),
              "get_schema entry count matches AllSettings() ground truth");
    }

    JsonValue config_req = JsonValue::MakeObject();
    config_req.Set("type", JsonValue::MakeStr("get_config"));
    const auto config_resp = client.Request(config_req, 3000);
    if (config_resp) {
        const JsonValue* cfg_obj = config_resp->Find("config");
        const bool       obj_ok  = cfg_obj && cfg_obj->type == JsonValue::Type::Object;
        check(obj_ok, "get_config returned a config object");
        // This used to assert cfg_obj->obj.size() == (AllSettings() minus Raw), and failed on a
        // clean main-branch build for anyone whose real config.json has a Raw key set. The
        // response is neither of those counts: SerializeConfigFlat (config.cpp) emits Action-kind
        // keys along with the scalars, and the engine's get_config branch (ombra/main.cpp) then
        // merges every Raw key actually present in config.json on top. So the size is
        // non-Raw-settings + however many Raw keys this machine happens to have -- a number no
        // static count can predict -- but it is predictable from the *response*, so this asserts
        // two things: every non-Raw key is present, and the response holds exactly
        // (non-Raw settings) + (however many Raw keys it actually carries) entries. The second half
        // is not redundant: it keeps the duplicate-key/tearing detection the old count check had,
        // and it fails on a key the engine emits that AllSettings() doesn't declare at all (such a
        // key counts toward obj.size() but toward neither term).
        std::string missing_key;
        size_t       non_raw_expected = 0;
        size_t       raw_present      = 0;
        for (const auto& m : AllSettings()) {
            if (m.kind == SettingKind::Raw) {
                if (obj_ok && cfg_obj->Find(m.key)) ++raw_present;
                continue;
            }
            ++non_raw_expected;
            if (obj_ok && !cfg_obj->Find(m.key) && missing_key.empty()) missing_key = m.key;
        }
        check(obj_ok && missing_key.empty(),
              missing_key.empty() ? "get_config carries every non-raw AllSettings() key"
                                  : ("get_config is missing a non-raw key: " + missing_key).c_str());
        check(obj_ok && cfg_obj->obj.size() == non_raw_expected + raw_present,
              "get_config entry count == non-raw settings + the raw keys it carries");
    } else {
        check(false, "get_config responded");
    }

    // First-run marker detection, proven both ways (not just "reads whatever happens to be on
    // disk right now") via a temporary create/delete cycle, restored by the RAII guard below.
    {
        const std::wstring marker = ombra_ui::FirstRunMarkerPath();
        check(!marker.empty(), "first-run marker path resolves under the instance's config folder");
        FirstRunMarkerBackup backup(marker);
        DeleteFileW(marker.c_str());
        check(!ombra_ui::FirstRunComplete(), "FirstRunComplete() is false with the marker absent");
        ombra_ui::MarkFirstRunComplete();
        check(ombra_ui::FirstRunComplete(), "FirstRunComplete() is true after ombra_ui::MarkFirstRunComplete()");
    }


    // Test bar: headless — build both HomeScreen and SettingsScreen from real
    // get_schema/get_config responses; assert every wired non-Raw key produced exactly one control,
    // every group has a heading, hidden-key count matches AllSettings(), and action cards exist.
    {
        JsonValue schema_req2 = JsonValue::MakeObject();
        schema_req2.Set("type", JsonValue::MakeStr("get_schema"));
        const auto schema_resp2 = client.Request(schema_req2, 3000);
        JsonValue config_req2 = JsonValue::MakeObject();
        config_req2.Set("type", JsonValue::MakeStr("get_config"));
        const auto config_resp2 = client.Request(config_req2, 3000);
        check(schema_resp2.has_value() && config_resp2.has_value(),
              "screens: get_schema/get_config both responded");
        if (schema_resp2 && config_resp2) {
            const std::vector<ombra_ui::FieldSpec> specs = ombra_ui::ParseSchema(*schema_resp2);
            check(!specs.empty(), "screens: schema parsed into non-empty field specs");

            ombra_ui::OverlayHost host;
            auto* settings_screen = host.GetSettingsScreen();
            auto* home_screen = host.GetHomeScreen();

            check(settings_screen != nullptr, "screens: settings screen initialized");
            check(home_screen != nullptr, "screens: home screen initialized");

            if (settings_screen) {
                const JsonValue* config_obj = config_resp2->Find("config");
                settings_screen->InitSchema(specs, config_obj);
                check(settings_screen->Specs().size() == AllSettings().size(),
                      "screens: schema-driven specs cover every AllSettings() entry (implemented or not)");
                check(settings_screen->GroupHeadingCount() > 0,
                      "screens: group headings generated for active settings tier");

                // audio.sources' rows are
                // hand-built (BuildAudioSourceRows), not the generic one-row-per-spec path -- assert
                // they actually landed in Monitors & Audio's Audio sub-tab, since it sits behind a
                // sub-tab click in the fixed-size offscreen snapshot further down this function and
                // a silent zero-row group wouldn't show up there. Default config (no audio.sources
                // key) produces: the Desktop card (mode+target now live inline on it, not a separate
                // row), an "Add app" grid tile, an "Add microphone" grid tile (no mic entry yet), and
                // whatever the generic path adds for audio.bitrate_kbps (Int) in the same group.
                settings_screen->SetActiveRail(ombra_ui::kRailCapture);
                settings_screen->SetActiveSubTab(1);  // Audio
                check(settings_screen->RowCountForGroup(L"Audio") >= 3,
                      "screens: Monitors & Audio/Audio group has audio.sources' hand-built rows "
                      "(Desktop card + Add app/Add microphone tiles) alongside audio.bitrate_kbps");

                // Two hand-built pages: capture.monitors (key_panels_ dispatch) on Capture > Video,
                // and Advanced > Encoder's cards. A snapshot render succeeding proves nothing
                // about row *count*, only that nothing crashed. BuildDisplaysCard depends on a real
                // list_monitors round-trip to the spawned engine, which this machine always has at
                // least one display for.
                settings_screen->SetActiveSubTab(0);  // Video
                check(settings_screen->RowCountForGroup(L"Monitors") >= 1,
                      "screens: Monitors & Audio/Monitors group has capture.monitors' hand-built "
                      "rows (key_panels_ dispatch)");

                settings_screen->SetActiveRail(ombra_ui::kRailAdvanced);
                settings_screen->SetActiveSubTab(0);  // Encoder
                check(settings_screen->RowCountForGroup(L"Encoder") >= 1,
                      "screens: Advanced/Encoder group has its hand-built cards");
                settings_screen->SetActiveRail(ombra_ui::kRailGames);
                check(settings_screen->GamesListForTest() != nullptr,
                      "screens: Games page has its list (list_games against the spawned engine)");

                // Default config exercises only the "all" branch (target picker hidden on the
                // Desktop card, no mic entry) -- inject a synthetic audio.sources with mode="only" +
                // a mic source so the target-picker branch and the mic-present card actually run
                // once, not just compile. SetActiveRail(ombra_ui::kRailShortcuts) then back to Monitors & Audio forces
                // RebuildGroupRows to re-read the new value (it's a no-op if the index doesn't change).
                ombra::JsonValue only_source = ombra::JsonValue::MakeObject();
                only_source.Set("id", ombra::JsonValue::MakeStr("desktop"));
                only_source.Set("kind", ombra::JsonValue::MakeStr("output_loopback"));
                only_source.Set("device", ombra::JsonValue::MakeStr("default"));
                only_source.Set("mode", ombra::JsonValue::MakeStr("only"));
                only_source.Set("target", ombra::JsonValue::MakeStr("Discord.exe"));
                only_source.Set("enabled", ombra::JsonValue::MakeBool(true));
                ombra::JsonValue mic_source = ombra::JsonValue::MakeObject();
                mic_source.Set("id", ombra::JsonValue::MakeStr("mic"));
                mic_source.Set("kind", ombra::JsonValue::MakeStr("input"));
                mic_source.Set("device", ombra::JsonValue::MakeStr("default"));
                mic_source.Set("enabled", ombra::JsonValue::MakeBool(true));
                ombra::JsonValue sources_arr = ombra::JsonValue::MakeArray();
                sources_arr.arr.push_back(only_source);
                sources_arr.arr.push_back(mic_source);
                settings_screen->UpdateConfigValue("audio.sources", sources_arr);
                settings_screen->SetActiveRail(ombra_ui::kRailShortcuts);
                settings_screen->SetActiveRail(ombra_ui::kRailCapture);  // Monitors & Audio
                settings_screen->SetActiveSubTab(1);  // Audio
                check(settings_screen->RowCountForGroup(L"Audio") >= 4,
                      "screens: Monitors & Audio/Audio group renders the Desktop card's inline "
                      "target picker when mode=\"only\" (Desktop card + mic card + Add app tile + "
                      "audio.bitrate_kbps)");

                // Audio panel: a second output_loopback source (a per-app track, distinct
                // `id` from the primary) must render as its own row, not alias the primary's --
                // the exact bug class an earlier draft of BuildAudioSourceRows would have hit by
                // resolving sources by `kind` (ambiguous once two output_loopback entries exist)
                // instead of by `id`. Reset the primary to mode="all" (no target row) and add one
                // per-app source, so the row-count delta below is attributable only to the new
                // per-app row, not a stray target-picker row.
                ombra::JsonValue primary_all = ombra::JsonValue::MakeObject();
                primary_all.Set("id", ombra::JsonValue::MakeStr("desktop"));
                primary_all.Set("kind", ombra::JsonValue::MakeStr("output_loopback"));
                primary_all.Set("device", ombra::JsonValue::MakeStr("default"));
                primary_all.Set("mode", ombra::JsonValue::MakeStr("all"));
                primary_all.Set("target", ombra::JsonValue::MakeStr(""));
                primary_all.Set("enabled", ombra::JsonValue::MakeBool(true));
                ombra::JsonValue app_source = ombra::JsonValue::MakeObject();
                app_source.Set("id", ombra::JsonValue::MakeStr("only_Discord.exe"));
                app_source.Set("kind", ombra::JsonValue::MakeStr("output_loopback"));
                app_source.Set("device", ombra::JsonValue::MakeStr("default"));
                app_source.Set("mode", ombra::JsonValue::MakeStr("only"));
                app_source.Set("target", ombra::JsonValue::MakeStr("Discord.exe"));
                app_source.Set("enabled", ombra::JsonValue::MakeBool(true));
                ombra::JsonValue two_source_arr = ombra::JsonValue::MakeArray();
                two_source_arr.arr.push_back(primary_all);
                two_source_arr.arr.push_back(app_source);
                two_source_arr.arr.push_back(mic_source);
                // Simple mode (the default) hides real per-app cards -- these checks are about
                // per-app card/grouping behavior, so switch the config-backed mode on.
                settings_screen->UpdateConfigValue("audio.per_app_mode", ombra::JsonValue::MakeBool(true));
                settings_screen->UpdateConfigValue("audio.sources", two_source_arr);
                settings_screen->SetActiveRail(ombra_ui::kRailShortcuts);
                settings_screen->SetActiveRail(ombra_ui::kRailCapture);  // Monitors & Audio
                settings_screen->SetActiveSubTab(1);  // Audio
                check(settings_screen->AudioPerAppModeForTest(),
                      "screens: the Simple/Per-app toggle follows audio.per_app_mode from config");
                // Desktop, the Discord app and the mic: a track card each.
                check(settings_screen->AudioTrackTitlesForTest().size() == 3,
                      "screens: Capture > Audio shows a per-app source as a track of its own");

                // The app and the mic sharing a group_id are one track, so one card.
                ombra::JsonValue app_grouped = app_source;
                app_grouped.Set("group_id", ombra::JsonValue::MakeStr("only_Discord.exe"));
                ombra::JsonValue mic_grouped = mic_source;
                mic_grouped.Set("group_id", ombra::JsonValue::MakeStr("only_Discord.exe"));
                ombra::JsonValue grouped_arr = ombra::JsonValue::MakeArray();
                grouped_arr.arr.push_back(primary_all);
                grouped_arr.arr.push_back(app_grouped);
                grouped_arr.arr.push_back(mic_grouped);
                settings_screen->UpdateConfigValue("audio.sources", grouped_arr);
                settings_screen->SetActiveRail(ombra_ui::kRailShortcuts);
                settings_screen->SetActiveRail(ombra_ui::kRailCapture);  // Monitors & Audio
                settings_screen->SetActiveSubTab(1);  // Audio
                check(settings_screen->AudioTrackTitlesForTest().size() == 2,
                      "screens: two grouped audio sources are one track card");

                // Staging contract (audio_sources_dirty_, screens.h): audio edits used
                // to commit to the engine (and current_config_) immediately on every drag/volume
                // change; they now stage locally until Save is clicked. Verify the actual contract,
                // not just that BuildAudioSourceRows still compiles: a staged edit must not touch
                // current_config_, must survive a rebuild (rail round trip) instead of silently
                // reloading from config, and Discard must throw it away.
                check(!settings_screen->AudioSourcesDirtyForTest(),
                      "screens: audio panel starts clean (not dirty) before any staged edit");
                settings_screen->StageAudioGainForTest(6.0);
                check(settings_screen->AudioSourcesDirtyForTest(),
                      "screens: staging a gain change marks the audio panel dirty");
                check(settings_screen->CommittedDesktopGainForTest() == 0.0,
                      "screens: a staged gain change does not reach current_config_ before Save");
                settings_screen->SetActiveRail(ombra_ui::kRailShortcuts);
                settings_screen->SetActiveRail(ombra_ui::kRailCapture);  // Monitors & Audio
                settings_screen->SetActiveSubTab(1);  // Audio
                check(settings_screen->AudioSourcesDirtyForTest(),
                      "screens: a staged audio edit survives a rail/tab round trip instead of "
                      "BuildAudioSourceRows silently reloading it from current_config_");
                settings_screen->DiscardAudioSourcesForTest();
                check(!settings_screen->AudioSourcesDirtyForTest() &&
                      settings_screen->CommittedDesktopGainForTest() == 0.0,
                      "screens: Discard clears the staged edit and reverts to committed config");

                // Simple: the app source is hidden, so the mic is alone on its track.
                settings_screen->UpdateConfigValue("audio.per_app_mode", ombra::JsonValue::MakeBool(false));
                settings_screen->SetActiveRail(ombra_ui::kRailShortcuts);
                settings_screen->SetActiveRail(ombra_ui::kRailCapture);
                settings_screen->SetActiveSubTab(1);
                host.SetTestScreen(ombra_ui::ActiveScreen::Settings, ombra_ui::kRailCapture, 1);  // Capture > Audio
                settings_screen->Layout(D2D1::RectF(0, 0, 1000, 1100), 1.0f);
                check(settings_screen->AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop", L"Mic"},
                      "screens: Simple mode shows Desktop and Mic, the app hidden");
                const bool simple_snapshot_ok = host.RenderOffscreenTest(
                    1000, 1100, L"out\\snapshots\\audio_simple_hidden.png", nullptr);
                check(simple_snapshot_ok, "snapshot: Simple mode with hidden per-app + Mic rendered");

                settings_screen->SetActiveRail(ombra_ui::kRailShortcuts);  // back to General, the real default
            }

            if (home_screen) {
                home_screen->UpdateState(nullptr, &*config_resp2);
                check(home_screen->ActionCardCount() == 3,
                      "screens: home screen has exactly 3 quick action cards");
            }
        }
    }

    {
        const int overlay_rc = ombra_ui::RunOverlayTest(L"out\\ombra_overlay_test.png");
        check(overlay_rc == 0, "overlay host: DirectComposition/Direct2D offscreen spike render passes");
    }

    RenderSnapshots(check);

    client.Close();
    std::printf("\nselftest: %s (%d failure%s)\n", fails == 0 ? "PASSED" : "FAILED", fails,
                fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}

}  // namespace

// ombra-ui is WIN32-subsystem with Per-Monitor V2 DPI awareness.
int RunUi() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Only rebind to the console screen buffer when stdout/stderr aren't already pointing
    // somewhere (e.g. redirected to a file by a test script) -- freopen to CONOUT$
    // unconditionally would silently discard redirected output.
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }

    wchar_t*     cmdline = GetCommandLineW();
    int          argc    = 0;
    wchar_t**    argv    = CommandLineToArgvW(cmdline, &argc);

    // --drive is always an isolated instance: set before Log::Init, the first reader of OMBRA_HOME.
    const bool drive = argv && argc > 1 && std::wstring(argv[1]) == L"--drive";
    // Headless renders show end states: a snap right after a hover never catches a fade mid-way.
    if (drive || (argv && argc > 1 && (std::wstring(argv[1]) == L"--gallery" || std::wstring(argv[1]) == L"--snapshots")))
    {
        ombra_ui::motion::SetForcedOff(true);
        ombra_ui::SetSystemThemeLightProbe([] { return false; });
    }
#ifdef OMBRA_GALLERY_INC
    if (argv && argc > 1 && std::wstring(argv[1]) == L"--gallery") {
        const int rc = ombra_gallery::Run(argc, argv);
        LocalFree(argv);
        return rc;
    }
#endif
#ifdef OMBRA_PREVIEW_ONLY
    if (!drive) {
        LocalFree(argv);
        return 2;
    }
#endif
    if (drive) {
        const std::wstring home = ombra_ui::DriveHomeDir();
        SHCreateDirectoryExW(nullptr, home.c_str(), nullptr);
        SetEnvironmentVariableW(L"OMBRA_HOME", home.c_str());
    } else {
        // --drive keeps the meters still (its `fixture meters` sets them), so snapshots repeat.
        ombra_ui::SetLevelReaderFactory(ombra_ui::MakeWasapiLevelReader);
    }

    // Before the first line: toasts are their own processes (T), the rest is the overlay (U).
    const bool toast = argv && argc > 1 && std::wstring(argv[1]) == L"--toast";
    Log::SetProcessTag(toast ? 'T' : 'U');
    Log::SetThreadName("main");
    Log::Init();
    // Toasts start one process per save; their lines carry the pid already, a header each is noise.
    if (!toast) Log::Info("ombra-ui %s (%s) starting", ombra::AppVersion(), ombra::BuildCommit());
    ombra::crash::Install("ombra-ui");
    // Overlay and toasts share the name; whichever starts next reports it (the engine has telemetry
    // and prunes the dumps; the UI only logs).
    if (const auto prev = ombra::crash::TakePreviousCrash("ombra-ui"))
        Log::Info("previous ombra-ui run crashed: %s (dump %ls)", prev->reason.c_str(),
                  prev->dump_name.empty() ? L"none" : prev->dump_name.c_str());

    if (drive) {
        const int rc = ombra_ui::RunDrive(argc, argv);
        LocalFree(argv);
        return rc;
    }

    if (argv && argc > 1 && std::wstring(argv[1]) == L"--selftest") {
        const int rc = RunSelfTest();
        LocalFree(argv);
        return rc;
    }
    if (argv && argc > 1 && std::wstring(argv[1]) == L"--snapshots") {
        int fails = 0;
        ombra_ui::SetSoftwareRendering(true);
        RenderSnapshots([&](bool ok, const char* what) {
            std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
            if (!ok) ++fails;
        });
        LocalFree(argv);
        return fails == 0 ? 0 : 1;
    }
    if (argv && argc > 1 && std::wstring(argv[1]) == L"--overlaytest") {
        const std::wstring out_png = (argc > 2) ? argv[2] : L"out\\ombra_overlay_test.png";
        const int rc = ombra_ui::RunOverlayTest(out_png);
        LocalFree(argv);
        return rc;
    }
    if (argv && argc > 1 && std::wstring(argv[1]) == L"--toast") {
        const std::wstring event = (argc > 2) ? argv[2] : L"";
        // --toast save --wait <ok> <nothing_new> <failed> <engine> <elsewhere> <info>: handles the
        // engine made inheritable for this process (SpawnSaveToast, engine.cpp).
        std::vector<HANDLE> waits;
        HANDLE info = nullptr;
        const auto handle = [&](int i) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[i], nullptr, 10))); };
        if (argc == 10 && std::wstring(argv[3]) == L"--wait") {
            for (int i = 4; i < 9; ++i) waits.push_back(handle(i));
            info = handle(9);
        }
        const int rc = ombra_ui::RunToast(event, std::move(waits), info);
        LocalFree(argv);
        return rc;
    }
    if (argv && argc > 1 && std::wstring(argv[1]) == L"--toasttest") {
        const std::wstring out_png = (argc > 2) ? argv[2] : L"out\\ombra_toast_test.png";
        // --toasttest [PNG] [EVENT] [card|compact] [SECONDS] [alpha|opaque] [SCALE]
        const std::wstring event = (argc > 3) ? argv[3] : L"save_ok";
        const bool compact = argc > 4 && std::wstring(argv[4]) == L"compact";
        const double seconds = argc > 5 ? _wtof(argv[5]) : 0.0;
        const bool alpha = argc > 6 && std::wstring(argv[6]) == L"alpha";
        const float scale = argc > 7 ? std::clamp(static_cast<float>(_wtof(argv[7])), 0.5f, 8.0f) : 1.0f;
        const int rc = ombra_ui::RunToastTest(out_png, event, compact, seconds, alpha, scale);
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    PipeClient  client;
    std::string fatal_reason;
    if (!EnsureEngineRunningAndConnect(client, &fatal_reason)) {
        std::wstring msg = L"Could not start or connect to the " OMBRA_PRODUCT_NAME " engine.";
        if (!fatal_reason.empty()) {
            msg = L"The " OMBRA_PRODUCT_NAME " engine rejected the connection: " + WideFromUtf8(fatal_reason);
        } else {
            msg += L" See the log for details.";
        }
        MessageBoxW(nullptr, msg.c_str(), L"" OMBRA_PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    ombra_ui::OverlayHost overlay;
    const bool first_run = !ombra::Instance().isolated && !ombra_ui::FirstRunComplete();
    if (overlay.Show(&client, first_run ? ombra_ui::ActiveScreen::Setup : ombra_ui::ActiveScreen::Home)) {
        if (first_run) ombra_ui::NoteFirstRunShown();
        overlay.RunModalLoop();
    }

    client.Close();
    return 0;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return RunUi();
}
