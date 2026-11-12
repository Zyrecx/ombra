#include "ombra/hotkeys.h"

#include "ombra/hotkey_spec.h"

#include "ombra/log.h"

namespace ombra {

HotkeyManager* HotkeyManager::s_active_for_hook_ = nullptr;

HotkeyManager::~HotkeyManager() { UnregisterAll(); }

namespace {
std::atomic<HotkeyManager::KeyStateFn> g_key_state_for_test{nullptr};

bool KeyDown(int vk) {
    const HotkeyManager::KeyStateFn fn = g_key_state_for_test.load(std::memory_order_relaxed);
    return ((fn ? fn(vk) : GetAsyncKeyState(vk)) & 0x8000) != 0;
}

UINT CurrentModifierMask() {
    UINT mods = 0;
    if (KeyDown(VK_MENU))    mods |= MOD_ALT;
    if (KeyDown(VK_CONTROL)) mods |= MOD_CONTROL;
    if (KeyDown(VK_SHIFT))   mods |= MOD_SHIFT;
    if (KeyDown(VK_LWIN) || KeyDown(VK_RWIN)) mods |= MOD_WIN;
    return mods;
}
// A binding registered while its combo is still held (a rebind or resume lands within milliseconds
// of the key press that caused it) must not fire until the keys are released and pressed again.
// RegisterHotKey's MOD_NOREPEAT gives that for free; the poll and hook paths start latched.
bool HeldNow(const HotkeyBinding& b) {
    return KeyDown(static_cast<int>(b.vk)) && CurrentModifierMask() == b.mods;
}
}  // namespace

void HotkeyManager::SetKeyStateForTest(KeyStateFn fn) { g_key_state_for_test.store(fn); }

HotkeyManager::RegisterResult HotkeyManager::Register(int id, HotkeyBinding binding) {
    RegisterResult result;
    const std::string name = binding.name;

    if (policy_ == HotkeyPolicy::kPolling) {
        if (!EnsurePollingStarted()) {
            Log::Error("hotkey %s: could not start the polling thread", name.c_str());
            result.ok = false;
            return result;
        }
        Log::Info("hotkey %s -> GetAsyncKeyState polling", name.c_str());
        RegisteredHotkey rh;
        rh.id          = id;
        rh.binding     = std::move(binding);
        rh.via_polling = true;
        rh.pressed     = HeldNow(rh.binding);
        {
            std::lock_guard<std::mutex> lock(bindings_mutex_);
            bindings_.push_back(std::move(rh));
        }
        result.ok         = true;
        result.via_polling = true;
        return result;
    }

    if (policy_ != HotkeyPolicy::kLowLevelHook) {
        if (RegisterHotKey(target_, id, binding.mods | MOD_NOREPEAT, binding.vk)) {
            Log::Info("hotkey %s -> RegisterHotKey (id=%d)", name.c_str(), id);
            RegisteredHotkey rh;
            rh.id          = id;
            rh.binding     = std::move(binding);
            rh.via_ll_hook = false;
            {
                std::lock_guard<std::mutex> lock(bindings_mutex_);
                bindings_.push_back(std::move(rh));
            }
            result.ok          = true;
            result.via_ll_hook = false;
            return result;
        }
        const DWORD err = GetLastError();
        if (policy_ == HotkeyPolicy::kRegisterHotKey) {
            // Explicit user choice: fail loudly, no fallback.
            Log::Error("hotkey %s: RegisterHotKey failed (err=%lu), backend=register_hotkey — no "
                       "fallback", name.c_str(), err);
            result.ok = false;
            return result;
        }
        // auto: including ERROR_HOTKEY_ALREADY_REGISTERED (the ShadowPlay case, config.h's own
        // tradeoff string for hotkeys.save_replay) — that is precisely when the hook is the right
        // answer, since it still sees the keystroke regardless of who else registered the combo.
        Log::Warn("hotkey %s: RegisterHotKey failed (err=%lu) — falling back to WH_KEYBOARD_LL",
                  name.c_str(), err);
    }

    if (!EnsureHookInstalled()) {
        Log::Error("hotkey %s: could not install WH_KEYBOARD_LL", name.c_str());
        result.ok = false;
        return result;
    }
    Log::Info("hotkey %s -> WH_KEYBOARD_LL", name.c_str());
    RegisteredHotkey rh;
    rh.id          = id;
    rh.binding     = std::move(binding);
    rh.via_ll_hook = true;
    rh.pressed     = HeldNow(rh.binding);
    {
        std::lock_guard<std::mutex> lock(bindings_mutex_);
        bindings_.push_back(std::move(rh));
    }
    result.ok          = true;
    result.via_ll_hook = true;
    return result;
}

bool HotkeyManager::EnsureHookInstalled() {
    if (ll_hook_) return true;
    s_active_for_hook_ = this;
    ll_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &HotkeyManager::LowLevelKeyboardProc,
                                  GetModuleHandleW(nullptr), 0);
    if (!ll_hook_) {
        s_active_for_hook_ = nullptr;
        return false;
    }
    return true;
}

