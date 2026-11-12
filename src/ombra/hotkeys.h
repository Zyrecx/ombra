// Hotkey backends.
//
// RegisterHotKey is the default — it's simple and it works. Its failure mode
// is not "the API call returns FALSE" in the case that actually matters: a game holding exclusive
// input via certain DirectInput modes can prevent WM_HOTKEY from ever being delivered while the
// registration itself succeeds. What *does* fail loudly is the ShadowPlay case — Alt+F10 is
// ShadowPlay's own default save hotkey too (config.h's hotkeys.save_replay tradeoff string), and
// two processes can't RegisterHotKey the same combo. WH_KEYBOARD_LL is the fallback essentially
// every capture tool ends up on for exactly this reason: it sees the raw keystroke stream before
// any single window claims it, so it still fires under both failure modes above.
//
// config's hotkeys.backend picks the policy:
//   auto            - RegisterHotKey first; a registration failure (including
//                      ERROR_HOTKEY_ALREADY_REGISTERED) falls that one binding back to the LL
//                      hook, not the whole process to a fatal exit.
//   register_hotkey - RegisterHotKey only; a failure is reported to the caller, not silently
//                      downgraded. Explicit user choice, fail loudly.
//   ll_hook         - the LL hook only, skipping RegisterHotKey entirely.
//   poll            - neither of the above: a dedicated thread polls GetAsyncKeyState for each
//                      bound key (same mechanism OBS uses — libobs/obs-windows.c's vk_down(),
//                      polled by a generic hotkey thread — rather than RegisterHotKey's
//                      window-message delivery or WH_KEYBOARD_LL's hook callback). Exists because
//                      some titles' anti-cheat has been observed to let RegisterHotKey succeed
//                      (the call returns TRUE) while still never delivering WM_HOTKEY, which is
//                      silent and gives auto/register_hotkey nothing to fall back from. A plain state query claims no
//                      combo and installs no hook/message registration, so there's nothing for
//                      that class of interference to intercept. The default:
//                      a dead save key in a game is the worst failure a replay tool can have, and
//                      auto can't detect it. It costs a dedicated thread (at most ~0.4% of one
//                      core, measured) and a tap shorter than a poll interval can be missed.
//
// Every binding's outcome is logged once at Register() time ("the log must say which
// path is live").
//
// THREADING: must be constructed and used from the thread that pumps messages (GetMessage).
// RegisterHotKey delivers WM_HOTKEY to the *registering* thread's queue; SetWindowsHookEx(
// WH_KEYBOARD_LL, ...)'s callback is dispatched by the OS through a hidden mechanism that also
// requires the installing thread to pump messages. The poll thread only reads keys and posts
// WM_HOTKEY. So every action runs on wmain's pump thread, whichever backend fired it: actions touch
// state only that thread owns (the tray's UI process handle), and must stay cheap (see below).
//
// HOOK PROC DISCIPLINE, same as controller.h's OnForegroundChanged: Windows silently unhooks a
// WH_KEYBOARD_LL callback that runs long enough to exceed LowLevelHooksTimeout, and a silently
// unhooked callback is indistinguishable from an idle one — the hook proc only latches modifier
// state and calls the bound action, and every action registered here must itself return quickly
// (spawn a thread for real work, as OnHotkey already does for saves). CallNextHookEx is always
// called, never swallowed: a recorder eating keystrokes out from under a game is worse than a
// hotkey firing in both this process and whatever else is also bound to it.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

enum class HotkeyPolicy { kAuto, kRegisterHotKey, kLowLevelHook, kPolling };

struct HotkeyBinding {
    std::string           name;  // for logging, e.g. "hotkeys.save_replay"
    UINT                   mods = 0;  // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN — no MOD_NOREPEAT
    UINT                   vk   = 0;
    std::function<void()>  action;  // must return quickly — see the header comment
};

class HotkeyManager {
public:
    explicit HotkeyManager(HotkeyPolicy policy) : policy_(policy) {}

    // Only between UnregisterAll() and the next Register(): bindings keep the backend they got.
    void SetPolicy(HotkeyPolicy policy) { policy_ = policy; }
    HotkeyPolicy Policy() const { return policy_; }
    // The window RegisterHotKey delivers to; null means the thread's queue. A window message survives
    // a modal loop (the tray menu's TrackPopupMenu), where a thread message is lost. Set it before the
    // first Register(): UnregisterHotKey needs the same window.
    void SetTargetWindow(HWND hwnd) { target_ = hwnd; }
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&)            = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    struct RegisterResult {
        bool ok           = false;
        bool via_ll_hook  = false;  // which backend actually ended up live
        bool via_polling  = false;
    };

    // `id` must be unique per instance, > 0 — used as RegisterHotKey's id and WM_HOTKEY's wParam
    // (the LL-hook path doesn't need it, but shares the field so callers have one id space).
    RegisterResult Register(int id, HotkeyBinding binding);

    // Call from the pump loop on every WM_HOTKEY.
    void DispatchWmHotkey(WPARAM id);

    void UnregisterAll();

    // Replaces GetAsyncKeyState for this process; null restores it. Tests only.
    using KeyStateFn = SHORT (*)(int vk);
    static void SetKeyStateForTest(KeyStateFn fn);

