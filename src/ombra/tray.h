// Tray icon for the resident engine.
//
// Lives entirely in ombra.exe: the tray is the engine's own affordance for
// launching/focusing the settings UI and exiting, not something ombra-ui owns. Hosted on
// The hidden EngineWndProc window — Shell_NotifyIconW's callback and TrackPopupMenu's
// WM_COMMAND both need an HWND to route through, and the window already exists.
//
// THREADING: HandleWindowMessage is called from EngineWndProc, i.e. on the pump thread, same as
// every other message this codebase routes there (WM_HOTKEY, WM_TIMER). Keep it cheap — the
// "Open Settings" path does a CreateProcessW, not a blocking wait, for exactly this reason.
#pragma once

#include "ombra/ui_palette.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ombra {

// Resolves the full path to ombra-ui.exe, assumed to sit next to the running process's own module
// (GetModuleFileNameW's directory). Shared by Tray::HandleOpenSettings (spawning the overlay) and
// main.cpp's toast spawn helper (spawning a short-lived `--toast` instance) -- previously inlined
// only in tray.cpp, factored out so a second caller doesn't have to duplicate it. Returns an empty
// string on failure (GetModuleFileNameW truncation/error), same as the original inline check.
std::wstring ResolveOmbraUiExePath();

// What the context menu shows, read on the pump thread each time it opens.
// ui.themes as loaded or as set_config last wrote it, for a ui.theme naming one (the menu's light or
// dark). Any thread.
void SetTrayUserThemes(std::vector<UserTheme> themes);

struct TrayMenuState {
    bool         replay_on = false;   // the Instant Replay switch (BufferController::UserEnabled)
    bool         recording = false;   // the pipelines are armed (footage is going into the ring)
    std::wstring game;                // the game being recorded, empty when none
    double       buffer_seconds = 0;
    std::string  theme = "dark";      // ui.theme (ombra/ui_palette.h)
    std::wstring save_hotkey, toggle_hotkey, overlay_hotkey;
    std::wstring update_ready;        // a downloaded, verified update's version, empty when none
};

// The engine's side of the menu's items. Every callback runs on the pump thread.
struct TrayMenuActions {
    std::function<TrayMenuState()> state;
    std::function<void()>          save_clip;
    std::function<void()>          toggle_replay;
    std::function<std::wstring()>  clips_folder;  // resolved output root
    std::function<void()>          install_update;
};

class Tray {
public:
    // hwnd must be the window whose WndProc forwards every message to HandleWindowMessage —
    // Shell_NotifyIconW's callback_msg and the context menu's WM_COMMAND both target it.
    // on_exit is invoked when the user picks "Exit"; main.cpp wires it to PostQuitMessage(0),
    // matching the "Exit (PostQuitMessage)" — called on the pump thread, so this is the
    // same thread GetMessage's loop is already spinning on.
    // initial_accent seeds the tray icon's ring color at startup (a ui.accent value; "theme" reads
    // initial_theme, the ui.theme value);
    // defaults to "mono" so every existing call site (tests, anything not yet passing cfg.ui_accent)
    // keeps today's look.
    Tray(HWND hwnd, UINT callback_msg, std::function<void()> on_exit,
         const std::string& initial_accent = "mono", const std::string& initial_theme = "dark");
    ~Tray();

    Tray(const Tray&)            = delete;
    Tray& operator=(const Tray&) = delete;

    // Sets/replaces the tooltip text (armed/disarmed + mode). Push, not pull — Shell_NotifyIconW
    // has no way for Explorer to ask the tooltip back, so this must be called again after any
    // state change the tooltip should reflect. Only called once at startup for now; wiring it to
    // arm/disarm transitions is a follow-up (BufferController has no "state changed" callback
    // yet to hang it off).
    void SetTooltip(const std::wstring& text);

