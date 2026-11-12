// Hotkey specs (hotkey_spec.h), set_config's hotkey validation (config.cpp) and the engine's live
// rebind/suspend path (HotkeyRegistry, hotkeys.h). Headless, no GPU.
//
// Registry tests run under the polling policy with Ctrl+Alt+Shift+F2x combos: polling claims nothing
// system-wide (no RegisterHotKey, no hook), and nobody presses those while the suite runs.

#include <doctest/doctest.h>

#include "ombra/config.h"
#include "ombra/hotkey_spec.h"
#include "ombra/hotkeys.h"
#include "ombra/json.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

TEST_CASE("hotkey spec: parse is case-insensitive and order-free; format is canonical") {
    HotkeyCombo a, b;
    REQUIRE(ParseHotkeySpec("alt+f10", a));
    REQUIRE(ParseHotkeySpec("Alt+F10", b));
    CHECK(a == b);
    CHECK(a.mods == MOD_ALT);
    CHECK(a.vk == VK_F10);

    HotkeyCombo c;
    REQUIRE(ParseHotkeySpec("shift+win+ctrl+alt+pageup", c));
    CHECK(FormatHotkeySpec(c) == "Ctrl+Alt+Shift+Win+PageUp");

    HotkeyCombo n;
    REQUIRE(ParseHotkeySpec("Ctrl+Num7", n));
    CHECK(n.vk == VK_NUMPAD7);
    CHECK(FormatHotkeySpec(n) == "Ctrl+Num7");
}

TEST_CASE("hotkey spec: rejects what it can't bind") {
    HotkeyCombo c;
    CHECK_FALSE(ParseHotkeySpec("", c));
    CHECK_FALSE(ParseHotkeySpec("Alt+", c));
    CHECK_FALSE(ParseHotkeySpec("Alt+Alt+X", c));
    CHECK_FALSE(ParseHotkeySpec("Hyper+X", c));
    CHECK_FALSE(ParseHotkeySpec("Alt+F25", c));
    CHECK_FALSE(ParseHotkeySpec("Alt+`", c));  // layout-dependent, deliberately unsupported
    CHECK(FormatHotkeySpec(HotkeyCombo{MOD_ALT, VK_OEM_3}).empty());
}

TEST_CASE("hotkey spec: a global hotkey needs Ctrl, Alt or Win") {
    CHECK(ValidateHotkeySpec("Alt+F10") == HotkeyProblem::None);
    CHECK(ValidateHotkeySpec("Win+F9") == HotkeyProblem::None);
    CHECK(ValidateHotkeySpec("A") == HotkeyProblem::NeedsModifier);
    CHECK(ValidateHotkeySpec("F12") == HotkeyProblem::NeedsModifier);
    // Shift alone would eat every capital letter under RegisterHotKey.
    CHECK(ValidateHotkeySpec("Shift+A") == HotkeyProblem::NeedsModifier);
    // Macro keys nobody types may stand alone.
    CHECK(ValidateHotkeySpec("F13") == HotkeyProblem::None);
    CHECK(ValidateHotkeySpec("Shift+F24") == HotkeyProblem::None);
    CHECK(ValidateHotkeySpec("Alt+F4") == HotkeyProblem::Reserved);
    CHECK(ValidateHotkeySpec("Ctrl+Alt+F4") == HotkeyProblem::None);
    CHECK(ValidateHotkeySpec("nonsense") == HotkeyProblem::Unparseable);
}

