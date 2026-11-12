// ombra-ui.exe --drive: the real overlay, headless and scriptable, so a UI change can be clicked
// through and looked at without anyone at the live overlay.
//
//   ombra-ui.exe --drive [--engine [--fresh] [--keep]] [--gpu] [--size WxH] [--dpi S] [--out DIR] [SCRIPT|-]
//
// OverlayHost::StartHeadless renders into an offscreen texture; input goes through the live
// window's own message handler; nothing appears on screen and nothing takes focus. The process is
// always an isolated instance (OMBRA_HOME = %TEMP%\ombra-drive\home, set in main.cpp before
// anything reads it). --engine starts an isolated ombra.exe there with a copy of the user's
// config.json (--fresh: defaults), so Save applies to the isolated config.
// Isolated engines show no tray icon, bind no hotkeys, spawn no toasts and run no Auto-Tune
// benchmark; this one's saves go to the sandbox (RedirectOutputs). It still captures the desktop
// and audio like the real one. --keep leaves it running for the next --drive run. Folder pickers
// and Explorer never open: the driver prints "blocked: would open ..." (SetNativeWindowSink).
// Without --engine: schema defaults, and every async list stays empty.
//
// A target is "text" (exact match, else prefix; "text"#2 = second match in draw order, default
// the top-most), key:<setting.key>, x,y, gear, back or close. Commands (one per line, # comments):
//   home | settings | clips | setup | rail NAME|N | tab N|"label"
//   seedclips N [SECONDS]: N short cuts of the user's newest clips into the sandbox's clips folder
//     (the only folder the Clips screen lists in an isolated instance); the originals are only read
//   clearclips | seedclip SRC GAME [START LEN] [AGE]: an exact grid (the demo video): SRC's cut into
//     clips\GAME, named and dated AGE ago (5m, 3h, 2d, 1d4h)
//   click T | rclick T | hover T | drag T T2|dx,dy [hold] | release | scroll T NOTCHES | key [ctrl+|alt+|shift+]NAME
//     (NAME: a letter, f1-f12, esc, enter, tab, space, arrows, home, end, pageup, comma, period, ...)
//     ("hold" keeps the button down so a snap shows the drag in progress; release lets go there)
//   type "text" | set KEY JSON | commit KEY JSON (set, as if saved; no engine) | wait MS | waitfor "text" [MS]
//   fixture settings [panel [game]]: seeds illustrative devices/memory, two displays and 60 fps, no engine
//     (panel: the AMD iGPU drives the display and records, not the RTX)
//   fixture windows [SAVED]: eight fake windows and One window, SAVED the saved one (chrome.exe)
//   fixture audio [perapp]: Desktop, Mic (off), three app tracks (two mixed), three apps playing
//   fixture setup: the setup screen with fixed sample facts (a dGPU and an iGPU, 1080p 120 Hz, 15.3 GB)
//   fixture update STATE [KIND]: the Updates card (Advanced > Diagnostics) in one update_status state
//   fixture monitors [plain]: three displays laid out as a desktop (one portrait, one left of the
//     primary), with synthetic captures unless `plain`
//   fixture meters: fixed levels for the Audio page built next (then `fixture audio`): Desktop
//     -12 dB, Discord -3 dB (red), League -30 dB, Spotify -44 dB; the off mic stays unlit
//   fixture games: 14 detected games (two switched off), two that looked like one, three added by hand
//   fixture home [on|off|waiting] [SECONDS] [game:NAME|window:NAME|displays:N]: Home with an engine's
//     status and config (hotkeys, folder, stats), recording that subject ("game:A B" for spaces)
//   fixture run GAME|"GAME"|- TARGET MODE [off]: the status Capture > Video reads (- = no game running)
//   fixture export PCT: the open clip shows a frame-exact save at PCT%
//   fixture sent [empty]: nine made-up records of what was sent (every kind; failed, unconfirmed)
//     in the sandbox's sent_reports, then Settings; `empty` leaves none
//   fixture ask: a made-up crash from before setup was finished, and Home asking whether to send it
//   snap NAME [card|T|x,y,w,h] [SCALE] | backdrop PNG|gray
//   text [FILTER] | lint | expect "text" | expect-not "text" | status | engine JSON | echo ...
// Snaps go to out\drive\NAME.png, composited opaque over the backdrop (default flat gray). Every
// snap reports text that overflows its box (lint lists overlaps too). Lint problems, failed expects
// and unknown commands fail the run. A snap reports bounds issues; use lint as an explicit gate.
// Text under a popup or clipped out by a scroll view (a card scrolled under the Save bar) is
// "hidden": `text` marks it, and find-by-text, overflow and overlap checks skip it.
#include "ombra-ui/overlay_window.h"
#include "ombra/brand.h"
#include "ombra-ui/pipe_client.h"
#include "ombra-ui/screens.h"
#include "ombra-ui/setup_screen.h"
#include "ombra-ui/ui/draw_probe.h"
#include "ombra-ui/ui/motion.h"

#include "ombra/clip_file.h"
#include "ombra/crash.h"
#include "ombra/json.h"
#include "ombra/paths.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ombra_ui {
namespace {

using Microsoft::WRL::ComPtr;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}

// Splits a command line into words; "quoted words" keep their spaces (\" escapes a quote). A
// quoted word keeps its quotes, so a target can tell "text" from key:/x,y.
std::vector<std::string> Words(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size() || line[i] == '#') break;
        std::string w;
        if (line[i] == '"') {
            w += line[i++];
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                w += line[i++];
            }
            w += '"';
            ++i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) w += line[i++];  // "x"#2
        } else if (line[i] == '{' || line[i] == '[') {
            w = line.substr(i);  // JSON runs to the end of the line
            i = line.size();
        } else {
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) w += line[i++];
        }
        out.push_back(w);
    }
    return out;
}

D2D1_POINT_2F Center(const D2D1_RECT_F& r) { return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f); }

std::string RectStr(const D2D1_RECT_F& r) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "@%.0f,%.0f %.0fx%.0f", r.left, r.top, r.right - r.left, r.bottom - r.top);
    return buf;
}

struct Options {
    bool         engine = false, fresh = false, keep = false, gpu = false;
    int          width = 1920, height = 1080;
    float        dpi = 1.0f;
    std::wstring out_dir = L"out\\drive";
    std::wstring script;  // empty or "-" = stdin
};

class Driver {
public:
    explicit Driver(Options opt) : opt_(std::move(opt)) {}
    ~Driver() { g_draw_probe = nullptr; }

    int Run() {
        // Folder pickers and Explorer would land on the user's screen: report them instead.
        SetNativeWindowSink([](const std::wstring& what) { std::printf("  blocked: would open %s\n", Narrow(what).c_str()); });
        if (opt_.engine && !StartEngine()) return 2;
        if (!host_.StartHeadless(opt_.width, opt_.height, opt_.dpi, opt_.engine ? &client_ : nullptr)) {
            std::printf("FAIL  could not start the headless host\n");
            return 2;
        }
        probe_ = std::make_unique<DrawProbe>(host_.GetTheme().dwrite.Get());
        g_draw_probe = probe_.get();
        Pump(opt_.engine ? 300 : 0);
        std::printf("drive: %dx%d at %.0f%%, %s\n", opt_.width, opt_.height, opt_.dpi * 100.0f,
                    opt_.engine ? "isolated engine" : "no engine (schema defaults)");

        std::istream* in = &std::cin;
        std::ifstream file;
        if (!opt_.script.empty() && opt_.script != L"-") {
            file.open(opt_.script);
            if (!file) {
                std::printf("FAIL  cannot open script\n");
                return 2;
            }
            in = &file;
        }
        std::string line;
        bool first = true;
        while (std::getline(*in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // PowerShell pipes a UTF-8 BOM, sometimes two.
            while (first && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
            first = false;
            const auto w = Words(line);
            if (w.empty()) continue;
            std::printf("> %s\n", line.c_str());
            Execute(w);
            std::fflush(stdout);
            if (!host_.HeadlessOpen()) {
                std::printf("  overlay closed (Esc or close); stopping\n");
                break;
            }
        }
        StopEngine();
        std::printf("drive: %d failure%s\n", fails_, fails_ == 1 ? "" : "s");
        return fails_;
    }

private:
    // ---- engine ----------------------------------------------------------------------------
    bool StartEngine() {
        const ombra::InstancePaths& sandbox = ombra::Instance();
        if (!sandbox.isolated) {
            std::printf("FAIL  not an isolated instance; refusing to start an engine\n");
            return false;
        }
        if (client_.Connect(300) && client_.Hello(1500)) {
            std::printf("drive: reusing the isolated engine already running\n");
            return RedirectOutputs();
        }
        CreateDirectoryW(sandbox.config_dir.c_str(), nullptr);
        const std::wstring dst = sandbox.config_dir + L"\\config.json";
        if (opt_.fresh) {
            DeleteFileW(dst.c_str());
        } else {
            const ombra::InstancePaths real = ombra::ResolveInstancePaths(L"");
            if (!real.config_dir.empty()) CopyFileW((real.config_dir + L"\\config.json").c_str(), dst.c_str(), FALSE);
        }
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring exe(self, n);
        exe.resize(exe.find_last_of(L"\\/") + 1);
        exe += L"" OMBRA_ENGINE_EXE;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        std::wstring cmd = L"\"" + exe + L"\"";
        // Null environment block: the child inherits OMBRA_HOME (paths.h).
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &si, &engine_pi_)) {
            std::printf("FAIL  could not start %s (err %lu)\n", Narrow(exe).c_str(), GetLastError());
            return false;
        }
        if (!(client_.Connect(20000) && client_.Hello(3000))) {
            DWORD code = 0;
            GetExitCodeProcess(engine_pi_.hProcess, &code);
            std::printf("FAIL  isolated engine did not answer (exit code 0x%08lX; logs in %s\\logs)\n", code,
                        Narrow(sandbox.data_dir).c_str());
            return false;
        }
        std::printf("drive: started isolated engine pid %lu (%s config)\n", engine_pi_.dwProcessId,
                    opt_.fresh ? "default" : "copy of the user's");
        return RedirectOutputs();
    }

