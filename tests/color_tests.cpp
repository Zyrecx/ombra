// Theme tokens (ui/color.h, Theme::color/size/Font) and the ui.accent / ui.density keys.
#include <doctest/doctest.h>

#include "ombra-ui/ui/color.h"
#include "ombra-ui/ui/theme.h"
#include "ombra/config.h"
#include "ombra/json.h"

#include <dwrite.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ombra_ui;

namespace {

// What the token sheet rendered (2026-10-28), per picked colour and theme: fill, fill
// hover, fill pressed, text on the fill, accent as text.
struct Golden { uint32_t picked; bool light; uint32_t fill, hover, pressed, on_fill, text; };
const Golden kGolden[] = {
    {0xe6e8ee, false, 0xe6e8ee, 0xfbfcff, 0xd9dbe1, 0x111216, 0xe6e8ee},
    {0xe6e8ee, true, 0x8d8f95, 0x7c7d83, 0x6a6c71, 0xffffff, 0x6a6c71},
    {0x16171c, false, 0x5d5f65, 0x6f7076, 0x52545a, 0xffffff, 0x83848b},
    {0x16171c, true, 0x16171c, 0x090a0f, 0x020204, 0xffffff, 0x16171c},
    {0xa78bfa, false, 0xa78bfa, 0xb8a4fc, 0x9b7fec, 0xffffff, 0xa78bfa},
    {0xa78bfa, true, 0x9b7fec, 0x896cd8, 0x785ac4, 0xffffff, 0x7557c0},
    {0x22d3ee, false, 0x22d3ee, 0x62e5fc, 0x24c5de, 0x111216, 0x22d3ee},
    {0x22d3ee, true, 0x0aa0b5, 0x148b9e, 0x147888, 0xffffff, 0x0e7585},
    {0x5b9bff, false, 0x5b9bff, 0x7db0fe, 0x4f8ef1, 0xffffff, 0x5b9bff},
    {0x5b9bff, true, 0x5291f5, 0x407fe0, 0x2e6ccc, 0xffffff, 0x2b69c8},
    {0x76b900, false, 0x76b900, 0x88cd2e, 0x6dab03, 0x111216, 0x76b900},
    {0x76b900, true, 0x67a10d, 0x5a8c13, 0x4c7900, 0xffffff, 0x4b750d},
    {0xf97316, false, 0xf97316, 0xfd935b, 0xe66a15, 0xffffff, 0xf97316},
    {0xf97316, true, 0xea6d1b, 0xd15e0b, 0xb5520f, 0xffffff, 0xb14f09},
    {0xec4899, false, 0xec4899, 0xfd63ab, 0xde398d, 0xffffff, 0xec4899},
    {0xec4899, true, 0xec4899, 0xd63187, 0xc11375, 0xffffff, 0xc81f7b},
    {0x8b5cf6, false, 0x8b5cf6, 0x9b76fe, 0x804fe8, 0xffffff, 0x936afa},
    {0x8b5cf6, true, 0x8b5cf6, 0x7a48e1, 0x6a32cc, 0xffffff, 0x7a48e1},
    {0x3b82f6, false, 0x3b82f6, 0x5897fe, 0x2e75e8, 0xffffff, 0x3b82f6},
    {0x3b82f6, true, 0x3b82f6, 0x286fe1, 0x125ccd, 0xffffff, 0x1e65d7},
    {0xfacc15, false, 0xfacc15, 0xfee391, 0xeac01d, 0x111216, 0xfacc15},
    {0xfacc15, true, 0xb08e00, 0x9a7d0e, 0x856c10, 0xffffff, 0x82690a},
    {0x14b8a6, false, 0x14b8a6, 0x39cbb9, 0x14aa99, 0x111216, 0x14b8a6},
    {0x14b8a6, true, 0x1fa293, 0x0c8f80, 0x0f7b6e, 0xffffff, 0x07786b},
    {0xef4444, false, 0xef4444, 0xfb6660, 0xe03438, 0xffffff, 0xef4444},
    {0xef4444, true, 0xef4444, 0xd92c32, 0xc3051e, 0xffffff, 0xd2222b},
    {0x26262b, false, 0x606066, 0x717177, 0x55555a, 0xffffff, 0x86868c},
    {0x26262b, true, 0x26262b, 0x18181d, 0x0b0b0f, 0xffffff, 0x26262b},
    {0xf4f4f5, false, 0xf4f4f5, 0xffffff, 0xe7e7e8, 0x111216, 0xf4f4f5},
    {0xf4f4f5, true, 0x919192, 0x7f7f80, 0x6e6e6f, 0xffffff, 0x6b6b6c},
};

// Within one 8-bit step per channel: float rounding may differ between builds, the look does not.
bool same(uint32_t got, uint32_t want) {
    for (int sh : {16, 8, 0}) {
        if (std::abs(static_cast<int>((got >> sh) & 255) - static_cast<int>((want >> sh) & 255)) > 1) return false;
    }
    return true;
}
uint32_t HexOf(const D2D1_COLOR_F& c) { return RgbToHex({c.r, c.g, c.b}); }

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", v);
    return b;
}

