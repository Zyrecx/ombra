#include "ombra/window_target.h"
#include "ombra/brand.h"

#include <windows.h>
#include <dwmapi.h>

namespace ombra {

namespace {

// Locale-independent, covers non-ASCII paths (CharLowerBuffW maps by the OS case table).
std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsOmbraImage(const std::wstring& image_lower) {
    const std::wstring name = FileNameOf(image_lower);
    return name == L"" OMBRA_ENGINE_EXE || name == L"" OMBRA_UI_EXE;
}

// Strict "a outranks b" for a fresh pick.
bool Outranks(const WindowCandidate& a, const WindowCandidate& b) {
    if (a.minimized != b.minimized) return !a.minimized;
    if (a.foreground != b.foreground) return a.foreground;
    if (a.covers_monitor != b.covers_monitor) return a.covers_monitor;
    return a.area > b.area;
}

}  // namespace

std::wstring NormalizeFolderPath(const std::wstring& path) {
    std::wstring out = Lower(path);
    for (wchar_t& c : out)
        if (c == L'/') c = L'\\';
    while (!out.empty() && out.back() == L'\\') out.pop_back();
    return out;
}

WindowSwitchAction StepWindowSwitch(WindowSwitchState& state, uint64_t want, uint64_t current,
                                    bool current_exists, int64_t now_ms, int64_t settle_ms) {
    if (want == current || (want == 0 && current != 0 && !current_exists)) {
        state.pending = false;
        return WindowSwitchAction::None;
    }
    if (!state.pending || state.to != want) {
        state.pending  = true;
        state.to       = want;
        state.since_ms = now_ms;
        return WindowSwitchAction::Wait;
    }
    if (now_ms - state.since_ms < settle_ms) return WindowSwitchAction::Wait;
    state.pending = false;
    return WindowSwitchAction::Switch;
}

bool WindowMatchesTarget(const WindowTarget& target, const WindowCandidate& w) {
    const std::wstring image_lower = Lower(w.image_path);
    if (IsOmbraImage(image_lower)) return false;
    if (target.is_game) return !w.image_path.empty() && target.is_game(w.image_path);
    if (target.match.empty()) return false;
    const std::wstring want = Lower(target.match);
    return Lower(w.title).find(want) != std::wstring::npos || image_lower.find(want) != std::wstring::npos;
}

int ChooseTargetWindow(const WindowTarget& target, const std::vector<WindowCandidate>& windows,
                       uint64_t current_id) {
    int best    = -1;
    int current = -1;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (!WindowMatchesTarget(target, windows[i])) continue;
        if (best < 0 || Outranks(windows[i], windows[static_cast<size_t>(best)])) best = static_cast<int>(i);
        if (current_id != 0 && windows[i].id == current_id) current = static_cast<int>(i);
    }
    if (current < 0) return best;

    const WindowCandidate& cur = windows[static_cast<size_t>(current)];
    const WindowCandidate& top = windows[static_cast<size_t>(best)];
    if (best != current && top.foreground && top.covers_monitor && !cur.covers_monitor) return best;
    return current;
}

std::vector<WindowCandidate> SnapshotTargetWindows() {
    struct Ctx {
        HWND                         foreground;
        std::vector<WindowCandidate> out;
    } ctx{GetForegroundWindow(), {}};

    EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lparam);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            if (hwnd == GetShellWindow()) return TRUE;
            if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            // Cloaked: e.g. UWP shell surfaces that report visible but are never composited.
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                return TRUE;

            WindowCandidate w;
            w.id = reinterpret_cast<uint64_t>(hwnd);
            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, 255);
            w.title = title;

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) {
                if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                    wchar_t image[MAX_PATH]{};
                    DWORD   len = MAX_PATH;
                    if (QueryFullProcessImageNameW(proc, 0, image, &len)) w.image_path.assign(image, len);
                    CloseHandle(proc);
                }
            }
            if (IsOmbraImage(Lower(w.image_path))) return TRUE;

            w.minimized  = IsIconic(hwnd) != FALSE;
            w.foreground = hwnd == c->foreground;
            RECT rc{};
            if (!w.minimized && GetWindowRect(hwnd, &rc)) {
                w.area = static_cast<int64_t>(rc.right - rc.left) * (rc.bottom - rc.top);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    w.covers_monitor = rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
                                       rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom;
                }
            }
            c->out.push_back(std::move(w));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

}  // namespace ombra