    // The copied config points saves, screenshots and a disk-backed ring at the user's folders.
    // Repointed through the engine's own set_config before the UI loads, so nothing this instance
    // writes lands outside the sandbox.
    bool RedirectOutputs() {
        const std::wstring home = ombra::Instance().data_dir;
        ombra::JsonValue patch = ombra::JsonValue::MakeObject();
        patch.Set("output.root", ombra::JsonValue::MakeStr(Narrow(home + L"\\clips")));
        patch.Set("buffer.file_path", ombra::JsonValue::MakeStr(Narrow(home + L"\\ring.dat")));
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("set_config"));
        req.Set("config", std::move(patch));
        const auto resp = client_.Request(req, 10000);
        const ombra::JsonValue* ok = resp ? resp->Find("ok") : nullptr;
        if (!ok || !ok->b) {
            std::printf("FAIL  could not repoint the isolated engine's output folders: %s\n",
                        resp ? ombra::WriteJsonCompact(*resp).c_str() : "no reply");
            return false;
        }
        std::printf("drive: saves go to %s\\clips\n", Narrow(home).c_str());
        return true;
    }

    void StopEngine() {
        if (!opt_.engine) return;
        if (opt_.keep) {
            std::printf("drive: engine left running (--keep)\n");
        } else if (client_.Connected()) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("shutdown"));
            client_.Request(req, 3000);
            if (engine_pi_.hProcess && WaitForSingleObject(engine_pi_.hProcess, 8000) == WAIT_TIMEOUT)
                TerminateProcess(engine_pi_.hProcess, 1);
        }
        client_.Close();
        if (engine_pi_.hProcess) CloseHandle(engine_pi_.hProcess);
        if (engine_pi_.hThread) CloseHandle(engine_pi_.hThread);
    }

    // ---- frames and messages ---------------------------------------------------------------
    // Runs posted replies and timers for `ms`, then renders a fresh frame (and text record).
    void Pump(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0 || !host_.HeadlessOpen()) break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left), QS_ALLINPUT);
        }
        Frame();
    }

    void Frame() {
        probe_->Clear();
        host_.RenderForDriver();
    }

    void Send(UINT msg, WPARAM wp, D2D1_POINT_2F pt) {
        host_.DispatchForDriver(msg, wp, MAKELPARAM(static_cast<int>(pt.x), static_cast<int>(pt.y)));
    }
    void Move(D2D1_POINT_2F pt) { Send(WM_MOUSEMOVE, 0, pt); }

    // ---- targets ---------------------------------------------------------------------------
    struct Hit {
        D2D1_RECT_F rect{};
        std::string what;
    };

    bool InView(const D2D1_RECT_F& r) const {
        return r.right > r.left && r.bottom > r.top && r.right > 0 && r.bottom > 0 &&
               r.left < static_cast<float>(opt_.width) && r.top < static_cast<float>(opt_.height);
    }

    // Exact (case-insensitive, trimmed) matches, else prefix matches ("Browse" finds "Browse…").
    // Never a substring: "Save" must not land on "...hit the save hotkey".
    std::vector<const DrawnText*> FindText(const std::wstring& needle, bool* exact) {
        std::vector<const DrawnText*> eq, prefix;
        const std::wstring n = Lower(needle);
        for (const DrawnText& t : probe_->texts) {
            if (!InView(t.rect) || t.occluded) continue;
            const std::wstring s = Lower(Trim(t.text));
            if (s == n) eq.push_back(&t);
            else if (s.rfind(n, 0) == 0) prefix.push_back(&t);
        }
        *exact = !eq.empty();
        return eq.empty() ? prefix : eq;
    }

    std::optional<Hit> Target(const std::string& spec) {
        Frame();
        if (spec.size() >= 2 && spec[0] == '"') {
            const size_t close = spec.find('"', 1);
            const std::wstring needle = Widen(spec.substr(1, close - 1));
            int nth = 0;
            if (close + 1 < spec.size() && spec[close + 1] == '#') nth = std::atoi(spec.c_str() + close + 2);
            bool exact = false;
            const auto found = FindText(needle, &exact);
            if (found.empty()) {
                std::printf("  no text matching %s on screen\n", spec.c_str());
                return std::nullopt;
            }
            if (nth > static_cast<int>(found.size())) {
                std::printf("  only %zu match(es) for %s\n", found.size(), spec.c_str());
                return std::nullopt;
            }
            const DrawnText* t = nth > 0 ? found[static_cast<size_t>(nth - 1)] : found.back();
            Hit h{t->rect, "\"" + Narrow(t->text) + "\""};
            if (found.size() > 1 && nth == 0) h.what += " (top-most of " + std::to_string(found.size()) + (exact ? " exact)" : " prefix)");
            return h;
        }
        if (spec.rfind("key:", 0) == 0) {
            SettingsScreen* s = host_.GetSettingsScreen();
            auto w = s ? s->ControlForTest(spec.substr(4)) : nullptr;
            if (!w || !w->visible) {
                std::printf("  no visible control for %s on this page\n", spec.c_str());
                return std::nullopt;
            }
            // Page widgets keep unscrolled bounds; the scroll view offsets them when it draws.
            D2D1_RECT_F r = w->bounds;
            r.top -= s->ScrollYForTest();
            r.bottom -= s->ScrollYForTest();
            return Hit{r, spec};
        }
        if (spec == "gear" || spec == "back" || spec == "close") return Hit{host_.HeaderButtonBounds(spec == "close"), spec};
        float x = 0, y = 0, w = 0, h = 0;
        if (sscanf_s(spec.c_str(), "%f,%f,%f,%f", &x, &y, &w, &h) == 4) return Hit{D2D1::RectF(x, y, x + w, y + h), spec};
        if (sscanf_s(spec.c_str(), "%f,%f", &x, &y) == 2) return Hit{D2D1::RectF(x, y, x + 1, y + 1), spec};
        std::printf("  bad target %s\n", spec.c_str());
        return std::nullopt;
    }

    // ---- commands --------------------------------------------------------------------------
    void Fail(const char* why) {
        std::printf("  FAIL %s\n", why);
        ++fails_;
    }

    void Execute(const std::vector<std::string>& w) {
        const std::string& c = w[0];
        auto arg = [&](size_t i) -> std::string { return i < w.size() ? w[i] : std::string(); };

        if (c == "echo") return;
        if (c == "record") return arg(1) == "end" ? RecordEnd() : RecordStart(arg(1), arg(2));
        if (rec_) {
            if (c == "click") return RecClick(arg(1));
            if (c == "drag") return RecDrag(arg(1), arg(2));
            if (c == "glide") return RecGlide(arg(1), arg(2));
            if (c == "wait") return RecHold(std::atoi(arg(1).c_str()));
            if (c == "mark") {
                rec_marks_.push_back({arg(1), rec_frame_});
                return;
            }
            if (c == "rect") {
                auto h = Target(arg(2));
                if (!h) return Fail("rect target");
                rec_rects_.push_back({arg(1), h->rect});
                return;
            }
        }
        if (c == "cursor") {
            if (sscanf_s(arg(1).c_str(), "%f,%f", &cursor_.x, &cursor_.y) != 2) return Fail("cursor x,y");
            return;
        }
        if (c == "fixture") {
            if (opt_.engine) return Fail("fixtures require no engine");
            if (arg(1) == "home") return HomeFixture(arg(2), arg(3), Unquote(arg(4)));
            if (arg(1) == "run") return RunFixture(Unquote(arg(2)), arg(3), arg(4), arg(5));
            if (arg(1) == "export") {  // fixture export PCT: the open clip shows a frame-exact save at PCT
                ClipsScreen* cs = host_.GetClipsScreen();
                if (!cs || !cs->PlayerOpen()) return Fail("fixture export: open a clip first");
                cs->SetExportForTest(static_cast<uint32_t>(std::atoi(arg(2).c_str())), 100);
                return Settle();
            }
            if (arg(1) == "setup") {
                host_.SwitchScreen(ActiveScreen::Setup);
                ombra::SetupFacts facts;
                facts.display_hz = 120;
                facts.gpus = {{"nvidia", true, 1, 1}, {"amd", true, 64, 16}, {"cpu", false, 1, 1}};
                host_.GetSetupScreen()->SetFactsForTest(facts,
                    {{"nvidia", L"NVIDIA GeForce RTX 5060 Laptop GPU", true, 1, 1},
                     {"amd", L"AMD Radeon 780M Graphics", true, 64, 16},
                     {"cpu", L"CPU (x264)", false, 1, 1}},
                    L"NVIDIA GeForce RTX 5060 Laptop GPU", 15.3);
                return Settle();
            }
            if (arg(1) == "meters") {  // fixture meters: fixed levels for the next Audio page
                // Desktop in the yellow, Discord running hot (red), the off mic lit as if it heard.
                SetLevelReaderFactory([] {
                    return std::make_unique<FixedLevelReader>(std::map<std::string, float>{
                        {"desktop", 0.25f}, {"mic", 0.5f},
                        {"app:discord", 0.7f}, {"app:spotify", 0.006f}});
                });
                return Settle();
            }
            if (arg(1) == "sent") return SeedSent(arg(2) == "empty");
            if (arg(1) == "ask") return SeedCrashAsk();
            if (arg(1) != "settings" && arg(1) != "windows" && arg(1) != "audio" && arg(1) != "games" &&
                arg(1) != "update" && arg(1) != "monitors")
                return Fail("fixture: settings | windows [SAVED] | audio [perapp] | monitors [plain] | meters | games"
                            " | home [on|off|waiting] | setup | update STATE [KIND] | sent [empty] | ask");
            host_.SwitchScreen(ActiveScreen::Settings);
            auto* s = host_.GetSettingsScreen();
            if (!s) return Fail("no settings screen");
            if (arg(1) == "windows") {
                // Eight fake windows, One window chosen (committed), SAVED (default chrome.exe) saved.
                s->SeedWindowsForSnapshot(8, arg(2).empty() ? "chrome.exe" : arg(2).c_str());
                return Settle();
            }
            if (arg(1) == "audio") {
                s->SeedAudioForSnapshot(0.0f, arg(2) == "perapp");
                return Settle();
            }
            if (arg(1) == "monitors") {  // fixture monitors [plain]
                s->SeedMonitorsForSnapshot(3, true, arg(2) != "plain");
                return Settle();
            }
            if (arg(1) == "games") {
                s->SeedGamesForSnapshot();
                return Settle();
            }
            if (arg(1) == "update") {  // fixture update up_to_date|ready|available|failed|checking [installer|scoop|portable]
                s->SeedUpdateForSnapshot(arg(2).empty() ? "up_to_date" : arg(2), arg(3));
                return Settle();
            }
            // Reuse the snapshot seeds. Memory is illustrative, not an encode recommendation.
            s->SeedMemoryForSnapshot(true);
            s->SeedRecordingGpusForSnapshot(false, /*panel=*/arg(2) == "panel", /*game_on_rtx=*/arg(3) == "game");
            s->SeedSourceSizeForSnapshot(2560, 1440);
            s->SeedDisplayFpsForSnapshot(144);
            s->SeedMonitorsForSnapshot(2);
            s->ApplySettingChangeForTest("capture.framerate", ombra::JsonValue::MakeNum(60));
            return Settle();
        }
        if (c == "setup") {
            host_.SwitchScreen(ActiveScreen::Setup);
            return Settle();
        }
        if (c == "home" || c == "settings" || c == "clips") {
            host_.SwitchScreen(c == "home" ? ActiveScreen::Home : c == "settings" ? ActiveScreen::Settings : ActiveScreen::Clips);
            return Settle();
        }
        if (c == "seedclips") return SeedClips(std::atoi(arg(1).c_str()), arg(2).empty() ? 8.0 : std::atof(arg(2).c_str()));
        if (c == "clearclips") return ClearClips();
        if (c == "seedclip") return SeedClip(arg(1), arg(2), arg(3), arg(4), arg(5));
        if (c == "rail") return Rail(arg(1));
        if (c == "tab") {
            if (!arg(1).empty() && arg(1)[0] == '"') return Click(arg(1), WM_LBUTTONDOWN);
            host_.SwitchScreen(ActiveScreen::Settings);
            if (auto* s = host_.GetSettingsScreen()) s->SetActiveSubTab(std::atoi(arg(1).c_str()));
            return Settle();
        }
        if (c == "click") return Click(arg(1), WM_LBUTTONDOWN);
        if (c == "rclick") return Click(arg(1), WM_RBUTTONDOWN);
        if (c == "hover") {
            auto h = Target(arg(1));
            if (!h) return Fail("hover target");
            Move(Center(h->rect));
            Settle();
            std::printf("  hovering %s %s\n", h->what.c_str(), RectStr(h->rect).c_str());
            return;
        }
        if (c == "drag") return Drag(arg(1), arg(2), arg(3) == "hold");
        if (c == "release") {
            if (!held_) return Fail("release: no drag held");
            held_ = false;
            Send(WM_LBUTTONUP, 0, held_pt_);
            return Settle();
        }
        if (c == "scroll") {
            auto h = Target(arg(1));
            if (!h) return Fail("scroll target");
            Move(Center(h->rect));
            const int notches = std::atoi(arg(2).c_str());
            host_.DispatchForDriver(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(-notches * WHEEL_DELTA)), 0);
            return Settle();
        }
        if (c == "key") return Key(arg(1));
        if (c == "type") {
            const std::wstring text = Widen(arg(1).size() >= 2 ? arg(1).substr(1, arg(1).size() - 2) : arg(1));
            for (wchar_t ch : text) host_.DispatchForDriver(WM_CHAR, ch, 0);
            return Settle();
        }
        if (c == "set") {
            SettingsScreen* s = host_.GetSettingsScreen();
            try {
                if (s) s->ApplySettingChangeForTest(arg(1), ombra::ParseJson(arg(2)));
            } catch (const std::string& e) {
                return Fail(("bad JSON: " + e).c_str());
            }
            return Settle();
        }
        if (c == "commit") {  // as `set`, then as if saved: no Save bar (README stills, no engine)
            if (opt_.engine) return Fail("commit requires no engine; use set and Save");
            SettingsScreen* s = host_.GetSettingsScreen();
            try {
                const ombra::JsonValue v = ombra::ParseJson(arg(2));
                if (s) {
                    s->UpdateConfigValue(arg(1), v);
                    s->ApplySettingChangeForTest(arg(1), v);
                }
            } catch (const std::string& e) {
                return Fail(("bad JSON: " + e).c_str());
            }
            return Settle();
        }
        if (c == "wait") return Pump(std::atoi(arg(1).c_str()));
        if (c == "waitfor") return WaitFor(arg(1), arg(2).empty() ? 5000 : std::atoi(arg(2).c_str()));
        if (c == "snap") return Snap(arg(1), arg(2), arg(3));
        if (c == "backdrop") return Backdrop(arg(1));
        if (c == "text") return DumpText(arg(1));
        if (c == "lint") {
            if (Lint(true)) Fail("layout lint");
            return;
        }
        if (c == "expect" || c == "expect-not") {
            Frame();
            bool exact = false;
            const bool found = !FindText(Widen(arg(1).substr(1, arg(1).size() - 2)), &exact).empty();
            if (found == (c == "expect")) std::printf("  ok\n");
            else Fail(found ? "text is on screen" : "text not on screen");
            return;
        }
        if (c == "status") return Status();
        if (c == "engine") return EngineRequest(arg(1));
        Fail(("unknown command " + c).c_str());
    }

    // After an input: let posted replies land, then a fresh frame.
    void Settle() { Pump(opt_.engine ? 60 : 10); }

    void Click(const std::string& spec, UINT down) {
        auto h = Target(spec);
        if (!h) return Fail("click target");
        const D2D1_POINT_2F pt = Center(h->rect);
        Move(pt);
        Send(down, down == WM_LBUTTONDOWN ? MK_LBUTTON : MK_RBUTTON, pt);
        if (down == WM_LBUTTONDOWN && host_.HeadlessOpen()) Send(WM_LBUTTONUP, 0, pt);
        if (host_.HeadlessOpen()) Settle();
        std::printf("  clicked %s %s\n", h->what.c_str(), RectStr(h->rect).c_str());
    }

    bool held_ = false;
    D2D1_POINT_2F held_pt_{};

    void Drag(const std::string& from, const std::string& to, bool hold) {
        auto a = Target(from);
        if (!a) return Fail("drag source");
        const D2D1_POINT_2F p0 = Center(a->rect);
        D2D1_POINT_2F p1{};
        float dx = 0, dy = 0;
        if (!to.empty() && to[0] != '"' && to.rfind("key:", 0) != 0 && sscanf_s(to.c_str(), "%f,%f", &dx, &dy) == 2 &&
            to.find(',') != std::string::npos && (to[0] == '+' || to[0] == '-')) {
            p1 = D2D1::Point2F(p0.x + dx, p0.y + dy);
        } else {
            auto b = Target(to);
            if (!b) return Fail("drag destination");
            p1 = Center(b->rect);
        }
        Move(p0);
        Send(WM_LBUTTONDOWN, MK_LBUTTON, p0);
        for (int i = 1; i <= 10; ++i) {
            const float t = static_cast<float>(i) / 10.0f;
            Send(WM_MOUSEMOVE, MK_LBUTTON, D2D1::Point2F(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t));
        }
        if (hold) {
            held_ = true;
            held_pt_ = p1;
        } else {
            Send(WM_LBUTTONUP, 0, p1);
        }
        Settle();
        std::printf("  dragged %s to %.0f,%.0f%s\n", a->what.c_str(), p1.x, p1.y, hold ? " (held)" : "");
    }

    void Key(const std::string& spec) {
        std::string s = spec;
        std::vector<UINT> mods;
        for (;;) {
            if (s.rfind("ctrl+", 0) == 0) { mods.push_back(VK_CONTROL); s = s.substr(5); }
            else if (s.rfind("alt+", 0) == 0) { mods.push_back(VK_MENU); s = s.substr(4); }
            else if (s.rfind("shift+", 0) == 0) { mods.push_back(VK_SHIFT); s = s.substr(6); }
            else break;
        }
        static const std::pair<const char*, UINT> kNames[] = {
            {"esc", VK_ESCAPE}, {"enter", VK_RETURN}, {"tab", VK_TAB}, {"space", VK_SPACE},
            {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"up", VK_UP}, {"down", VK_DOWN},
            {"left", VK_LEFT}, {"right", VK_RIGHT}, {"home", VK_HOME}, {"end", VK_END},
            {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD}};
        UINT vk = 0;
        for (const auto& [name, code] : kNames)
            if (s == name) vk = code;
        if (!vk && s.size() >= 2 && (s[0] == 'f' || s[0] == 'F') && std::isdigit(static_cast<unsigned char>(s[1])))
            vk = VK_F1 + static_cast<UINT>(std::atoi(s.c_str() + 1)) - 1;
        if (!vk && s.size() == 1) vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(s[0])));
        if (!vk) return Fail("unknown key name");
        const bool alt = std::find(mods.begin(), mods.end(), static_cast<UINT>(VK_MENU)) != mods.end();
        const UINT down = alt ? WM_SYSKEYDOWN : WM_KEYDOWN, up = alt ? WM_SYSKEYUP : WM_KEYUP;
        for (UINT m : mods) host_.DispatchForDriver(down, m, 0);
        host_.DispatchForDriver(down, vk, 0);
        if (host_.HeadlessOpen()) host_.DispatchForDriver(up, vk, 0);
        for (auto it = mods.rbegin(); it != mods.rend() && host_.HeadlessOpen(); ++it) host_.DispatchForDriver(up, *it, 0);
        if (host_.HeadlessOpen()) Settle();
    }

    void Rail(const std::string& which) {
        // A rail label or a prefix of it; "monitors" is Capture's old name.
        int idx = -1;
        if (!which.empty() && std::isdigit(static_cast<unsigned char>(which[0]))) idx = std::atoi(which.c_str());
        else idx = SettingsRailByName(Widen(which == "monitors" ? std::string("capture") : which));
        if (idx < 0 || idx >= kRailCount) return Fail("rail: capture|replay|games|appearance|shortcuts|advanced or 0-5");
        host_.SwitchScreen(ActiveScreen::Settings);
        if (auto* s = host_.GetSettingsScreen()) s->SetActiveRail(idx);
        Settle();
    }

    // The user's output folder, read from their config (never written).
    static std::wstring OwnerClipsRoot() {
        const ombra::InstancePaths real = ombra::ResolveInstancePaths(L"");
        std::wstring root = L"%USERPROFILE%\\Videos\\" OMBRA_PRODUCT_NAME;
        std::ifstream f(real.config_dir + L"\\config.json", std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string text = ss.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);
            try {
                const ombra::JsonValue cfg = ombra::ParseJson(text);
                const ombra::JsonValue* out = cfg.Find("output");
                const ombra::JsonValue* r = out ? out->Find("root") : nullptr;
                if (r && r->type == ombra::JsonValue::Type::String) root = Widen(r->str);
            } catch (const std::string&) {
            }
        }
        wchar_t buf[MAX_PATH];
        return ExpandEnvironmentStringsW(root.c_str(), buf, MAX_PATH) ? std::wstring(buf) : root;
    }

    // Home with what an engine would report: switched on and keeping (on), switched off, or on with
    // nothing to record (waiting); real-length hotkeys, folder and stats line.
    // WHAT is what it records: game:NAME, window:NAME or displays:N (default one display).
    void HomeFixture(const std::string& state, const std::string& seconds, const std::string& what) {
        auto* home = host_.GetHomeScreen();
        if (!home) return Fail("no home screen");
        using ombra::JsonValue;
        const size_t colon = what.find(':');
        const std::string kind = what.substr(0, colon), name = colon == std::string::npos ? "" : what.substr(colon + 1);
        JsonValue status = JsonValue::MakeObject();
        status.Set("enabled", JsonValue::MakeBool(state != "off"));
        status.Set("armed", JsonValue::MakeBool(state.empty() || state == "on"));
        status.Set("target", JsonValue::MakeStr(kind == "game" || kind == "window" ? kind : "monitor"));
        if (kind == "game") status.Set("current_game", JsonValue::MakeStr(name));
        status.Set("buffer_fill_pct", JsonValue::MakeNum(state == "off" ? 0 : 100));
        status.Set("rss_private_mb", JsonValue::MakeNum(412));
        status.Set("encoder_backend", JsonValue::MakeStr("NVENC"));
        // SECONDS given (the demo video): that buffer length and a same-GPU encode, no "(cross-GPU)".
        status.Set("gpu_topology", JsonValue::MakeStr(seconds.empty() ? "T2" : "T1"));
        JsonValue cfg = JsonValue::MakeObject();
        cfg.Set("buffer.seconds", JsonValue::MakeNum(seconds.empty() ? 120 : (std::max)(1, std::atoi(seconds.c_str()))));
        cfg.Set("hotkeys.save_replay", JsonValue::MakeStr("Alt+F10"));
        cfg.Set("hotkeys.toggle_buffer", JsonValue::MakeStr("Ctrl+Shift+F9"));
        cfg.Set("output.root", JsonValue::MakeStr("D:\\Recordings\\Ombra Clips"));
        if (kind == "window") cfg.Set("capture.target_window", JsonValue::MakeStr(name));
        if (kind == "displays") {
            JsonValue monitors = JsonValue::MakeArray();
            for (int i = 0; i < std::atoi(name.c_str()); ++i) monitors.arr.push_back(JsonValue::MakeStr("\\\\.\\DISPLAY" + std::to_string(i + 1)));
            cfg.Set("capture.monitors", std::move(monitors));
        }
        JsonValue config = JsonValue::MakeObject();
        config.Set("config", std::move(cfg));
        host_.SwitchScreen(ActiveScreen::Home);
        home->UpdateState(&status, &config);
        Settle();
    }

    // What the engine reports running, for Settings: GAME (- for none), TARGET, MODE, and "off".
    void RunFixture(const std::string& game, const std::string& target, const std::string& mode, const std::string& off) {
        auto* settings = host_.GetSettingsScreen();
        if (!settings) return Fail("no settings screen");
        using ombra::JsonValue;
        JsonValue status = JsonValue::MakeObject();
        if (game != "-") status.Set("current_game", JsonValue::MakeStr(game));
        status.Set("target", JsonValue::MakeStr(target));
        status.Set("mode", JsonValue::MakeStr(mode));
        status.Set("enabled", JsonValue::MakeBool(off != "off"));
        settings->SeedRunStatusForSnapshot(status);
        Settle();
    }

    void SeedClips(int count, double seconds) {
        if (count <= 0) return Fail("seedclips N [SECONDS]");
        const std::wstring dst_dir = ombra::Instance().data_dir + L"\\clips";
        CreateDirectoryW(ombra::Instance().data_dir.c_str(), nullptr);
        CreateDirectoryW(dst_dir.c_str(), nullptr);
        namespace fs = std::filesystem;
        std::vector<std::pair<fs::file_time_type, fs::path>> found;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(OwnerClipsRoot(), ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == L".mp4") found.push_back({it->last_write_time(ec), it->path()});
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int made = 0;
        for (const auto& [when, src] : found) {
            if (made >= count) break;
            const fs::path rel = fs::relative(src, OwnerClipsRoot(), ec);
            const fs::path dst = fs::path(dst_dir) / (ec ? src.filename() : rel);
            fs::create_directories(dst.parent_path(), ec);
            ombra::ClipInfo info;
            if (!ombra::ReadClipInfo(src.wstring(), info)) continue;
            const double start = (std::max)(0.0, info.duration_s * 0.5 - seconds * 0.5);
            const ombra::TrimResult r = ombra::TrimClip(src.wstring(), dst.wstring(), start, start + seconds);
            if (!r.ok) continue;
            std::error_code tec;
            fs::last_write_time(dst, when, tec);  // keep the original's age for the grid's dates
            std::printf("  seeded %s (%.1f s)\n", Narrow(dst.wstring()).c_str(), r.end_s - r.start_s);
            ++made;
        }
        if (made == 0) Fail("no clips seeded (none found, or none readable)");
    }

    // ---- record mode (the demo video) ------------------------------------------------------
    // `record DIR [FPS]` films what follows: every frame goes to DIR\frames\NNNNNN.png (the
    // overlay alone, premultiplied BGRA, so a compositor lays it over gameplay) and `record end`
    // writes DIR\track.json: {fps, width, height, dpi, frames:[[x, y, pressed]...] (the cursor,
    // which the compositor draws), marks:{name: frame}, rects:{name: [x, y, w, h]}}, in pixels.
    // A virtual clock (1/FPS a frame) drives the fades, which run here though --drive forces
    // them off elsewhere. Real-time work (thumbnails, a clip loading) gets real time before a
    // frame is written, with the virtual clock stopped, so no frame shows a half-loaded screen.
    static inline double g_vclock_ms = 0;
    static double VClock() { return g_vclock_ms; }

    bool                       rec_ = false;
    std::wstring               rec_dir_;
    int                        rec_fps_ = 60;
    int                        rec_frame_ = 0;
    bool                       rec_pressed_ = false;
    D2D1_POINT_2F              cursor_{-1, -1};
    std::vector<std::array<float, 3>> rec_track_;
    std::vector<std::pair<std::string, int>> rec_marks_;
    std::vector<std::pair<std::string, D2D1_RECT_F>> rec_rects_;

    void RecordStart(const std::string& dir, const std::string& fps) {
        if (rec_) return Fail("already recording");
        if (dir.empty()) return Fail("record DIR [FPS]");
        rec_dir_ = Widen(Unquote(dir));
        rec_fps_ = fps.empty() ? 60 : (std::clamp)(std::atoi(fps.c_str()), 1, 240);
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(rec_dir_) / L"frames", ec);
        std::filesystem::create_directories(std::filesystem::path(rec_dir_) / L"frames", ec);
        if (ec) return Fail("cannot create the record directory");
        rec_ = true;
        rec_frame_ = 0;
        rec_track_.clear();
        rec_marks_.clear();
        rec_rects_.clear();
        if (cursor_.x < 0) cursor_ = D2D1::Point2F(opt_.width * 0.5f, opt_.height * 0.62f);
        g_vclock_ms = motion::NowMs();
        motion::SetClockForTest(&VClock);
        motion::SetForcedOff(false);
        motion::SetForcedOn(true);
        std::printf("  recording to %s at %d fps\n", Narrow(rec_dir_).c_str(), rec_fps_);
    }

    void RecordEnd() {
        if (!rec_) return Fail("not recording");
        rec_ = false;
        motion::SetForcedOn(false);
        motion::SetForcedOff(true);
        motion::SetClockForTest(nullptr);
        std::ofstream f(std::filesystem::path(rec_dir_) / L"track.json", std::ios::binary);
        f << "{\"fps\":" << rec_fps_ << ",\"width\":" << opt_.width << ",\"height\":" << opt_.height << ",\"dpi\":" << opt_.dpi
          << ",\"frames\":[";
        for (size_t i = 0; i < rec_track_.size(); ++i)
            f << (i ? "," : "") << "[" << rec_track_[i][0] << "," << rec_track_[i][1] << "," << rec_track_[i][2] << "]";
        f << "],\"marks\":{";
        for (size_t i = 0; i < rec_marks_.size(); ++i) f << (i ? "," : "") << "\"" << rec_marks_[i].first << "\":" << rec_marks_[i].second;
        f << "},\"rects\":{";
        for (size_t i = 0; i < rec_rects_.size(); ++i) {
            const D2D1_RECT_F& r = rec_rects_[i].second;
            f << (i ? "," : "") << "\"" << rec_rects_[i].first << "\":[" << r.left << "," << r.top << "," << r.right - r.left << ","
              << r.bottom - r.top << "]";
        }
        f << "}}\n";
        if (!f) return Fail("track.json write");
        std::printf("  recorded %d frames (%.2f s)\n", rec_frame_, rec_frame_ / static_cast<double>(rec_fps_));
    }

    bool SettledForRecord() {
        if (host_.ActiveScreenForDriver() != ActiveScreen::Clips) return true;
        ClipsScreen* cs = host_.GetClipsScreen();
        return !cs || cs->SettledForDriver();
    }

    // One filmed frame: real time for whatever is loading (virtual clock stopped), then the frame.
    void RecTick() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        Pump(4);
        while (!SettledForRecord() && std::chrono::steady_clock::now() < until && host_.HeadlessOpen()) Pump(20);
        g_vclock_ms += 1000.0 / rec_fps_;
        Frame();
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!host_.ReadFrame(px, w, h)) return Fail("frame readback");
        wchar_t name[32];
        swprintf_s(name, L"%06d.png", rec_frame_);
        if (!WritePng((std::filesystem::path(rec_dir_) / L"frames" / name).wstring(), px, w, h)) return Fail("frame write");
        rec_track_.push_back({cursor_.x, cursor_.y, rec_pressed_ ? 1.0f : 0.0f});
        ++rec_frame_;
    }

    bool WritePng(const std::wstring& path, const std::vector<uint8_t>& px, int w, int h) {
        IWICImagingFactory* wic = Wic();
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        return wic && SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
               SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
               SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
               SUCCEEDED(fr->WritePixels(h, w * 4, static_cast<UINT>(px.size()), const_cast<uint8_t*>(px.data()))) &&
               SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }

    void RecHold(int ms) {
        const int n = (std::max)(1, static_cast<int>(std::lround(ms * rec_fps_ / 1000.0)));
        for (int i = 0; i < n && host_.HeadlessOpen(); ++i) RecTick();
    }

    // The cursor eases to `to` over `ms` (default from the distance), a mouse move every frame so
    // hovers fade in and out as it passes.
    void GlideTo(D2D1_POINT_2F to, int ms) {
        const float dist = std::hypot(to.x - cursor_.x, to.y - cursor_.y) / opt_.dpi;
        if (ms <= 0) ms = static_cast<int>((std::clamp)(220.0f + dist * 0.45f, 260.0f, 700.0f));
        const D2D1_POINT_2F from = cursor_;
        const int n = (std::max)(1, static_cast<int>(std::lround(ms * rec_fps_ / 1000.0)));
        for (int i = 1; i <= n && host_.HeadlessOpen(); ++i) {
            const float u = static_cast<float>(i) / n;
            const float e = u < 0.5f ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3.0f) / 2;  // ease in-out cubic
            cursor_ = D2D1::Point2F(from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e);
            Send(WM_MOUSEMOVE, rec_pressed_ ? MK_LBUTTON : 0, cursor_);
            RecTick();
        }
    }

    void RecGlide(const std::string& target, const std::string& ms) {
        auto h = Target(target);
        if (!h) return Fail("glide target");
        GlideTo(Center(h->rect), std::atoi(ms.c_str()));
    }

    void RecClick(const std::string& target) {
        auto h = Target(target);
        if (!h) return Fail("click target");
        GlideTo(Center(h->rect), 0);
        RecHold(60);
        rec_pressed_ = true;
        Send(WM_LBUTTONDOWN, MK_LBUTTON, cursor_);
        RecHold(90);
        rec_pressed_ = false;
        if (host_.HeadlessOpen()) Send(WM_LBUTTONUP, 0, cursor_);
        if (host_.HeadlessOpen()) RecHold(30);
        std::printf("  clicked %s %s\n", h->what.c_str(), RectStr(h->rect).c_str());
    }

    // drag T T2|+dx,dy: glide to T, press, glide (button down) to T2, let go.
    void RecDrag(const std::string& from, const std::string& to) {
        auto a = Target(from);
        if (!a) return Fail("drag source");
        GlideTo(Center(a->rect), 0);
        const D2D1_POINT_2F p0 = cursor_;
        D2D1_POINT_2F p1{};
        float dx = 0, dy = 0;
        if (!to.empty() && (to[0] == '+' || to[0] == '-') && sscanf_s(to.c_str(), "%f,%f", &dx, &dy) == 2) {
            p1 = D2D1::Point2F(p0.x + dx * opt_.dpi, p0.y + dy * opt_.dpi);
        } else {
            auto b = Target(to);
            if (!b) return Fail("drag destination");
            p1 = Center(b->rect);
        }
        RecHold(80);
        rec_pressed_ = true;
        Send(WM_LBUTTONDOWN, MK_LBUTTON, cursor_);
        RecHold(100);
        GlideTo(p1, 0);
        RecHold(120);
        rec_pressed_ = false;
        if (host_.HeadlessOpen()) Send(WM_LBUTTONUP, 0, cursor_);
        if (host_.HeadlessOpen()) RecHold(30);
        std::printf("  dragged %s to %.0f,%.0f\n", a->what.c_str(), p1.x, p1.y);
    }

    static std::string Unquote(const std::string& s) {
        return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
    }

    // Empties the sandbox's clips folder (the only one an isolated Clips screen lists).
    void ClearClips() {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::path(ombra::Instance().data_dir) / L"clips";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (ec) return Fail("cannot reset the sandbox's clips folder");
        std::printf("  cleared %s\n", Narrow(dir.wstring()).c_str());
    }

    // fixture sent [empty]: made-up records in the sandbox's sent_reports\ (every kind; sent, failed
    // and unconfirmed), dated minutes to days ago; `empty` leaves the folder empty.
    void SeedSent(bool empty) {
        namespace fs = std::filesystem;
        namespace sr = ombra::sent_records;
        if (!ombra::Instance().isolated) return Fail("fixture sent: not an isolated instance");
        std::error_code ec;
        const fs::path dir = sr::Dir();
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (ec) return Fail("cannot reset the sandbox's sent_reports folder");
        struct Rec { const wchar_t* name; std::string text; const char* result; int64_t age_s; };
        std::string log;
        const char* lines[] = {
            "[12:00:00.000] [INFO ] [E 4242 main] ombra 0.1.0 (abc1234) starting, pid 4242, OS build 26200",
            "[12:00:00.500] [INFO ] [E 4242 main] encode adapter  NVIDIA GeForce RTX 4070 (NVENC)",
            "[12:00:00.750] [INFO ] [E 4242 main] capture target  monitor 2560x1440",
            "[12:00:01.000] [INFO ] [E 4242 main] buffer controller  when=desktop  what=monitor  games=5",
            "[12:20:00.000] [INFO ] [E 4242 watchdog] ring  612 MB committed of 640 MB",
            "[12:30:00.000] [INFO ] [E 4242 save] saved 60 s to Desktop\\Desktop 2026-11-09 12-30-00.mp4",
            "[12:45:00.000] [ERROR] [E 4242 main] FATAL unhandled exception: access violation (0xC0000005)"};
        for (int i = 0; i < 6; ++i)
            for (const char* l : lines) log += std::string(l) + "\n";
        const std::string crash_event =
            "{\"event_id\":\"00000000feedface00000000feedface\",\"timestamp\":1794200000,\"platform\":\"native\",\"level\":\"fatal\","
            "\"release\":\"ombra@0.1.0+abc1234\",\"tags\":{\"process\":\"ombra\",\"crash_kind\":\"unhandled exception\"},"
            "\"extra\":{\"reason\":\"access violation (0xC0000005) at 00007FF700001000, writing 0000000000000000\",\"thread\":\"main\","
            "\"system\":\"Ombra 0.1.0\\nWindows build 26200\\nGPU 0: NVIDIA GeForce RTX 4070\"}}";
        const auto error_event = [](const char* message) {
            return std::string("{\"event_id\":\"0a1b2c3d\",\"level\":\"error\",\"message\":\"") + message +
                   "\",\"release\":\"ombra@0.1.0+abc1234\",\"tags\":{\"process\":\"ombra\"}}";
        };
        const Rec recs[] = {
            {L"4d5e6f708192a3b4c5d6e7f801234567.txt", std::string(sr::kLogsTitle) + "4d5e6f708192a3b4c5d6e7f801234567\n" +
                 sr::kLogsSizeLabel + "14 files, 2.3 MB\n\nFiles in send-logs-4d5e6f70.zip:\nREADME.txt\nsystem.txt\n"
                 "logs/ombra-20261110.log\ntelemetry/samples-20261110.csv\n\nFeedback:\n"
                 "{\"contexts\":{\"feedback\":{\"message\":\"Logs sent from Settings\"}}}\n", "HTTP 200", 3 * 60},
            {L"usage-day-20261110.txt", std::string(sr::kDailyTitle) + "\n\n{\"items\":[{\"body\":\"daily usage\",\"attributes\":{}}]}\n", "HTTP 200", 10 * 60},
            {L"usage-games-20261110.txt", std::string(sr::kGamesTitle) +
                 "\n\n{\"items\":[{\"attributes\":{\"ombra.game_exe\":{\"value\":\"eldenring.exe\"}}},"
                 "{\"attributes\":{\"ombra.game_exe\":{\"value\":\"Cyberpunk2077.exe\"}}}]}\n", "HTTP 200", 11 * 60},
            {L"session-20261110-1794300000.txt", std::string(sr::kSessionTitle) +
                 "2026-11-10 (UTC)\n\n{\"aggregates\":[{\"started\":\"2026-11-10T00:00:00Z\",\"exited\":1}],\"attrs\":{\"release\":\"ombra@0.1.0\"}}\n",
                 "HTTP 200", 12 * 60},
            {L"5e6f7a8b.txt", std::string(sr::kErrorTitle) + "5e6f7a8b\n\nEvent:\n" + error_event("save: save folder can't take the clip (no_room)") +
                 "\n\nLog lines:\n" + log + "\n", "HTTP 200", 26 * 3600},
            {L"00000000feedface00000000feedface.txt", std::string(sr::kCrashTitle) + "00000000feedface00000000feedface\n\nMinidump: "
                 "ombra-20261109-120000-4242.dmp (98304 bytes, a memory snapshot; not copied here)\n\nEvent:\n" + crash_event +
                 "\n\nLog lines:\n" + log + "\n", "WinHTTP error 12007", 2 * 86400},
            {L"9c0d1e2f.txt", std::string(sr::kErrorTitle) + "9c0d1e2f\n\nEvent:\n" +
                 error_event("encoder: pipeline fault, rebuilding (nvEncEncodePicture:8)") + "\n\nLog lines:\n" + log + "\n", nullptr, 3 * 86400},
            {L"3a4b5c6d.txt", std::string(sr::kErrorTitle) + "3a4b5c6d\n\nEvent:\n" +
                 error_event("capture: the window closed while recording (window_gone)") + "\n\nLog lines:\n" + log + "\n", "HTTP 413", 4 * 86400},
            {L"7e8f9a0b.txt", std::string(sr::kErrorTitle) + "7e8f9a0b\n\nEvent:\n" +
                 error_event("audio: the default output device changed while a save was reading the ring, and the new "
                             "device's mix format could not be matched (format_mismatch)") +
                 "\n\nLog lines:\n" + log + "\n", "HTTP 429", 5 * 86400},
        };
        if (!empty) {
            FILETIME now_ft;
            GetSystemTimeAsFileTime(&now_ft);
            const ULONGLONG now = (ULONGLONG{now_ft.dwHighDateTime} << 32) | now_ft.dwLowDateTime;
            for (const Rec& r : recs) {
                const std::wstring path = (dir / r.name).wstring();
                const std::string text = r.text + (r.result ? std::string(sr::kResultPrefix) + r.result + "\n" : std::string());
                const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f == INVALID_HANDLE_VALUE) return Fail("cannot write a sent record");
                DWORD wrote = 0;
                WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
                const ULONGLONG t = now - static_cast<ULONGLONG>(r.age_s) * 10000000ull;
                const FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
                SetFileTime(f, nullptr, nullptr, &ft);
                CloseHandle(f);
            }
        }
        std::printf("  %s %s\n", empty ? "emptied" : "seeded 9 records in", Narrow(dir.wstring()).c_str());
        host_.SwitchScreen(ActiveScreen::Settings);
        if (auto* s = host_.GetSettingsScreen()) s->ReloadSentRecords();
        Settle();
    }

    // fixture ask: a made-up crash from before setup was finished in the sandbox's crashes\, then
    // Home with the question over it (crash.h UnansweredCrash). Its answer is written there too.
    void SeedCrashAsk() {
        if (!ombra::Instance().isolated) return Fail("fixture ask: not an isolated instance");
        const std::wstring dir = ombra::crash::CrashesDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring base = dir + L"\\ombra-20261110-120000-4242";
        for (const wchar_t* ext : {L".sent", L".asked"}) DeleteFileW((base + ext).c_str());
        const std::pair<const wchar_t*, std::string> files[] = {
            {L".dmp", std::string(64, '\0')},
            {L".txt", "exe: ombra\nshare: ask\nreason: access violation (0xC0000005)\n"}};
        for (const auto& [ext, text] : files) {
            const HANDLE f = CreateFileW((base + ext).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return Fail("cannot write the crash fixture");
            DWORD wrote = 0;
            WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
            CloseHandle(f);
        }
        host_.SwitchScreen(ActiveScreen::Home);
        host_.ShowCrashAsk();
        Settle();
    }

    // seedclip SRC GAME [START LEN] [AGE]: [START, START+LEN) of SRC (a stream copy from the keyframe
    // at or before START; LEN 0 = to the end) into clips\GAME as "GAME <date>.mp4", dated AGE ago
    // ("0", "5m", "3h", "2d", "1d4h"), so a grid shows chosen clips under real names in a chosen order.
    void SeedClip(const std::string& src_s, const std::string& game_s, const std::string& start_s, const std::string& len_s,
                  const std::string& age_s) {
        namespace fs = std::filesystem;
        const std::wstring src = Widen(Unquote(src_s)), game = Widen(Unquote(game_s));
        if (src.empty() || game.empty()) return Fail("seedclip SRC GAME [START LEN] [AGE]");
        double age_s_total = 0;
        for (size_t i = 0; i < age_s.size();) {
            size_t used = 0;
            double v = 0;
            try { v = std::stod(age_s.substr(i), &used); } catch (...) { return Fail("AGE: like 5m, 3h, 2d, 1d4h"); }
            i += used;
            const char u = i < age_s.size() ? age_s[i++] : 's';
            age_s_total += v * (u == 'd' ? 86400 : u == 'h' ? 3600 : u == 'm' ? 60 : 1);
        }
        ombra::ClipInfo info;
        std::string err;
        if (!ombra::ReadClipInfo(src, info, &err)) return Fail(("seedclip: " + err).c_str());
        const double start = start_s.empty() ? 0.0 : std::atof(start_s.c_str());
        const double len = len_s.empty() ? 0.0 : std::atof(len_s.c_str());
        const double end = len > 0 ? start + len : info.video_offset_s + info.duration_s;

        // The file's date is its name's date, as a real save's is.
        const auto when = std::chrono::system_clock::now() - std::chrono::seconds(static_cast<long long>(age_s_total));
        const std::time_t tt = std::chrono::system_clock::to_time_t(when);
        std::tm lt{};
        localtime_s(&lt, &tt);
        wchar_t stamp[32];
        wcsftime(stamp, 32, L"%Y-%m-%d %H-%M-%S", &lt);
        std::error_code ec;
        const fs::path dir = fs::path(ombra::Instance().data_dir) / L"clips" / game;
        fs::create_directories(dir, ec);
        const fs::path dst = dir / (game + L" " + stamp + L".mp4");
        const ombra::TrimResult r = ombra::TrimClip(src, dst.wstring(), start, end);
        if (!r.ok) return Fail(("seedclip: " + r.error).c_str());
        fs::last_write_time(dst, std::chrono::clock_cast<fs::file_time_type::clock>(when), ec);
        std::printf("  seeded %s (%.1f s)\n", Narrow(dst.wstring()).c_str(), r.end_s - r.start_s);
    }

    void WaitFor(const std::string& spec, int ms) {
        const std::wstring needle = Widen(spec.size() >= 2 ? spec.substr(1, spec.size() - 2) : spec);
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            Pump(50);
            bool exact = false;
            if (!FindText(needle, &exact).empty()) {
                const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                std::printf("  appeared after %lld ms\n", static_cast<long long>(took));
                return;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return Fail("timed out");
        }
    }

    void Status() {
        Frame();
        const ActiveScreen s = host_.ActiveScreenForDriver();
        std::printf("  screen %s%s%s, %zu text items\n",
                    s == ActiveScreen::Home ? "home" : s == ActiveScreen::Settings ? "settings" : s == ActiveScreen::Setup ? "setup" : "clips",
                    host_.ModalOpen() ? ", modal open" : "", host_.PopupOpen() ? ", popup open" : "",
                    probe_->texts.size());
    }

    void EngineRequest(const std::string& json) {
        if (!client_.Connected()) return Fail("no engine (run with --engine)");
        try {
            auto resp = client_.Request(ombra::ParseJson(json), 5000);
            std::string out = resp ? ombra::WriteJsonCompact(*resp) : std::string("(no reply)");
            if (out.size() > 3000) out = out.substr(0, 3000) + "...";
            std::printf("  %s\n", out.c_str());
        } catch (const std::string& e) {
            Fail(("bad JSON: " + e).c_str());
        }
    }

    // ---- text and lint ---------------------------------------------------------------------
    static std::string Flags(const DrawnText& t) {
        std::string f;
        if (t.trimmed) f += " trimmed";
        if (t.overflow_x) f += " OVERFLOW-X";
        if (t.overflow_y) f += " OVERFLOW-Y";
        if (t.occluded) f += " hidden";
        return f;
    }

    void DumpText(const std::string& filter) {
        Frame();
        const std::wstring f = Lower(Widen(filter.size() >= 2 && filter[0] == '"' ? filter.substr(1, filter.size() - 2) : filter));
        for (const DrawnText& t : probe_->texts) {
            if (!InView(t.rect) || Trim(t.text).empty()) continue;
            if (!f.empty() && Lower(t.text).find(f) == std::wstring::npos) continue;
            std::printf("  \"%s\" %s%s\n", Narrow(t.text).c_str(), RectStr(t.rect).c_str(), Flags(t).c_str());
        }
    }

    // Returns the problem count. Overlap: two different strings whose boxes' ink areas intersect.
    int Lint(bool verbose) {
        Frame();
        int problems = 0;
        std::vector<const DrawnText*> shown;
        for (const DrawnText& t : probe_->texts)
            if (InView(t.rect) && !t.occluded && !Trim(t.text).empty()) shown.push_back(&t);
        for (const DrawnText* t : shown) {
            if (!t->overflow_x && !t->overflow_y) continue;
            ++problems;
            if (verbose || problems <= 5)
                std::printf("  overflow \"%s\" %s text %.0fx%.0f, %u line%s%s\n", Narrow(t->text).c_str(),
                            RectStr(t->rect).c_str(), t->text_w, t->text_h, t->lines, t->lines == 1 ? "" : "s",
                            Flags(*t).c_str());
        }
        if (verbose) {
            // Ink boxes from the measured size; alignment unknown, so anchored where the box says
            // most text sits (left/top). Only flags clear overlaps between different strings.
            for (size_t i = 0; i < shown.size(); ++i) {
                for (size_t j = i + 1; j < shown.size(); ++j) {
                    const DrawnText& a = *shown[i];
                    const DrawnText& b = *shown[j];
                    if (a.text == b.text || a.text_w <= 0 || b.text_w <= 0) continue;
                    auto ink = [](const DrawnText& t) {
                        return D2D1::RectF(t.rect.left, t.rect.top, t.rect.left + (std::min)(t.text_w, t.rect.right - t.rect.left),
                                           t.rect.top + (std::min)(t.text_h, t.rect.bottom - t.rect.top));
                    };
                    const D2D1_RECT_F ra = ink(a), rb = ink(b);
                    const float iw = (std::min)(ra.right, rb.right) - (std::max)(ra.left, rb.left);
                    const float ih = (std::min)(ra.bottom, rb.bottom) - (std::max)(ra.top, rb.top);
                    if (iw > 2.0f && ih > 4.0f) {
                        ++problems;
                        std::printf("  overlap \"%s\" %s with \"%s\" %s\n", Narrow(a.text).c_str(), RectStr(ra).c_str(),
                                    Narrow(b.text).c_str(), RectStr(rb).c_str());
                    }
                }
            }
            std::printf("  %d problem%s\n", problems, problems == 1 ? "" : "s");
        }
        return problems;
    }

    // ---- images ----------------------------------------------------------------------------
    IWICImagingFactory* Wic() {
        if (!wic_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
        return wic_.Get();
    }

    void Backdrop(const std::string& what) {
        backdrop_.clear();
        if (what.empty() || what == "gray") return;
        IWICImagingFactory* wic = Wic();
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> conv;
        const std::wstring path = Widen(what[0] == '"' ? what.substr(1, what.size() - 2) : what);
        if (!wic || FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) ||
            FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), opt_.width, opt_.height, WICBitmapInterpolationModeFant)) ||
            FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            return Fail("cannot load backdrop image");
        }
        backdrop_.resize(static_cast<size_t>(opt_.width) * opt_.height * 4);
        conv->CopyPixels(nullptr, opt_.width * 4, static_cast<UINT>(backdrop_.size()), backdrop_.data());
        std::printf("  backdrop %s\n", what.c_str());
    }

    void Snap(const std::string& name, const std::string& region, const std::string& scale_s) {
        if (name.empty()) return Fail("snap needs a name");
        const int problems = Lint(false);
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!host_.ReadFrame(px, w, h)) return Fail("frame readback");
        // Premultiplied overlay over an opaque backdrop: out = src + dst * (1 - a).
        for (size_t i = 0; i < px.size(); i += 4) {
            const uint32_t a = px[i + 3];
            for (int c = 0; c < 3; ++c) {
                const uint32_t dst = backdrop_.empty() ? 72u : backdrop_[i + c];
                px[i + c] = static_cast<uint8_t>((std::min)(255u, px[i + c] + dst * (255u - a) / 255u));
            }
            px[i + 3] = 255;
        }

        D2D1_RECT_F crop = D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h));
        float scale = 1.0f;
        std::string region_spec = region;
        if (!region.empty() && std::isdigit(static_cast<unsigned char>(region[0])) && region.find(',') == std::string::npos) {
            scale = static_cast<float>(std::atof(region.c_str()));  // snap NAME SCALE
            region_spec.clear();
        } else if (!scale_s.empty()) {
            scale = static_cast<float>(std::atof(scale_s.c_str()));
        }
        if (region_spec == "card") {
            if (SettingsScreen* s = host_.GetSettingsScreen(); s && host_.ActiveScreenForDriver() == ActiveScreen::Settings) {
                crop = s->CardBoundsForTest();
                crop.left -= 16; crop.top -= 16; crop.right += 16; crop.bottom += 16;
            }
        } else if (!region_spec.empty() && region_spec != "full") {
            auto t = Target(region_spec);
            if (!t) return Fail("snap region");
            crop = t->rect;
            if (region_spec.find(',') == std::string::npos || region_spec[0] == '"') {
                crop.left -= 24; crop.top -= 24; crop.right += 24; crop.bottom += 24;
            }
        }
        const int x0 = (std::max)(0, static_cast<int>(crop.left)), y0 = (std::max)(0, static_cast<int>(crop.top));
        const int x1 = (std::min)(w, static_cast<int>(crop.right)), y1 = (std::min)(h, static_cast<int>(crop.bottom));
        if (x1 <= x0 || y1 <= y0) return Fail("empty snap region");
        if (scale <= 0.0f || scale > 4.0f) scale = 1.0f;

        std::error_code directory_error;
        std::filesystem::create_directories(opt_.out_dir, directory_error);
        if (directory_error) return Fail("cannot create output directory");
        const std::wstring path = opt_.out_dir + L"\\" + Widen(name) + L".png";
        const int cw = x1 - x0, ch = y1 - y0;
        const UINT ow = (std::max)(1u, static_cast<UINT>(cw * scale)), oh = (std::max)(1u, static_cast<UINT>(ch * scale));
        IWICImagingFactory* wic = Wic();
        ComPtr<IWICBitmap> bmp;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        const bool ok = wic &&
            SUCCEEDED(wic->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppBGRA, w * 4, static_cast<UINT>(w * 4 * ch),
                                                  px.data() + (static_cast<size_t>(y0) * w + x0) * 4, &bmp)) &&
            SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(bmp.Get(), ow, oh, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
            SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(ow, oh)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
            SUCCEEDED(fr->WriteSource(scaler.Get(), nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        if (!ok) return Fail("PNG write");
        std::printf("  wrote %s (%ux%u)%s\n", Narrow(path).c_str(), ow, oh,
                    problems ? (", " + std::to_string(problems) + " overflowing text item(s), run lint").c_str() : "");
    }

    Options                    opt_;
    OverlayHost                host_;
    PipeClient                 client_;
    PROCESS_INFORMATION        engine_pi_{};
    std::unique_ptr<DrawProbe> probe_;
    ComPtr<IWICImagingFactory> wic_;
    std::vector<uint8_t>       backdrop_;
    int                        fails_ = 0;
};

}  // namespace