void HotkeyManager::DispatchWmHotkey(WPARAM id) {
    std::function<void()> action;
    {
        std::lock_guard<std::mutex> lock(bindings_mutex_);
        for (const auto& rh : bindings_) {
            if (!rh.via_ll_hook && rh.id == static_cast<int>(id)) {
                action = rh.binding.action;
                break;
            }
        }
    }
    if (action) action();
}

// static
LRESULT CALLBACK HotkeyManager::LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    // Latch-and-return: this must stay fast enough that Windows never decides to silently unhook
    // it (LowLevelHooksTimeout) — see the header's discipline comment. All it does is read the vk
    // out of the OS-owned struct and hand off; OnLowLevelKeyEvent does the (still cheap) matching
    // and calls into whatever action was registered, which is itself required to return quickly.
    if (code == HC_ACTION && s_active_for_hook_) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) {
            s_active_for_hook_->OnLowLevelKeyEvent(kb->vkCode, /*down=*/true);
        } else if (wparam == WM_KEYUP || wparam == WM_SYSKEYUP) {
            s_active_for_hook_->OnLowLevelKeyEvent(kb->vkCode, /*down=*/false);
        }
    }
    // Never swallow: a recorder eating keystrokes out from under a game is worse than a hotkey
    // firing in more than one place (see the header comment; RegisterHotKey does consume the
    // combo, so this is a deliberate asymmetry between the two backends, not an oversight).
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void HotkeyManager::OnLowLevelKeyEvent(UINT vk, bool down) {
    // Uncontended-lock cost here is nanoseconds, far under LowLevelHooksTimeout — see the header's
    // hook-proc discipline comment for the budget this must still fit inside.
    // Actions run after the lock is dropped: one that rebinds would take this mutex again.
    std::vector<std::function<void()>> fire;
    {
        std::lock_guard<std::mutex> lock(bindings_mutex_);
        if (down) {
            const UINT mods = CurrentModifierMask();
            for (auto& rh : bindings_) {
                // Exact modifier match, not subset — RegisterHotKey doesn't fire Alt+F10 for
                // Ctrl+Alt+F10 either, and the two backends must agree.
                if (rh.via_ll_hook && rh.binding.vk == vk && rh.binding.mods == mods) {
                    if (!rh.pressed) {
                        rh.pressed = true;
                        // RegisterHotKey's MOD_NOREPEAT has no LL-hook equivalent — the hook delivers
                        // every OS key-repeat WM_KEYDOWN, so `pressed` is this backend's own
                        // no-repeat edge, or a held key would spam the action.
                        if (rh.binding.action) fire.push_back(rh.binding.action);
                    }
                }
            }
        } else {
            for (auto& rh : bindings_) {
                if (rh.via_ll_hook && rh.binding.vk == vk) rh.pressed = false;
            }
        }
    }
    for (auto& action : fire) action();
}