private:
    struct RegisteredHotkey {
        int            id = 0;
        HotkeyBinding  binding;
        bool           via_ll_hook    = false;
        bool           via_polling    = false;
        bool           pressed        = false;  // LL-hook/polling paths' own no-repeat edge tracking
    };

    bool EnsureHookInstalled();
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam);
    void OnLowLevelKeyEvent(UINT vk, bool down);

    bool EnsurePollingStarted();
    void PollLoop();

    HotkeyPolicy                  policy_;
    HWND                          target_ = nullptr;
    std::vector<RegisteredHotkey> bindings_;
    std::mutex                    bindings_mutex_;  // guards bindings_ against the poll thread
    HHOOK                         ll_hook_ = nullptr;
    std::thread                   poll_thread_;
    DWORD                         pump_thread_ = 0;  // where a polled press goes without a window
    std::atomic<bool>             poll_stop_{false};

    // SetWindowsHookEx's callback carries no user-data parameter, and there is only ever one
    // HotkeyManager per process — same reasoning as controller.h's self_for_hook_.
    static HotkeyManager* s_active_for_hook_;
};

// "auto|register_hotkey|ll_hook|poll" (config's hotkeys.backend). Unknown -> kPolling, *known = false.
HotkeyPolicy ParseHotkeyPolicy(const std::string& name, bool* known = nullptr);
const char*  HotkeyPolicyName(HotkeyPolicy policy);

// One engine action a hotkey triggers.
struct HotkeyActionSpec {
    int                   id = 0;        // > 0, unique; RegisterHotKey's id
    std::string           key;           // config key, "hotkeys.save_replay"
    std::string           default_spec;  // used when the configured spec doesn't parse
    std::function<void()> action;
};

struct HotkeyBindingStatus {
    std::string key;
    std::string spec;     // what's actually bound (the default if the configured one didn't parse)
    bool        ok = false;
    std::string backend;  // "register_hotkey" | "ll_hook" | "poll"; empty when not registered
};

struct HotkeyStatus {
    std::string                      policy;
    bool                             suspended = false;
    std::vector<HotkeyBindingStatus> bindings;  // empty while suspended
};

// Owns the engine's hotkeys for its whole run, so Settings can change them live. Registration only
// ever happens on the pump thread (RegisterHotKey binds to the calling thread's queue; the LL hook
// needs that thread's loop), so other threads only record what they want and post a bare notify
// to the engine window; ApplyPending() on the pump thread reads the latest request. A burst of
// requests coalesces, and nothing heap-allocated rides in a message that could be dropped.
//
// Suspend exists for Settings' capture control: while it listens for a new combo, the live ones
// must neither fire nor be swallowed by RegisterHotKey before the overlay sees them. It is a lease:
// it lapses kSuspendLeaseMs after the last RequestSuspend(true) unless renewed, so an overlay
// that crashes mid-capture can't leave Alt+Z dead.
class HotkeyRegistry {
public:
    static constexpr UINT     kNotifyMessage   = WM_APP + 20;
    static constexpr UINT_PTR kLeaseTimerId    = 0x484B;  // 'HK'
    static constexpr DWORD    kSuspendLeaseMs  = 10000;

    explicit HotkeyRegistry(std::vector<HotkeyActionSpec> actions,
                            HotkeyPolicy initial_policy = HotkeyPolicy::kPolling);
    ~HotkeyRegistry();

    HotkeyRegistry(const HotkeyRegistry&)            = delete;
    HotkeyRegistry& operator=(const HotkeyRegistry&) = delete;

    // Pump thread, before the first ApplyPending(). Null (test modes, tests) means requests are
    // applied only when the caller calls ApplyPending() itself. The window also receives WM_HOTKEY.
    void SetNotifyWindow(HWND hwnd) {
        hwnd_.store(hwnd);
        manager_.SetTargetWindow(hwnd);
    }
    void SetSuspendLeaseMsForTest(DWORD ms) { suspend_lease_ms_ = ms; }

    // Any thread. `specs` maps config key -> spec; missing keys keep their current spec.
    void RequestBindings(HotkeyPolicy policy, const std::map<std::string, std::string>& specs);
    void RequestSuspend(bool suspend);
    HotkeyStatus Status() const;

    // Pump thread only.
    void ApplyPending();
    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void DispatchWmHotkey(WPARAM id) { manager_.DispatchWmHotkey(id); }
    void Shutdown();

private:
    struct Wanted {
        HotkeyPolicy                       policy = HotkeyPolicy::kPolling;
        std::map<std::string, std::string> specs;
        bool operator==(const Wanted&) const = default;
    };

    void Notify();

    std::vector<HotkeyActionSpec> actions_;
    HotkeyManager                 manager_;
    std::atomic<HWND>             hwnd_{nullptr};

    mutable std::mutex request_mutex_;  // guards wanted_, suspend_until_
    Wanted             wanted_;
    ULONGLONG          suspend_until_ = 0;  // GetTickCount64; 0 = not suspended
    DWORD              suspend_lease_ms_ = kSuspendLeaseMs;

    // Pump thread only.
    bool   applied_once_      = false;
    bool   applied_suspended_ = false;
    Wanted applied_;

    mutable std::mutex status_mutex_;
    HotkeyStatus       status_;
};

}  // namespace ombra
