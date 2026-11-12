#pragma once

// Colour math for the theme tokens: contrast, OKLCH lightness
// steps and the accent derivation. Pure arithmetic in sRGB 0..1, no D2D, so tests link it alone.
// Ported from the approved token sheet (never committed); tests/color_tests.cpp holds its values.
// Oklab matrices adapted from Bjorn Ottosson, bottosson.github.io/posts/oklab (public domain, or MIT).
// No APCA code: its licence (apca-w3) limits use to web content and needs an agreement for
// commercial use, so the text-on-fill polarity is a WCAG luminance cut (PrefersWhiteText).

#include <cstdint>
#include <vector>

namespace ombra_ui {

struct Rgb { float r = 0, g = 0, b = 0; };

Rgb RgbFromHex(uint32_t rgb);  // 0xRRGGBB
uint32_t RgbToHex(Rgb c);      // rounded, clamped
Rgb Mix(Rgb a, Rgb b, float t);

// WCAG 2 relative luminance and contrast ratio (1..21).
float Luminance(Rgb c);
float Contrast(Rgb a, Rgb b);
// White (true) or near-black text on a fill. This cut reproduces APCA's polarity on 99.85% of a 48^3
// sRGB grid and on every sheet colour, using only the WCAG luminance.
constexpr float kWhiteTextBelowLuminance = 0.368f;
bool PrefersWhiteText(Rgb fill);

// OKLCH (bottosson.github.io/posts/oklab): lightness moves without the hue drifting. h in radians.
struct Lch { float L = 0, C = 0, h = 0; };
Lch ToOklch(Rgb c);
// Out-of-gamut colours keep L and h and give up chroma.
Rgb FromOklchClamped(Lch p);

struct Readable { Rgb color; float worst_ratio; };
// Steps `c` lighter (dark theme) or darker (light theme) in OKLCH until it reaches `target` WCAG
// contrast against every surface, or lightness hits its bound.
Readable MakeReadable(Rgb c, const std::vector<Rgb>& surfaces, bool light, float target);

// Every accent role, derived from the one colour the user picks.
struct AccentRoles {
    Rgb picked;        // as chosen; tints (soft fill, outline) use it with alpha
    Rgb fill;          // moved only as far as needed to stand 3:1 off the card
    Rgb fill_hover;    // +0.06 L away from the panel
    Rgb fill_pressed;  // a step darker than rest in both themes
    Rgb on_fill;       // white or near-black (PrefersWhiteText)
    Rgb text;          // accent used as text: 4.5:1 on card, raised and a hovered card
    float fill_ratio = 0, text_ratio = 0;
    bool fill_moved = false, text_moved = false;
};
AccentRoles DeriveAccent(Rgb picked, Rgb card, Rgb raised, Rgb card_hover, bool light);

constexpr uint32_t kOnAccentDark = 0x111216;  // the near-black text on a light accent fill

// True when `picked` is a saturated colour within 15 degrees of `status`'s OKLCH hue: the picker
// warns that it may read as that status.
bool NearStatusHue(Rgb picked, Rgb status);

}  // namespace ombra_ui
