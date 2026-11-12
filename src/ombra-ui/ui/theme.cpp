#include "ombra-ui/ui/theme.h"

#include "ombra/config.h"
#include "ombra/json.h"
#include "ombra/util.h"
#include "ombra-ui/ui/color.h"

#include <windows.h>

namespace ombra_ui {

namespace {

ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* f, const wchar_t* family, IDWriteFontCollection* collection,
                                     DWRITE_FONT_WEIGHT weight, float size, DWRITE_TEXT_ALIGNMENT h,
                                     DWRITE_PARAGRAPH_ALIGNMENT v) {
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = f->CreateTextFormat(family, collection, weight, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &fmt);
    if (FAILED(hr) || !fmt) return nullptr;
    fmt->SetTextAlignment(h);
    fmt->SetParagraphAlignment(v);
    return fmt;
}

}  // namespace

bool Theme::InitFonts(IDWriteFactory* f) {
    if (!f) return false;
    dwrite = f;
    // Called once, right after D2D device/context creation (overlay_window.cpp's InitGraphics) --
    // Theme's lifetime already matches d2d_context_'s 1:1, so this is also where the SVG icon
    // cache (device-dependent) gets torn down, keeping a future device-recreate path from leaving
    // a stale ID2D1SvgDocument bound to a dead device. See theme.h's ResetIconCache() comment.
    ResetIconCache();
    DropFonts();
    no_grid_fit_.Reset();
    return Font(TypeRole::Body) != nullptr;
}

void Theme::DropFonts() {
    for (auto& a : type_cache_) for (auto& b : a) for (auto& c : b) for (auto& d : c) for (auto& e : d) e.Reset();
    wordmark_format_.Reset();
}

void Theme::SetFontScale(float scale) {
    if (scale <= 0.0f || scale == font_scale) return;
    font_scale = scale;
    DropFonts();
}

void Theme::SetUiFont(UiFont f) {
    if (f == ui_font) return;
    ui_font = f;
    DropFonts();
}

IDWriteRenderingParams* Theme::TextRendering() const {
    return ui_font == UiFont::System ? nullptr : NoGridFit();
}

