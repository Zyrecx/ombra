// ui.theme's palettes: Ombra's own and a few well-known community ones, plus the user's own (ui.themes).
// The one table both processes read: the overlay derives its colour roles from it (ui/theme.cpp),
// the engine only asks whether a theme is light (the tray menu, which follows light/dark).
//
// Community palettes take their colours from each project's published palette, all MIT-licensed:
// Catppuccin (github.com/catppuccin/catppuccin, Mocha and Latte), Nord (github.com/nordtheme/nord),
// Dracula (github.com/dracula/dracula-theme), Gruvbox (github.com/morhetz/gruvbox, dark hard) and
// Rosé Pine (github.com/rose-pine/rose-pine-theme). Where a project's own muted tone reads below
// 4.5:1 on its card colour, the muted slot uses a lighter tone of the same hue. Each one's accent is
// the hue the project itself leads with: Catppuccin mauve, Nord frost (nord8), Dracula purple,
// Gruvbox bright yellow, Rosé Pine rose. Ombra's own and these all take dark text on the fill.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ombra {

// The six colours a palette (and a user theme) sets; the rest of the roles derive from them.
struct UiPaletteColors {
    uint32_t background;  // the panel behind every card
    uint32_t cards;
    uint32_t menus;       // raised surfaces: menus, tooltips, dialogs
    uint32_t lines;       // card edges and dividers
    uint32_t text;
    uint32_t muted;       // help text
};

struct UiPalette {
    const char* key;      // the ui.theme value
    const wchar_t* name;  // the tile's label
    bool light;           // status hues, scrim and the tray menu follow light or dark
    UiPaletteColors colors;
    uint32_t accent;      // ui.accent "theme" picks it; 0: the mono accent
};

// Settings shows them in this order, then System, then the user's themes. "dark" and "light" are Ombra's
// hand-tuned palettes; their other roles are tuned too rather than derived (theme.cpp).
inline constexpr UiPalette kUiPalettes[] = {
    {"dark",     L"Dark",       false, {0x050506, 0x0E0E11, 0x18181D, 0x232329, 0xF2F4F8, 0x7D8492}, 0},
    {"light",    L"Light",      true,  {0xF3F3F6, 0xFBFBFD, 0xFFFFFF, 0xDCDCE3, 0x16171C, 0x666D7C}, 0},
    {"black",    L"True black", false, {0x000000, 0x0E0E0E, 0x1A1A1A, 0x2A2A2A, 0xF2F4F8, 0x7D8492}, 0},
    {"slate",    L"Slate",      false, {0x141A24, 0x1B2331, 0x263042, 0x34405A, 0xF2F4F8, 0x8D96A8}, 0x8AB4F8},
    {"ember",    L"Ember",      false, {0x201A16, 0x2B2420, 0x38302A, 0x4C423A, 0xF2E8DC, 0xA69888}, 0xF5A05A},
    {"forest",   L"Forest",     false, {0x131C18, 0x1A2620, 0x24332B, 0x34473C, 0xEEF4F0, 0x8E9F95}, 0x5FC48A},
    {"mocha",    L"Catppuccin", false, {0x11111B, 0x1E1E2E, 0x313244, 0x313244, 0xCDD6F4, 0xA6ADC8}, 0xCBA6F7},
    {"latte",    L"Latte",      true,  {0xDCE0E8, 0xEFF1F5, 0xFFFFFF, 0xCCD0DA, 0x4C4F69, 0x5C5F77}, 0x8839EF},
    {"nord",     L"Nord",       false, {0x2E3440, 0x3B4252, 0x434C5E, 0x4C566A, 0xECEFF4, 0xB4BCCB}, 0x88C0D0},
    {"dracula",  L"Dracula",    false, {0x21222C, 0x282A36, 0x343746, 0x44475A, 0xF8F8F2, 0xA7ABBE}, 0xBD93F9},
    {"gruvbox",  L"Gruvbox",    false, {0x1D2021, 0x282828, 0x3C3836, 0x3C3836, 0xEBDBB2, 0xA89984}, 0xFABD2F},
    {"rosepine", L"Rosé Pine",  false, {0x191724, 0x1F1D2E, 0x26233A, 0x403D52, 0xE0DEF4, 0x908CAA}, 0xEBBCBA},
};

inline const UiPalette* FindUiPalette(std::string_view key) {
    for (const UiPalette& p : kUiPalettes)
        if (key == p.key) return &p;
    return nullptr;
}

// WCAG relative luminance of 0xRRGGBB.
inline double RelativeLuminance(uint32_t rgb) {
    auto lin = [](uint32_t c) {
        const double v = c / 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255);
}

inline double ContrastRatio(uint32_t a, uint32_t b) {
    const double la = RelativeLuminance(a), lb = RelativeLuminance(b);
    return ((la > lb ? la : lb) + 0.05) / ((la > lb ? lb : la) + 0.05);
}

// A user theme's background is light when black text would read better on it than white.
inline bool UiBackgroundIsLight(uint32_t background) { return ContrastRatio(background, 0x000000) > ContrastRatio(background, 0xFFFFFF); }

// One of the user's own themes (ui.themes). `id` is what ui.theme stores, so a rename keeps the
// selection: "user" and a number, never a preset key.
struct UserTheme {
    std::string id;
    std::string name;  // UTF-8
    UiPaletteColors colors{};
};
inline constexpr size_t kMaxUserThemes = 24;
inline constexpr size_t kMaxUserThemeName = 32;  // characters

inline bool IsUserThemeId(std::string_view s) {
    if (s.size() < 5 || s.size() > 8 || s.substr(0, 4) != "user" || s[4] == '0') return false;
    for (size_t i = 4; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

inline const UserTheme* FindUserTheme(const std::vector<UserTheme>& themes, std::string_view id) {
    for (const UserTheme& t : themes)
        if (t.id == id) return &t;
    return nullptr;
}

// The first free "userN".
inline std::string NextUserThemeId(const std::vector<UserTheme>& themes) {
    for (int n = 1;; ++n) {
        std::string id = "user" + std::to_string(n);
        if (!FindUserTheme(themes, id)) return id;
    }
}

// The accent ui.accent "theme" resolves to for a ui.theme value, 0xRRGGBB, or 0 for the mono accent:
// Dark, Light, System, True black and the user's own themes, which set no accent.
inline uint32_t UiThemeAccentRgb(std::string_view theme) {
    const UiPalette* p = FindUiPalette(theme);
    return p ? p->accent : 0;
}

// Whether ui.theme resolves to a light palette. `system_light` is Windows' app mode (for "system").
// A user theme missing from `themes`, like any unknown key, is Dark, the default.
inline bool UiThemeIsLight(std::string_view theme, bool system_light, const std::vector<UserTheme>& themes) {
    if (theme == "system") return system_light;
    if (const UserTheme* u = FindUserTheme(themes, theme)) return UiBackgroundIsLight(u->colors.background);
    const UiPalette* p = FindUiPalette(theme);
    return p && p->light;
}

}  // namespace ombra