bool HotkeyManager::EnsurePollingStarted() {
    if (poll_thread_.joinable()) return true;
    pump_thread_ = GetCurrentThreadId();
    poll_stop_.store(false, std::memory_order_relaxed);
    poll_thread_ = std::thread(&HotkeyManager::PollLoop, this);
    return true;
}

void HotkeyManager::PollLoop() {
    Log::SetThreadName("hotkeys");
    // Same shape as OBS's own hotkey thread (libobs/obs-hotkey.c polls at 40Hz). Sleep(15) measured
    // ~46 Hz at the default timer resolution (2026-11-03). Edge detection mirrors
    // OnLowLevelKeyEvent's: `pressed` latches on an exact-modifier-match down state and clears the
    // moment that state stops holding, so a held key doesn't repeat-fire. A press is posted, not
    // run: the action belongs on the pump thread, like every other backend's (header comment).
    constexpr DWORD kPollIntervalMs = 15;
    while (!poll_stop_.load(std::memory_order_relaxed)) {
        std::vector<int> fire;
        {
            std::lock_guard<std::mutex> lock(bindings_mutex_);
            const UINT mods = CurrentModifierMask();
            for (auto& rh : bindings_) {
                if (!rh.via_polling) continue;
                if (KeyDown(static_cast<int>(rh.binding.vk)) && rh.binding.mods == mods) {
                    if (!rh.pressed) {
                        rh.pressed = true;
                        fire.push_back(rh.id);
                    }
                } else {
                    rh.pressed = false;
                }
            }
        }
        for (const int id : fire) {
            const BOOL posted = target_ ? PostMessageW(target_, WM_HOTKEY, id, 0)
                                        : PostThreadMessageW(pump_thread_, WM_HOTKEY, id, 0);
            if (!posted) Log::Error("hotkey id %d: could not reach the pump thread (err=%lu)", id, GetLastError());
        }
        Sleep(kPollIntervalMs);
    }
}

void HotkeyManager::UnregisterAll() {
    {
        std::lock_guard<std::mutex> lock(bindings_mutex_);
        for (const auto& rh : bindings_)
            if (!rh.via_ll_hook && !rh.via_polling) UnregisterHotKey(target_, rh.id);
        bindings_.clear();
    }
    if (ll_hook_) {
        UnhookWindowsHookEx(ll_hook_);
        ll_hook_ = nullptr;
    }
    if (s_active_for_hook_ == this) s_active_for_hook_ = nullptr;
    if (poll_thread_.joinable()) {
        poll_stop_.store(true, std::memory_order_relaxed);
        poll_thread_.join();
    }
}

HotkeyPolicy ParseHotkeyPolicy(const std::string& name, bool* known) {
    if (known) *known = true;
    if (name == "register_hotkey") return HotkeyPolicy::kRegisterHotKey;
    if (name == "ll_hook")         return HotkeyPolicy::kLowLevelHook;
    if (name == "poll")            return HotkeyPolicy::kPolling;
    if (name == "auto")            return HotkeyPolicy::kAuto;
    if (known) *known = false;
    return HotkeyPolicy::kPolling;
}

const char* HotkeyPolicyName(HotkeyPolicy policy) {
    switch (policy) {
        case HotkeyPolicy::kAuto:           return "auto";
        case HotkeyPolicy::kRegisterHotKey: return "register_hotkey";
        case HotkeyPolicy::kLowLevelHook:   return "ll_hook";
        case HotkeyPolicy::kPolling:        return "poll";
    }
    return "auto";
}

// --- HotkeyRegistry ------------------------------------------------------------------------------

HotkeyRegistry::HotkeyRegistry(std::vector<HotkeyActionSpec> actions, HotkeyPolicy initial_policy)
    : actions_(std::move(actions)), manager_(initial_policy) {
    wanted_.policy = initial_policy;
    for (const auto& a : actions_) wanted_.specs[a.key] = a.default_spec;
}

