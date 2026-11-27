// What an API app may call itself (the hello's "app", the command line's --as). The name lands in
// logs and on toasts, so it must be well-formed UTF-8 that reads as what it is: no control or
// format characters (bidi overrides, zero-width spaces, tag characters that hide text), and
// something visible. Header-only: the engine and ombra.com both check it.
#pragma once

#include <cstddef>
#include <string_view>

namespace ombra {

inline constexpr size_t kAppNameMaxBytes = 64;

namespace app_name_detail {

struct Range {
    char32_t lo, hi;
};

// Generated from Unicode 16.0.0 (DerivedGeneralCategory.txt): Cc, Cf, Zl, Zp, minus U+200C and
// U+200D (ZWNJ, ZWJ), which join emoji and scripts and hide nothing.
inline constexpr Range kRefused[] = {
    {0x0000, 0x001F}, {0x007F, 0x009F}, {0x00AD, 0x00AD}, {0x0600, 0x0605}, {0x061C, 0x061C},
    {0x06DD, 0x06DD}, {0x070F, 0x070F}, {0x0890, 0x0891}, {0x08E2, 0x08E2}, {0x180E, 0x180E},
    {0x200B, 0x200B}, {0x200E, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x206F},
    {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x110BD, 0x110BD}, {0x110CD, 0x110CD}, {0x13430, 0x1343F},
    {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F},
};

// Unicode 16.0.0 White_Space (PropList.txt): never first or last, so a name can't pad itself.
inline constexpr Range kWhiteSpace[] = {
    {0x0009, 0x000D}, {0x0020, 0x0020}, {0x0085, 0x0085}, {0x00A0, 0x00A0}, {0x1680, 0x1680},
    {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000},
};

// Generated from Unicode 16.0.0: White_Space (PropList.txt) and Default_Ignorable_Code_Point
// (DerivedCoreProperties.txt), plus U+2800 (BRAILLE PATTERN BLANK, drawn empty). A name of only
// these shows nothing.
inline constexpr Range kInvisible[] = {
    {0x0009, 0x000D}, {0x0020, 0x0020}, {0x0085, 0x0085}, {0x00A0, 0x00A0}, {0x00AD, 0x00AD},
    {0x034F, 0x034F}, {0x061C, 0x061C}, {0x115F, 0x1160}, {0x1680, 0x1680}, {0x17B4, 0x17B5},
    {0x180B, 0x180F}, {0x2000, 0x200F}, {0x2028, 0x202F}, {0x205F, 0x206F}, {0x2800, 0x2800}, {0x3000, 0x3000},
    {0x3164, 0x3164}, {0xFE00, 0xFE0F}, {0xFEFF, 0xFEFF}, {0xFFA0, 0xFFA0}, {0xFFF0, 0xFFF8},
    {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0000, 0xE0FFF},
};

template <size_t N>
constexpr bool In(const Range (&table)[N], char32_t c) {
    size_t lo = 0, hi = N;  // sorted, disjoint
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (c < table[mid].lo) hi = mid;
        else if (c > table[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

// One code point from `s` at `i` (advanced past it); false on anything but shortest-form UTF-8 of
// a scalar value (no surrogates, nothing past U+10FFFF).
constexpr bool Next(std::string_view s, size_t& i, char32_t& out) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = byte(i);
    size_t len = 0;
    char32_t c = 0, min = 0;
    if (b0 < 0x80) { out = b0; ++i; return true; }
    if ((b0 & 0xE0) == 0xC0) { len = 2; c = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; c = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; c = b0 & 0x07; min = 0x10000; }
    else return false;
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) return false;
        c = (c << 6) | (byte(i + k) & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
    out = c;
    i += len;
    return true;
}

}  // namespace app_name_detail

// 1-64 bytes of UTF-8, none of it refused, at least one character visible, no space at either end.
constexpr bool ValidAppName(std::string_view name) {
    using namespace app_name_detail;
    if (name.empty() || name.size() > kAppNameMaxBytes) return false;
    bool visible = false;
    char32_t c = 0, lead = 0;
    for (size_t i = 0; i < name.size();) {
        if (!Next(name, i, c) || In(kRefused, c)) return false;
        if (!lead) lead = c;  // never 0: U+0000 is refused
        visible |= !In(kInvisible, c);
    }
    return visible && !In(kWhiteSpace, lead) && !In(kWhiteSpace, c);  // `c`: the last code point
}

}  // namespace ombra