IDWriteTextFormat* Theme::WordmarkFont() const {
    if (!dwrite) return nullptr;
    if (!wordmark_format_) {
        IDWriteFontCollection* inter = EmbeddedFontCollection(dwrite.Get(), UiFont::Inter);
        const TypeSpec s = SpecFor(TypeRole::Display);
        wordmark_format_ = MakeFormat(dwrite.Get(), inter ? L"Inter" : L"Segoe UI", inter, s.weight, s.size * font_scale,
                                      DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (wordmark_format_) wordmark_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return wordmark_format_.Get();
}

IDWriteRenderingParams* Theme::NoGridFit() const {
    if (!dwrite) return nullptr;
    if (!no_grid_fit_) {
        ComPtr<IDWriteFactory3> f3;
        ComPtr<IDWriteRenderingParams> def;
        if (FAILED(dwrite.As(&f3)) || FAILED(f3->CreateRenderingParams(&def))) return nullptr;
        ComPtr<IDWriteRenderingParams3> p;
        if (FAILED(f3->CreateCustomRenderingParams(def->GetGamma(), def->GetEnhancedContrast(),
                                                   def->GetEnhancedContrast(), def->GetClearTypeLevel(),
                                                   def->GetPixelGeometry(), DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC,
                                                   DWRITE_GRID_FIT_MODE_DISABLED, &p)))
            return nullptr;
        no_grid_fit_ = p;
    }
    return no_grid_fit_.Get();
}

TypeSpec SpecFor(TypeRole role) {
    switch (role) {
        case TypeRole::Display: return {20, DWRITE_FONT_WEIGHT_SEMI_BOLD, false};
        case TypeRole::Title:   return {16, DWRITE_FONT_WEIGHT_SEMI_BOLD, false};
        case TypeRole::Label:   return {13, DWRITE_FONT_WEIGHT_SEMI_BOLD, false};
        case TypeRole::Body:    return {13, DWRITE_FONT_WEIGHT_REGULAR, false};
        case TypeRole::Caption: return {11, DWRITE_FONT_WEIGHT_REGULAR, false};
        case TypeRole::Mono:    return {13, DWRITE_FONT_WEIGHT_REGULAR, true};
    }
    return {13, DWRITE_FONT_WEIGHT_REGULAR, false};
}

IDWriteTextFormat* Theme::Font(TypeRole role, TextOpts o) const {
    if (!dwrite) return nullptr;
    const int ri = static_cast<int>(role), ai = static_cast<int>(o.align);
    ComPtr<IDWriteTextFormat>& slot = type_cache_[ri][ai][o.wrap][o.trim][o.vcenter];
    if (slot) return slot.Get();
    const TypeSpec s = SpecFor(role);
    const DWRITE_TEXT_ALIGNMENT h = o.align == TextAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                  : o.align == TextAlign::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                   : DWRITE_TEXT_ALIGNMENT_LEADING;
    // ui.font: Inter and Geist from the embedded collection; System is Segoe UI Variable, whose optical
    // sizes are separate families (Small for captions, Display for the largest role). Text in a
    // script the face lacks (a CJK game name) falls back to a system font.
    const wchar_t* family = L"Cascadia Code";
    IDWriteFontCollection* collection = nullptr;
    if (!s.mono) {
        switch (ui_font) {
            case UiFont::Inter:  family = L"Inter"; break;
            case UiFont::Geist:  family = L"Geist"; break;
            case UiFont::System:
                family = role == TypeRole::Caption ? L"Segoe UI Variable Small"
                       : role == TypeRole::Display ? L"Segoe UI Variable Display"
                                                   : L"Segoe UI Variable Text";
                break;
        }
        collection = EmbeddedFontCollection(dwrite.Get(), ui_font);
        if (ui_font != UiFont::System && !collection) family = L"Segoe UI";  // resources missing
    }
    slot = MakeFormat(dwrite.Get(), family, collection, s.weight, s.size * font_scale, h,
                      o.vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (!slot) return nullptr;
    slot->SetWordWrapping(o.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (o.trim) {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        // The sign takes this format's font and size, so each trimmed format gets its own.
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(slot.Get(), &ellipsis))) slot->SetTrimming(&trimming, ellipsis.Get());
    }
    return slot.Get();
}

float DensityScale(Density d) {
    return d == Density::Compact ? 0.85f : d == Density::Comfortable ? 1.15f : 1.0f;
}

Density DensityFromKey(const std::string& key) {
    if (key == "compact") return Density::Compact;
    if (key == "comfortable") return Density::Comfortable;
    return Density::Default;
}

bool AnimationsEnabled() {
    BOOL on = TRUE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

Theme::Theme() { RefreshRoles(); }

uint32_t AccentPickedRgb(AccentId id, ThemeMode mode) {
    switch (id) {
        case AccentId::Violet: return 0xa78bfa;
        case AccentId::Cyan:   return 0x22d3ee;
        case AccentId::Blue:   return 0x5b9bff;
        case AccentId::Green:  return 0x76b900;
        default:               return mode == ThemeMode::Light ? 0x16171c : 0xe6e8ee;  // mono
    }
}

void Theme::RefreshRoles() {
    auto c = [](uint32_t h, float a = 1.0f) {
        return D2D1::ColorF(((h >> 16) & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, (h & 255) / 255.0f, a);
    };
    const bool light = mode == ThemeMode::Light;
    ColorRoles& r = color;
    if (light) {
        r.panel = c(0xf3f3f6); r.card = c(0xfbfbfd); r.raised = c(0xffffff);
        r.line = c(0xdcdce3); r.line_strong = c(0xc3c3cd); r.focus = c(0x16171c);
        r.text = c(0x16171c); r.text2 = c(0x4f5566); r.text3 = c(0x666d7c); r.text_off = c(0x9096a3);
        r.success = c(0x0a8f63); r.warning = c(0xb96e00); r.danger = c(0xd92d2d); r.info = c(0x2563eb);
    } else {
        r.panel = c(0x050506); r.card = c(0x0e0e11); r.raised = c(0x18181d);
        r.line = c(0x232329); r.line_strong = c(0x393942); r.focus = c(0xd8dbe4);
        r.text = c(0xf2f4f8); r.text2 = c(0xa0a6b5); r.text3 = c(0x7d8492); r.text_off = c(0x5f6675);
        r.success = c(0x10b981); r.warning = c(0xf59e0b); r.danger = c(0xef4444); r.info = c(0x7fb0ff);
    }
    r.recording = c(0xef4444);
    r.scrim = D2D1::ColorF(0, 0, 0, light ? 0.30f : 0.35f);

    auto rgb = [](const D2D1_COLOR_F& d) { return Rgb{d.r, d.g, d.b}; };
    auto d2d = [](Rgb v, float a = 1.0f) { return D2D1::ColorF(v.r, v.g, v.b, a); };

    // Any palette but Ombra's own two sets six colours; the rest derive the way Dark's tuned
    // values relate to its six (control edge 12% toward text, secondary text 30% from muted
    // toward text, disabled 28% from muted toward the card). Status hues stay the mode's.
    if (palette_key_ != "dark" && palette_key_ != "light") {
        const ombra::UiPalette* p = ombra::FindUiPalette(palette_key_);
        const ombra::UserTheme* u = p ? nullptr : ombra::FindUserTheme(user_themes, palette_key_);
        const ombra::UiPaletteColors& pc = p ? p->colors : u ? u->colors : ombra::kUiPalettes[0].colors;
        r.panel = c(pc.background); r.card = c(pc.cards); r.raised = c(pc.menus);
        r.line = c(pc.lines); r.text = c(pc.text); r.text3 = c(pc.muted);
        const Rgb text = RgbFromHex(pc.text), muted = RgbFromHex(pc.muted);
        r.line_strong = d2d(Mix(RgbFromHex(pc.lines), text, 0.12f));
        r.text2 = d2d(Mix(muted, text, 0.30f));
        r.text_off = d2d(Mix(muted, RgbFromHex(pc.cards), 0.28f));
        r.focus = d2d(Mix(text, RgbFromHex(pc.cards), 0.08f));
    }
    const Rgb card = rgb(r.card);
    const Rgb picked = RgbFromHex(accent_id == AccentId::Custom  ? accent_rgb
                                  : accent_id == AccentId::Theme ? ThemeAccentRgb()
                                                                 : AccentPickedRgb(accent_id, mode));
    const AccentRoles a = DeriveAccent(picked, card, rgb(r.raised), Mix(card, rgb(r.text), r.hover_a), light);
    r.accent = d2d(a.fill);
    r.accent_hover = d2d(a.fill_hover);
    r.accent_pressed = d2d(a.fill_pressed);
    r.on_accent = d2d(a.on_fill);
    r.accent_text = d2d(a.text);
    r.accent_soft = d2d(picked, r.soft_a);
    r.accent_line = d2d(picked, r.line_a);
    r.accent_near_danger = NearStatusHue(picked, rgb(r.danger));
}

D2D1_SIZE_F Theme::MeasureText(IDWriteTextFormat* fmt, const std::wstring& text, float max_w) const {
    if (!dwrite || !fmt || text.empty()) return D2D1::SizeF(0.0f, 0.0f);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, max_w, 1.0e6f, &layout)) ||
        !layout) {
        return D2D1::SizeF(0.0f, 0.0f);
    }
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return D2D1::SizeF(0.0f, 0.0f);
    return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
}

void Theme::ApplyAccentKey(const std::string& key) {
    if (uint32_t rgb = 0; ombra::ParseHexRgb(key, &rgb)) {
        accent_rgb = rgb;
        ApplyAccent(AccentId::Custom);
    } else {
        ApplyAccent(AccentIdFromKey(key));
    }
}

void Theme::ApplyAccent(AccentId id) {
    accent_id = id;
    RefreshRoles();
}

void Theme::ApplyMode(ThemeMode m) {
    mode = m;
    palette_key_ = m == ThemeMode::Light ? "light" : "dark";
    RefreshRoles();  // every role has a per-mode value, the accent included
}

void Theme::ApplyThemeKey(const std::string& key) {
    if (key == "system") return ApplyMode(ThemeModeFromKey(key));
    if (const ombra::UserTheme* u = ombra::FindUserTheme(user_themes, key)) {
        palette_key_ = key;
        mode = ombra::UiBackgroundIsLight(u->colors.background) ? ThemeMode::Light : ThemeMode::Dark;
    } else {
        const ombra::UiPalette* p = ombra::FindUiPalette(key);
        if (!p) p = &ombra::kUiPalettes[0];
        palette_key_ = p->key;
        mode = p->light ? ThemeMode::Light : ThemeMode::Dark;
    }
    RefreshRoles();
}

uint32_t Theme::PickedRgb() const {
    if (accent_id == AccentId::Custom) return accent_rgb;
    if (accent_id == AccentId::Theme) {
        if (const uint32_t own = ombra::UiThemeAccentRgb(palette_key_)) return own;
        return AccentPickedRgb(AccentId::Mono, ThemeMode::Dark);
    }
    return AccentPickedRgb(accent_id, ThemeMode::Dark);
}

uint32_t Theme::ThemeAccentRgb() const {
    const uint32_t own = ombra::UiThemeAccentRgb(palette_key_);
    return own ? own : AccentPickedRgb(AccentId::Mono, mode);
}

AccentId AccentIdFromKey(const std::string& key) {
    if (key == "violet") return AccentId::Violet;
    if (key == "cyan")   return AccentId::Cyan;
    if (key == "blue")   return AccentId::Blue;
    if (key == "green")  return AccentId::Green;
    if (key == "mono")   return AccentId::Mono;
    if (ombra::ParseHexRgb(key, nullptr)) return AccentId::Custom;
    return AccentId::Theme;
}

const char* AccentIdToKey(AccentId id) {
    switch (id) {
        case AccentId::Violet: return "violet";
        case AccentId::Cyan:   return "cyan";
        case AccentId::Blue:   return "blue";
        case AccentId::Green:  return "green";
        case AccentId::Theme:  return "theme";
        case AccentId::Mono:
        default:                return "mono";
    }
}

PanelPosition PanelPositionFromKey(const std::string& key) {
    if (key == "center") return PanelPosition::Center;
    if (key == "bottom") return PanelPosition::Bottom;
    if (key == "left")   return PanelPosition::Left;
    if (key == "right")  return PanelPosition::Right;
    return PanelPosition::Top;
}

RailPosition RailPositionFromKey(const std::string& key) {
    if (key == "right")  return RailPosition::Right;
    if (key == "top")    return RailPosition::Top;
    if (key == "bottom") return RailPosition::Bottom;
    return RailPosition::Left;
}

PanelAttach PanelAttachFromKey(const std::string& key) {
    return key == "attached" ? PanelAttach::Attached : PanelAttach::Floating;
}

ScreenFrameColor ScreenFrameColorFromKey(const std::string& key) {
    if (key == "contrast" || key == "white") return ScreenFrameColor::Contrast;  // "white" = pre-light-theme name
    if (key == "accent") return ScreenFrameColor::Accent;
    return ScreenFrameColor::Match;  // "match", and the old "dark"
}

namespace {
bool (*g_system_theme_light)() = &ombra::WindowsAppsUseLightTheme;
}  // namespace

void SetSystemThemeLightProbe(bool (*probe)()) { g_system_theme_light = probe ? probe : &ombra::WindowsAppsUseLightTheme; }

ThemeMode ThemeModeFromKey(const std::string& key) {
    if (key == "system") return g_system_theme_light() ? ThemeMode::Light : ThemeMode::Dark;
    return key == "light" ? ThemeMode::Light : ThemeMode::Dark;
}

void Theme::ApplyUiConfig(const std::function<const ombra::JsonValue*(const char*)>& get) {
    using T = ombra::JsonValue::Type;
    auto str = [&](const char* key) -> const std::string* {
        const auto* v = get(key);
        return (v && v->type == T::String) ? &v->str : nullptr;
    };
    // The user's themes, then the theme: it re-applies the current accent, so the accent line
    // below only has to pick the id.
    const auto* themes = get("ui.themes");
    if (themes && themes->type == T::Array) user_themes = ombra::ParseUserThemes(*themes);
    if (const auto* s = str("ui.theme")) ApplyThemeKey(*s);
    else if (themes) ApplyThemeKey(palette_key_);
    if (const auto* s = str("ui.accent")) ApplyAccentKey(*s);
    if (const auto* s = str("ui.density")) {
        density = DensityFromKey(*s);
        size.density = DensityScale(density);
    }
    if (const auto* s = str("ui.panel_position")) panel_position = PanelPositionFromKey(*s);
    if (const auto* s = str("ui.panel_attach")) panel_attach = PanelAttachFromKey(*s);
    if (const auto* s = str("ui.rail_position")) rail_position = RailPositionFromKey(*s);
    if (const auto* s = str("ui.screen_frame_color")) screen_frame_color = ScreenFrameColorFromKey(*s);
    if (const auto* v = get("ui.screen_frame"); v && v->type == T::Bool) screen_frame = v->b;
    if (const auto* v = get("ui.memory_card"); v && v->type == T::Bool) memory_card = v->b;
    if (const auto* v = get("ui.card_borders"); v && v->type == T::Bool) size.card_lines = v->b;
    if (const auto* s = str("ui.font")) SetUiFont(UiFontFromKey(*s));
    if (const auto* s = str("ui.corners")) { size.corners = (*s == "sharp") ? 0.4f : (*s == "round") ? 1.6f : 1.0f; size.pill = (*s == "round"); }
    if (const auto* v = get("ui.screen_frame_radius"); v && v->type == T::Number) {
        screen_frame_radius = static_cast<float>(v->num);
    }
    if (const auto* v = get("ui.screen_frame_thickness"); v && v->type == T::Number) {
        screen_frame_thickness = static_cast<float>(v->num);
    }
}

}  // namespace ombra_ui