namespace {

JsonValue Patch(const char* key, const char* value) {
    JsonValue o = JsonValue::MakeObject();
    o.Set(key, JsonValue::MakeStr(value));
    return o;
}

bool Rejected(const ConfigPatchResult& r, const std::string& key) {
    for (const auto& issue : r.rejected) {
        if (issue.key == key) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("config: a hotkey patch is canonicalised, and a bad or duplicate one is refused") {
    Config cfg;
    REQUIRE(cfg.hotkeys_save_replay == "Alt+F10");

    // Toggle's default is Alt+F9: an invalid Toggle goes back to it, so Save can't keep it too.
    {
        Config clash;
        const std::string toggle = clash.hotkeys_toggle_buffer, save = clash.hotkeys_save_replay;
        JsonValue patch = JsonValue::MakeObject();
        patch.Set("hotkeys.save_replay", JsonValue::MakeStr(toggle));
        patch.Set("hotkeys.toggle_buffer", JsonValue::MakeStr("invalid"));
        ApplyConfigPatch(clash, patch);
        CHECK(clash.hotkeys_toggle_buffer == toggle);
        CHECK(clash.hotkeys_save_replay == save);
    }

    auto r = ApplyConfigPatch(cfg, Patch("hotkeys.save_replay", "ctrl+shift+s"));
    CHECK_FALSE(Rejected(r, "hotkeys.save_replay"));
    CHECK(cfg.hotkeys_save_replay == "Ctrl+Shift+S");

    r = ApplyConfigPatch(cfg, Patch("hotkeys.save_replay", "S"));
    CHECK(Rejected(r, "hotkeys.save_replay"));
    CHECK(cfg.hotkeys_save_replay == "Ctrl+Shift+S");

    // Same combo as the screenshot hotkey, spelled differently.
    r = ApplyConfigPatch(cfg, Patch("hotkeys.save_replay", "alt+f1"));
    CHECK(Rejected(r, "hotkeys.save_replay"));
    CHECK(cfg.hotkeys_save_replay == "Ctrl+Shift+S");

    // A swap in one patch is fine: the result has no duplicate.
    JsonValue swap = JsonValue::MakeObject();
    swap.Set("hotkeys.screenshot", JsonValue::MakeStr("Alt+F9"));
    swap.Set("hotkeys.toggle_buffer", JsonValue::MakeStr("Alt+F1"));
    r = ApplyConfigPatch(cfg, swap);
    CHECK(r.rejected.empty());
    CHECK(cfg.hotkeys_screenshot == "Alt+F9");
    CHECK(cfg.hotkeys_toggle_buffer == "Alt+F1");
}

namespace {

std::vector<HotkeyActionSpec> NoopActions() {
    return {
        {1, "hotkeys.save_replay", "Ctrl+Alt+Shift+F20", [] {}},
        {2, "hotkeys.screenshot", "Ctrl+Alt+Shift+F21", [] {}},
        {3, "hotkeys.toggle_buffer", "Ctrl+Alt+Shift+F22", [] {}},
        {4, "hotkeys.overlay", "Ctrl+Alt+Shift+F23", [] {}},
    };
}

const HotkeyBindingStatus* Find(const HotkeyStatus& s, const std::string& key) {
    for (const auto& b : s.bindings) {
        if (b.key == key) return &b;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("hotkey registry: live rebind, suspend and resume under polling") {
    HotkeyRegistry reg(NoopActions(), HotkeyPolicy::kPolling);
    reg.ApplyPending();
    HotkeyStatus s = reg.Status();
    CHECK(s.policy == "poll");
    CHECK_FALSE(s.suspended);
    REQUIRE(s.bindings.size() == 4);
    for (const auto& b : s.bindings) {
        CHECK(b.ok);
        CHECK(b.backend == "poll");
    }

    reg.RequestBindings(HotkeyPolicy::kPolling, {{"hotkeys.save_replay", "ctrl+alt+shift+f19"}});
    reg.ApplyPending();
    s = reg.Status();
    REQUIRE(Find(s, "hotkeys.save_replay"));
    CHECK(Find(s, "hotkeys.save_replay")->spec == "Ctrl+Alt+Shift+F19");
    CHECK(Find(s, "hotkeys.overlay")->spec == "Ctrl+Alt+Shift+F23");

    // Unparseable falls back to that action's default instead of leaving it unbound.
    reg.RequestBindings(HotkeyPolicy::kPolling, {{"hotkeys.overlay", "garbage"}});
    reg.ApplyPending();
    CHECK(Find(reg.Status(), "hotkeys.overlay")->spec == "Ctrl+Alt+Shift+F23");
    CHECK(Find(reg.Status(), "hotkeys.overlay")->ok);

    reg.RequestSuspend(true);
    reg.ApplyPending();
    s = reg.Status();
    CHECK(s.suspended);
    CHECK(s.bindings.empty());

    // A rebind while suspended waits for the resume.
    reg.RequestBindings(HotkeyPolicy::kPolling, {{"hotkeys.screenshot", "Ctrl+Alt+Shift+F18"}});
    reg.ApplyPending();
    CHECK(reg.Status().suspended);

    reg.RequestSuspend(false);
    reg.ApplyPending();
    s = reg.Status();
    CHECK_FALSE(s.suspended);
    REQUIRE(s.bindings.size() == 4);
    CHECK(Find(s, "hotkeys.screenshot")->spec == "Ctrl+Alt+Shift+F18");
    CHECK(Find(s, "hotkeys.save_replay")->spec == "Ctrl+Alt+Shift+F19");
}

namespace {

// A real TrackPopupMenu on a separate desktop, so nothing shows or takes focus.
// WM_HOTKEY is posted rather than pressed: posting to the window is what RegisterHotKey(hwnd) does.
struct MenuRun {
    HotkeyRegistry* reg = nullptr;
    bool in_menu = false, entered = false;
    int  window_hotkeys_in_menu = 0, thread_hotkeys = 0;
    DWORD tid = 0;
};
MenuRun* g_menu_run = nullptr;

LRESULT CALLBACK MenuTestWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MenuRun* run = g_menu_run;
    if (msg == WM_ENTERMENULOOP) {
        run->in_menu = run->entered = true;
        PostMessageW(hwnd, WM_HOTKEY, 1, 0);          // what the engine does now
        PostThreadMessageW(run->tid, WM_HOTKEY, 2, 0);  // what it did: the menu's loop drops this
        SetTimer(hwnd, 7, 100, nullptr);
        return 0;
    }
    if (msg == WM_TIMER && wparam == 7) {
        KillTimer(hwnd, 7);
        EndMenu();
        return 0;
    }
    if (msg == WM_EXITMENULOOP) run->in_menu = false;
    if (run && run->reg && run->reg->HandleWindowMessage(hwnd, msg, wparam, lparam)) return 0;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RunMenuOnOwnDesktop(MenuRun* run, bool* registered) {
    run->tid = GetCurrentThreadId();
    HDESK original = GetThreadDesktop(run->tid);
    HDESK desk =CreateDesktopW(L"ombra-tests-menu", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desk || !SetThreadDesktop(desk)) return;
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = MenuTestWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OmbraTestsMenuOwner";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                    wc.hInstance, nullptr);

        std::vector<HotkeyActionSpec> actions = NoopActions();
        actions[0].action = [run] { if (run->in_menu) run->window_hotkeys_in_menu++; };
        actions[1].action = [run] { run->thread_hotkeys++; };
        HotkeyRegistry reg(std::move(actions), HotkeyPolicy::kRegisterHotKey);
        reg.SetNotifyWindow(hwnd);
        reg.ApplyPending();
        *registered = reg.Status().bindings.size() == 4 && reg.Status().bindings[0].ok;
        run->reg = &reg;

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Item");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, 0, 0, 0, hwnd, nullptr);
        DestroyMenu(menu);

        // The engine's own pump after the menu: a dropped thread message must not still be queued.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) reg.DispatchWmHotkey(msg.wParam);
            else DispatchMessageW(&msg);
        }
        run->reg = nullptr;
        reg.Shutdown();
        DestroyWindow(hwnd);
    }
    SetThreadDesktop(original);
    CloseDesktop(desk);
}

}  // namespace

TEST_CASE("hotkey registry: a press during the tray menu's modal loop still fires") {
    MenuRun run;
    bool    registered = false;
    g_menu_run = &run;
    std::thread(RunMenuOnOwnDesktop, &run, &registered).join();
    g_menu_run = nullptr;
    REQUIRE(run.entered);  // the menu's loop really ran
    CHECK(registered);
    CHECK(run.window_hotkeys_in_menu == 1);
    CHECK(run.thread_hotkeys == 0);
}

namespace {
std::atomic<bool> g_fake_combo_down{false};
std::atomic<int>  g_fake_combo_polls{0};  // one per pass of the poll loop: it reads the key once

SHORT FakeKeyState(int vk) {
    if (vk == VK_F20) ++g_fake_combo_polls;
    return g_fake_combo_down.load() && (vk == VK_MENU || vk == VK_F20) ? static_cast<SHORT>(0x8000) : 0;
}

// The pump loop's job, until `done` or a timeout far beyond any scheduling delay.
template <typename Done>
void PumpUntil(HotkeyManager& manager, Done done) {
    const ULONGLONG end = GetTickCount64() + 10000;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) manager.DispatchWmHotkey(msg.wParam);
        }
        if (done()) return;
        Sleep(5);
    }
}

// Waits out `polls` more passes of the poll loop. The first may have read the key before the
// caller changed it, so a pass that is sure to see a change needs polls >= 2.
void PumpPolls(HotkeyManager& manager, int polls) {
    const int target = g_fake_combo_polls.load() + polls;
    PumpUntil(manager, [&] { return g_fake_combo_polls.load() >= target; });
}
}  // namespace

// The action touches state only the pump thread owns (Tray's UI process handle), so a polled press
// must not run it on the poll thread.
TEST_CASE("hotkey manager: a polled press runs on the pump thread, once per press") {
    MSG unused;
    PeekMessageW(&unused, nullptr, 0, 0, PM_NOREMOVE);  // this thread gets its message queue
    HotkeyManager::SetKeyStateForTest(&FakeKeyState);
    g_fake_combo_down = false;
    {
        int   fired = 0;
        DWORD fired_on = 0;
        HotkeyManager manager(HotkeyPolicy::kPolling);
        REQUIRE(manager.Register(9, HotkeyBinding{"test", MOD_ALT, VK_F20, [&] {
            ++fired;
            fired_on = GetCurrentThreadId();
        }}).via_polling);

        g_fake_combo_down = true;
        PumpUntil(manager, [&] { return fired == 1; });
        PumpPolls(manager, 10);  // held throughout: no repeat
        CHECK(fired == 1);
        CHECK(fired_on == GetCurrentThreadId());

        g_fake_combo_down = false;
        PumpPolls(manager, 2);
        g_fake_combo_down = true;
        PumpUntil(manager, [&] { return fired == 2; });
        CHECK(fired == 2);
        manager.UnregisterAll();
    }
    g_fake_combo_down = false;
    HotkeyManager::SetKeyStateForTest(nullptr);
}

TEST_CASE("hotkey registry: an unknown method falls back to the default, polling") {
    bool known = true;
    CHECK(ParseHotkeyPolicy("nonsense", &known) == HotkeyPolicy::kPolling);
    CHECK_FALSE(known);
    CHECK(ParseHotkeyPolicy(Config{}.hotkeys_backend, &known) == HotkeyPolicy::kPolling);
    CHECK(known);
    CHECK(ParseHotkeyPolicy("auto", &known) == HotkeyPolicy::kAuto);
    CHECK(known);
}

TEST_CASE("hotkey registry: a suspend lapses on its own") {
    HotkeyRegistry reg(NoopActions(), HotkeyPolicy::kPolling);
    reg.SetSuspendLeaseMsForTest(50);
    reg.RequestSuspend(true);
    reg.ApplyPending();
    CHECK(reg.Status().suspended);
    Sleep(120);
    reg.ApplyPending();  // the engine's 1 s lease timer does this
    CHECK_FALSE(reg.Status().suspended);
    CHECK(reg.Status().bindings.size() == 4);
}

}  // namespace ombra::test