    // Re-renders the tray icon's ring/halo in the given accent (ui.accent; "theme" takes theme_key's
    // palette accent, ui_palette.h — tray.cpp keeps its own small RGB table for the presets since the
    // engine doesn't link Direct2D/theme.cpp) and pushes it to Explorer via NIM_MODIFY. Ombra-ui's
    // AccentSwatchRow calls set_config{"ui.accent":...}, which also repaints the tray. Safe to
    // call with the icon's current accent (e.g. at every Free-tier apply) — it always re-renders,
    // no dirty-check, since this only runs on an explicit settings change, not a hot path.
    void SetAccent(const std::string& accent_key, const std::string& theme_key);

    // Shows a balloon notification on this same icon (NIF_INFO / NIIF_INFO) — non-blocking, no
    // focus stolen. Silent-first-run decision: ombra-ui has no tray icon of
    // its own, so it asks the engine (over the pipe's new "notify" message) to show this on the
    // one persistent icon rather than ombra-ui standing up a second, throwaway icon just to
    // balloon once.
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

    // Call from EngineWndProc for every message. Returns true if this call was the tray's
    // (callback_msg, a WM_COMMAND for one of the tray's own menu ids, or a re-broadcast
    // "TaskbarCreated" — see taskbar_created_msg_ below) — the caller should return 0 without
    // falling through to DefWindowProc in that case.
    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    // hotkeys.overlay ("Alt+Z" by default, main.cpp): open the overlay if it isn't on screen,
    // close it if it is. Process-agnostic on purpose — unlike HandleOpenSettings's ui_process_
    // bookkeeping (which only tracks a spawn this Tray instance made), this probes for the live
    // overlay window by class name (ombra::kOverlayWindowClassW) so a manually-launched or
    // already-open ombra-ui.exe is found too. Getting this wrong under repeated Alt+Z presses
    // means stacked WS_EX_TOPMOST overlay windows, all talking to the same pipe.
    void ToggleOverlay();
    // Opens it if it isn't on screen; never closes it.
    void OpenOverlay();

    // Without these the menu shows only Settings, Get logs and Exit.
    void SetMenuActions(TrayMenuActions actions) { menu_actions_ = std::move(actions); }

private:
    void AddIcon();
    void ShowContextMenu(HWND hwnd);
    // Spawns ombra-ui.exe (found next to this process's own module), or — if a previous spawn's
    // process handle is still alive — brings its (best-effort found, by pid) top-level window to
    // the foreground instead of launching a second instance. This only guards against the tray
    // itself double-launching; a manually-started second ombra-ui.exe is a known gap that needs the pipe
    // to close properly.
    void HandleOpenSettings();
    // "Get logs": writes the diagnostics bundle (diagnostics.h) to Downloads on a worker thread,
    // selects it in Explorer, then reports back to the pump thread (balloon).
    void StartBundleExport();

    HWND                   hwnd_;
    std::atomic<bool>      bundle_busy_{false};
    UINT                   callback_msg_;
    std::function<void()>  on_exit_;
    // Pipe workers (a settings Save) and the pump thread (the toggle hotkey) both update the icon.
    std::mutex             nid_mutex_;
    NOTIFYICONDATAW        nid_{};
    bool                   added_ = false;
    // Explorer broadcasts this registered message to every top-level window after it restarts
    // (crash, "Restart Windows Explorer", taskkill /f /im explorer.exe) — every existing tray
    // icon is gone at that point and NIM_ADD must be reissued, or the icon is lost for the rest
    // of the process's life with no way for the user to get it back short of Task Manager. Cached
    // once here (RegisterWindowMessageW is the same id process-wide, but there's no reason to
    // call it more than once) rather than recomputed per message.
    UINT                   taskbar_created_msg_;

    HANDLE                 ui_process_ = nullptr;
    DWORD                  ui_pid_     = 0;

    // The procedurally-rendered brand-mark icon (tray.cpp's RenderEclipseIcon), owned here and
    // destroyed in ~Tray -- unlike the stock IDI_APPLICATION icon it replaces, this one is a real
    // GDI object that leaks if not explicitly freed.
    HICON                  icon_ = nullptr;
    std::string            accent_key_ = "mono";
    std::string            theme_key_ = "dark";

    TrayMenuActions        menu_actions_;
    std::optional<bool>    menu_dark_;     // the app mode last set for menus
};

}  // namespace ombra