struct Surfaces { Rgb card, raised, hover; };
Surfaces SurfacesFor(bool light) {
    const Rgb card = RgbFromHex(light ? 0xfbfbfd : 0x0e0e11);
    const Rgb text = RgbFromHex(light ? 0x16171c : 0xf2f4f8);
    return {card, RgbFromHex(light ? 0xffffff : 0x18181d), Mix(card, text, 0.06f)};
}

bool Accepted(const char* key, const char* value) {
    ombra::JsonValue patch = ombra::JsonValue::MakeObject();
    patch.Set(key, ombra::JsonValue::MakeStr(value));
    ombra::Config cfg;
    return ombra::ApplyConfigPatch(cfg, patch).rejected.empty();
}

}  // namespace

TEST_CASE("color: accent derivation matches the approved token sheet") {
    for (const Golden& g : kGolden) {
        const Surfaces s = SurfacesFor(g.light);
        const AccentRoles a = DeriveAccent(RgbFromHex(g.picked), s.card, s.raised, s.hover, g.light);
        INFO(Hex(g.picked), (g.light ? " light" : " dark"));
        CHECK(same(RgbToHex(a.fill), g.fill));
        CHECK(same(RgbToHex(a.fill_hover), g.hover));
        CHECK(same(RgbToHex(a.fill_pressed), g.pressed));
        CHECK(RgbToHex(a.on_fill) == g.on_fill);
        CHECK(same(RgbToHex(a.text), g.text));
    }
}

TEST_CASE("color: any picked colour stays readable and keeps its hue") {
    for (int light = 0; light < 2; ++light) {
        const Surfaces s = SurfacesFor(light != 0);
        for (uint32_t r = 0; r < 256; r += 51)
            for (uint32_t g = 0; g < 256; g += 51)
                for (uint32_t b = 0; b < 256; b += 51) {
                    const uint32_t hex = (r << 16) | (g << 8) | b;
                    const Rgb picked = RgbFromHex(hex);
                    const AccentRoles a = DeriveAccent(picked, s.card, s.raised, s.hover, light != 0);
                    INFO(Hex(hex), (light ? " light" : " dark"));
                    REQUIRE_FALSE(std::isnan(a.fill.r + a.fill.g + a.fill.b + a.text.r + a.text.g + a.text.b));
                    CHECK(Contrast(a.fill, s.card) >= 2.99f);
                    CHECK(Contrast(a.text, s.card) >= 4.49f);
                    CHECK(Contrast(a.text, s.raised) >= 4.49f);
                    CHECK(Contrast(a.text, s.hover) >= 4.49f);
                    const uint32_t on = RgbToHex(a.on_fill);  // white or near-black, nothing else
                    CHECK((on == 0xffffffu || on == kOnAccentDark));
                    // A saturated colour keeps its hue when it moves (chroma may drop at the gamut edge).
                    const Lch p = ToOklch(picked), f = ToOklch(a.fill);
                    if (p.C > 0.08f && f.C > 0.04f) {
                        float d = std::abs(p.h - f.h);
                        if (d > 3.14159265f) d = 2 * 3.14159265f - d;
                        CHECK(d < 0.12f);
                    }
                }
    }
}

