// Which window a window-mode capture records: the target (the Games list, or one program), the rule that picks one window from what's on screen, and the Win32 snapshot that
// rule runs on. Shared by the engine (Capture::Open, the supervisor's re-resolve) and the Settings
// window picker, so both agree on what "the target is running" means.
//
// A game is often several programs (a launcher exe for the lobby, a second exe for the match,
// both under one install folder). OBS's window matching
// (libobs/util/windows/window-helpers.c, window_rating) scores windows against one saved
// title:class:exe and keeps the best; it has no notion of a set. Here "The game" matches any
// window the Games list calls a game, and ChooseTargetWindow picks among matches.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ombra {

struct WindowTarget {
    // "The game" target: a window matches when this says its image (full path) is a game -- the
    // Games list's ClassifyImage (games.h), so a "not a game" tool inside a game's folder stays
    // out. Wins over `match` when set.
    std::function<bool(const std::wstring& image_path)> is_game;
    std::wstring match;  // capture.target_window: an exe path, or a title or image-path substring
    bool Empty() const { return !is_game && match.empty(); }
};

// One capturable top-level window at snapshot time.
struct WindowCandidate {
    uint64_t     id = 0;  // HWND value; same-session only
    std::wstring title;
    std::wstring image_path;  // full path; empty when the process couldn't be queried
    bool         minimized      = false;
    bool         foreground     = false;
    bool         covers_monitor = false;  // fills its monitor (borderless or exclusive fullscreen)
    int64_t      area = 0;                // window rect, px
};

bool WindowMatchesTarget(const WindowTarget& target, const WindowCandidate& w);

// Index into `windows` of the window to record, or -1 if none matches.
//
// Every switch between windows is a pipeline rebuild, which cuts a ring barrier, and a save can't
// reach past a barrier. So the current window (`current_id`) is kept while it still matches, even
// minimized: alt-tabbing out of an exclusive-fullscreen game minimizes it, and switching to the
// lobby then back would cost two barriers. Another match takes over only when it is in the
// foreground and covers its monitor while the current one doesn't (a minimized window never
// covers): the lobby -> match hand-off. With no current window, the best match wins: not
// minimized, then foreground, then covers its monitor, then larger, then z-order.
//
// Callers debounce a switch (the supervisor holds it for kWindowSwitchSettle) so a focus flicker
// doesn't cost a barrier.
int ChooseTargetWindow(const WindowTarget& target, const std::vector<WindowCandidate>& windows,
                       uint64_t current_id);

// The supervisor's switch debounce: a new pick must hold for `settle_ms` before it's acted on.
// `want` is ChooseTargetWindow's pick (0: nothing matches). A recorded window that no longer
// exists is left to the item-closed fault, which rebuilds on its own.
struct WindowSwitchState {
    bool     pending  = false;
    uint64_t to       = 0;
    int64_t  since_ms = 0;
};
enum class WindowSwitchAction { None, Wait, Switch };
WindowSwitchAction StepWindowSwitch(WindowSwitchState& state, uint64_t want, uint64_t current,
                                    bool current_exists, int64_t now_ms, int64_t settle_ms);

// Visible, unowned, non-tool, non-cloaked top-level windows in z-order, excluding the shell and
// Ombra's own processes: the same filter an alt-tab list uses.
std::vector<WindowCandidate> SnapshotTargetWindows();

// L"E:/Games/Example/Game/" -> L"e:\games\example\game".
std::wstring NormalizeFolderPath(const std::wstring& path);

}  // namespace ombra
