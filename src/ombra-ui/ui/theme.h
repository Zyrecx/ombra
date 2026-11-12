#pragma once

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1_3.h>    // ID2D1DeviceContext5 -- SVG icon rendering
#include <d2d1svg.h>   // ID2D1SvgDocument
#include <dwrite.h>
#include <wrl/client.h>
#include "ombra-ui/ui/draw_probe.h"
#include "ombra-ui/ui/fonts.h"
#include "ombra/ui_palette.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace ombra { struct JsonValue; }

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

// Which of the five accent presets is active, Custom (ui.accent "#rrggbb", Theme::accent_rgb) or
// Theme (the palette's own, ombra::UiThemeAccentRgb, mono where it has none).
// Kept alongside Theme so widgets that need to know "is this the mono accent" (e.g. the swatch
// picker's gradient look) don't have to compare colors.
enum class AccentId {
    Mono,
    Violet,
    Cyan,
    Blue,
    Green,  // NVIDIA/GeForce NOW green (#76b900)
    Custom,
    Theme
};

// The accent picker's preset swatches, left to right (its custom swatch follows them); the first,
// the theme's own, stands apart.
inline constexpr AccentId kAccentSwatches[] = {AccentId::Theme, AccentId::Mono, AccentId::Violet,
                                               AccentId::Cyan, AccentId::Blue, AccentId::Green};
inline constexpr int kAccentSwatchCount = static_cast<int>(std::size(kAccentSwatches));
// id's swatch in kAccentSwatches, or kAccentSwatchCount (the custom one) for Custom.
inline int AccentSwatchIndex(AccentId id) {
    for (int i = 0; i < kAccentSwatchCount; ++i)
        if (kAccentSwatches[i] == id) return i;
    return kAccentSwatchCount;
}

// --- Design tokens.
// Shape first ported from Noctalia's ui/style.h (MIT). Everything draws from these:
// Theme::color, Theme::size, Theme::Font().

// ui.density: spacing and control heights x0.85 / 1 / 1.15, rounded to whole DIPs. Type never scales.
enum class Density { Compact, Default, Comfortable };
float DensityScale(Density d);

// Type roles: one size and weight each. Alignment, wrapping and trimming are options of the call
// (TextOpts), not separate roles.
enum class TypeRole { Display, Title, Label, Body, Caption, Mono };
struct TypeSpec { float size; DWRITE_FONT_WEIGHT weight; bool mono; };
TypeSpec SpecFor(TypeRole role);  // Display 20 / Title 16 semibold, Label 13 semibold, Body 13,
                                  // Caption 11, Mono 13 (Cascadia Code)
// A single line's box for a role in DIPs (size x 4/3, rounded up): Caption 15, Body 18.
inline float LineHeight(TypeRole role) { return std::ceil(SpecFor(role).size * 4.0f / 3.0f); }
enum class TextAlign { Leading, Center, Trailing };
struct TextOpts {
    TextAlign align = TextAlign::Leading;
    bool wrap = false;     // single line unless asked
    bool trim = false;     // "…" when a single line doesn't fit
    bool vcenter = true;   // centred in its box; false = top-aligned (wrapping paragraphs)
};

// Motion: hover/selection fades, popups fade in, press is instant. Nothing loops or moves layout,
// and all of it is off when Windows "Animation effects" is off (AnimationsEnabled). Performance first
//: frames only during a transition, none at rest. Not built yet.
constexpr int kFadeMs      = 100;
constexpr int kPopupFadeMs = 120;
bool AnimationsEnabled();  // SPI_GETCLIENTAREAANIMATION, read per call (cheap)