TEST_CASE("color: contrast, OKLCH round trip and text polarity on a fill") {
    CHECK(Contrast(RgbFromHex(0xffffff), RgbFromHex(0x000000)) == doctest::Approx(21.0f).epsilon(0.001));
    CHECK(Contrast(RgbFromHex(0x777777), RgbFromHex(0xffffff)) == doctest::Approx(4.48f).epsilon(0.01));
    for (uint32_t hex : {0x000000u, 0xffffffu, 0x808080u, 0xef4444u, 0x22d3eeu, 0x76b900u}) {
        INFO(Hex(hex));
        CHECK(same(RgbToHex(FromOklchClamped(ToOklch(RgbFromHex(hex)))), hex));
    }
    // White on mid-tone violet, blue, orange, pink (where WCAG 2 says black).
    for (uint32_t hex : {0xa78bfau, 0x5b9bffu, 0xf97316u, 0xec4899u, 0x8b5cf6u, 0x3b82f6u, 0x1e3a8au}) {
        INFO(Hex(hex));
        CHECK(PrefersWhiteText(RgbFromHex(hex)));
    }
    for (uint32_t hex : {0xfacc15u, 0x22d3eeu, 0x76b900u, 0xe6e8eeu, 0x14b8a6u}) {
        INFO(Hex(hex));
        CHECK_FALSE(PrefersWhiteText(RgbFromHex(hex)));
    }
}

TEST_CASE("color: accents near the danger red get the picker note, others don't") {
    const Rgb danger = RgbFromHex(0xef4444);
    for (uint32_t hex : {0xef4444u, 0xdc2626u, 0xf87171u, 0xf43f5eu}) {
        INFO(Hex(hex));
        CHECK(NearStatusHue(RgbFromHex(hex), danger));
    }
    for (uint32_t hex : {0xec4899u, 0xf97316u, 0xfacc15u, 0x5b9bffu, 0x76b900u, 0x808080u, 0xe6e8eeu, 0xfecacau}) {
        INFO(Hex(hex));
        CHECK_FALSE(NearStatusHue(RgbFromHex(hex), danger));
    }
}

TEST_CASE("theme: roles follow mode and accent") {
    Theme t;
    CHECK(same(HexOf(t.color.accent), 0xe6e8ee));
    CHECK(HexOf(t.color.on_accent) == kOnAccentDark);

    t.ApplyAccentKey("violet");
    CHECK(t.accent_id == AccentId::Violet);
    CHECK(t.PickedRgb() == 0xa78bfau);
    CHECK(same(HexOf(t.color.accent), 0xa78bfa));

    t.ApplyAccentKey("#F97316");
    CHECK(t.accent_id == AccentId::Custom);
    CHECK(t.accent_rgb == 0xf97316u);
    CHECK(t.PickedRgb() == 0xf97316u);
    CHECK_FALSE(t.color.accent_near_danger);

    t.ApplyMode(ThemeMode::Light);  // re-derives the custom colour against the light card
    CHECK(t.accent_id == AccentId::Custom);
    CHECK(same(HexOf(t.color.accent), 0xea6d1b));
    CHECK(HexOf(t.color.card) == 0xfbfbfdu);

    t.ApplyMode(ThemeMode::Light);
    t.ApplyAccent(AccentId::Mono);
    CHECK(same(HexOf(t.color.accent), 0x16171c));  // mono is near-black in light

    t.ApplyAccentKey("#ef4444");
    CHECK(t.color.accent_near_danger);
    t.ApplyAccentKey("nonsense");  // like any unknown preset name: the default
    CHECK(t.accent_id == AccentId::Theme);
    CHECK(AccentIdFromKey("#00ff00") == AccentId::Custom);
}

