#include "ombra/tray.h"

#include "ombra/brand.h"
#include "ombra/brand_mark.h"
#include "ombra/config.h"
#include "ombra/diagnostics.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"
#include "ombra/ui_palette.h"
#include "ombra/util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <thread>
#include <vector>

namespace ombra {

namespace {
constexpr UINT kMenuIdOpenSettings = 1;
constexpr UINT kMenuIdExit         = 2;
constexpr UINT kMenuIdGetLogs      = 3;
constexpr UINT kMenuIdSaveClip     = 4;
constexpr UINT kMenuIdToggleReplay = 5;
constexpr UINT kMenuIdClipsFolder  = 6;
constexpr UINT kMenuIdHeader       = 7;
constexpr UINT kMenuIdInstallUpdate = 8;
// Posted only by StartBundleExport's worker, lParam a BundleDone it allocated. A private WM_APP id,
// not a registered message: those can arrive by broadcast, and this handler frees lParam.
// engine.cpp uses WM_APP + 1 (tray callback), hotkeys.h WM_APP + 20.
constexpr UINT kBundleDoneMessage  = WM_APP + 40;

struct BundleDone {
    BundleResult result;
    std::wstring path;
};

// Ring RGB per accent key, mirroring ui/theme.cpp's AccentPickedRgb by hand (that table lives in
// ombra-ui, which the engine doesn't link). The disc doesn't vary by accent.
struct AccentRgb { float r, g, b; };

AccentRgb RingColorForAccent(const std::string& accent_key, const std::string& theme_key) {
    if (accent_key == "theme") {  // the palette's own (ui_palette.h), else mono
        if (const uint32_t rgb = UiThemeAccentRgb(theme_key))
            return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
    }
    if (accent_key == "violet") return {0.655f, 0.545f, 0.980f};  // #a78bfa
    if (accent_key == "cyan")   return {0.133f, 0.827f, 0.933f};  // #22d3ee
    if (accent_key == "blue")   return {0.357f, 0.608f, 1.0f};    // #5b9bff
    if (accent_key == "green")  return {0.463f, 0.725f, 0.0f};    // #76b900 GeForce NOW green
    if (uint32_t rgb = 0; ParseHexRgb(accent_key, &rgb))         // custom accent, as picked
        return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
    return {((kBrandMarkMonoRgb >> 16) & 255) / 255.0f, ((kBrandMarkMonoRgb >> 8) & 255) / 255.0f,
            (kBrandMarkMonoRgb & 255) / 255.0f};                   // mono
}

// The brand mark (brand_mark.h) in ring_rgb as a tray HICON. Sized by the caller
// (GetSystemMetrics(SM_CXSMICON)), not a fixed 16/32 px, or Explorer blurs it scaling for a >100%
// DPI taskbar.
HICON RenderEclipseIcon(int size, AccentRgb ring_rgb) {
    if (size <= 0) return nullptr;

    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;  // top-down, so row 0 is the top row
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HDC   screen_dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen_dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);
    if (!color || !bits) {
        Log::Warn("RenderEclipseIcon: CreateDIBSection failed (err=%lu)", GetLastError());
        return nullptr;
    }

    const std::vector<uint8_t> pixels =
        RenderBrandMark(size, ring_rgb.r, ring_rgb.g, ring_rgb.b, BrandMarkAlpha::Straight);
    std::memcpy(bits, pixels.data(), pixels.size());

    // All-zero AND mask: the 32bpp color bitmap's own alpha channel carries transparency (the
    // documented Vista+ path for alpha-blended icons), so the mask contributes nothing.
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon  = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!icon) {
        Log::Warn("RenderEclipseIcon: CreateIconIndirect failed (err=%lu)", GetLastError());
    }
    return icon;
}

