#include "ombra-ui/ui/draw_probe.h"

#include <wrl/client.h>

#include <algorithm>

namespace ombra_ui {

DrawProbe* g_draw_probe = nullptr;

namespace {

// The layout box in view pixels: the widgets draw under translations (scroll areas) and the
// overlay never rotates, so mapping two corners is exact.
D2D1_RECT_F ToView(ID2D1RenderTarget* rt, const D2D1_RECT_F& r) {
    D2D1_MATRIX_3X2_F m;
    rt->GetTransform(&m);
    auto map = [&m](float x, float y) {
        return D2D1::Point2F(x * m._11 + y * m._21 + m._31, x * m._12 + y * m._22 + m._32);
    };
    const D2D1_POINT_2F a = map(r.left, r.top), b = map(r.right, r.bottom);
    return D2D1::RectF((std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::max)(a.x, b.x), (std::max)(a.y, b.y));
}

// `own_layout`: the trimming check below changes the layout, so only on one made for measuring.
void Measure(IDWriteTextLayout* layout, float box_w, float box_h, DrawnText& t, bool own_layout) {
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return;
    t.text_w = m.width;
    t.text_h = m.height;
    t.lines  = m.lineCount;
    // Half a pixel of slack: DirectWrite's fractional advances round differently from the boxes.
    t.overflow_x = m.width > box_w + 0.5f;
    // Vertically, a line a pixel or two taller than a tight box isn't clipped (DrawString doesn't
    // clip unless asked); a fifth of a line over means a wrapped line spilling out.
    t.overflow_y = m.height > box_h + (std::max)(2.0f, 0.2f * m.height / (std::max)(1u, m.lineCount));
    // A trimmed line (ellipsis) reports the trimmed width; compare against the untrimmed one.
    DWRITE_TRIMMING trim{};
    Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
    if (own_layout && SUCCEEDED(layout->GetTrimming(&trim, &sign)) && trim.granularity != DWRITE_TRIMMING_GRANULARITY_NONE) {
        DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&none, nullptr);
        if (SUCCEEDED(layout->GetMetrics(&m)) && m.widthIncludingTrailingWhitespace > box_w + 0.5f) {
            t.trimmed    = true;
            t.overflow_x = false;
        }
    }
}

}  // namespace

void DrawProbe::Record(ID2D1RenderTarget* rt, const wchar_t* s, UINT32 n, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& r) {
    DrawnText t;
    t.text.assign(s, n);
    t.rect = ToView(rt, r);
    const float w = (std::max)(0.0f, r.right - r.left), h = (std::max)(0.0f, r.bottom - r.top);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (dwrite_ && fmt && SUCCEEDED(dwrite_->CreateTextLayout(s, n, fmt, w, h, &layout))) Measure(layout.Get(), w, h, t, true);
    texts.push_back(std::move(t));
}

void DrawProbe::Occlude(ID2D1RenderTarget* rt, const D2D1_RECT_F& r) {
    const D2D1_RECT_F v = ToView(rt, r);
    for (DrawnText& t : texts) {
        if (t.rect.left < v.right && t.rect.right > v.left && t.rect.top < v.bottom && t.rect.bottom > v.top)
            t.occluded = true;
    }
}

void DrawProbe::ClipSince(size_t first, ID2D1RenderTarget* rt, const D2D1_RECT_F& r) {
    const D2D1_RECT_F v = ToView(rt, r);
    for (size_t i = first; i < texts.size(); ++i) {
        DrawnText& t = texts[i];
        if (t.rect.left >= v.right || t.rect.right <= v.left || t.rect.top >= v.bottom || t.rect.bottom <= v.top) {
            t.occluded = true;
        } else {
            // Cut by the clip (a row half scrolled out): only the part inside shows, or can be clicked.
            t.rect.top = (std::max)(t.rect.top, v.top);
            t.rect.bottom = (std::min)(t.rect.bottom, v.bottom);
        }
    }
}

void DrawProbe::Note(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& r) {
    DrawnText t;
    t.text = s;
    t.rect = ToView(rt, r);
    texts.push_back(std::move(t));
}

}  // namespace ombra_ui