TEST_CASE("theme: the theme accent is the palette's own, mono where it has none") {
    Theme t;
    t.ApplyAccentKey("theme");
    CHECK(t.accent_id == AccentId::Theme);
    CHECK(t.ThemeAccentRgb() == 0xe6e8eeu);  // Dark: mono
    CHECK(same(HexOf(t.color.accent), 0xe6e8ee));
    t.ApplyThemeKey("light");
    CHECK(t.ThemeAccentRgb() == 0x16171cu);  // Light: mono's near-black
    t.ApplyThemeKey("mocha");                // re-resolves with the theme, the accent key untouched
    CHECK(t.accent_id == AccentId::Theme);
    CHECK(t.ThemeAccentRgb() == 0xcba6f7u);
    CHECK(t.PickedRgb() == 0xcba6f7u);
    t.user_themes = {{"user1", "Mine", {0x101010, 0x202020, 0x303030, 0x404040, 0xf0f0f0, 0x909090}}};
    t.ApplyThemeKey("user1");                // a user theme sets no accent
    CHECK(t.ThemeAccentRgb() == 0xe6e8eeu);
    CHECK(t.PickedRgb() == 0xe6e8eeu);
    t.ApplyAccentKey("blue");                // a preset still wins over the theme's
    t.ApplyThemeKey("mocha");
    CHECK(t.PickedRgb() == 0x5b9bffu);

    CHECK(AccentIdFromKey("theme") == AccentId::Theme);
    CHECK(std::string(AccentIdToKey(AccentId::Theme)) == "theme");
    CHECK(AccentSwatchIndex(AccentId::Theme) == 0);
    CHECK(AccentSwatchIndex(AccentId::Mono) == 1);
    CHECK(AccentSwatchIndex(AccentId::Custom) == kAccentSwatchCount);
    CHECK(ombra::UiThemeAccentRgb("system") == 0u);
    CHECK(ombra::UiThemeAccentRgb("nord") == 0x88c0d0u);
}

TEST_CASE("theme: no palette's own accent reads as the error red") {
    for (const ombra::UiPalette& p : ombra::kUiPalettes) {
        Theme t;
        t.ApplyThemeKey(p.key);
        t.ApplyAccentKey("theme");
        INFO(p.key);
        CHECK_FALSE(t.color.accent_near_danger);
    }
}

TEST_CASE("theme: density scales spacing and control heights to whole DIPs, corners scale radii") {
    Theme t;
    CHECK(t.size.control_sm() == 28);
    CHECK(t.size.control() == 32);
    CHECK(t.size.card_padding() == 16);
    static const ombra::JsonValue compact = ombra::JsonValue::MakeStr("compact");
    static const ombra::JsonValue sharp = ombra::JsonValue::MakeStr("sharp");
    t.ApplyUiConfig([](const char* key) -> const ombra::JsonValue* {
        if (std::string(key) == "ui.density") return &compact;
        if (std::string(key) == "ui.corners") return &sharp;
        return nullptr;
    });
    CHECK(t.density == Density::Compact);
    CHECK(t.size.control_sm() == 24);  // 23.8
    CHECK(t.size.control() == 27);     // 27.2
    CHECK(t.size.control_lg() == 34);
    CHECK(t.size.space_xxs() == 2);    // 1.7
    CHECK(t.size.radius_control() == doctest::Approx(6.0f * 0.4f));
    t.size.density = DensityScale(Density::Comfortable);
    CHECK(t.size.control_sm() == 32);  // 32.2
    CHECK(t.size.control() == 37);     // 36.8
    CHECK(t.size.card_padding() == 18);
}

TEST_CASE("theme: round makes controls pills, sharp squares off the round-by-design shapes") {
    auto with_corners = [](const char* v) {
        Theme t;
        const ombra::JsonValue val = ombra::JsonValue::MakeStr(v);
        t.ApplyUiConfig([&val](const char* key) -> const ombra::JsonValue* {
            return std::string(key) == "ui.corners" ? &val : nullptr;
        });
        return t.size;
    };
    const Sizes def = with_corners("default"), round = with_corners("round"), sharp = with_corners("sharp");
    CHECK(def.control_radius(28, 1) == doctest::Approx(6));
    CHECK(round.control_radius(28, 1.5f) == doctest::Approx(14));  // a pill at any scale
    CHECK(round.key_radius(24, 1) == doctest::Approx(12));
    CHECK(round.radius_card() == doctest::Approx(8 * 1.6f));       // cards just get rounder
    CHECK(def.radius_full(20, 1) == doctest::Approx(10));           // the switch stays round
    CHECK(sharp.radius_full(20, 1) == doctest::Approx(6 * 0.4f));   // ...until corners are sharp
    CHECK(sharp.control_radius(28, 1) == doctest::Approx(6 * 0.4f));
}

