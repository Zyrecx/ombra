#include "ombra-ui/ui/color.h"

#include <algorithm>
#include <cmath>

namespace ombra_ui {

namespace {

float ToLin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float FromLin(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

bool FromOklch(Lch p, Rgb* out) {
    const float A = p.C * std::cos(p.h), B = p.C * std::sin(p.h);
    const float l = std::pow(p.L + 0.3963377774f * A + 0.2158037573f * B, 3.0f);
    const float m = std::pow(p.L - 0.1055613458f * A - 0.0638541728f * B, 3.0f);
    const float s = std::pow(p.L - 0.0894841775f * A - 1.2914855480f * B, 3.0f);
    const float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    const float e = 1e-4f;
    if (r < -e || g < -e || b < -e || r > 1 + e || g > 1 + e || b > 1 + e) return false;
    *out = {FromLin((std::clamp)(r, 0.0f, 1.0f)), FromLin((std::clamp)(g, 0.0f, 1.0f)),
            FromLin((std::clamp)(b, 0.0f, 1.0f))};
    return true;
}

}  // namespace

Rgb RgbFromHex(uint32_t h) {
    return {((h >> 16) & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, (h & 255) / 255.0f};
}

uint32_t RgbToHex(Rgb c) {
    auto b = [](float v) { return static_cast<uint32_t>(std::lround((std::clamp)(v, 0.0f, 1.0f) * 255.0f)); };
    return (b(c.r) << 16) | (b(c.g) << 8) | b(c.b);
}

Rgb Mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

float Luminance(Rgb c) { return 0.2126f * ToLin(c.r) + 0.7152f * ToLin(c.g) + 0.0722f * ToLin(c.b); }

float Contrast(Rgb a, Rgb b) {
    const float la = Luminance(a), lb = Luminance(b);
    return ((std::max)(la, lb) + 0.05f) / ((std::min)(la, lb) + 0.05f);
}

bool PrefersWhiteText(Rgb fill) { return Luminance(fill) < kWhiteTextBelowLuminance; }

Lch ToOklch(Rgb c) {
    const float r = ToLin(c.r), g = ToLin(c.g), b = ToLin(c.b);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    const float L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    const float A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    const float B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    return {L, std::sqrt(A * A + B * B), std::atan2(B, A)};
}

Rgb FromOklchClamped(Lch p) {
    p.L = (std::clamp)(p.L, 0.0f, 1.0f);
    Rgb out{};
    for (int i = 0; i < 64 && !FromOklch(p, &out); ++i) p.C *= 0.94f;
    if (!FromOklch(p, &out)) { p.C = 0; FromOklch(p, &out); }
    return out;
}

Readable MakeReadable(Rgb c, const std::vector<Rgb>& surfaces, bool light, float target) {
    Lch p = ToOklch(c);
    for (int i = 0; i < 120; ++i) {
        const Rgb t = FromOklchClamped(p);
        float worst = 99.0f;
        for (const Rgb& s : surfaces) worst = (std::min)(worst, Contrast(t, s));
        // Only the bound it is moving toward stops it: pure white on light starts at L = 1.
        if (worst >= target || (light ? p.L <= 0.0f : p.L >= 1.0f)) return {t, worst};
        p.L += light ? -0.01f : 0.01f;
    }
    return {FromOklchClamped(p), 0.0f};
}

AccentRoles DeriveAccent(Rgb picked, Rgb card, Rgb raised, Rgb card_hover, bool light) {
    AccentRoles a;
    a.picked = picked;
    const Readable fill = MakeReadable(picked, {card}, light, 3.0f);
    a.fill = fill.color;
    a.fill_ratio = fill.worst_ratio;
    a.fill_moved = Contrast(picked, card) < 3.0f;
    Lch h = ToOklch(a.fill);
    h.L += light ? -0.06f : 0.06f;
    a.fill_hover = FromOklchClamped(h);
    h.L += light ? -0.06f : -0.10f;  // pressed: a step darker than rest in both themes
    a.fill_pressed = FromOklchClamped(h);
    a.on_fill = PrefersWhiteText(a.fill) ? Rgb{1, 1, 1} : RgbFromHex(kOnAccentDark);
    const Readable text = MakeReadable(picked, {card, raised, card_hover}, light, 4.5f);
    a.text = text.color;
    a.text_ratio = text.worst_ratio;
    a.text_moved = (std::min)({Contrast(picked, card), Contrast(picked, raised), Contrast(picked, card_hover)}) < 4.5f;
    return a;
}

bool NearStatusHue(Rgb picked, Rgb status) {
    const Lch p = ToOklch(picked), s = ToOklch(status);
    if (p.C < 0.1f) return false;  // greys and pastels never read as a status
    constexpr float kPi = 3.14159265f;
    float d = std::abs(p.h - s.h);
    if (d > kPi) d = 2 * kPi - d;
    return d < 15.0f * kPi / 180.0f;
}

}  // namespace ombra_ui