HotkeyRegistry::~HotkeyRegistry() { Shutdown(); }

void HotkeyRegistry::Notify() {
    if (HWND hwnd = hwnd_.load()) PostMessageW(hwnd, kNotifyMessage, 0, 0);
}

void HotkeyRegistry::RequestBindings(HotkeyPolicy policy, const std::map<std::string, std::string>& specs) {
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        wanted_.policy = policy;
        for (const auto& [key, spec] : specs) {
            if (wanted_.specs.count(key)) wanted_.specs[key] = spec;
        }
    }
    Notify();
}

void HotkeyRegistry::RequestSuspend(bool suspend) {
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        suspend_until_ = suspend ? GetTickCount64() + suspend_lease_ms_ : 0;
    }
    Notify();
}

HotkeyStatus HotkeyRegistry::Status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void HotkeyRegistry::ApplyPending() {
    Wanted wanted;
    bool   suspended = false;
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        wanted    = wanted_;
        suspended = suspend_until_ != 0 && GetTickCount64() < suspend_until_;
        if (!suspended) suspend_until_ = 0;
    }
    HWND hwnd = hwnd_.load();
    if (hwnd) {
        // While suspended, a 1 s timer re-checks the lease; nothing else would wake the pump.
        if (suspended) SetTimer(hwnd, kLeaseTimerId, 1000, nullptr);
        else KillTimer(hwnd, kLeaseTimerId);
    }
    if (applied_once_ && wanted == applied_ && suspended == applied_suspended_) return;

    // A full re-register even when one spec changed: unregistering everything first means a swap
    // (save <-> screenshot) never collides with itself halfway through.
    manager_.UnregisterAll();
    manager_.SetPolicy(wanted.policy);
    HotkeyStatus status;
    status.policy    = HotkeyPolicyName(wanted.policy);
    status.suspended = suspended;
    if (suspended) {
        Log::Info("hotkeys: suspended while Settings records a new combo");
    } else {
        for (const auto& a : actions_) {
            const std::string& configured = wanted.specs[a.key];
            HotkeyCombo combo;
            std::string spec = configured;
            if (!ParseHotkeySpec(configured, combo)) {
                Log::Warn("config %s: could not parse \"%s\" — falling back to %s", a.key.c_str(),
                          configured.c_str(), a.default_spec.c_str());
                spec = a.default_spec;
                ParseHotkeySpec(spec, combo);
            }
            const auto reg = manager_.Register(a.id, HotkeyBinding{a.key, combo.mods, combo.vk, a.action});
            HotkeyBindingStatus bs;
            bs.key  = a.key;
            bs.spec = FormatHotkeySpec(combo);
            bs.ok   = reg.ok;
            if (reg.ok) bs.backend = reg.via_polling ? "poll" : reg.via_ll_hook ? "ll_hook" : "register_hotkey";
            else Log::Error("hotkey %s (%s) is not registered under any backend", a.key.c_str(), spec.c_str());
            status.bindings.push_back(std::move(bs));
        }
        if (applied_suspended_) Log::Info("hotkeys: resumed");
    }
    applied_once_      = true;
    applied_           = std::move(wanted);
    applied_suspended_ = suspended;
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = std::move(status);
}

bool HotkeyRegistry::HandleWindowMessage(HWND, UINT msg, WPARAM wparam, LPARAM) {
    // Only a modal loop gets here (the tray menu): the engine's pump dispatches WM_HOTKEY itself.
    if (msg == WM_HOTKEY) {
        DispatchWmHotkey(wparam);
        return true;
    }
    if (msg == kNotifyMessage || (msg == WM_TIMER && wparam == kLeaseTimerId)) {
        ApplyPending();
        return true;
    }
    return false;
}

void HotkeyRegistry::Shutdown() {
    manager_.UnregisterAll();
    if (HWND hwnd = hwnd_.load()) KillTimer(hwnd, kLeaseTimerId);
}

}  // namespace ombra