TEST_CASE("theme: ui.card_borders turns cards' outlines off and back on") {
    Theme t;
    CHECK(t.size.card_lines);
    static const ombra::JsonValue off = ombra::JsonValue::MakeBool(false);
    static const ombra::JsonValue on = ombra::JsonValue::MakeBool(true);
    const ombra::JsonValue* v = &off;
    auto get = [&v](const char* key) -> const ombra::JsonValue* {
        return std::string(key) == "ui.card_borders" ? v : nullptr;
    };
    t.ApplyUiConfig(get);
    CHECK_FALSE(t.size.card_lines);
    v = &on;
    t.ApplyUiConfig(get);
    CHECK(t.size.card_lines);
}

TEST_CASE("theme: Font() builds one format per role and options, at the font scale") {
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    REQUIRE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dw.GetAddressOf()))));
    Theme t;
    CHECK(t.Font(TypeRole::Body) == nullptr);  // before InitFonts
    REQUIRE(t.InitFonts(dw.Get()));
    IDWriteTextFormat* body = t.Font(TypeRole::Body);
    REQUIRE(body);
    CHECK(body == t.Font(TypeRole::Body));
    CHECK(body != t.Font(TypeRole::Body, {TextAlign::Center}));
    CHECK(body->GetFontSize() == 13.0f);
    CHECK(body->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP);
    CHECK(t.Font(TypeRole::Caption, {TextAlign::Leading, true})->GetWordWrapping() == DWRITE_WORD_WRAPPING_WRAP);
    CHECK(t.Font(TypeRole::Label)->GetFontWeight() == DWRITE_FONT_WEIGHT_SEMI_BOLD);
    t.SetFontScale(1.5f);
    CHECK(t.Font(TypeRole::Display)->GetFontSize() == 30.0f);
}

TEST_CASE("config: ui.accent takes a preset or #rrggbb, ui.density its three levels") {
    for (const char* v : {"mono", "violet", "cyan", "blue", "green", "#a1b2c3", "#A1B2C3"}) {
        INFO(v);
        CHECK(Accepted("ui.accent", v));
    }
    for (const char* v : {"#fff", "a1b2c3", "#a1b2c", "#a1b2c3d", "#g1b2c3", "red", "#rrggbb", ""}) {
        INFO(v);
        CHECK_FALSE(Accepted("ui.accent", v));
    }
    for (const char* v : {"compact", "default", "comfortable"}) CHECK(Accepted("ui.density", v));
    CHECK_FALSE(Accepted("ui.density", "dense"));
    uint32_t rgb = 0;
    CHECK(ombra::ParseHexRgb("#0aFf10", &rgb));
    CHECK(rgb == 0x0aff10u);
}

TEST_CASE("theme palettes: text and muted text read on their cards, and light ones say so") {
    for (const ombra::UiPalette& p : ombra::kUiPalettes) {
        INFO(p.key);
        CHECK(ombra::ContrastRatio(p.colors.text, p.colors.cards) >= 7.0);
        CHECK(ombra::ContrastRatio(p.colors.muted, p.colors.cards) >= 4.5);
        CHECK(ombra::UiBackgroundIsLight(p.colors.background) == p.light);
        CHECK(Accepted("ui.theme", p.key));
        CHECK(ombra::FindUiPalette(p.key) == &p);
    }
    for (const char* v : {"system", "user1", "user24", "user9999"}) CHECK(Accepted("ui.theme", v));
    for (const char* v : {"Mocha", "catppuccin", "", "custom", "user", "user0", "user01", "user12345", "userx"}) {
        INFO(v);
        CHECK_FALSE(Accepted("ui.theme", v));
    }
    std::vector<ombra::UserTheme> mine(2);
    mine[0] = {"user1", "Paper", ombra::FindUiPalette("latte")->colors};
    mine[1] = {"user2", "Cave", ombra::kUiPalettes[0].colors};
    mine[1].colors.background = 0x101010;
    CHECK(ombra::UiThemeIsLight("latte", false, {}));
    CHECK_FALSE(ombra::UiThemeIsLight("nord", true, mine));
    CHECK(ombra::UiThemeIsLight("system", true, {}));
    CHECK(ombra::UiThemeIsLight("user1", false, mine));  // the engine's tray and toasts follow the background
    CHECK_FALSE(ombra::UiThemeIsLight("user2", true, mine));
    CHECK_FALSE(ombra::UiThemeIsLight("user3", true, mine));  // deleted: Dark
    CHECK_FALSE(ombra::UiThemeIsLight("no-such-theme", true, mine));
}