// Dark menus. uxtheme exports these by ordinal only (Windows 10 1903 on, unchanged through 11
// 24H2); Notepad++ and most dark Win32 apps call the same ones. ForceDark/ForceLight pick the
// menus' theme for the whole process (the engine has no other themed UI); FlushMenuThemes makes
// a change apply to the next menu.
enum class PreferredAppMode : int { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
struct UxDarkMode {
    PreferredAppMode (WINAPI* set_preferred_app_mode)(PreferredAppMode) = nullptr;
    void (WINAPI* flush_menu_themes)() = nullptr;
};

const UxDarkMode& DarkModeApi() {
    static const UxDarkMode api = [] {
        UxDarkMode a;
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return a;
        a.set_preferred_app_mode = reinterpret_cast<decltype(a.set_preferred_app_mode)>(
            GetProcAddress(ux, MAKEINTRESOURCEA(135)));
        a.flush_menu_themes = reinterpret_cast<decltype(a.flush_menu_themes)>(
            GetProcAddress(ux, MAKEINTRESOURCEA(136)));
        return a;
    }();
    return api;
}

// A Segoe Fluent Icons glyph as a premultiplied 32bpp bitmap, the form a menu item's hbmpItem
// alpha-blends. Drawn white on black in grayscale antialiasing, so any channel is the coverage.
HBITMAP RenderGlyphBitmap(wchar_t glyph, int size, COLORREF rgb) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void*   bits = nullptr;
    HBITMAP bmp  = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Segoe Fluent Icons");
    HGDIOBJ old_font = SelectObject(dc, font);
    RECT rc{0, 0, size, size};
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < size * size; ++i) {
        const uint32_t a = px[i] & 0xff;
        px[i] = (a << 24) | ((GetRValue(rgb) * a / 255) << 16) | ((GetGValue(rgb) * a / 255) << 8) |
                (GetBValue(rgb) * a / 255);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    SelectObject(dc, old_bmp);
    DeleteDC(dc);
    return bmp;
}

// An antialiased filled circle of diameter d centred in a size x size premultiplied bitmap.
HBITMAP RenderDotBitmap(int size, int d, COLORREF rgb) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void*   bits = nullptr;
    HBITMAP bmp  = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    const float c = size / 2.0f, r = d / 2.0f;
    for (int j = 0; j < size; ++j) {
        for (int i = 0; i < size; ++i) {
            const float dx = i + 0.5f - c, dy = j + 0.5f - c;
            const float cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            const uint32_t a = static_cast<uint32_t>(cov * 255.0f + 0.5f);
            px[j * size + i] = (a << 24) | ((GetRValue(rgb) * a / 255) << 16) |
                               ((GetGValue(rgb) * a / 255) << 8) | (GetBValue(rgb) * a / 255);
        }
    }
    return bmp;
}

std::wstring FormatBufferLength(double seconds) {
    const int s = static_cast<int>(seconds + 0.5);
    wchar_t buf[32];
    if (s < 60)            swprintf_s(buf, L"%d s", s);
    else if (s % 60 == 0)  swprintf_s(buf, L"%d min", s / 60);
    else                   swprintf_s(buf, L"%d:%02d", s / 60, s % 60);
    return buf;
}

struct FindWindowByPidCtx {
    DWORD pid;
    HWND  result = nullptr;
};

BOOL CALLBACK FindWindowByPidProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindWindowByPidCtx*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd)) {
        ctx->result = hwnd;
        return FALSE;  // stop enumerating
    }
    return TRUE;
}
}  // namespace

Tray::Tray(HWND hwnd, UINT callback_msg, std::function<void()> on_exit,
           const std::string& initial_accent, const std::string& initial_theme)
    : hwnd_(hwnd), callback_msg_(callback_msg), on_exit_(std::move(on_exit)),
      accent_key_(initial_accent), theme_key_(initial_theme) {
    taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");
    nid_.cbSize           = sizeof(nid_);
    nid_.hWnd             = hwnd_;
    nid_.uID              = 1;
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = callback_msg_;
    // Rendered rather than loaded from the .exe's icon resource: the ring follows the accent.
    icon_ = RenderEclipseIcon(GetSystemMetrics(SM_CXSMICON), RingColorForAccent(accent_key_, theme_key_));
    nid_.hIcon = icon_ ? icon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid_.szTip, L"" OMBRA_PRODUCT_NAME, _TRUNCATE);
    AddIcon();
}

void Tray::AddIcon() {
    // An isolated instance (OMBRA_HOME: tests, ombra-ui --drive) keeps the Tray object, which the
    // pipe handler reads as "this is the resident engine", but shows nothing on the user's taskbar.
    if (Instance().isolated) return;
    std::lock_guard<std::mutex> lock(nid_mutex_);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    if (!added_) {
        Log::Error("Shell_NotifyIconW(NIM_ADD) failed (err=%lu)", GetLastError());
    }
}

Tray::~Tray() {
    if (added_) Shell_NotifyIconW(NIM_DELETE, &nid_);
    if (ui_process_) CloseHandle(ui_process_);
    if (icon_) DestroyIcon(icon_);
}

