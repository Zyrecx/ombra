// The brand mark ("eclipse": a glowing ring on a near-black disc), the one renderer behind the
// tray icon (tray.cpp) and the overlay's header (overlay_window.cpp). Replacing the mark means
// replacing RenderBrandMark. Header-only: ombra_core and ombra_ui_core both use it and
// ombra-tests links both.
//
// The disc fills the canvas edge to
// edge, the ring sits well inside it as a soft gaussian glow band around a bright core, both
// clipped to the disc so neither bleeds onto the taskbar. Glow and core widths are fractions of
// the canvas, not of the ring, so they stay legible at 16-20 px.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ombra {

// The ring's colour for the mono accent (#e6e8ee), the tray's default.
inline constexpr uint32_t kBrandMarkMonoRgb = 0xe6e8ee;

// Direct2D bitmaps take premultiplied alpha; an HICON's 32 bpp DIB takes straight alpha.
enum class BrandMarkAlpha { Premultiplied, Straight };

// The mark's geometry, shared by the raster renderer and the SVG.
namespace brand_mark {
inline constexpr float kRingRFrac      = 0.62f;   // ring radius, fraction of the disc's radius
inline constexpr float kCoreStrokeFrac = 0.035f;  // bright core's width, fraction of the canvas
inline constexpr float kGlowSigmaFrac  = 0.035f;  // glow's softness, fraction of the canvas
inline constexpr float kGlowPeak       = 0.70f;
inline constexpr float kCorePeak       = 0.98f;
inline constexpr uint32_t kDiscRgb     = 0x0A0A0C;
}  // namespace brand_mark

// size x size pixels, BGRA, top-down. ring_r/g/b in [0, 1].
inline std::vector<uint8_t> RenderBrandMark(int size, float ring_r, float ring_g, float ring_b,
                                            BrandMarkAlpha alpha = BrandMarkAlpha::Premultiplied) {
    using namespace brand_mark;
    constexpr float kDiscR = ((kDiscRgb >> 16) & 255) / 255.0f, kDiscG = ((kDiscRgb >> 8) & 255) / 255.0f,
                    kDiscB = (kDiscRgb & 255) / 255.0f;

    std::vector<uint8_t> pixels(size > 0 ? static_cast<size_t>(size) * size * 4 : 0);
    const float r         = static_cast<float>(size) * 0.5f;
    const float ring_rad  = r * kRingRFrac;
    const float sigma     = (std::max)(0.9f, static_cast<float>(size) * kGlowSigmaFrac);
    const float core_half = (std::max)(0.6f, static_cast<float>(size) * kCoreStrokeFrac * 0.5f);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) - r;
            const float dy = (static_cast<float>(y) + 0.5f) - r;
            const float d  = std::sqrt(dx * dx + dy * dy);

            const float disc_cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            const float t    = (d - ring_rad) / sigma;
            const float glow = kGlowPeak * std::exp(-0.5f * t * t);
            const float core = kCorePeak * std::clamp(core_half - std::fabs(d - ring_rad) + 0.5f, 0.0f, 1.0f);

            // Premultiplied src-over: disc, then the glow band, then the core.
            float pr = 0, pg = 0, pb = 0, pa = 0;
            auto over = [&](float sr, float sg, float sb, float sa) {
                pr = sr * sa + pr * (1.0f - sa);
                pg = sg * sa + pg * (1.0f - sa);
                pb = sb * sa + pb * (1.0f - sa);
                pa = sa + pa * (1.0f - sa);
            };
            over(kDiscR, kDiscG, kDiscB, disc_cov);
            over(ring_r, ring_g, ring_b, (std::min)(glow, disc_cov));
            over(ring_r, ring_g, ring_b, (std::min)(core, disc_cov));

            uint8_t* px = &pixels[(static_cast<size_t>(y) * size + x) * 4];
            px[3] = static_cast<uint8_t>(std::clamp(pa, 0.0f, 1.0f) * 255.0f + 0.5f);
            if (alpha == BrandMarkAlpha::Straight) {
                if (px[3] == 0) {
                    pr = pg = pb = 0;
                } else {
                    pr /= pa; pg /= pa; pb /= pa;
                }
            }
            px[2] = static_cast<uint8_t>(std::clamp(pr, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[1] = static_cast<uint8_t>(std::clamp(pg, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[0] = static_cast<uint8_t>(std::clamp(pb, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return pixels;
}

// The same mark as an SVG, for places that need a vector (README, site, installer art). Radially
// symmetric, so it maps exactly: a disc, the glow as a radial gradient sampling the gaussian, the
// core as a stroked circle. Matches RenderBrandMark at sizes where its floors (sigma >= 0.9 px,
// core >= 1.2 px) don't apply, about 26 px and up; the .ico covers smaller sizes.
inline std::string BrandMarkSvg(uint32_t ring_rgb) {
    using namespace brand_mark;
    constexpr double kView = 1000, kR = kView / 2, kRing = kR * kRingRFrac;
    constexpr double kSigma = kView * kGlowSigmaFrac, kCore = kView * kCoreStrokeFrac;
    char buf[160];
    auto hex = [&](uint32_t rgb) {
        std::snprintf(buf, sizeof buf, "#%06X", rgb & 0xFFFFFF);
        return std::string(buf);
    };
    const std::string ring = hex(ring_rgb);
    std::string s =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1000 1000\">\n"
        "  <defs>\n"
        "    <radialGradient id=\"glow\" gradientUnits=\"userSpaceOnUse\" cx=\"500\" cy=\"500\" r=\"500\">\n";
    // The gaussian at a quarter sigma per stop, out to 4.5 sigma (below 1/255 of the peak).
    for (int i = -18; i <= 18; ++i) {
        const double d = kRing + i * kSigma / 4;
        const double t = (d - kRing) / kSigma;
        std::snprintf(buf, sizeof buf, "      <stop offset=\"%.4f\" stop-color=\"%s\" stop-opacity=\"%.4f\"/>\n",
                      d / kR, ring.c_str(), kGlowPeak * std::exp(-0.5 * t * t));
        s += buf;
    }
    s += "    </radialGradient>\n  </defs>\n";
    s += "  <circle cx=\"500\" cy=\"500\" r=\"500\" fill=\"" + hex(kDiscRgb) + "\"/>\n";
    s += "  <circle cx=\"500\" cy=\"500\" r=\"500\" fill=\"url(#glow)\"/>\n";
    std::snprintf(buf, sizeof buf,
                  "  <circle cx=\"500\" cy=\"500\" r=\"%g\" fill=\"none\" stroke=\"%s\" stroke-width=\"%g\" "
                  "stroke-opacity=\"%g\"/>\n",
                  kRing, ring.c_str(), kCore, static_cast<double>(kCorePeak));
    s += buf;
    s += "</svg>\n";
    return s;
}

}  // namespace ombra