namespace {
bool ThemesOk(const std::string& json) {
    std::string err;
    return ombra::ValidateRawConfigValue("ui.themes", ombra::ParseJson(json), err);
}
const char* kSix = R"("background":"#101010","cards":"#202020","menus":"#303030","lines":"#404040","text":"#f0f0f0","muted":"#a0a0a0")";
std::string OneTheme(const std::string& id, const std::string& name) {
    return R"({"id":")" + id + R"(","name":")" + name + R"(",)" + kSix + "}";
}
}  // namespace

TEST_CASE("your themes: ui.themes round-trips, and bad entries, names and ids are refused") {
    CHECK(ThemesOk("[]"));
    CHECK(ThemesOk("[" + OneTheme("user1", "Paper") + "," + OneTheme("user7", "Cave") + "]"));
    CHECK_FALSE(ThemesOk("{}"));
    CHECK_FALSE(ThemesOk("[" + OneTheme("user1", "A") + "," + OneTheme("user1", "B") + "]"));   // same id
    CHECK_FALSE(ThemesOk("[" + OneTheme("user1", "Cave") + "," + OneTheme("user2", "cave") + "]"));  // same name
    CHECK_FALSE(ThemesOk("[" + OneTheme("dark", "Paper") + "]"));
    for (const char* bad : {"", " Paper", "Paper ", "Dark", "nord", "SYSTEM", "Ros\u00e9 Pine",
                            "123456789012345678901234567890123"}) {
        INFO(bad);
        CHECK_FALSE(ThemesOk("[" + OneTheme("user1", bad) + "]"));
    }
    CHECK(ThemesOk("[" + OneTheme("user1", "12345678901234567890123456789012") + "]"));  // 32: the cap
    CHECK_FALSE(ThemesOk(R"([{"id":"user1","name":"Paper","background":"#101010"}])"));  // colours missing
    CHECK_FALSE(ThemesOk("[" + std::string(OneTheme("user1", "P")).replace(OneTheme("user1", "P").find("#202020"), 7, "#2020") + "]"));
    std::string many = "[";
    for (size_t i = 1; i <= ombra::kMaxUserThemes + 1; ++i)
        many += (i > 1 ? "," : "") + OneTheme("user" + std::to_string(i), "T" + std::to_string(i));
    CHECK_FALSE(ThemesOk(many + "]"));

    // Parsing keeps the good ones; serializing gives back what validates.
    const ombra::JsonValue list = ombra::ParseJson("[" + OneTheme("user1", "Paper") + R"(,{"id":"nope"},)" + OneTheme("user3", "Cave") + "]");
    const std::vector<ombra::UserTheme> parsed = ombra::ParseUserThemes(list);
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].name == "Paper");
    CHECK(parsed[1].id == "user3");
    CHECK(parsed[1].colors.cards == 0x202020u);
    std::string err;
    CHECK(ombra::ValidateRawConfigValue("ui.themes", ombra::SerializeUserThemes(parsed), err));
    CHECK(ombra::ParseUserThemes(ombra::SerializeUserThemes(parsed))[0].colors.text == 0xf0f0f0u);
    CHECK(ombra::NextUserThemeId(parsed) == "user2");

    // A rename may keep its own name, in another case too; not another theme's.
    CHECK(ombra::UserThemeNameOk("Paper", parsed, "user1"));
    CHECK(ombra::UserThemeNameOk("PAPER", parsed, "user1"));
    CHECK_FALSE(ombra::UserThemeNameOk("Cave", parsed, "user1"));
    CHECK_FALSE(ombra::UserThemeNameOk("Latte", parsed, "user1"));
}