void Tray::SetTooltip(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(nid_mutex_);
    wcsncpy_s(nid_.szTip, text.c_str(), _TRUNCATE);
    if (added_) Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::SetAccent(const std::string& accent_key, const std::string& theme_key) {
    std::lock_guard<std::mutex> lock(nid_mutex_);
    accent_key_ = accent_key;
    theme_key_ = theme_key;
    HICON new_icon = RenderEclipseIcon(GetSystemMetrics(SM_CXSMICON), RingColorForAccent(accent_key_, theme_key_));
    if (!new_icon) return;  // keep showing the previous icon rather than falling back to nothing

    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.hIcon  = new_icon;
    if (added_ && !Shell_NotifyIconW(NIM_MODIFY, &nid_)) {
        Log::Warn("Shell_NotifyIconW(NIM_MODIFY) failed for accent change (err=%lu)", GetLastError());
    }
    if (icon_) DestroyIcon(icon_);
    icon_ = new_icon;
}

void Tray::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    std::lock_guard<std::mutex> lock(nid_mutex_);
    if (!added_) return;
    NOTIFYICONDATAW balloon = nid_;
    balloon.uFlags |= NIF_INFO;
    balloon.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(balloon.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(balloon.szInfo, text.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_MODIFY, &balloon)) {
        Log::Warn("Shell_NotifyIconW(NIM_MODIFY, NIF_INFO) failed (err=%lu)", GetLastError());
    }
}

bool Tray::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == taskbar_created_msg_) {
        // Explorer just restarted and every tray icon it was holding is gone — re-add ours (with
        // whatever tooltip text nid_.szTip already has cached, so a restart doesn't also revert
        // the tooltip to the constructor's default).
        AddIcon();
        return true;
    }
    if (msg == callback_msg_) {
        // Legacy (non-versioned) Shell_NotifyIconW callback shape: lParam IS the mouse/keyboard
        // event code directly (WM_RBUTTONUP for a right-click, WM_CONTEXTMENU for the keyboard
        // "menu" key/Shift+F10 on a focused icon), not packed into a word of it — NIM_SETVERSION
        // was never called, so this is the contract in effect.
        const UINT event = static_cast<UINT>(lparam);
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU || event == WM_LBUTTONUP) {
            ShowContextMenu(hwnd);
        }
        return true;
    }
    if (msg == WM_COMMAND) {
        const UINT id = LOWORD(wparam);
        if (id == kMenuIdOpenSettings) {
            HandleOpenSettings();
            return true;
        }
        if (id == kMenuIdExit) {
            if (on_exit_) on_exit_();
            return true;
        }
        if (id == kMenuIdGetLogs) {
            StartBundleExport();
            return true;
        }
        if (id == kMenuIdHeader) {
            HandleOpenSettings();
            return true;
        }
        if (id == kMenuIdSaveClip) {
            if (menu_actions_.save_clip) menu_actions_.save_clip();
            return true;
        }
        if (id == kMenuIdInstallUpdate) {
            if (menu_actions_.install_update) menu_actions_.install_update();
            return true;
        }
        if (id == kMenuIdToggleReplay) {
            if (menu_actions_.toggle_replay) menu_actions_.toggle_replay();
            return true;
        }
        if (id == kMenuIdClipsFolder) {
            if (!menu_actions_.clips_folder) return true;
            std::wstring folder = menu_actions_.clips_folder();
            // Explorer from a worker, not the pump thread that runs the hotkeys (as Get logs).
            std::thread([folder = std::move(folder)] {
                EnsureParentDir(folder + L"\\x");
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (SUCCEEDED(com)) CoUninitialize();
            }).detach();
            return true;
        }
    }
    if (msg == kBundleDoneMessage) {
        std::unique_ptr<BundleDone> done(reinterpret_cast<BundleDone*>(lparam));
        bundle_busy_ = false;
        if (!done) return true;
        if (done->result.ok) {
            Log::Info("diagnostics bundle  %zu files, %.1f MB written", done->result.files,
                      done->result.bytes / (1024.0 * 1024.0));
            ShowBalloon(L"Logs saved", L"Attach the selected .zip to your report.");
        } else {
            Log::Error("diagnostics bundle  not written: %s", done->result.error.c_str());
            ShowBalloon(L"Couldn't save logs", WideFromUtf8(done->result.error));
        }
        return true;
    }
    return false;
}