// Colours named by job. Hover and pressed are layers of `text` at hover_a / pressed_a over
// whatever surface is underneath. The accent roles all derive from the one picked colour
// (ui/color.h DeriveAccent); status hues are never the accent.
struct ColorRoles {
    D2D1_COLOR_F scrim;                        // over the whole screen while the overlay is open
    D2D1_COLOR_F panel, card, raised;          // surface steps: panel < card < raised (menus, tooltips, dialogs)
    D2D1_COLOR_F line, line_strong, focus;     // card edge, control edge, keyboard focus ring
    D2D1_COLOR_F text, text2, text3, text_off; // primary, secondary, muted (help), disabled
    D2D1_COLOR_F success, warning, danger, info, recording;
    float hover_a = 0.06f, pressed_a = 0.10f;  // text-colour layers
    float soft_a = 0.14f, line_a = 0.45f;      // picked-accent tint and outline (soft selection)
    D2D1_COLOR_F accent, accent_hover, accent_pressed;  // fill (3:1 on card) and its states
    D2D1_COLOR_F on_accent;                    // text/icons on the fill: white or near-black (PrefersWhiteText)
    D2D1_COLOR_F accent_text;                  // accent as text/icon: 4.5:1 on card, raised, hover
    D2D1_COLOR_F accent_soft, accent_line;     // picked colour at soft_a / line_a
    bool accent_near_danger = false;           // the picker shows a note
    D2D1_COLOR_F HoverLayer(float k = 1.0f) const { return D2D1::ColorF(text.r, text.g, text.b, hover_a * k); }  // k: a hover fade
    D2D1_COLOR_F PressedLayer() const { return D2D1::ColorF(text.r, text.g, text.b, pressed_a); }
};

// Sizes in DIPs (multiply by dpi_scale at the draw site, like every other token). Spacing is a
// 4 px grid; density scales spacing and control heights, ui.corners scales radii.
struct Sizes {
    float density = 1.0f;
    float corners = 1.0f;
    bool card_lines = true;  // ui.card_borders: a card's 1 DIP outline (Paint::CardEdge); controls keep theirs
    float S(float dip) const { return static_cast<float>(static_cast<int>(dip * density + 0.5f)); }
    float space_xxs() const { return S(2); }
    float space_xs() const  { return S(4); }
    float space_sm() const  { return S(8); }
    float space_md() const  { return S(12); }
    float space_lg() const  { return S(16); }
    float space_xl() const  { return S(24); }
    float space_xxl() const { return S(32); }
    float control_xs() const { return S(24); }  // inline chips, colour swatches
    float control_sm() const { return S(28); }  // segmented, dropdown, field, menu item
    float control() const    { return S(32); }  // buttons
    float control_lg() const { return S(40); }
    float card_padding() const { return S(16); }
    float switch_w() const { return S(36); }
    float switch_h() const { return S(20); }
    float miniature_h() const { return S(72); }  // position picker's 16:9 screen
    float row_text_min() const { return S(160); }  // a row narrower than this for its text wraps its control under it
    // Glyphs sit with the text, so like type they ignore density: icon box (also the caret's
    // height) and drawn marks (chevron, +/-). Stroke weights (1, 1.5, 2 DIP) stay literal.
    float icon() const  { return 14; }
    float glyph() const { return 10; }
    float app_icon() const { return 28; }  // a program's own icon before a list item (a game)
    bool pill = false;  // ui.corners=round: controls are pills
    float radius_inner() const   { return 4.0f * corners; }  // keycaps, marks inside a control
    float radius_control() const { return 6.0f * corners; }
    // A control's corner for its height `h` (pixels; `s` is the DPI scale): radius_control, or half
    // the height when ui.corners=round. Buttons, fields, menus' openers, segmented, steppers and
    // keys use these; menus, cards, thumbnails and hover fills keep radius_control.
    float control_radius(float h, float s) const { return pill ? h * 0.5f : radius_control() * s; }
    float key_radius(float h, float s) const     { return pill ? h * 0.5f : radius_inner() * s; }
    // A shape that is fully round by design (switch, chips, tags, the rail's pill): half its height
    // `h` (pixels), except that ui.corners=sharp squares it off like every other control.
    // `s` is the DPI scale.
    float radius_full(float h, float s) const { return corners < 1.0f ? (std::min)(h * 0.5f, 6.0f * corners * s) : h * 0.5f; }
    float radius_card() const    { return 8.0f * corners; }
    float radius_panel() const   { return 12.0f * corners; }
};