std::wstring DriveHomeDir() {
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp, n) + L"ombra-drive\\home";
}

int RunDrive(int argc, wchar_t** argv) {
    Options opt;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--engine") opt.engine = true;
        else if (a == L"--fresh") opt.fresh = true;
        else if (a == L"--keep") opt.keep = true;
        else if (a == L"--gpu") opt.gpu = true;
        else if (a == L"--size" && i + 1 < argc) {
            wchar_t extra;
            if (swscanf_s(argv[++i], L"%dx%d%c", &opt.width, &opt.height, &extra, 1u) != 2) {
                std::printf("FAIL  size must be WxH\n");
                return 2;
            }
        } else if (a == L"--dpi" && i + 1 < argc) {
            wchar_t* end = nullptr;
            opt.dpi = std::wcstof(argv[++i], &end);
            if (end == argv[i] || *end) {
                std::printf("FAIL  DPI must be a number\n");
                return 2;
            }
        }
        else if (a == L"--out" && i + 1 < argc) opt.out_dir = argv[++i];
        else if (a.starts_with(L"--") || !opt.script.empty()) {
            std::printf("FAIL  unknown/incomplete option or extra script\n");
            return 2;
        } else opt.script = a;
    }
#ifdef OMBRA_PREVIEW_ONLY
    if (opt.engine || opt.keep || opt.fresh || opt.gpu) {
        std::printf("FAIL  playground requires backend-free WARP rendering\n");
        return 2;
    }
#endif
    if (opt.width <= 0 || opt.height <= 0 || opt.width > 16384 || opt.height > 16384 ||
        !std::isfinite(opt.dpi) || opt.dpi < 0.5f || opt.dpi > 4.0f) {
        std::printf("FAIL  invalid viewport/DPI\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // No engine, no GPU: WARP, like --snapshots. --gpu draws on the hardware adapter instead, which
    // the Clips preview needs (WARP has no video decode).
    if (!opt.engine && !opt.gpu) SetSoftwareRendering(true);
    int rc;
    {
        Driver d(opt);
        rc = d.Run();
        // `--drive --gpu` hung at exit at least once after a clip was opened (2026-11-06; 38 runs
        // later it wouldn't reproduce). If teardown stalls, dump every thread's stack and exit, so a
        // scripted caller (the demo renderer) isn't blocked and the dump in the sandbox's crashes
        // folder shows where it stuck. ExitProcess kills this thread on a normal exit.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::printf("FAIL  exit hung for 10 s; dump in %s\n", Narrow(ombra::crash::CrashesDir()).c_str());
            std::fflush(stdout);
            ombra::crash::Fatal("--drive: exit hung for 10 s");
        }).detach();
    }
    return rc;
}

}  // namespace ombra_ui
