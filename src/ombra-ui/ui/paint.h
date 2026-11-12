#pragma once

// Draw helpers shared by the controls, containers and screens. Everything here reads only the
// theme's tokens.

#include "ombra-ui/ui/draw_probe.h"
#include "ombra-ui/ui/theme.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ombra_ui::paint {

using Microsoft::WRL::ComPtr;

// One draw call's context: the dc, the tokens, the DPI scale and the disabled fade (everything a
// disabled control draws is at 40%).
struct Paint {
    ID2D1DeviceContext* dc;
    const Theme& t;
    float s;
    float op = 1.0f;

    D2D1_COLOR_F A(D2D1_COLOR_F c, float a = 1.0f) const { c.a *= a * op; return c; }
    ComPtr<ID2D1SolidColorBrush> B(const D2D1_COLOR_F& c) const {
        ComPtr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(A(c), &b);
        return b;
    }
    // Direct2D draws an ellipse when a radius passes half the box; a pill caps at half.
    static float Cap(const D2D1_RECT_F& r, float radius) {
        return (std::min)(radius, (std::min)(r.right - r.left, r.bottom - r.top) * 0.5f);
    }
    void Fill(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) const {
        radius = Cap(r, radius);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c).Get());
    }
    // A 1-DIP (or `w`) line inside `r`.
    void Stroke(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w = 1.0f) const {
        radius = Cap(r, radius);
        const float i = w * s * 0.5f;
        const auto in = D2D1::RectF(r.left + i, r.top + i, r.right - i, r.bottom - i);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(in, (std::max)(0.0f, radius - i), (std::max)(0.0f, radius - i)),
                                 B(c).Get(), w * s);
    }
    // A card's outline, unless ui.card_borders is off (then the fill alone marks it).
    void CardEdge(const D2D1_RECT_F& r, float radius) const {
        if (t.size.card_lines) Stroke(r, radius, t.color.line);
    }
    void Circle(float cx, float cy, float r, const D2D1_COLOR_F& c) const {
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), B(c).Get());
    }
    void Ring(float cx, float cy, float r, const D2D1_COLOR_F& c, float w) const {
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), B(c).Get(), w * s);
    }
    void Line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& c, float w = 1.5f) const {
        dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), B(c).Get(), w * s);
    }
    void Text(const std::wstring& str, TypeRole role, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
              TextOpts o = {}) const {
        if (IDWriteTextFormat* f = t.Font(role, o))
            DrawString(dc, str.c_str(), static_cast<UINT32>(str.size()), f, r, B(c).Get());
    }
    float TextW(const std::wstring& str, TypeRole role) const { return t.MeasureText(t.Font(role), str).width; }
    // The caret, or a divider inside a field: icon-high, centred in the control.
    void Bar(float x, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) const {
        const float inset = (r.bottom - r.top - t.size.icon() * s) * 0.5f;
        Line(x, r.top + inset, x, r.bottom - inset, c, 1.0f);
    }
    // A caption under a control (error, warning), as wide as its text.
    void Note(const std::wstring& str, float x, float y, const D2D1_COLOR_F& c) const {
        Text(str, TypeRole::Caption, D2D1::RectF(x, y, x + TextW(str, TypeRole::Caption) + 1, y + LineHeight(TypeRole::Caption) * s), c);
    }
    // The keyboard focus ring, 2 DIP, outside the control with a 2 DIP gap.
    void Focus(const D2D1_RECT_F& r, float radius) const {
        const float g = t.size.space_xxs() * s;
        Stroke(D2D1::RectF(r.left - 2 * g, r.top - 2 * g, r.right + 2 * g, r.bottom + 2 * g), radius + 2 * g, t.color.focus, 2.0f);
    }
    // Chevron pointing down (or up), centred at (cx, cy).
    void Chevron(float cx, float cy, bool up, const D2D1_COLOR_F& c) const {
        const float w = t.size.glyph() * 0.4f * s, h = (up ? -0.2f : 0.2f) * t.size.glyph() * s;
        Line(cx - w, cy - h, cx, cy + h, c);
        Line(cx, cy + h, cx + w, cy - h, c);
    }
    // Text entry with the caret (or a shortcut waiting for keys): the usual outline plus a 2 DIP
    // accent underline.
    void Typing(const D2D1_RECT_F& r, float radius) const {
        Fill(D2D1::RectF(r.left + radius * 0.5f, r.bottom - 2 * s, r.right - radius * 0.5f, r.bottom), 1 * s, t.color.accent);
    }
    // A key seen from the front (a shortcut): a raised top over a darker 2 DIP skirt. Shared by the
    // shortcut field and Home, so every shortcut on screen is the same key.
    // `compact`: Caption type and space_sm sides, for a key inside a line of compact text.
    float KeyW(const std::wstring& k, float h, bool compact = false) const {
        return (std::max)(h, TextW(k, compact ? TypeRole::Caption : TypeRole::Label) +
                                 2 * (compact ? t.size.space_sm() : t.size.space_md()) * s);
    }
    void Key(const std::wstring& k, const D2D1_RECT_F& cap, bool compact = false) const {
        const float kr = t.size.key_radius(cap.bottom - cap.top - 2 * s, s);
        const auto top = D2D1::RectF(cap.left, cap.top, cap.right, cap.bottom - 2 * s);
        Fill(cap, kr, t.color.line_strong);
        Fill(top, kr, t.color.raised);
        Stroke(top, kr, t.color.line);
        Text(k, compact ? TypeRole::Caption : TypeRole::Label, top, t.color.text, {TextAlign::Center});
    }
    // "Ctrl+Alt+F10" as keys from x, `h` tall, centred on cy, space_xs apart; returns the width.
    float Keys(const std::vector<std::wstring>& caps, float x, float cy, float h, bool draw = true, bool compact = false) const {
        const float gap = t.size.space_xs() * s;
        float w = 0;
        for (size_t i = 0; i < caps.size(); ++i) {
            const float kw = KeyW(caps[i], h, compact);
            if (draw) Key(caps[i], D2D1::RectF(x + w, cy - h * 0.5f, x + w + kw, cy + h * 0.5f), compact);
            w += kw + (i + 1 < caps.size() ? gap : 0.0f);
        }
        return w;
    }
    // A check mark centred at (cx, cy), glyph-sized.
    void Check(float cx, float cy, const D2D1_COLOR_F& c) const {
        const float u = t.size.glyph() * 0.1f * s;
        Line(cx - 4 * u, cy, cx - 1.2f * u, cy + 2.8f * u, c, 1.75f);
        Line(cx - 1.2f * u, cy + 2.8f * u, cx + 4 * u, cy - 2.8f * u, c, 1.75f);
    }
    // A scrolling view's thumb: a 4 DIP pill centred on x `cx`, sized and placed by how much of
    // `content_h` the `view_h` from `top` shows. Nothing when it all fits.
    void ScrollThumb(float cx, float top, float view_h, float content_h, float scroll_y) const {
        if (content_h <= view_h + 1.0f) return;
        const float w = 4 * s, h = (std::max)(t.size.control_xs() * s, view_h * view_h / content_h);
        const float y = top + (view_h - h) * std::clamp(scroll_y / (content_h - view_h), 0.0f, 1.0f);
        Fill(D2D1::RectF(cx - w * 0.5f, y, cx + w * 0.5f, y + h), w * 0.5f, t.color.line_strong);
    }
};