// Placement of the overlay's content panel. Center floats in the middle; the other four hug that
// screen edge. PanelAttach::Attached additionally fuses the panel into the edge (concave fillets,
// see ui/attached_panel.h) and is only meaningful on an edge position -- Center stays floating.
// Both come from config (ui.panel_position / ui.panel_attach); the dead ui.panel_edge_style key
// this replaces never had a renderer.
enum class PanelPosition { Top, Center, Bottom, Left, Right };
// ui.rail_position: which side of the Settings panel the tab rail sits on. Left/Right stack it
// vertically beside the content card; Top/Bottom lay it out as a horizontal strip above/below.
enum class RailPosition { Left, Right, Top, Bottom };
enum class PanelAttach { Floating, Attached };
// ui.theme: the whole palette (surfaces, borders, text, semantic hues), not just the accent.
enum class ThemeMode { Dark, Light };
// ui.screen_frame_color: Match is the panel surface (they fuse into one); Contrast is white on dark /
// black on light; Accent is the accent fill.
enum class ScreenFrameColor { Match, Contrast, Accent };

// The overlay's theme: the ui.* appearance keys and the tokens derived from them. Every draw site
// reads colours, sizes and fonts from here; no literal colours at a call site.
struct Theme {
    Theme();  // derives `color` for the default dark/mono palette

    ThemeMode mode = ThemeMode::Dark;
    AccentId accent_id = AccentId::Theme;  // ui.accent's default; on Dark that is mono
    uint32_t accent_rgb = 0;  // 0xRRGGBB, meaningful when accent_id == Custom
    Density density = Density::Default;

    // Tokens (see ColorRoles / Sizes above). Refreshed by ApplyMode/ApplyAccent/ApplyUiConfig.
    ColorRoles color;
    Sizes size;
    // The text format for a role with these options, built at font_scale on first use and cached
    // until the scale changes. Null before InitFonts.
    IDWriteTextFormat* Font(TypeRole role, TextOpts opts = {}) const;
    // The colour the user picked, before contrast adjusts it: what the brand mark's ring shows, the
    // same as the tray icon (tray.cpp RingColorForAccent), so mono is the pale one in both modes.
    uint32_t PickedRgb() const;
    // What AccentId::Theme picks for the current palette and mode (the mono one where it has none).
    uint32_t ThemeAccentRgb() const;
    PanelPosition panel_position = PanelPosition::Top;
    PanelAttach panel_attach = PanelAttach::Floating;
    RailPosition rail_position = RailPosition::Left;
    // Screen frame (ui.screen_frame*): only drawn while the overlay is open. Radius/thickness in DIPs.
    bool  screen_frame = false;
    // ui.memory_card: the RAM-cost card beside the Settings card (SettingsScreen::Layout).
    bool  memory_card = true;
    float screen_frame_radius = 12.0f;
    float screen_frame_thickness = 8.0f;
    ScreenFrameColor screen_frame_color = ScreenFrameColor::Match;

    ComPtr<IDWriteFactory> dwrite;  // kept for Font/MeasureText; set by InitFonts
    bool InitFonts(IDWriteFactory* dwrite_factory);

    // The D2D contexts draw at 96 DPI, so layout is in physical pixels (every widget multiplies
    // its DIP geometry by dpi_scale). Fonts have to follow the same rule or text stays 100%-sized
    // inside 150%-sized controls: the formats are built at size * font_scale. Hosts call this with
    // the window's dpi_scale once it's known; it drops the cached formats only when the scale changes.
    float font_scale = 1.0f;
    void SetFontScale(float scale);

    // ui.font (fonts.h). Changing it drops the cached formats, like a scale change.
    UiFont ui_font = UiFont::Inter;
    void SetUiFont(UiFont f);
    // What each host sets on its D2D context before drawing text (OverlayHost::RenderFrame, the toast,
    // the tests' HeadlessDraw): Inter and Geist carry no hinting, so DirectWrite's vertical
    // grid-fitting rounds their x-height up a pixel (13 px Inter drew 12% taller).
    // Grid-fit off for them; null (DirectWrite's default) for hinted Segoe.
    IDWriteRenderingParams* TextRendering() const;
    IDWriteRenderingParams* NoGridFit() const;  // what TextRendering returns for Inter and Geist
    // The OMBRA wordmark beside the mark: Display in Inter whatever ui.font is, so the brand looks
    // the same for everyone. Draw it under NoGridFit().
    IDWriteTextFormat* WordmarkFont() const;

    // Real DirectWrite measurement, in the same physical pixels as layout (the formats are already
    // DPI-scaled, so never multiply the result by dpi_scale). Unwrapped unless `max_w` is given.
    // Builds a throwaway layout, so cache the result if you call it per frame. Returns {0,0} if
    // fonts aren't initialised.
    D2D1_SIZE_F MeasureText(IDWriteTextFormat* fmt, const std::wstring& text, float max_w = 1.0e6f) const;