TEST_CASE("theme: a palette sets its surfaces and text, your theme its own colours, the accent stays legible") {
    auto same = [](const D2D1_COLOR_F& c, uint32_t hex) {
        const Rgb r = RgbFromHex(hex);
        return std::abs(c.r - r.r) < 0.003f && std::abs(c.g - r.g) < 0.003f && std::abs(c.b - r.b) < 0.003f;
    };
    Theme t;
    t.ApplyThemeKey("nord");
    const ombra::UiPalette& nord = *ombra::FindUiPalette("nord");
    CHECK(t.mode == ThemeMode::Dark);
    CHECK(t.palette_key() == "nord");
    CHECK(same(t.color.panel, nord.colors.background));
    CHECK(same(t.color.card, nord.colors.cards));
    CHECK(same(t.color.raised, nord.colors.menus));
    CHECK(same(t.color.line, nord.colors.lines));
    CHECK(same(t.color.text, nord.colors.text));
    CHECK(same(t.color.text3, nord.colors.muted));
    t.ApplyThemeKey("dark");
    CHECK(same(t.color.card, ombra::kUiPalettes[0].colors.cards));
    // Every palette keeps the accent fill visible on its cards and its label readable on the fill.
    auto hex = [](const D2D1_COLOR_F& c) { return RgbToHex(Rgb{c.r, c.g, c.b}); };
    for (const auto& p : ombra::kUiPalettes) {
        t.ApplyThemeKey(p.key);
        CAPTURE(std::string(p.key));
        CHECK(ombra::ContrastRatio(hex(t.color.accent), p.colors.cards) >= 3.0);
        CHECK(ombra::ContrastRatio(hex(t.color.on_accent), hex(t.color.accent)) >= 4.5);
    }

    t.ApplyThemeKey("latte");
    CHECK(t.mode == ThemeMode::Light);

    // Your theme: a light background makes the mode light (status hues, scrim), a dark one dark.
    t.user_themes = {{"user1", "Paper", ombra::FindUiPalette("latte")->colors}, {"user2", "Cave", ombra::kUiPalettes[0].colors}};
    t.user_themes[0].colors.cards = 0xfafafa;
    t.ApplyThemeKey("user1");
    CHECK(t.mode == ThemeMode::Light);
    CHECK(same(t.color.card, 0xfafafa));
    t.ApplyThemeKey("user2");
    CHECK(t.mode == ThemeMode::Dark);
    t.ApplyThemeKey("user9");  // deleted under it: Dark
    CHECK(same(t.color.card, ombra::kUiPalettes[0].colors.cards));

    // Staged config: an edited ui.themes re-applies while that theme is chosen, and an unknown theme is Dark.
    ombra::UserTheme staged{"user4", "Deep", ombra::kUiPalettes[0].colors};
    staged.colors.cards = 0x123456;
    ombra::JsonValue cfg = ombra::JsonValue::MakeObject();
    cfg.Set("ui.theme", ombra::JsonValue::MakeStr("user4"));
    cfg.Set("ui.themes", ombra::SerializeUserThemes({staged}));
    t.ApplyUiConfig([&](const char* k) { return cfg.Find(k); });
    CHECK(t.palette_key() == "user4");
    CHECK(same(t.color.card, 0x123456));
    staged.colors.cards = 0x234567;
    ombra::JsonValue only_list = ombra::JsonValue::MakeObject();
    only_list.Set("ui.themes", ombra::SerializeUserThemes({staged}));
    t.ApplyUiConfig([&](const char* k) { return only_list.Find(k); });  // the list alone changed
    CHECK(same(t.color.card, 0x234567));
    cfg.Set("ui.theme", ombra::JsonValue::MakeStr("no-such-theme"));
    t.ApplyUiConfig([&](const char* k) { return cfg.Find(k); });
    CHECK(t.mode == ThemeMode::Dark);
    CHECK(same(t.color.card, ombra::kUiPalettes[0].colors.cards));
}