inline float Cy(const D2D1_RECT_F& r) { return (r.top + r.bottom) * 0.5f; }

// "Ctrl+Alt+F10" -> {"Ctrl", "Alt", "F10"}; empty -> none.
inline std::vector<std::wstring> SplitKeys(const std::wstring& spec) {
    std::vector<std::wstring> parts;
    if (spec.empty()) return parts;
    size_t start = 0;
    for (;;) {
        const size_t plus = spec.find(L'+', start);
        parts.push_back(spec.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    return parts;
}

// A box of height h, vertically centred in `bounds`, from x for w.
inline D2D1_RECT_F RowBox(const D2D1_RECT_F& bounds, float x, float w, float h) {
    const float y = Cy(bounds) - h * 0.5f;
    return D2D1::RectF(x, y, x + w, y + h);
}

inline D2D1_COLOR_F Over(const D2D1_COLOR_F& base, const D2D1_COLOR_F& layer) {
    const float a = layer.a;
    return D2D1::ColorF(base.r + (layer.r - base.r) * a, base.g + (layer.g - base.g) * a,
                        base.b + (layer.b - base.b) * a, base.a);
}

// a at k = 0, b at k = 1 (a hover fade between two colours).
inline D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float k) {
    return D2D1::ColorF(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k);
}
inline D2D1_COLOR_F Faded(D2D1_COLOR_F c, float k) { c.a *= k; return c; }

// Field background: raised, a hover layer (`hover`: 0..1, a fade) or the pressed layer on top.
inline D2D1_COLOR_F FieldFill(const Theme& t, float hover, bool pressed) {
    if (pressed) return Over(t.color.raised, t.color.PressedLayer());
    return Over(t.color.raised, t.color.HoverLayer(hover));
}

}  // namespace ombra_ui::paint