    void ApplyAccent(AccentId id);
    // A ui.accent value: a preset name or "#rrggbb". Anything else leaves the accent alone.
    void ApplyAccentKey(const std::string& key);
    // Ombra's own Dark or Light palette (what the setup screen previews).
    void ApplyMode(ThemeMode m);
    // A ui.theme value (ombra/ui_palette.h): a palette, "system" (Dark or Light by Windows' app
    // mode, ThemeModeFromKey) or a user theme's id (user_themes). Unknown keys are Dark.
    void ApplyThemeKey(const std::string& key);
    // ui.themes: the user's own themes, by id. Change it, then ApplyThemeKey, to redraw with it.
    std::vector<ombra::UserTheme> user_themes;
    // The ui.theme the roles came from after resolving "system": a palette key or a user theme id.
    const std::string& palette_key() const { return palette_key_; }

    // Applies the ui.* appearance keys (accent, panel position/attach, screen frame) from a config
    // source. `get` returns the effective value for a key or nullptr. One entry point on purpose:
    // Show() calls it with the engine's saved config, Settings calls it with saved-plus-staged so
    // the overlay previews an edit live and Discard puts it back. Unknown/missing keys leave the
    // current value alone.
    void ApplyUiConfig(const std::function<const ombra::JsonValue*(const char*)>& get);

    // --- SVG icon cache ---
    // Device-dependent (ID2D1SvgDocument is bound to the D2D device that created it), so it lives
    // here rather than a file-static in widget.cpp: Theme is an OverlayHost member whose lifetime
    // already matches d2d_context_'s 1:1 (InitFonts() is called right after device creation, see
    // overlay_window.cpp's InitGraphics -- calling ResetIconCache() from the same place keeps a
    // device recreate from leaving a stale doc bound to a dead device). Mutable because Draw()
    // takes Theme by const& throughout the widget layer, and this is populated lazily on first
    // use per icon by widget.cpp's GetOrCreateSvgIcon, not eagerly here.
    mutable ComPtr<ID2D1DeviceContext5> svg_ctx5;         // QI'd once from whatever dc first draws an icon
    mutable std::vector<ComPtr<ID2D1SvgDocument>> svg_icon_cache;  // indexed by IconType; empty slot = not loaded yet / fell back
    void ResetIconCache() const { svg_ctx5.Reset(); svg_icon_cache.clear(); }

private:
    void RefreshRoles();  // `color` from mode + palette + accent
    std::string palette_key_ = "dark";
    // [role][align][wrap][trim][vcenter]
    mutable ComPtr<IDWriteTextFormat> type_cache_[6][3][2][2][2];
    mutable ComPtr<IDWriteRenderingParams> no_grid_fit_;  // built on first NoGridFit()
    mutable ComPtr<IDWriteTextFormat> wordmark_format_;
    void DropFonts();
};

AccentId AccentIdFromKey(const std::string& key);   // "theme"|"mono"|"violet"|"cyan"|"blue"|"green" -> id; "#rrggbb" -> Custom (default Theme)
Density DensityFromKey(const std::string& key);     // "compact"|"default"|"comfortable" (default Default)
uint32_t AccentPickedRgb(AccentId id, ThemeMode mode);  // the picked colour behind a preset (mono is per mode)
const char* AccentIdToKey(AccentId id);             // id -> config-file string

PanelPosition PanelPositionFromKey(const std::string& key);  // "top"|"center"|"bottom"|"left"|"right" -> id (default Top)
RailPosition RailPositionFromKey(const std::string& key);    // "left"|"right"|"top"|"bottom" -> id (default Left)
PanelAttach PanelAttachFromKey(const std::string& key);      // "floating"|"attached" -> id (default Floating)
ScreenFrameColor ScreenFrameColorFromKey(const std::string& key);  // "match"|"contrast"|"accent" (old "dark"/"white" accepted; default Match)
// "dark"|"light"|"system" (default Dark). "system" asks the probe: Windows' app mode by default;
// headless renders (--drive, --snapshots) pin it to dark so a PNG doesn't depend on the machine.
ThemeMode ThemeModeFromKey(const std::string& key);
void SetSystemThemeLightProbe(bool (*probe)());

}  // namespace ombra_ui
