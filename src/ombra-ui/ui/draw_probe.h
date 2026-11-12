// What a frame drew as text, for `ombra-ui.exe --drive` (driver.cpp): find-by-text clicks, a text
// dump of the screen and overflow checks, without a widget tree to walk. Every widget draws its text
// through DrawString, so one hook sees all of it. Off in normal runs: `g_draw_probe` is null and the
// cost is one pointer test per string.
#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <string>
#include <vector>

namespace ombra_ui {

struct DrawnText {
    std::wstring text;
    D2D1_RECT_F  rect{};          // layout box in view pixels (the render target's transform applied)
    float        text_w = 0.0f;   // measured ink box, view pixels
    float        text_h = 0.0f;
    UINT32       lines  = 0;
    bool         overflow_x = false;  // a line is wider than the box and spills or is clipped
    bool         trimmed    = false;  // wider than the box, cut with the format's ellipsis on purpose
    bool         overflow_y = false;  // the lines are taller than the box
    bool         occluded   = false;  // drawn earlier, then covered by a popup (Occlude)
};

class DrawProbe {
public:
    explicit DrawProbe(IDWriteFactory* dwrite) : dwrite_(dwrite) {}
    void Clear() { texts.clear(); }
    void Record(ID2D1RenderTarget* rt, const wchar_t* s, UINT32 n, IDWriteTextFormat* fmt, const D2D1_RECT_F& r);
    // Text drawn some other way (a text field's scrolling layout): recorded where it shows, unmeasured.
    void Note(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& r);
    // An opaque popup drawn over `r`: text already recorded under it can't be seen or clicked.
    void Occlude(ID2D1RenderTarget* rt, const D2D1_RECT_F& r);
    // Text recorded since `first` that lies wholly outside the clip `r` (a scroll view's) was never
    // seen: marked occluded, so a card scrolled under the Save bar doesn't read as an overlap. Text
    // the clip cuts keeps only its visible rows.
    void ClipSince(size_t first, ID2D1RenderTarget* rt, const D2D1_RECT_F& r);

    std::vector<DrawnText> texts;

private:
    IDWriteFactory* dwrite_;
};

extern DrawProbe* g_draw_probe;

// All widget text goes through here (not ID2D1RenderTarget::DrawText directly).
inline void DrawString(ID2D1RenderTarget* rt, const wchar_t* s, UINT32 n, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& r, ID2D1Brush* brush,
                       D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE) {
    if (g_draw_probe) g_draw_probe->Record(rt, s, n, fmt, r);
    rt->DrawTextW(s, n, fmt, r, brush, options);
}

}  // namespace ombra_ui