void Tray::StartBundleExport() {
    if (bundle_busy_.exchange(true)) return;
    const HWND hwnd = hwnd_;
    std::thread([hwnd] {
        auto done  = std::make_unique<BundleDone>();
        done->path = DefaultBundlePath();
        if (done->path.empty()) {
            done->result.error = "no Downloads folder";
        } else {
            done->result = WriteDiagnosticsBundle(done->path);
        }
        // The user asked for it from the menu: showing the file is the expected result. Here, not
        // on the pump thread, which also runs the hotkeys (a WH_KEYBOARD_LL hook must not stall).
        if (done->result.ok) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(done->path.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
            if (SUCCEEDED(com)) CoUninitialize();
        }
        // If the engine window is already gone (exit while exporting), nobody frees it but us.
        if (PostMessageW(hwnd, kBundleDoneMessage, 0, reinterpret_cast<LPARAM>(done.get()))) done.release();
    }).detach();
}

namespace {
std::mutex             g_user_themes_mutex;
std::vector<UserTheme> g_user_themes;
std::vector<UserTheme> TrayUserThemes() {
    std::lock_guard lock(g_user_themes_mutex);
    return g_user_themes;
}
}  // namespace

void SetTrayUserThemes(std::vector<UserTheme> themes) {
    std::lock_guard lock(g_user_themes_mutex);
    g_user_themes = std::move(themes);
}

void Tray::ShowContextMenu(HWND hwnd) {
    // The engine process is DPI-unaware; the menu window takes its thread's awareness when it is
    // created, so this keeps it sharp at 125-200% without touching anything else in the engine.
    const DPI_AWARENESS_CONTEXT prev_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    POINT pt;
    GetCursorPos(&pt);
    UINT dpi = 96, dpi_y = 96;
    GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi, &dpi_y);
    const TrayMenuState s = menu_actions_.state ? menu_actions_.state() : TrayMenuState{};
    // The menu follows whether Ombra's theme (ui.theme) is light, like the overlay; "system" is
    // Windows' app mode.
    const bool dark = !UiThemeIsLight(s.theme, s.theme == "system" && WindowsAppsUseLightTheme(), TrayUserThemes());
    const UxDarkMode& ux = DarkModeApi();
    if (ux.set_preferred_app_mode && dark != menu_dark_.value_or(!dark)) {
        ux.set_preferred_app_mode(dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
        if (ux.flush_menu_themes) ux.flush_menu_themes();
        menu_dark_ = dark;
    }
    const COLORREF fg      = dark ? RGB(255, 255, 255) : RGB(26, 26, 26);
    const int      icon_px = GetSystemMetricsForDpi(SM_CXSMICON, dpi);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        SetThreadDpiAwarenessContext(prev_dpi);
        return;
    }
    std::vector<HBITMAP> bitmaps;
    const auto add = [&](UINT id, const std::wstring& text, HBITMAP bmp, UINT state = 0) {
        MENUITEMINFOW mii{};
        mii.cbSize     = sizeof(mii);
        mii.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
        mii.fType      = MFT_STRING;
        mii.fState     = state;
        mii.wID        = id;
        mii.dwTypeData = const_cast<wchar_t*>(text.c_str());
        if (bmp) {
            bitmaps.push_back(bmp);
            mii.fMask   |= MIIM_BITMAP;
            mii.hbmpItem = bmp;
        }
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);
    };
    const auto glyph = [&](wchar_t g) { return RenderGlyphBitmap(g, icon_px, fg); };
    const auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    const auto with_key = [](const wchar_t* text, const std::wstring& key) {
        return key.empty() ? std::wstring(text) : std::wstring(text) + L"\t" + key;
    };

    if (menu_actions_.state) {
        // The status line: bold (the default item, which also makes a click open Settings) with a
        // coloured dot for its icon. Not owner-drawn: one owner-drawn item turns the whole menu
        // back into the unthemed classic one, and a disabled item's icon is drawn grey.
        std::wstring status;
        COLORREF dot;
        if (!s.replay_on) {
            status = L"Instant Replay off";
            dot    = dark ? RGB(110, 110, 110) : RGB(150, 150, 150);
        } else if (!s.recording) {
            status = L"Waiting for a game";
            dot    = dark ? RGB(245, 158, 11) : RGB(185, 110, 0);
        } else {
            status = (s.game.empty() ? std::wstring(L"Recording") : s.game) + L" \u00b7 last " +
                     FormatBufferLength(s.buffer_seconds);
            dot    = dark ? RGB(239, 68, 68) : RGB(217, 45, 45);
        }
        add(kMenuIdHeader, status, RenderDotBitmap(icon_px, MulDiv(10, dpi, 96), dot), MFS_DEFAULT);
        separator();
        add(kMenuIdSaveClip, with_key(L"Save clip", s.save_hotkey), glyph(0xE74E));
        add(kMenuIdToggleReplay, with_key(L"Instant Replay", s.toggle_hotkey), nullptr,
            s.replay_on ? MFS_CHECKED : 0);
        add(kMenuIdClipsFolder, L"Open clips folder", glyph(0xE838));
        separator();
        add(kMenuIdOpenSettings, with_key(L"Settings", s.overlay_hotkey), glyph(0xE713));
        if (!s.update_ready.empty() && menu_actions_.install_update)
            add(kMenuIdInstallUpdate, L"Install update " + s.update_ready, glyph(0xE896));
    } else {
        add(kMenuIdOpenSettings, L"Open Settings", glyph(0xE713));
    }
    add(kMenuIdGetLogs, L"Get logs", glyph(0xE9D9), bundle_busy_ ? MFS_DISABLED : 0);
    separator();
    add(kMenuIdExit, L"Exit", glyph(0xE7E8));
    // Checks and icons share one column, as in Explorer's menus.
    MENUINFO mi{};
    mi.cbSize  = sizeof(mi);
    mi.fMask   = MIM_STYLE;
    mi.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &mi);
    // A tray context menu needs its host window brought to the foreground first, or the menu
    // doesn't reliably dismiss on an outside click — a documented TrackPopupMenu gotcha for
    // notification-area menus, not specific to this app.
    SetForegroundWindow(hwnd);
    // TPM_RIGHTBUTTON: without it, TrackPopupMenu only *selects* an item on a left-click inside
    // the menu — the menu opened on a right-click (the common case here) would otherwise ignore a
    // right-click on "Exit" entirely, the standard gotcha for notification-area context menus.
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                   nullptr);
    // Companion to the SetForegroundWindow call above: without this extra post, the menu can fail
    // to close if the app doesn't otherwise receive a message right after TrackPopupMenu returns.
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    for (HBITMAP b : bitmaps) DeleteObject(b);
    SetThreadDpiAwarenessContext(prev_dpi);
}

void Tray::OpenOverlay() {
    if (!FindWindowW(kOverlayWindowClassW, nullptr)) HandleOpenSettings();
}

void Tray::ToggleOverlay() {
    HWND existing = FindWindowW(kOverlayWindowClassW, nullptr);
    if (existing) {
        PostMessageW(existing, WM_CLOSE, 0, 0);
        return;
    }
    HandleOpenSettings();
}

std::wstring ResolveOmbraUiExePath() {
    wchar_t exe_path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log::Error("GetModuleFileNameW failed, can't locate ombra-ui.exe next to it (err=%lu)",
                   GetLastError());
        return L"";
    }
    std::wstring dir(exe_path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir + L"" OMBRA_UI_EXE;
}

void Tray::HandleOpenSettings() {
    if (ui_process_) {
        if (WaitForSingleObject(ui_process_, 0) == WAIT_TIMEOUT) {
            // Still running — focus it instead of spawning a duplicate. Best-effort.
            FindWindowByPidCtx ctx{ui_pid_};
            EnumWindows(FindWindowByPidProc, reinterpret_cast<LPARAM>(&ctx));
            if (ctx.result) {
                ShowWindow(ctx.result, SW_RESTORE);
                SetForegroundWindow(ctx.result);
            }
            return;
        }
        CloseHandle(ui_process_);
        ui_process_ = nullptr;
        ui_pid_     = 0;
    }

    const std::wstring ui_path = ResolveOmbraUiExePath();
    if (ui_path.empty()) return;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + ui_path + L"\"";
    if (!CreateProcessW(ui_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         nullptr, &si, &pi)) {
        Log::Error("CreateProcessW failed for %ls (err=%lu)", ui_path.c_str(), GetLastError());
        return;
    }
    // This call is what's actually reaching HandleOpenSettings right now: the overlay hotkey's
    // WM_HOTKEY delivery, which Windows treats as a legitimate foreground-change trigger — but
    // that permission doesn't carry over to a freshly spawned child process on its own.  Without
    // this, ombra-ui.exe's own OverlayHost::Show() -> SetForegroundWindow() gets silently denied:
    // the topmost window still draws on top (so it *looks* focused), but doesn't actually own
    // keyboard focus until the user clicks it — reported as "esc runs through to the
    // game unless I click the overlay first" while testing the new poll hotkey backend.
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    ui_process_ = pi.hProcess;
    ui_pid_     = pi.dwProcessId;
}

}  // namespace ombra
