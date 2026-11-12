#include "ombra-ui/ui/containers.h"

#include "ombra-ui/ui/color.h"
#include "ombra-ui/ui/draw_probe.h"
#include "ombra-ui/ui/paint.h"
#include "ombra/brand_mark.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ombra_ui {

using namespace paint;

namespace {

// Children get every move/press/release (a press elsewhere clears their focus), keys until one
// takes it, and the cursor of the first that wants one.
template <class V> bool RouteMove(const V& kids, D2D1_POINT_2F pt) {
    bool changed = false;
    for (const auto& k : kids) if (k) changed |= k->OnMouseMove(pt);
    return changed;
}
// The ones the press missed first, then the one it hit: what a hit starts (reset beginning a
// shortcut recording) must not be undone by a sibling reacting to the same press as a click elsewhere.
template <class V> bool RouteDown(const V& kids, D2D1_POINT_2F pt) {
    bool took = false;
    for (const auto& k : kids) if (k && !k->HitTest(pt)) took |= k->OnMouseDown(pt);
    for (const auto& k : kids) if (k && k->HitTest(pt)) took |= k->OnMouseDown(pt);
    return took;
}
template <class V> bool RouteUp(const V& kids, D2D1_POINT_2F pt) {
    bool changed = false;
    for (const auto& k : kids) if (k) changed |= k->OnMouseUp(pt);
    return changed;
}
template <class V> bool RouteKey(const V& kids, UINT vk) {
    for (const auto& k : kids) if (k && k->OnKeyDown(vk)) return true;
    return false;
}
template <class V> bool RouteTooltip(const V& kids, std::wstring* text, D2D1_POINT_2F* anchor) {
    for (const auto& k : kids)
        if (k && k->visible && k->QueryTooltip(text, anchor)) return true;
    return false;
}
template <class V> CursorKind RouteCursor(const V& kids, D2D1_POINT_2F pt) {
    for (const auto& k : kids)
        if (k && k->CursorAt(pt) != CursorKind::Arrow) return k->CursorAt(pt);
    return CursorKind::Arrow;
}

float HelpHeight(const std::wstring& help, float w, const LayoutContext& ctx) {
    if (help.empty()) return 0.0f;
    if (!ctx.theme) return LineHeight(TypeRole::Caption) * ctx.dpi_scale;
    const Theme& t = *ctx.theme;
    return std::ceil(t.MeasureText(t.Font(TypeRole::Caption, {TextAlign::Leading, true, false, false}), help, w).height);
}

// The six-dot grip's column before a draggable row's text.
float GripWidth(const Sizes& z, float s) { return (z.space_sm() + z.space_md()) * s; }

// A note's text starts after its status dot.
float NoteIndent(const Sizes& z, float s) { return (z.space_sm() + z.space_xs()) * s; }

D2D1_COLOR_F ToneColor(const ColorRoles& c, SettingRow::Tone tone, const D2D1_COLOR_F& neutral) {
    switch (tone) {
        case SettingRow::Tone::Good:    return c.success;
        case SettingRow::Tone::Warning: return c.warning;
        case SettingRow::Tone::Error:   return c.danger;
        default:                        return neutral;
    }
}

}  // namespace

// --- Setting row --------------------------------------------------------------------------------

float SettingRow::Geometry::TextHeight(const Sizes& z, float s) const {
    return title_h + (help_h > 0 ? z.space_xxs() * s + help_h : 0.0f) + (note_h > 0 ? z.space_xs() * s + note_h : 0.0f) +
           (below_h > 0 ? z.space_xs() * s + below_h : 0.0f);
}

SettingRow::Geometry SettingRow::Layout(float w, const LayoutContext& ctx) const {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, pad_x = z.card_padding() * s, pad_y = z.space_md() * s;
    Geometry g;
    int n = 0;
    const Widget* last = nullptr;
    D2D1_SIZE_F last_size{};
    g.text_x = pad_x + (grip ? GripWidth(z, s) : 0.0f) + (HasPicture() ? (z.app_icon() + z.space_md()) * s : 0.0f);
    LayoutConstraints room;  // controls_below: the text column's width; otherwise their own
    if (controls_below) room.max_w = (std::max)(0.0f, w - g.text_x - pad_x);
    for (const auto& k : controls) {
        if (!k || !k->visible) continue;
        const D2D1_SIZE_F m = k->Measure(room, ctx);
        g.controls_w += m.width + (n++ ? z.space_sm() * s : 0.0f);
        g.controls_h = (std::max)(g.controls_h, m.height);
        last = k.get();
        last_size = m;
    }
    g.text_w = w - g.text_x - pad_x - (n == 0 ? 0.0f : g.controls_w + z.space_xl() * s);
    g.stacked = n > 0 && (controls_below || g.text_w < z.row_text_min() * s);
    if (g.stacked) {
        g.text_w = w - g.text_x - pad_x;
        // A trailing switch stays at the right edge on the title's line, so every row's switch
        // lines up whether or not its other controls had to move under the text.
        if (n > 1 && dynamic_cast<const SwitchControl*>(last)) {
            g.pinned = last;
            g.pinned_size = last_size;
            g.text_w -= last_size.width + z.space_xl() * s;
            g.controls_w -= last_size.width + z.space_sm() * s;
        }
        // Under the text, or further left if that keeps them on one line inside the right padding
        // (a grip's three menus and a switch at 150%); otherwise under the text, wrapping.
        const float gap = z.space_sm() * s, right = w - pad_x;
        const float x0 = right - g.controls_w >= pad_x ? (std::min)(g.text_x, right - g.controls_w) : g.text_x;
        float x = x0, y = 0, line_h = 0;
        size_t line_start = 0;
        auto end_line = [&] {  // centre the line's controls on its tallest
            for (size_t i = line_start; i < g.stacked_at.size(); ++i) {
                D2D1_RECT_F& r = g.stacked_at[i];
                const float dy = (line_h - (r.bottom - r.top)) * 0.5f;
                r.top += dy;
                r.bottom += dy;
            }
        };
        for (const auto& k : controls) {
            if (!k || !k->visible || k.get() == g.pinned) continue;
            const D2D1_SIZE_F m = k->Measure(room, ctx);
            if (x > x0 && x + m.width > right) {
                end_line();
                line_start = g.stacked_at.size();
                y += line_h + gap;
                x = x0;
                line_h = 0;
            }
            g.stacked_at.push_back(D2D1::RectF(x, y, x + m.width, y + m.height));
            line_h = (std::max)(line_h, m.height);
            x += m.width + gap;
        }
        end_line();
        g.controls_h = y + line_h;
    }
    g.title_h = LineHeight(TypeRole::Body) * s;
    g.help_h = help_one_line ? (help.empty() ? 0.0f : LineHeight(TypeRole::Caption) * s) : HelpHeight(help, g.text_w, ctx);
    g.note_h = HelpHeight(note, g.text_w - NoteIndent(z, s), ctx);
    if (below && below->visible) g.below_h = below->Measure(LayoutConstraints{}, ctx).height;
    const float text_h = (std::max)(g.TextHeight(z, s), g.pinned_size.height);
    g.height = g.stacked ? 2 * pad_y + text_h + z.space_sm() * s + g.controls_h
                         : 2 * pad_y + (std::max)(text_h, g.controls_h);
    g.height = (std::max)(g.height, (z.control_sm() + 2 * z.space_md()) * s);
    return g;
}

D2D1_SIZE_F SettingRow::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? c.max_w : 600 * ctx.dpi_scale;
    return D2D1::SizeF(w, Layout(w, ctx).height);
}

void SettingRow::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    g_ = Layout(rect.right - rect.left, ctx);
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, pad_x = z.card_padding() * s, pad_y = z.space_md() * s;
    if (below && below->visible) {
        const float text_h = g_.TextHeight(z, s);
        const float top = g_.stacked || align_top ? rect.top + pad_y : (rect.top + rect.bottom - text_h) * 0.5f;
        const float x = rect.left + g_.text_x, y = top + text_h - g_.below_h;
        const float bw = (std::min)(below->Measure(LayoutConstraints{}, ctx).width, g_.text_w);
        below->Arrange(D2D1::RectF(x, y, x + bw, y + g_.below_h), ctx);
    }
    if (g_.stacked) {
        const float band_top = rect.bottom - pad_y - g_.controls_h;
        size_t i = 0;
        for (const auto& k : controls) {
            if (!k || !k->visible) continue;
            if (k.get() == g_.pinned) {
                const float band_h = (std::max)(g_.TextHeight(z, s), g_.pinned_size.height);
                const float top = rect.top + pad_y + (band_h - g_.pinned_size.height) * 0.5f;
                k->Arrange(D2D1::RectF(rect.right - pad_x - g_.pinned_size.width, top, rect.right - pad_x, top + g_.pinned_size.height), ctx);
                continue;
            }
            const D2D1_RECT_F& at = g_.stacked_at[i++];
            k->Arrange(D2D1::RectF(rect.left + at.left, band_top + at.top, rect.left + at.right, band_top + at.bottom), ctx);
        }
        return;
    }
    float x = rect.right - pad_x - g_.controls_w;
    for (const auto& k : controls) {
        if (!k || !k->visible) continue;
        const D2D1_SIZE_F m = k->Measure(LayoutConstraints{}, ctx);
        // Top-aligned controls share the field line: a shorter one centres on a control-tall line.
        const float y = align_top ? rect.top + pad_y + (std::max)(0.0f, (z.control() * s - m.height) * 0.5f)
                                  : (rect.top + rect.bottom - m.height) * 0.5f;
        k->Arrange(D2D1::RectF(x, y, x + m.width, y + m.height), ctx);
        x += m.width + z.space_sm() * s;
    }
}

D2D1_RECT_F SettingRow::GripRect() const {
    if (!grip) return {};
    return D2D1::RectF(bounds.left, bounds.top, bounds.left + g_.text_x, bounds.bottom);
}

void SettingRow::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, !enabled ? 0.4f : lifted ? 0.3f : 1.0f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const D2D1_RECT_F& r = bounds;
    if (divider_above) p.Line(r.left, r.top + 0.5f * s, r.right, r.top + 0.5f * s, c.line, 1.0f);
    if (attention) {
        const float in = z.space_xs() * s;
        p.Fill(D2D1::RectF(r.left + in, r.top + in, r.right - in, r.bottom - in), z.radius_control() * s,
               D2D1::ColorF(c.warning.r, c.warning.g, c.warning.b, c.soft_a));
    }
    if (HasPicture()) {
        const float box = z.app_icon() * s;
        const float cx = r.left + g_.text_x - z.space_md() * s - box * 0.5f, cy = Cy(r);
        if (icon && !icon->bgra.empty()) {
            if (!icon_bitmap_) {
                const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(icon->w, icon->h), icon->bgra.data(), icon->w * 4, &props, &icon_bitmap_);
            }
            if (icon_bitmap_) {
                // Aspect-fit. Steam's cached icons are 32 px, drawn larger above 100%: cubic keeps
                // that soft rather than blocky.
                const D2D1_SIZE_F bs = icon_bitmap_->GetSize();
                const float k = (std::min)(box / bs.width, box / bs.height), hw = bs.width * k * 0.5f, hh = bs.height * k * 0.5f;
                dc->DrawBitmap(icon_bitmap_.Get(), D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh),
                               p.op * (faded ? 0.35f : 1.0f), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            }
        } else {
            DrawIcon(dc, t, glyph, cx, cy, box * 0.6f, p.B(faded ? c.text_off : c.text2).Get());
        }
    }
    if (grip) {
        // Two columns of three dots, centred in the padding before the text; brighter under the mouse.
        const float u = z.space_xs() * s, dot = 0.75f * s;
        const float cx = r.left + z.card_padding() * s * 0.5f + GripWidth(z, s) * 0.5f, cy = Cy(r);
        for (int i = 0; i < 2; ++i)
            for (int j = -1; j <= 1; ++j)
                p.Circle(cx + (i ? 0.5f : -0.5f) * u, cy + j * u, dot, hovered ? c.text2 : c.text3);
    }
    const float text_h = g_.TextHeight(z, s);
    const float top = g_.stacked || align_top ? r.top + z.space_md() * s : (r.top + r.bottom - text_h) * 0.5f;
    const float x = r.left + g_.text_x;
    float title_right = x + g_.text_w;
    tag_rect_ = {};
    if (!tag.empty()) {
        // The pill follows the title, so the title gets whatever the pill leaves.
        const float pad = z.space_sm() * s, tw = p.TextW(tag, TypeRole::Caption);
        const float pill_w = tw + 2 * pad, ph = (LineHeight(TypeRole::Caption) + z.space_xxs() * 2) * s;
        const float title_w = (std::min)(p.TextW(title, TypeRole::Body) + 1, g_.text_w - pill_w - z.space_sm() * s);
        if (title_w > 0) {
            title_right = x + title_w;
            const float px = title_right + z.space_sm() * s, cy = top + g_.title_h * 0.5f;
            tag_rect_ = D2D1::RectF(px, cy - ph * 0.5f, px + pill_w, cy + ph * 0.5f);
            p.Fill(tag_rect_, z.radius_full(ph, s), c.HoverLayer(tag_hovered_ ? 2.0f : 1.5f));
            p.Text(tag, TypeRole::Caption, tag_rect_, c.text2, {TextAlign::Center});
        }
    }
    p.Text(title, TypeRole::Body, D2D1::RectF(x, top, title_right, top + g_.title_h), c.text,
           {TextAlign::Leading, false, true});
    float y = top + g_.title_h;
    if (g_.help_h > 0) {
        y += z.space_xxs() * s;
        p.Text(help, TypeRole::Caption, D2D1::RectF(x, y, x + g_.text_w, y + g_.help_h), ToneColor(c, help_tone, c.text3),
               help_one_line ? TextOpts{TextAlign::Leading, false, true} : TextOpts{TextAlign::Leading, true, false, false});
        y += g_.help_h;
    }
    if (g_.note_h > 0) {
        y += z.space_xs() * s;
        const float dot = z.space_xs() * 0.75f * s;
        p.Circle(x + dot, y + LineHeight(TypeRole::Caption) * s * 0.5f, dot, ToneColor(c, note_tone, c.text3));
        const bool loud = note_tone == Tone::Error || note_tone == Tone::Warning;
        p.Text(note, TypeRole::Caption, D2D1::RectF(x + NoteIndent(z, s), y, x + g_.text_w, y + g_.note_h),
               loud ? ToneColor(c, note_tone, c.text2) : c.text2, {TextAlign::Leading, true, false, false});
    }
    if (below) below->Draw(dc, t, s);
    for (const auto& k : controls) if (k) k->Draw(dc, t, s);
}

CursorKind SettingRow::CursorAt(D2D1_POINT_2F pt) const {
    if (grip && on_grip && PointIn(GripRect(), pt)) return CursorKind::Move;
    return RouteCursor(controls, pt);
}
bool SettingRow::OnMouseMove(D2D1_POINT_2F pt) {
    const bool self = Widget::OnMouseMove(pt);
    const bool th = !tag_tooltip.empty() && tag_rect_.right > tag_rect_.left && PointIn(tag_rect_, pt);
    const bool tag_changed = th != tag_hovered_;
    tag_hovered_ = th;
    return RouteMove(controls, pt) || self || tag_changed;
}
bool SettingRow::OnMouseDown(D2D1_POINT_2F pt) {
    if (grip && on_grip && PointIn(GripRect(), pt)) {
        auto f = on_grip;  // the drag rebuilds the page, this row included
        f();
        return true;
    }
    return RouteDown(controls, pt);
}
bool SettingRow::OnMouseUp(D2D1_POINT_2F pt) { return RouteUp(controls, pt); }
bool SettingRow::OnKeyDown(UINT vk) { return RouteKey(controls, vk); }
bool SettingRow::QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const {
    if (tag_hovered_) {
        if (text) *text = tag_tooltip;
        if (anchor) *anchor = D2D1::Point2F((tag_rect_.left + tag_rect_.right) * 0.5f, tag_rect_.top);
        return true;
    }
    return RouteTooltip(controls, text, anchor);
}

// --- Row list -----------------------------------------------------------------------------------

float RowList::LeadHeight(size_t n) const {
    float h = 0;
    for (size_t i = 0; i < n && i < heights_.size(); ++i) h += heights_[i];
    return heights_.empty() ? empty_h_ : h;
}

D2D1_SIZE_F RowList::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, w = c.BoundedWidth() ? c.max_w : 600 * s;
    heights_.clear();
    content_h_ = 0;
    for (const auto& r : rows) {
        heights_.push_back(r ? r->Measure(LayoutConstraints::Exact(w), ctx).height : 0.0f);
        content_h_ += heights_.back();
    }
    empty_h_ = (z.control_sm() + 2 * z.space_md()) * s;
    if (rows.empty()) content_h_ = empty_h_;
    const float h = viewport_h_dip < 0 ? content_h_ : (std::min)(content_h_, viewport_h_dip * s);
    return D2D1::SizeF(w, h);
}

void RowList::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    ctx_ = ctx;
    scroll_y = std::clamp(scroll_y, 0.0f, MaxScroll());
    float y = rect.top - scroll_y;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i]) continue;
        const float h = i < heights_.size() ? heights_[i] : 0.0f;
        rows[i]->divider_above = i > 0;
        rows[i]->Arrange(D2D1::RectF(rect.left, y, rect.right, y + h), ctx);
        y += h;
    }
}

void RowList::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
    if (rows.empty()) {
        const float pad = t.size.card_padding() * s;
        p.Text(empty_text, TypeRole::Caption, D2D1::RectF(bounds.left + pad, bounds.top, bounds.right - pad, bounds.bottom),
               t.color.text3, {TextAlign::Leading, true, false, true});
    }
    const size_t probe_first = g_draw_probe ? g_draw_probe->texts.size() : 0;
    for (const auto& r : rows)
        if (r && r->bounds.bottom > bounds.top && r->bounds.top < bounds.bottom) r->Draw(dc, t, s);
    if (g_draw_probe) g_draw_probe->ClipSince(probe_first, dc, bounds);  // a row cut in half isn't hidden, one scrolled out is
    dc->PopAxisAlignedClip();
    // Rows scrolled under the header: a hairline marks where they go.
    if (scroll_y > 0) p.Line(bounds.left, bounds.top + 0.5f * s, bounds.right, bounds.top + 0.5f * s, t.color.line, 1.0f);
    // In the rows' right padding, clear of their controls.
    p.ScrollThumb(bounds.right - 6 * s, bounds.top, bounds.bottom - bounds.top, content_h_, scroll_y);
}

bool RowList::OnWheel(D2D1_POINT_2F pt, float delta) {
    if (!visible || !PointIn(bounds, pt) || MaxScroll() <= 0) return false;
    const float next = std::clamp(scroll_y - delta * 40.0f * ctx_.dpi_scale, 0.0f, MaxScroll());
    if (next == scroll_y) return false;  // at the end: the page takes it
    scroll_y = next;
    Arrange(bounds, ctx_);
    if (on_scroll) on_scroll(scroll_y);
    return true;
}

D2D1_POINT_2F RowList::Inside(D2D1_POINT_2F pt) const {
    return PointIn(bounds, pt) ? pt : D2D1::Point2F(-1e6f, -1e6f);
}
CursorKind RowList::CursorAt(D2D1_POINT_2F pt) const { return RouteCursor(rows, Inside(pt)); }
bool RowList::OnMouseMove(D2D1_POINT_2F pt) { return RouteMove(rows, Inside(pt)); }
bool RowList::OnMouseDown(D2D1_POINT_2F pt) { return RouteDown(rows, Inside(pt)); }
bool RowList::OnMouseUp(D2D1_POINT_2F pt) { return RouteUp(rows, Inside(pt)); }
bool RowList::OnKeyDown(UINT vk) { return RouteKey(rows, vk); }
bool RowList::QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const { return RouteTooltip(rows, text, anchor); }

// --- Sent list ----------------------------------------------------------------------------------

namespace {
constexpr int kSentBoxLines = 10;  // the open box shows this many lines of text, then scrolls
}  // namespace

SentList::Geometry SentList::Layout(float w, const LayoutContext& ctx) const {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, pad_x = z.card_padding() * s;
    const float text_x = pad_x + (z.app_icon() + z.space_md()) * s;
    Geometry g;
    float line = LineHeight(TypeRole::Label) * s;
    if (!items.empty()) line = (std::max)(line, folder->Measure(LayoutConstraints{}, ctx).height);
    g.help_h = HelpHeight(help, w - 2 * pad_x, ctx);
    g.head_h = z.space_md() * s + line + (g.help_h > 0 ? z.space_xxs() * s + g.help_h : 0.0f) + z.space_xs() * s;
    g.row_h = (2 * z.space_sm() + LineHeight(TypeRole::Body) + z.space_xxs() + LineHeight(TypeRole::Caption)) * s;
    if (open >= 0 && open < static_cast<int>(items.size())) {
        const float pad = z.space_sm() * s, box_w = w - text_x - pad_x;
        const float text_w = (std::max)(0.0f, box_w - 2 * pad - z.space_sm() * s);  // the thumb's strip
        g.text_h = open_text.empty() || !ctx.theme
                       ? LineHeight(TypeRole::Caption) * s
                       : std::ceil(ctx.theme->MeasureText(ctx.theme->Font(TypeRole::Caption, {TextAlign::Leading, true, false, false}),
                                                          open_text, text_w).height);
        g.box_h = (std::min)(g.text_h, kSentBoxLines * LineHeight(TypeRole::Caption) * s) + 2 * pad;
        g.foot_h = (std::max)(copy->Measure(LayoutConstraints{}, ctx).height, notepad->Measure(LayoutConstraints{}, ctx).height);
        g.open_h = g.box_h + z.space_sm() * s + g.foot_h + z.space_md() * s;
    }
    const float rows = items.empty() ? (z.control_sm() + 2 * z.space_md()) * s : g.row_h * items.size();
    g.height = g.head_h + rows + g.open_h + z.space_xs() * s;
    return g;
}

D2D1_SIZE_F SentList::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? c.max_w : 600 * ctx.dpi_scale;
    return D2D1::SizeF(w, Layout(w, ctx).height);
}

void SentList::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    s_ = ctx.dpi_scale;
    g_ = Layout(rect.right - rect.left, ctx);
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, pad_x = z.card_padding() * s, in = z.space_xs() * s;
    const float text_x = rect.left + pad_x + (z.app_icon() + z.space_md()) * s;
    folder->visible = !items.empty();
    copy->visible = notepad->visible = open >= 0 && open < static_cast<int>(items.size());
    if (folder->visible) {
        const D2D1_SIZE_F m = folder->Measure(LayoutConstraints{}, ctx);
        const float line = (std::max)(LineHeight(TypeRole::Label) * s, m.height), y = rect.top + z.space_md() * s + (line - m.height) * 0.5f;
        folder->Arrange(D2D1::RectF(rect.right - pad_x - m.width, y, rect.right - pad_x, y + m.height), ctx);
    }
    rows_.clear();
    box_ = note_ = {};
    box_pad_ = z.space_sm() * s;
    float y = rect.top + g_.head_h;
    for (size_t i = 0; i < items.size(); ++i) {
        rows_.push_back(D2D1::RectF(rect.left + in, y, rect.right - in, y + g_.row_h));
        y += g_.row_h;
        if (static_cast<int>(i) != open) continue;
        box_ = D2D1::RectF(text_x, y, rect.right - pad_x, y + g_.box_h);
        y += g_.box_h + z.space_sm() * s;
        const D2D1_SIZE_F np = notepad->Measure(LayoutConstraints{}, ctx), cp = copy->Measure(LayoutConstraints{}, ctx);
        float x = rect.right - pad_x - np.width;
        notepad->Arrange(D2D1::RectF(x, y + (g_.foot_h - np.height) * 0.5f, x + np.width, y + (g_.foot_h + np.height) * 0.5f), ctx);
        x -= z.space_sm() * s + cp.width;
        copy->Arrange(D2D1::RectF(x, y + (g_.foot_h - cp.height) * 0.5f, x + cp.width, y + (g_.foot_h + cp.height) * 0.5f), ctx);
        note_ = D2D1::RectF(text_x, y, x - z.space_md() * s, y + g_.foot_h);
        y += g_.foot_h + z.space_md() * s;
    }
    text_scroll = std::clamp(text_scroll, 0.0f, MaxScroll());
}

void SentList::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float pad_x = z.card_padding() * s, x0 = bounds.left + pad_x, right = bounds.right - pad_x;
    const float text_x = x0 + (z.app_icon() + z.space_md()) * s;
    p.Line(bounds.left, bounds.top + 0.5f * s, bounds.right, bounds.top + 0.5f * s, c.line, 1.0f);

    float line = LineHeight(TypeRole::Label) * s;
    if (folder->visible) line = (std::max)(line, folder->bounds.bottom - folder->bounds.top);
    const float hy = bounds.top + z.space_md() * s;
    p.Text(heading, TypeRole::Label, D2D1::RectF(x0, hy, (folder->visible ? folder->bounds.left - z.space_md() * s : right), hy + line),
           c.text, {TextAlign::Leading, false, true});
    if (g_.help_h > 0) {
        const float y = hy + line + z.space_xxs() * s;
        p.Text(help, TypeRole::Caption, D2D1::RectF(x0, y, right, y + g_.help_h), c.text3, {TextAlign::Leading, true, false, false});
    }
    folder->Draw(dc, t, s);

    if (items.empty()) {
        const float y = bounds.top + g_.head_h;
        p.Text(empty_text, TypeRole::Caption, D2D1::RectF(x0, y, right, y + (z.control_sm() + 2 * z.space_md()) * s), c.text3,
               {TextAlign::Leading, true, false, true});
        return;
    }
    const float body = LineHeight(TypeRole::Body) * s, cap = LineHeight(TypeRole::Caption) * s;
    for (size_t i = 0; i < items.size() && i < rows_.size(); ++i) {
        const Item& it = items[i];
        const D2D1_RECT_F& r = rows_[i];
        const bool is_open = static_cast<int>(i) == open;
        if (is_open || static_cast<int>(i) == hovered_row_)
            p.Fill(r, z.radius_control() * s, c.HoverLayer(is_open ? 1.5f : 1.0f));
        if (it.icon != IconType::None) {
            const float box = z.app_icon() * s;
            DrawIcon(dc, t, it.icon, x0 + box * 0.5f, Cy(r), box * 0.6f, p.B(c.text2).Get());
        }
        const float y1 = r.top + z.space_sm() * s, y2 = y1 + body + z.space_xxs() * s;
        const float when_w = p.TextW(it.when, TypeRole::Caption) + 1, status_w = p.TextW(it.status, TypeRole::Caption) + 1;
        const float dot = z.space_xs() * 0.75f * s, dot_room = NoteIndent(z, s);
        const float right_w = (std::max)(when_w, status_w + dot_room), text_r = right - right_w - z.space_lg() * s;
        p.Text(it.title, TypeRole::Body, D2D1::RectF(text_x, y1, text_r, y1 + body), c.text, {TextAlign::Leading, false, true});
        p.Text(it.summary, TypeRole::Caption, D2D1::RectF(text_x, y2, text_r, y2 + cap), c.text3, {TextAlign::Leading, false, true});
        p.Text(it.when, TypeRole::Caption, D2D1::RectF(right - when_w, y1, right, y1 + body), c.text3, {TextAlign::Trailing});
        const bool loud = it.tone == SettingRow::Tone::Warning || it.tone == SettingRow::Tone::Error;
        p.Circle(right - status_w - dot_room + dot, y2 + cap * 0.5f, dot, ToneColor(c, it.tone, c.text3));
        p.Text(it.status, TypeRole::Caption, D2D1::RectF(right - status_w, y2, right, y2 + cap), loud ? ToneColor(c, it.tone, c.text2) : c.text2,
               {TextAlign::Trailing});
        if (!is_open || box_.bottom <= box_.top) continue;

        p.Fill(box_, z.radius_control() * s, c.panel);
        p.Stroke(box_, z.radius_control() * s, c.line);
        const float inner_top = box_.top + box_pad_, inner_bottom = box_.bottom - box_pad_;
        const D2D1_RECT_F clip = D2D1::RectF(box_.left + 1, inner_top, box_.right - 1, inner_bottom);
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        const size_t probe_first = g_draw_probe ? g_draw_probe->texts.size() : 0;
        const float ty = inner_top - text_scroll;
        p.Text(open_text, TypeRole::Caption,
               D2D1::RectF(box_.left + box_pad_, ty, box_.right - box_pad_ - z.space_sm() * s, ty + g_.text_h + 1), c.text2,
               {TextAlign::Leading, true, false, false});
        if (g_draw_probe) g_draw_probe->ClipSince(probe_first, dc, clip);
        dc->PopAxisAlignedClip();
        p.ScrollThumb(box_.right - box_pad_, inner_top, inner_bottom - inner_top, g_.text_h, text_scroll);
        p.Text(open_note, TypeRole::Caption, note_, c.text3, {TextAlign::Leading, false, true});
        copy->Draw(dc, t, s);
        notepad->Draw(dc, t, s);
    }
}

std::vector<Widget*> SentList::Buttons() const {
    std::vector<Widget*> v;
    for (Widget* b : {static_cast<Widget*>(folder.get()), static_cast<Widget*>(copy.get()), static_cast<Widget*>(notepad.get())})
        if (b && b->visible) v.push_back(b);
    return v;
}

bool SentList::OnWheel(D2D1_POINT_2F pt, float delta) {
    if (!visible || !PointIn(box_, pt) || MaxScroll() <= 0) return false;
    const float next = std::clamp(text_scroll - delta * 40.0f * s_, 0.0f, MaxScroll());
    if (next == text_scroll) return false;  // at the end: the page takes it
    text_scroll = next;
    if (on_scroll) on_scroll(text_scroll);
    return true;
}

CursorKind SentList::CursorAt(D2D1_POINT_2F pt) const {
    const CursorKind k = RouteCursor(Buttons(), pt);
    if (k != CursorKind::Arrow) return k;
    for (const auto& r : rows_)
        if (PointIn(r, pt)) return CursorKind::Hand;
    return CursorKind::Arrow;
}

bool SentList::OnMouseMove(D2D1_POINT_2F pt) {
    const bool b = RouteMove(Buttons(), pt);
    int h = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (PointIn(rows_[i], pt)) h = static_cast<int>(i);
    const bool changed = h != hovered_row_;
    hovered_row_ = h;
    return b || changed;
}

bool SentList::OnMouseDown(D2D1_POINT_2F pt) {
    if (RouteDown(Buttons(), pt)) return true;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (PointIn(rows_[i], pt) && on_toggle) {
            auto f = on_toggle;  // the page rebuilds, this list included
            f(static_cast<int>(i));
            return true;
        }
    return false;
}

bool SentList::OnMouseUp(D2D1_POINT_2F pt) { return RouteUp(Buttons(), pt); }
bool SentList::OnKeyDown(UINT vk) { return RouteKey(Buttons(), vk); }
bool SentList::QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const { return RouteTooltip(Buttons(), text, anchor); }

// --- Drop slot ----------------------------------------------------------------------------------

D2D1_SIZE_F DropSlot::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float w = c.BoundedWidth() ? c.max_w : 600 * ctx.dpi_scale;
    return D2D1::SizeF(w, (z.control_sm() + 2 * z.space_md()) * ctx.dpi_scale);
}

void DropSlot::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const float rad = t.size.radius_card() * s, w = (active ? 1.5f : 1.0f) * s, i = w * 0.5f;
    if (active) p.Fill(bounds, rad, c.accent_soft);
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1StrokeStyle> dash;
    const float dashes[] = {4.0f, 3.0f};
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                           D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM),
                               dashes, 2, &dash);
    const auto in = D2D1::RectF(bounds.left + i, bounds.top + i, bounds.right - i, bounds.bottom - i);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(in, rad - i, rad - i), p.B(active ? c.accent : c.line_strong).Get(), w, dash.Get());
    p.Text(label, TypeRole::Body, bounds, active ? c.accent_text : c.text2, {TextAlign::Center});
}

// --- Card ---------------------------------------------------------------------------------------

namespace {
// A SettingRow (or a list of them) pads itself; anything else in a card (a tile grid) gets the card's padding.
bool FullBleed(Widget& r) { return dynamic_cast<SettingRow*>(&r) || dynamic_cast<RowList*>(&r) || dynamic_cast<SentList*>(&r); }
float BlockHeight(Widget& r, float w, const LayoutContext& ctx) {
    if (FullBleed(r)) return r.Measure(LayoutConstraints::Exact(w), ctx).height;
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float px = z.card_padding() * ctx.dpi_scale, py = z.space_md() * ctx.dpi_scale;
    return r.Measure(LayoutConstraints::Exact(w - 2 * px), ctx).height + 2 * py;
}
}  // namespace

float CardContainer::ActionsWidth(const LayoutContext& ctx) const {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    float w = 0;
    for (const auto& a : actions)
        if (a && a->visible) w += a->Measure(LayoutConstraints{}, ctx).width + (w > 0 ? z.space_sm() * ctx.dpi_scale : 0.0f);
    return w;
}

// A card with actions reserves a full control height, so swapping ⋯ for Done doesn't move the rows.
float CardContainer::TitleLine(const LayoutContext& ctx) const {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    float h = LineHeight(TypeRole::Title) * ctx.dpi_scale;
    if (!actions.empty()) h = (std::max)(h, z.control() * ctx.dpi_scale);
    for (const auto& a : actions)
        if (a && a->visible) h = (std::max)(h, a->Measure(LayoutConstraints{}, ctx).height);
    return h;
}

float CardContainer::HeaderHeight(float w, const LayoutContext& ctx) const {
    if (title.empty()) return 0.0f;
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, pad = z.card_padding() * s;
    const float help_h = HelpHeight(help, w - 2 * pad, ctx);
    return pad + TitleLine(ctx) + (help_h > 0 ? z.space_xxs() * s + help_h : 0.0f) + z.space_xs() * s;
}

D2D1_SIZE_F CardContainer::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? c.max_w : 600 * ctx.dpi_scale;
    float h = HeaderHeight(w, ctx);
    for (const auto& r : rows)
        if (r && r->visible) h += BlockHeight(*r, w, ctx);
    return D2D1::SizeF(w, h);
}

void CardContainer::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, w = rect.right - rect.left, pad = z.card_padding() * s;
    if (!title.empty()) {
        const float line = TitleLine(ctx);
        float x = rect.right - pad - ActionsWidth(ctx);
        for (const auto& a : actions) {
            if (!a || !a->visible) continue;
            const D2D1_SIZE_F m = a->Measure(LayoutConstraints{}, ctx);
            const float y = rect.top + pad + (line - m.height) * 0.5f;
            a->Arrange(D2D1::RectF(x, y, x + m.width, y + m.height), ctx);
            x += m.width + z.space_sm() * s;
        }
    }
    float y = rect.top + HeaderHeight(w, ctx);
    bool first = true;
    for (const auto& r : rows) {
        if (!r || !r->visible) continue;
        const float h = BlockHeight(*r, w, ctx);
        const auto box = D2D1::RectF(rect.left, y, rect.right, y + h);
        if (auto* row = dynamic_cast<SettingRow*>(r.get())) {
            row->divider_above = !first;
            r->Arrange(box, ctx);
        } else if (FullBleed(*r)) {
            r->Arrange(box, ctx);
        } else {
            const float px = z.card_padding() * s, py = z.space_md() * s;
            r->Arrange(D2D1::RectF(box.left + px, box.top + py, box.right - px, box.bottom - py), ctx);
        }
        y += h;
        first = false;
    }
}

void CardContainer::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const float rad = t.size.radius_card() * s;
    p.Fill(bounds, rad, c.card);
    p.CardEdge(bounds, rad);
    if (drop_target) {
        p.Fill(bounds, rad, c.accent_soft);
        p.Stroke(bounds, rad, c.accent, 1.5f);
    }
    if (!title.empty()) {
        const LayoutContext ctx{s, &t};
        const float pad = t.size.card_padding() * s, line = TitleLine(ctx);
        const float x = bounds.left + pad, w = bounds.right - bounds.left - 2 * pad, th = LineHeight(TypeRole::Title) * s;
        const float ty = bounds.top + pad + (line - th) * 0.5f;
        const float aw = ActionsWidth(ctx);
        p.Text(title, TypeRole::Title, D2D1::RectF(x, ty, x + w - (aw > 0 ? aw + t.size.space_md() * s : 0.0f), ty + th), c.text,
               {TextAlign::Leading, false, true});
        if (!help.empty()) {
            const float hy = bounds.top + pad + line + t.size.space_xxs() * s;
            // Its own height, not to the card's foot: the box is what --drive sees and checks.
            p.Text(help, TypeRole::Caption, D2D1::RectF(x, hy, x + w, hy + HelpHeight(help, w, ctx)), c.text3,
                   {TextAlign::Leading, true, false, false});
        }
        for (const auto& a : actions) if (a) a->Draw(dc, t, s);
    }
    for (const auto& r : rows) if (r) r->Draw(dc, t, s);
}

CursorKind CardContainer::CursorAt(D2D1_POINT_2F pt) const {
    const CursorKind k = RouteCursor(actions, pt);
    return k != CursorKind::Arrow ? k : RouteCursor(rows, pt);
}
bool CardContainer::OnMouseMove(D2D1_POINT_2F pt) {
    const bool a = RouteMove(actions, pt);
    return RouteMove(rows, pt) || a;
}
bool CardContainer::OnMouseDown(D2D1_POINT_2F pt) {
    const bool a = RouteDown(actions, pt);
    return RouteDown(rows, pt) || a;
}
bool CardContainer::OnMouseUp(D2D1_POINT_2F pt) {
    const bool a = RouteUp(actions, pt);
    return RouteUp(rows, pt) || a;
}
bool CardContainer::OnKeyDown(UINT vk) { return RouteKey(actions, vk) || RouteKey(rows, vk); }
bool CardContainer::QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const {
    return RouteTooltip(actions, text, anchor) || RouteTooltip(rows, text, anchor);
}

// --- Tile ---------------------------------------------------------------------------------------

D2D1_SIZE_F TileControl::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, w = c.BoundedWidth() ? c.max_w : 200 * s;
    const bool pic = HasPicture();
    const float pad = (pic ? z.space_sm() : z.space_md()) * s;
    const float thumb = pic ? (w - 2 * pad) * 9 / 16 + z.space_sm() * s : 0.0f;
    const float text = LineHeight(TypeRole::Label) * s + (detail.empty() ? 0.0f : LineHeight(TypeRole::Caption) * s);
    return D2D1::SizeF(w, 2 * pad + thumb + text + ExtrasHeight(z, s));
}

float TileControl::ChipHeight(const Sizes& z, float s) {
    return (LineHeight(TypeRole::Caption) + z.space_xxs() * 2) * s;
}

float TileControl::ExtrasHeight(const Sizes& z, float s) const {
    return (chips.empty() ? 0.0f : z.space_xs() * s + ChipHeight(z, s)) +
           (note.empty() ? 0.0f : z.space_xs() * s + LineHeight(TypeRole::Caption) * s);
}

void TileControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    action_rect_ = hide_rect_ = {};
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const D2D1_RECT_F& r = bounds;
    const float rad = z.radius_card() * s;
    const bool pic = HasPicture();
    const float pad = (pic ? z.space_sm() : z.space_md()) * s;

    // Chosen: accent tint and a 1.5 DIP accent outline.
    p.Fill(r, rad, c.raised);
    if (selected) p.Fill(r, rad, c.accent_soft);
    p.Fill(r, rad, pressed ? c.PressedLayer() : c.HoverLayer(Hover()));
    if (selected) p.Stroke(r, rad, c.accent, 1.5f);
    else p.Stroke(r, rad, c.line_strong);

    // A dimmed tile fades its picture and words, not its frame: still chosen, just not there now.
    const Paint q{dc, t, s, p.op * (dimmed ? 0.45f : 1.0f)};
    float y = r.top + pad;
    D2D1_RECT_F mark_box = r;
    if (pic) {
        const auto box = D2D1::RectF(r.left + pad, y, r.right - pad, y + (r.right - r.left - 2 * pad) * 9 / 16);
        const float ir = z.radius_control() * s;
        if (!thumb_bgra.empty() && !bitmap_ && thumb_w && thumb_h) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            dc->CreateBitmap(D2D1::SizeU(thumb_w, thumb_h), thumb_bgra.data(), thumb_w * 4, &props, &bitmap_);
            thumb_bgra.clear();
            thumb_bgra.shrink_to_fit();
        }
        if (bitmap_) {
            // Aspect-fit: a 16:10 display or a portrait window keeps its shape, letterboxed.
            q.Fill(box, ir, c.panel);
            const D2D1_SIZE_F bs = bitmap_->GetSize();
            const float bw = box.right - box.left, bh = box.bottom - box.top;
            const float k = (std::min)(bw / bs.width, bh / bs.height), dw = bs.width * k, dh = bs.height * k;
            const auto dst = D2D1::RectF(box.left + (bw - dw) / 2, box.top + (bh - dh) / 2,
                                         box.left + (bw + dw) / 2, box.top + (bh + dh) / 2);
            ComPtr<ID2D1Factory> f;
            dc->GetFactory(&f);
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            if (SUCCEEDED(f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, ir, ir), &clip))) {
                dc->PushLayer(D2D1::LayerParameters1(box, clip.Get()), nullptr);
                dc->DrawBitmap(bitmap_.Get(), dst, q.op, D2D1_INTERPOLATION_MODE_LINEAR);
                dc->PopLayer();
            }
        } else if (thumb_from || thumb_to) {
            const Rgb a = RgbFromHex(thumb_from), b = RgbFromHex(thumb_to);
            ComPtr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[2] = {{0.0f, q.A(D2D1::ColorF(a.r, a.g, a.b))}, {1.0f, q.A(D2D1::ColorF(b.r, b.g, b.b))}};
            ComPtr<ID2D1LinearGradientBrush> grad;
            if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &stops)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(box.left, box.top), D2D1::Point2F(box.right, box.bottom)), stops.Get(), &grad)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(box, ir, ir), grad.Get());
        } else if (picture_glyph != IconType::None) {
            q.Fill(box, ir, c.panel);
            if (!dimmed) q.Fill(box, ir, c.accent_soft);
            const float cx = (box.left + box.right) / 2, cy = Cy(box), side = (box.bottom - box.top) * 0.4f;
            if (picture_icon && !picture_icon->bgra.empty() && !icon_bitmap_) {
                const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(picture_icon->w, picture_icon->h), picture_icon->bgra.data(),
                                 picture_icon->w * 4, &props, &icon_bitmap_);
            }
            if (icon_bitmap_) {
                const float h = side / 2;
                dc->DrawBitmap(icon_bitmap_.Get(), D2D1::RectF(cx - h, cy - h, cx + h, cy + h), q.op,
                               D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else {
                DrawIcon(dc, t, picture_glyph, cx, cy, side, q.B(dimmed ? c.text3 : c.text).Get());
            }
        } else {
            q.Fill(box, ir, c.panel);
            DrawIcon(dc, t, IconType::Camera, (box.left + box.right) / 2, Cy(box), (box.bottom - box.top) * 0.16f,
                     q.B(c.text3).Get());
        }
        mark_box = box;
        y = box.bottom + z.space_sm() * s;

        // Over the picture, on the hovered tile only (on every tile they were a wall of buttons). A
        // fixed dark disc and white ink, so they read on any capture.
        const float d = z.control_sm() * s * 0.85f, inset = z.space_sm() * s;
        const auto disc = D2D1::ColorF(0, 0, 0, 0.6f), ink = D2D1::ColorF(1, 1, 1);
        float x = box.left + inset;
        if (on_hide && hovered) {
            hide_rect_ = D2D1::RectF(x, box.top + inset, x + d, box.top + inset + d);
            p.Fill(hide_rect_, d / 2, disc);
            DrawIcon(dc, t, IconType::Eye, x + d / 2, Cy(hide_rect_), z.icon() * s, p.B(ink).Get());
            x += d + z.space_xs() * s;
        }
        if (on_action && !action_label.empty() && hovered) {
            const float w = p.TextW(action_label, TypeRole::Caption) + 2 * z.space_sm() * s;
            action_rect_ = D2D1::RectF(x, box.top + inset, x + w, box.top + inset + d);
            p.Fill(action_rect_, d / 2, disc);
            p.Text(action_label, TypeRole::Caption, action_rect_, ink, {TextAlign::Center});
        }
    }
    const float lh = LineHeight(TypeRole::Label) * s, ch = LineHeight(TypeRole::Caption) * s;
    float tx = r.left + pad;
    if (title_icon != IconType::None) {
        const float isz = z.icon() * s;
        DrawIcon(dc, t, title_icon, tx + isz / 2, y + lh / 2, isz, q.B(c.text2).Get());
        tx += isz + z.space_xs() * s;
    }
    q.Text(title, TypeRole::Label, D2D1::RectF(tx, y, r.right - pad, y + lh), c.text, {TextAlign::Leading, false, true});
    if (!detail.empty()) {
        q.Text(detail, TypeRole::Caption, D2D1::RectF(r.left + pad, y + lh, r.right - pad, y + lh + ch), c.text3,
               {TextAlign::Leading, false, true});
        y += ch;
    }
    y += lh;
    if (!chips.empty()) {
        y += z.space_xs() * s;
        const float cp = z.space_sm() * s, ph = ChipHeight(z, s);
        float x = r.left + pad;
        for (const auto& chip : chips) {
            const float cw = q.TextW(chip, TypeRole::Caption) + 2 * cp;
            if (x + cw > r.right - pad) break;
            const auto pill = D2D1::RectF(x, y, x + cw, y + ph);
            q.Fill(pill, t.size.radius_full(ph, s), c.HoverLayer(1.5f));
            q.Text(chip, TypeRole::Caption, pill, c.text2, {TextAlign::Center});
            x += cw + z.space_xs() * s;
        }
        y += ph;
    }
    if (!note.empty()) {
        // One line, trimmed, at the tile's foot: tiles side by side share one height, and their
        // notes one line even when only some have chips.
        y = (std::max)(y + z.space_xs() * s, r.bottom - pad - ch);
        const float dot = z.space_xs() * 0.75f * s;
        q.Circle(r.left + pad + dot, y + ch * 0.5f, dot, ToneColor(c, note_tone, c.text3));
        const bool loud = note_tone == SettingRow::Tone::Error || note_tone == SettingRow::Tone::Warning;
        q.Text(note, TypeRole::Caption, D2D1::RectF(r.left + pad + NoteIndent(z, s), y, r.right - pad, y + ch),
               loud ? ToneColor(c, note_tone, c.text2) : c.text2, {TextAlign::Leading, false, true});
    }

    // The check circle: on a set's tiles (chosen or not) only.
    if (multi) {
        const float cr = z.glyph() * 0.9f * s, inset = z.space_sm() * s;
        const float cx = mark_box.right - inset - cr, cy = mark_box.top + inset + cr;
        if (selected) {
            p.Circle(cx, cy, cr, c.accent);
            p.Check(cx, cy, c.on_accent);
        } else {
            p.Circle(cx, cy, cr, D2D1::ColorF(c.panel.r, c.panel.g, c.panel.b, 0.6f));
            p.Ring(cx, cy, cr - 0.75f * s, c.text2, 1.5f);
        }
    }
    if (focus_visible) p.Focus(r, rad);
}

void TileControl::Press(D2D1_POINT_2F pt) {
    // Copies: each stages an edit, which rebuilds the page and this tile with it.
    if (on_hide && PointIn(hide_rect_, pt)) { auto f = on_hide; f(); return; }
    if (on_action && PointIn(action_rect_, pt)) { auto f = on_action; f(); return; }
    const bool next = multi ? !selected : true;
    if (next == selected) return;
    selected = next;
    if (on_change) { auto f = on_change; f(selected); }
}

bool TileControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

// --- Action tile --------------------------------------------------------------------------------

D2D1_SIZE_F ActionTileControl::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? c.max_w : 220 * ctx.dpi_scale;
    return D2D1::SizeF(w, w);
}

void ActionTileControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    // Disabled fades what's on the tile, not the tile: over a game a faded surface lets it through.
    const Paint surface{dc, t, s};
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const D2D1_RECT_F& r = bounds;
    const float rad = z.radius_card() * s, pad = z.card_padding() * s;
    const float hv = Hover();

    surface.Fill(r, rad, c.raised);
    surface.Fill(r, rad, pressed ? c.PressedLayer() : c.HoverLayer(hv));
    surface.Stroke(r, rad, c.line_strong);

    // The foot: a rule, the label, the shortcut's keys and the switch at the trailing edge.
    const bool foot = !foot_label.empty();
    const float foot_h = foot || reserve_foot ? z.control_sm() * s + 2 * z.space_sm() * s : 0.0f;
    toggle.visible = foot;
    if (foot) {
        const auto row = D2D1::RectF(r.left + pad, r.bottom - foot_h, r.right - pad, r.bottom);
        p.Line(row.left, row.top, row.right, row.top, c.line, 1.0f);
        const float sw = z.switch_w() * s;
        toggle.enabled = enabled;
        toggle.bounds = RowBox(row, row.right - sw, sw, z.control_sm() * s);
        // The label names the switch, so a narrow tile drops the keys and keeps it (trimmed if need be).
        const std::vector<std::wstring> keys = SplitKeys(foot_keys);
        const float kh = z.switch_h() * s, kw = p.Keys(keys, 0, 0, kh, false, true), sp = z.space_sm() * s;
        const float lw = p.TextW(foot_label, TypeRole::Caption);
        float label_r = toggle.bounds.left - sp;
        if (!keys.empty() && row.left + lw + sp + kw <= label_r) {
            const float kx = label_r - kw;
            p.Keys(keys, kx, Cy(row), kh, true, true);
            label_r = kx - sp;
        }
        p.Text(foot_label, TypeRole::Caption, D2D1::RectF(row.left, row.top, label_r, row.bottom), c.text2,
               {TextAlign::Leading, false, true});
        toggle.Draw(dc, t, s);
    }

    // Icon disc, title and state as one group, centred above the foot.
    const float lt = LineHeight(TypeRole::Title) * s, lb = LineHeight(TypeRole::Body) * s, gap = z.space_md() * s;
    const float lc = LineHeight(TypeRole::Caption) * s;
    const float words = gap + lt + (detail.empty() ? 0.0f : z.space_xxs() * s + lb) +
                        (live_line.empty() ? 0.0f : z.space_xxs() * s + lc);
    // The disc gives way first on a small tile (720p, Comfortable), down to one control high.
    const float room = r.bottom - foot_h - r.top - 2 * pad - words;
    const float d = std::clamp(room, z.control_sm() * s, 2 * z.control_sm() * s);
    // Centred without the live line, which hangs below: the row's titles stay level.
    const float group = d + words - (live_line.empty() ? 0.0f : z.space_xxs() * s + lc);
    float y = r.top + (r.bottom - foot_h - r.top - group) * 0.5f;
    const float cx = (r.left + r.right) * 0.5f;
    if (primary) p.Circle(cx, y + d / 2, d / 2, pressed ? c.accent_pressed : Mix(c.accent, c.accent_hover, hv));
    else p.Circle(cx, y + d / 2, d / 2, c.HoverLayer(2.0f));
    const D2D1_COLOR_F dot = p.A(c.recording);
    DrawIcon(dc, t, icon, cx, y + d / 2, (std::min)(z.app_icon() * s, d / 2), p.B(primary ? c.on_accent : c.text).Get(),
             dot_active ? &dot : nullptr);
    y += d + gap;
    const TextOpts centre{TextAlign::Center, false, true};
    p.Text(title, TypeRole::Title, D2D1::RectF(r.left + pad, y, r.right - pad, y + lt), c.text, centre);
    if (!detail.empty()) {
        y += lt + z.space_xxs() * s;
        p.Text(detail, TypeRole::Body, D2D1::RectF(r.left + pad, y, r.right - pad, y + lb), c.text2, centre);
        y += lb;
    } else {
        y += lt;
    }
    if (!live_line.empty()) {
        // The dot and the words centred as one; a long name trims inside the tile.
        y += z.space_xxs() * s;
        const float dot = z.space_xs() * 0.75f * s, sp = z.space_xs() * s, lead = 2 * dot + sp;
        const float tw = (std::min)(p.TextW(live_line, TypeRole::Caption), r.right - r.left - 2 * pad - lead);
        const float x = cx - (lead + tw) / 2;
        p.Circle(x + dot, y + lc / 2, dot, c.success);
        p.Text(live_line, TypeRole::Caption, D2D1::RectF(x + lead, y, x + lead + tw + 1, y + lc), c.text2,
               {TextAlign::Leading, false, true});
    }
    if (focus_visible) p.Focus(r, rad);
}

bool ActionTileControl::OnMouseMove(D2D1_POINT_2F pt) {
    bool changed = Control::OnMouseMove(pt);
    if (toggle.visible) changed |= toggle.OnMouseMove(pt);
    return changed;
}

bool ActionTileControl::OnMouseDown(D2D1_POINT_2F pt) {
    // The switch sits inside the tile: a press on it flips it and doesn't also act.
    if (toggle.visible && toggle.OnMouseDown(pt)) return true;
    return Control::OnMouseDown(pt);
}

bool ActionTileControl::OnMouseUp(D2D1_POINT_2F pt) {
    const bool sw = toggle.OnMouseUp(pt);
    return Control::OnMouseUp(pt) || sw;
}

void ActionTileControl::Press(D2D1_POINT_2F) {
    if (on_click) on_click();
}

bool ActionTileControl::OnKeyDown(UINT vk) {
    if (toggle.OnKeyDown(vk)) return true;
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

// --- Tile grid ----------------------------------------------------------------------------------

// --- LevelDetail --------------------------------------------------------------------------------

namespace {
// Pixels from the tiles' bottom to the stop's text, and the text's height for `w` pixels.
float LevelTextTop(const Theme* t, float s) { return (t ? t->size.space_md() : 12.0f) * s; }
float LevelTextHeight(const Theme& t, const LevelDetail::Stop& stop, float w, float s) {
    const float body = t.MeasureText(t.Font(TypeRole::Body, {TextAlign::Leading, true}), stop.body, w).height;
    return LineHeight(TypeRole::Label) * s + t.size.space_xs() * s + body;
}
}  // namespace

D2D1_SIZE_F LevelDetail::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? c.max_w : width_dip * ctx.dpi_scale;
    tiles->lines.clear();
    for (const Stop& st : stops) tiles->lines.push_back(st.tile);
    float h = tiles->Measure(LayoutConstraints::Exact(w), ctx).height + LevelTextTop(ctx.theme, ctx.dpi_scale);
    float text = 0;
    if (ctx.theme)
        for (const Stop& st : stops) text = (std::max)(text, LevelTextHeight(*ctx.theme, st, w, ctx.dpi_scale));
    return D2D1::SizeF(w, h + text);
}

void LevelDetail::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    const float th = tiles->Measure(LayoutConstraints::Exact(rect.right - rect.left), ctx).height;
    tiles->Arrange(D2D1::RectF(rect.left, rect.top, rect.right, rect.top + th), ctx);
}

void LevelDetail::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    tiles->Draw(dc, t, s);
    if (tiles->selected < 0 || tiles->selected >= static_cast<int>(stops.size())) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const Stop& st = stops[tiles->selected];
    const float y = tiles->bounds.bottom + LevelTextTop(&t, s), lh = LineHeight(TypeRole::Label) * s;
    p.Text(st.lead, TypeRole::Label, D2D1::RectF(bounds.left, y, bounds.right, y + lh), t.color.text);
    const float by = y + lh + t.size.space_xs() * s;
    // Its own height, not the box's: text is centred in its rect, and the box fits the tallest stop.
    const TextOpts wrap{TextAlign::Leading, true};
    const float bh = t.MeasureText(t.Font(TypeRole::Body, wrap), st.body, bounds.right - bounds.left).height;
    p.Text(st.body, TypeRole::Body, D2D1::RectF(bounds.left, by, bounds.right, by + bh + 1), t.color.text2, wrap);
}

D2D1_SIZE_F TileGrid::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float w = c.BoundedWidth() ? c.max_w : 600 * ctx.dpi_scale, gap = z.space_md() * ctx.dpi_scale;
    const int cols = (std::max)(1, columns);
    const float cw = (w - gap * (cols - 1)) / cols;
    float h = 0, line = 0;
    for (size_t i = 0; i < tiles.size(); ++i) {
        line = (std::max)(line, tiles[i]->Measure(LayoutConstraints::Exact(cw), ctx).height);
        if ((i + 1) % cols == 0 || i + 1 == tiles.size()) {
            h += line + (h > 0 ? gap : 0.0f);
            line = 0;
        }
    }
    return D2D1::SizeF(w, h);
}

void TileGrid::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float w = rect.right - rect.left, gap = z.space_md() * ctx.dpi_scale;
    const int cols = (std::max)(1, columns);
    const float cw = (w - gap * (cols - 1)) / cols;
    float y = rect.top;
    for (size_t i = 0; i < tiles.size(); i += cols) {
        float line = 0;
        for (size_t j = i; j < tiles.size() && j < i + cols; ++j)
            line = (std::max)(line, tiles[j]->Measure(LayoutConstraints::Exact(cw), ctx).height);
        for (size_t j = i; j < tiles.size() && j < i + cols; ++j) {
            const float x = rect.left + (cw + gap) * static_cast<float>(j - i);
            tiles[j]->Arrange(D2D1::RectF(x, y, x + cw, y + line), ctx);
        }
        y += line + gap;
    }
}

void TileGrid::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    for (const auto& k : tiles) k->Draw(dc, t, s);
}

CursorKind TileGrid::CursorAt(D2D1_POINT_2F pt) const { return RouteCursor(tiles, pt); }
bool TileGrid::OnMouseMove(D2D1_POINT_2F pt) { return RouteMove(tiles, pt); }
bool TileGrid::OnMouseUp(D2D1_POINT_2F pt) { return RouteUp(tiles, pt); }

// A single-choice grid keeps one tile chosen: pressing one clears the rest.
bool TileGrid::OnMouseDown(D2D1_POINT_2F pt) {
    const bool took = RouteDown(tiles, pt);
    for (const auto& k : tiles) {
        if (k->multi || !k->pressed) continue;
        for (const auto& o : tiles) if (o != k && !o->multi) o->selected = false;
    }
    return took;
}

float RecordingSources::TileWidth(float w, const Sizes& z, float s) const {
    // One column to the game, the rest to the displays; the rule's gap is twice a tile gap.
    const float gap = z.space_md() * s, n = static_cast<float>((std::max)(2, columns));
    return (w - gap * (n - 2) - 2 * gap - 1 * s) / n;
}

float RecordingSources::CaptionHeight(const Sizes& z, float s) const {
    return displays ? (LineHeight(TypeRole::Caption) + z.space_xs()) * s : 0.0f;
}

D2D1_SIZE_F RecordingSources::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, w = c.BoundedWidth() ? c.max_w : 600 * s, tw = TileWidth(w, z, s);
    float h = game ? game->Measure(LayoutConstraints::Exact(tw), ctx).height : 0.0f;
    if (displays) {
        const float dw = tw * (columns - 1) + z.space_md() * s * (columns - 2);
        h = (std::max)(h, displays->Measure(LayoutConstraints::Exact(dw), ctx).height);
    }
    return D2D1::SizeF(w, h + CaptionHeight(z, s));
}

void RecordingSources::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale, gap = z.space_md() * s, tw = TileWidth(rect.right - rect.left, z, s);
    const float top = rect.top + CaptionHeight(z, s);
    if (game) {
        const float gh = game->Measure(LayoutConstraints::Exact(tw), ctx).height;
        game->Arrange(D2D1::RectF(rect.left, top, rect.left + tw, top + gh), ctx);
    }
    rule_x_ = rect.left + tw + gap;
    if (displays) {
        const float x = rule_x_ + 1 * s + gap;
        displays->Arrange(D2D1::RectF(x, top, rect.right, rect.bottom), ctx);
    }
}

void RecordingSources::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    if (displays) {
        const float ch = LineHeight(TypeRole::Caption) * s;
        const float gx = bounds.left, dx = rule_x_ + 1 * s + t.size.space_md() * s;
        p.Text(game_caption, TypeRole::Caption, D2D1::RectF(gx, bounds.top, rule_x_ - t.size.space_xs() * s, bounds.top + ch),
               t.color.text2, {TextAlign::Leading, false, true});
        p.Text(displays_caption, TypeRole::Caption, D2D1::RectF(dx, bounds.top, bounds.right, bounds.top + ch), t.color.text2,
               {TextAlign::Leading, false, true});
        p.Line(rule_x_, bounds.top, rule_x_, bounds.bottom, t.color.line, 1.0f);
        displays->Draw(dc, t, s);
    }
    if (game) game->Draw(dc, t, s);
}

CursorKind RecordingSources::CursorAt(D2D1_POINT_2F pt) const {
    return displays ? displays->CursorAt(pt) : Widget::CursorAt(pt);
}
bool RecordingSources::OnMouseMove(D2D1_POINT_2F pt) { return displays && displays->OnMouseMove(pt); }
bool RecordingSources::OnMouseDown(D2D1_POINT_2F pt) { return displays && displays->OnMouseDown(pt); }
bool RecordingSources::OnMouseUp(D2D1_POINT_2F pt) { return displays && displays->OnMouseUp(pt); }

// --- Monitor diagram ----------------------------------------------------------------------------

void ScreenControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    hide_rect_ = {};
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const D2D1_RECT_F& r = bounds;
    const float rad = z.radius_control() * s;

    if (!thumb_bgra.empty() && !bitmap_ && thumb_w && thumb_h) {
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        dc->CreateBitmap(D2D1::SizeU(thumb_w, thumb_h), thumb_bgra.data(), thumb_w * 4, &props, &bitmap_);
        thumb_bgra.clear();
        thumb_bgra.shrink_to_fit();
    }
    p.Fill(r, rad, c.raised);
    if (bitmap_) {
        // The capture has the screen's own shape, so it fills the box; a chosen screen at full
        // strength, the others faded back.
        ComPtr<ID2D1Factory> f;
        dc->GetFactory(&f);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (SUCCEEDED(f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, rad, rad), &clip))) {
            dc->PushLayer(D2D1::LayerParameters1(r, clip.Get()), nullptr);
            dc->DrawBitmap(bitmap_.Get(), r, p.op * (selected ? 1.0f : 0.4f), D2D1_INTERPOLATION_MODE_LINEAR);
            dc->PopLayer();
        }
    } else if (selected) {
        p.Fill(r, rad, c.accent_soft);
    }
    p.Fill(r, rad, pressed ? c.PressedLayer() : c.HoverLayer(Hover()));
    if (selected) p.Stroke(r, rad, c.accent, 2.0f);
    else p.Stroke(r, rad, c.line_strong);

    // Name and resolution centred on a dark label at the foot, white ink, so they read on any
    // capture. The resolution goes first when the box is too short for both.
    const float pad = z.space_sm() * s, lh = LineHeight(TypeRole::Label) * s, ch = LineHeight(TypeRole::Caption) * s;
    const float bw = r.right - r.left, bh = r.bottom - r.top;
    const bool two = !detail.empty() && bh >= lh + ch + 2 * pad + z.glyph() * 2 * s;
    const float label_h = (two ? lh + ch : lh) + 2 * z.space_xxs() * s;
    const float text_w = (std::max)(p.TextW(title, TypeRole::Label), two ? p.TextW(detail, TypeRole::Caption) : 0.0f);
    const float label_w = (std::min)(text_w + 2 * pad, bw - 2 * z.space_xs() * s);
    const float cx = (r.left + r.right) / 2;
    const auto label = D2D1::RectF(cx - label_w / 2, r.bottom - z.space_xs() * s - label_h, cx + label_w / 2,
                                   r.bottom - z.space_xs() * s);
    const auto ink = D2D1::ColorF(1, 1, 1);
    const bool on_picture = bitmap_ != nullptr;
    if (on_picture) p.Fill(label, z.radius_control() * s, D2D1::ColorF(0, 0, 0, 0.6f));
    const float ty = label.top + z.space_xxs() * s;
    const TextOpts trim{TextAlign::Center, false, true};
    p.Text(title, TypeRole::Label, D2D1::RectF(label.left + pad / 2, ty, label.right - pad / 2, ty + lh),
           on_picture ? ink : c.text, trim);
    if (two)
        p.Text(detail, TypeRole::Caption, D2D1::RectF(label.left + pad / 2, ty + lh, label.right - pad / 2, ty + lh + ch),
               on_picture ? D2D1::ColorF(1, 1, 1, 0.75f) : c.text3, trim);

    // The check circle, chosen or not, as a multi-choice tile's.
    const float cr = z.glyph() * 0.9f * s, inset = z.space_sm() * s;
    const float kx = r.right - inset - cr, ky = r.top + inset + cr;
    if (selected) {
        p.Circle(kx, ky, cr, c.accent);
        p.Check(kx, ky, c.on_accent);
    } else {
        p.Circle(kx, ky, cr, D2D1::ColorF(c.panel.r, c.panel.g, c.panel.b, 0.6f));
        p.Ring(kx, ky, cr - 0.75f * s, c.text2, 1.5f);
    }
    if (on_hide && hovered) {
        const float d = z.control_sm() * s * 0.85f;
        hide_rect_ = D2D1::RectF(r.left + inset, r.top + inset, r.left + inset + d, r.top + inset + d);
        p.Fill(hide_rect_, d / 2, D2D1::ColorF(0, 0, 0, 0.6f));
        DrawIcon(dc, t, IconType::Eye, (hide_rect_.left + hide_rect_.right) / 2, Cy(hide_rect_), z.icon() * s, p.B(ink).Get());
    }
    if (focus_visible) p.Focus(r, rad);
}

void ScreenControl::Press(D2D1_POINT_2F pt) {
    if (on_hide && PointIn(hide_rect_, pt)) { auto f = on_hide; f(); return; }
    selected = !selected;
    if (on_change) { auto f = on_change; f(selected); }
}

bool ScreenControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

std::vector<MonitorDiagram::Box> MonitorDiagram::Desktop() const {
    std::vector<Box> out;
    float row_x = 0;
    for (const auto& sc : screens) {
        if (!sc->visible) { out.push_back({}); continue; }
        const float w = static_cast<float>((std::max)(1, sc->width)), h = static_cast<float>((std::max)(1, sc->height));
        const float l = sc->positioned ? static_cast<float>(sc->left) : row_x, t = sc->positioned ? static_cast<float>(sc->top) : 0.0f;
        out.push_back({l, t, l + w, t + h});
        row_x = (std::max)(row_x, l + w);
    }
    return out;
}

// Desktop pixels to device pixels, the drawn size in `out_w`/`out_h`.
float MonitorDiagram::Scale(float w, float cap_h, float* out_w, float* out_h) const {
    float l = 0, t = 0, r = 0, b = 0;
    bool any = false;
    const auto boxes = Desktop();
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!screens[i]->visible) continue;
        const Box& x = boxes[i];
        if (!any) { l = x.l; t = x.t; r = x.r; b = x.b; any = true; continue; }
        l = (std::min)(l, x.l); t = (std::min)(t, x.t); r = (std::max)(r, x.r); b = (std::max)(b, x.b);
    }
    if (!any || r <= l || b <= t) { *out_w = *out_h = 0; return 0; }
    const float k = (std::min)(w / (r - l), cap_h / (b - t));
    *out_w = (r - l) * k;
    *out_h = (b - t) * k;
    return k;
}

D2D1_SIZE_F MonitorDiagram::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? c.max_w : 600 * ctx.dpi_scale;
    float dw = 0, dh = 0;
    Scale(w, max_h_dip * ctx.dpi_scale, &dw, &dh);
    return D2D1::SizeF(w, dh);
}

void MonitorDiagram::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    float dw = 0, dh = 0;
    const float k = Scale(rect.right - rect.left, max_h_dip * ctx.dpi_scale, &dw, &dh);
    const auto boxes = Desktop();
    float l = 0, t = 0;
    bool any = false;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!screens[i]->visible) continue;
        if (!any) { l = boxes[i].l; t = boxes[i].t; any = true; }
        l = (std::min)(l, boxes[i].l);
        t = (std::min)(t, boxes[i].t);
    }
    // Screens that touch on the desktop get a hairline apart, so each keeps its own outline.
    const float gap = (ctx.theme ? ctx.theme->size.space_xxs() : 2.0f) * ctx.dpi_scale;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const Box& x = boxes[i];
        screens[i]->Arrange(D2D1::RectF(rect.left + (x.l - l) * k + gap, rect.top + (x.t - t) * k + gap,
                                        rect.left + (x.r - l) * k - gap, rect.top + (x.b - t) * k - gap),
                            ctx);
    }
}

void MonitorDiagram::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    for (const auto& sc : screens) sc->Draw(dc, t, s);
}

CursorKind MonitorDiagram::CursorAt(D2D1_POINT_2F pt) const { return RouteCursor(screens, pt); }
bool MonitorDiagram::OnMouseMove(D2D1_POINT_2F pt) { return RouteMove(screens, pt); }
bool MonitorDiagram::OnMouseDown(D2D1_POINT_2F pt) { return RouteDown(screens, pt); }
bool MonitorDiagram::OnMouseUp(D2D1_POINT_2F pt) { return RouteUp(screens, pt); }
bool MonitorDiagram::OnKeyDown(UINT vk) { return RouteKey(screens, vk); }

D2D1_SIZE_F StatusStrip::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale;
    return D2D1::SizeF(c.BoundedWidth() ? c.max_w : 600 * s, (LineHeight(TypeRole::Label) + 2 * z.space_md()) * s);
}

void StatusStrip::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const float rad = t.size.radius_card() * s, pad = t.size.card_padding() * s, dot = t.size.space_xs() * 0.75f * s;
    p.Fill(bounds, rad, t.color.card);
    p.CardEdge(bounds, rad);
    p.Circle(bounds.left + pad + dot, Cy(bounds), dot, live ? t.color.success : t.color.text3);
    p.Text(text, TypeRole::Label, D2D1::RectF(bounds.left + pad + 2 * dot + t.size.space_sm() * s, bounds.top, bounds.right - pad, bounds.bottom),
           live ? t.color.text : t.color.text2, {TextAlign::Leading, false, true});
}

// The icon in its tone on a soft disc of it; a save shows the
// brand mark instead. Compact is a pill as wide as its content, against the
// screen edge; a title too long for one line makes it the full card, wrapped.
void ToastCard::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    using namespace paint;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float pad = z.space_md() * s, d = z.control() * s;
    D2D1_RECT_F r = bounds;
    float rad = z.radius_panel() * s;
    float lead = pad, glyph = brand ? 40.0f * s : d, gap = z.space_md() * s;
    TypeRole role = TypeRole::Title;
    bool wrap = false;
    if (compact) {
        lead = z.space_xs() * s;
        glyph = d;
        gap = z.space_sm() * s;
        role = TypeRole::Label;
        const float w = lead + d + gap + p.TextW(title, role) + pad;
        if (w <= r.right - r.left) {
            if (trailing) r.left = r.right - w;
            else r.right = r.left + w;
            const float h = d + 2 * lead;
            r.top = Cy(bounds) - h / 2;
            r.bottom = r.top + h;
            rad = h / 2;
        } else {
            wrap = true;  // the card's shape and inset, Label wrapped
            lead = pad;
        }
    }
    p.Fill(r, rad, c.raised);
    p.Stroke(r, rad, c.line_strong);

    const float cx = r.left + lead + glyph / 2, cy = Cy(r);
    if (brand) {
        const uint32_t rgb = t.PickedRgb();
        const int px = (std::max)(16, static_cast<int>(std::ceil(glyph)) * 2);  // drawn once a toast: no cache
        const auto pixels = ombra::RenderBrandMark(px, ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                                                   (rgb & 255) / 255.0f);
        ComPtr<ID2D1Bitmap> bmp;
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(px, px), pixels.data(), px * 4, props, &bmp)))
            dc->DrawBitmap(bmp.Get(), D2D1::RectF(cx - glyph / 2, cy - glyph / 2, cx + glyph / 2, cy + glyph / 2),
                           p.op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else {
        D2D1_COLOR_F hue = c.accent_text;
        switch (tone) {
            case Tone::Success: hue = c.success; break;
            case Tone::Warning: hue = c.warning; break;
            case Tone::Danger:  hue = c.danger; break;
            case Tone::Muted:   hue = c.text3; break;
            case Tone::Neutral: break;
        }
        p.Circle(cx, cy, glyph / 2, Faded(hue, c.soft_a));
        DrawIcon(dc, t, icon, cx, cy, glyph * 0.6f, p.B(hue).Get());
    }
    const auto tr = D2D1::RectF(cx + glyph / 2 + gap, r.top, r.right - pad, r.bottom);
    // A title too long for one line (the CPU-encoder fallback) drops to Label and wraps to two.
    if (!wrap && p.TextW(title, role) <= tr.right - tr.left) p.Text(title, role, tr, c.text);
    else p.Text(title, TypeRole::Label, tr, c.text, {TextAlign::Leading, true, true});
}

// The old dialog's shape: 400 DIP wide, sized to its text (an Auto-Tune result runs to several
// paragraphs), buttons sized to their labels.
void ModalDialog::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    using namespace paint;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    p.Fill(bounds, 0, D2D1::ColorF(0, 0, 0, 0.6f));  // the overlay behind recedes; the same in both themes
    if (g_draw_probe) g_draw_probe->Occlude(dc, bounds);  // nothing behind it can be read or clicked

    const float pad = z.space_xl() * s, card_w = 400 * s, text_w = card_w - 2 * pad;
    const TextOpts wrap{TextAlign::Leading, true, false, false};
    if (measured_title_ != title || measured_message_ != message || measured_scale_ != t.font_scale) {
        title_h_ = t.MeasureText(t.Font(TypeRole::Title, wrap), title, text_w).height;
        message_h_ = message.empty() ? 0.0f : t.MeasureText(t.Font(TypeRole::Body, wrap), message, text_w).height;
        measured_title_ = title;
        measured_message_ = message;
        measured_scale_ = t.font_scale;
    }
    const LayoutContext ctx{s, &t};
    confirm_.label = confirm_text;
    confirm_.kind = is_danger ? ButtonControl::Kind::Danger : ButtonControl::Kind::Primary;
    cancel_.label = cancel_text;
    cancel_.kind = ButtonControl::Kind::Secondary;
    cancel_.visible = !cancel_text.empty();
    const D2D1_SIZE_F cm = confirm_.Measure({}, ctx), xm = cancel_.Measure({}, ctx);
    const float card_h = pad + title_h_ + (message_h_ > 0 ? z.space_sm() * s + message_h_ : 0.0f) + z.space_xl() * s + cm.height + pad;
    const float cx = (bounds.left + bounds.right) / 2, cy = Cy(bounds);
    const auto card = D2D1::RectF(std::round(cx - card_w / 2), std::round(cy - card_h / 2),
                                  std::round(cx + card_w / 2), std::round(cy + card_h / 2));
    const float rad = z.radius_panel() * s;
    shadow_.Draw(dc, t, s, card, rad);
    p.Fill(card, rad, c.raised);
    p.Stroke(card, rad, c.line_strong);

    const float left = card.left + pad, right = card.right - pad;
    float y = card.top + pad;
    p.Text(title, TypeRole::Title, D2D1::RectF(left, y, right, y + title_h_), c.text, wrap);
    y += title_h_ + z.space_sm() * s;
    if (message_h_ > 0) p.Text(message, TypeRole::Body, D2D1::RectF(left, y, right, y + message_h_), c.text2, wrap);

    const float by = card.bottom - pad - cm.height;
    confirm_.Arrange(D2D1::RectF(right - cm.width, by, right, by + cm.height), ctx);
    const float xr = confirm_.bounds.left - z.space_sm() * s;
    cancel_.Arrange(cancel_.visible ? D2D1::RectF(xr - xm.width, by, xr, by + xm.height) : D2D1::RectF(0, 0, 0, 0), ctx);
    cancel_.Draw(dc, t, s);
    confirm_.Draw(dc, t, s);
}

CursorKind ModalDialog::CursorAt(D2D1_POINT_2F pt) const {
    return visible && (confirm_.HitTest(pt) || (!cancel_text.empty() && cancel_.HitTest(pt))) ? CursorKind::Hand : CursorKind::Arrow;
}

bool ModalDialog::OnMouseMove(D2D1_POINT_2F pt) {
    if (!visible) return false;
    confirm_.OnMouseMove(pt);
    if (!cancel_text.empty()) cancel_.OnMouseMove(pt);
    return true;
}

bool ModalDialog::OnMouseDown(D2D1_POINT_2F pt) {
    if (!visible) return false;
    if (confirm_.HitTest(pt)) last_result = Result::Confirm;
    else if (!cancel_text.empty() && cancel_.HitTest(pt)) last_result = Result::Cancel;
    else return true;
    visible = false;
    return true;
}

bool ModalDialog::OnKeyDown(UINT vk) {
    if (!visible) return false;
    if (vk == VK_ESCAPE) last_result = cancel_text.empty() ? Result::Confirm : Result::Cancel;  // one button: dismissing is its action
    else if (vk == VK_RETURN) last_result = Result::Confirm;
    else return true;
    visible = false;
    return true;
}

// One flow measures and draws (draw = false only advances y). A value shares its name's line at
// the trailing edge.
float MemoryCard::Flow(ID2D1DeviceContext* dc, const Theme& t, float s, bool draw) const {
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float pad = z.card_padding() * s;
    const float x0 = bounds.left + pad, x1 = bounds.right - pad, w = x1 - x0;
    const float cap = LineHeight(TypeRole::Caption) * s, body = LineHeight(TypeRole::Body) * s,
                label = LineHeight(TypeRole::Label) * s, title = LineHeight(TypeRole::Title) * s;
    const TextOpts trail{TextAlign::Trailing};
    const TextOpts trim{TextAlign::Leading, false, true};
    float y = bounds.top + pad;

    auto text = [&](const std::wstring& str, TypeRole role, float x, float h, const D2D1_COLOR_F& col,
                    const TextOpts& o) {
        if (!draw || str.empty()) return;
        // A trailing value gets a box of its own width, so it never shares one with its name.
        if (o.align == TextAlign::Trailing) x = (std::max)(x, x1 - p.TextW(str, role) - 1 * s);
        p.Text(str, role, D2D1::RectF(x, y, x1, y + h), col, o);
    };
    auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& col) {
        if (draw) p.Fill(r, radius, col);
    };
    // A hairline between sections, space_md either side.
    auto divider = [&] {
        y += z.space_md() * s;
        fill(D2D1::RectF(x0, y, x1, y + 1 * s), 0, c.line);
        y += 1 * s + z.space_md() * s;
    };
    const D2D1_COLOR_F track = c.HoverLayer(2.0f);  // VolumeControl's track

    text(L"Memory", TypeRole::Label, x0, label, c.text, trim);
    y += label;
    text(data.file_backed ? L"Video buffer is a file on disk" : L"Replay buffer + engine", TypeRole::Caption, x0,
         cap, c.text3, trim);
    y += cap + z.space_md() * s;

    if (!data.valid) {
        text(L"Waiting for the engine\x2026", TypeRole::Caption, x0, cap, c.text3, trim);
        return y + cap + pad;
    }

    // Headline: what Ombra holds, and its share of the machine.
    const double total = data.TotalBytes();
    text(FormatMemory(total), TypeRole::Title, x0, title, c.text, trim);
    if (data.sys_total_bytes > 0.0) {
        wchar_t pct[48];
        swprintf(pct, std::size(pct), L"%.0f%% of %ls", 100.0 * total / data.sys_total_bytes,
                 FormatMemory(data.sys_total_bytes).c_str());
        text(pct, TypeRole::Caption, x0, title, c.text3, trail);
    }
    y += title;
    // Rings commit memory as footage arrives: say what the total grows to once the buffer is full,
    // or a fresh start reads as cheaper than it is.
    const double max_total = data.MaxBytes();
    if (max_total - total >= 1024.0 * 1024.0) {
        text(L"Up to " + FormatMemory(max_total) + L" once the buffer is full", TypeRole::Caption, x0, cap, c.text3,
             trim);
        y += cap;
    }
    y += z.space_sm() * s;

    // Composition bar: pills with gaps, sized by share of Ombra's own footprint (a bar against the
    // whole machine would be a 5% sliver). A tiny segment keeps a round pill's width.
    const D2D1_COLOR_F c_video = c.info, c_audio = c.success, c_engine = c.text3;
    const float bar_h = 8 * s;
    fill(D2D1::RectF(x0, y, x1, y + bar_h), bar_h * 0.5f, track);
    if (total > 0.0) {
        struct Seg { double bytes; D2D1_COLOR_F color; };
        const Seg segs[] = {{data.file_backed ? 0.0 : data.video_bytes, c_video}, {data.audio_bytes, c_audio},
                            {data.engine_bytes, c_engine}};
        const float gap = 2 * s;
        int shown = 0;
        for (const Seg& sg : segs) if (sg.bytes > 0.0) ++shown;
        const float usable = w - gap * (std::max)(0, shown - 1);
        float x = x0;
        for (const Seg& sg : segs) {
            if (sg.bytes <= 0.0) continue;
            const float sw = (std::max)(bar_h, static_cast<float>(sg.bytes / total) * usable);
            fill(D2D1::RectF(x, y, (std::min)(x + sw, x1), y + bar_h), bar_h * 0.5f, sg.color);
            x += sw + gap;
        }
    }
    y += bar_h + z.space_md() * s;

    // Legend: colour dot, name, size, and the setting that buys it (a bar alone isn't actionable).
    const float dot = 4 * s, indent = z.space_lg() * s;
    auto legend = [&](const wchar_t* name, const D2D1_COLOR_F& col, const std::wstring& value,
                      const std::wstring& cause, bool last) {
        if (draw) p.Circle(x0 + dot, y + body * 0.5f, dot, col);
        text(name, TypeRole::Body, x0 + indent, body, c.text, trim);
        text(value, TypeRole::Label, x0, body, c.text, trail);
        y += body;
        // Several sources make a long cause; it wraps rather than losing the per-source size.
        const TextOpts wrap{TextAlign::Leading, true, false, false};
        const float cause_h = (std::max)(cap, std::ceil(t.MeasureText(t.Font(TypeRole::Caption, wrap), cause,
                                                                      x1 - x0 - indent).height));
        text(cause, TypeRole::Caption, x0 + indent, cause_h, c.text3, wrap);
        y += cause_h + (last ? 0.0f : z.space_sm() * s);
    };
    // A ring still filling shows what it holds and what it grows to.
    auto of_max = [](double held, double max, const std::wstring& cause) {
        return max - held >= 1024.0 * 1024.0 ? L"of " + FormatMemory(max) + L" \x00b7 " + cause : cause;
    };
    if (data.file_backed) {
        legend(L"Video buffer", c_video, L"disk " + FormatMemory(data.video_max_bytes), data.video_cause, false);
    } else {
        legend(L"Video buffer", c_video, FormatMemory(data.video_bytes),
               of_max(data.video_bytes, data.video_max_bytes, data.video_cause), false);
    }
    legend(L"Audio buffer", c_audio, FormatMemory(data.audio_bytes),
           of_max(data.audio_bytes, data.audio_max_bytes, data.audio_cause), false);
    legend(L"Engine", c_engine, FormatMemory(data.engine_bytes), L"Fixed overhead", true);

    // Staged edits: what Save would change the total to.
    if (data.has_projection) {
        divider();
        text(L"After Save, once the buffer is full", TypeRole::Caption, x0, cap, c.text3, trim);
        y += cap;
        const double proj = data.ProjectedBytes();
        text(FormatMemory(proj), TypeRole::Title, x0, title, c.text, trim);
        const double delta = proj - max_total;
        if (std::abs(delta) >= 1024.0 * 1024.0) {
            text((delta > 0 ? L"+" : L"\x2212") + FormatMemory(std::abs(delta)), TypeRole::Label, x0, title,
                 delta > 0 ? c.warning : c.success, trail);
        }
        y += title;
    }

    // The machine's RAM, with Ombra's slice of it -- answers "can I afford 120 s?".
    if (data.sys_total_bytes > 0.0) {
        divider();
        text(L"System RAM", TypeRole::Caption, x0, cap, c.text3, trim);
        wchar_t used[32];
        swprintf(used, std::size(used), L"%.0f%% used", data.sys_load_pct);
        text(used, TypeRole::Caption, x0, cap, c.text3, trail);
        y += cap + z.space_xs() * s;
        const float th = 6 * s;
        fill(D2D1::RectF(x0, y, x1, y + th), th * 0.5f, track);
        const float used_w = (std::max)(th, static_cast<float>(std::clamp(data.sys_load_pct, 0.0, 100.0) / 100.0) * w);
        fill(D2D1::RectF(x0, y, x0 + used_w, y + th), th * 0.5f, c.line_strong);
        const float ombra_w = (std::max)(th, static_cast<float>(total / data.sys_total_bytes) * w);
        fill(D2D1::RectF(x0, y, x0 + ombra_w, y + th), th * 0.5f, c.accent);
        y += th;
    }

    // VRAM is its own line, never a segment of the RAM bar: it's a different pool.
    if (data.vram_bytes >= 0.0) {
        divider();
        text(L"GPU memory", TypeRole::Body, x0, body, c.text, trim);
        text(FormatMemory(data.vram_bytes), TypeRole::Label, x0, body, c.text, trail);
        y += body;
        text(L"Capture and encoder surfaces", TypeRole::Caption, x0, cap, c.text3, trim);
        y += cap;
    }
    return y + pad;
}

float MemoryCard::ContentHeight(ID2D1DeviceContext* dc, const Theme& t, float s) const {
    return Flow(dc, t, s, false) - bounds.top;
}

void MemoryCard::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const float r = t.size.radius_panel() * s;
    p.Fill(bounds, r, t.color.panel);
    p.CardEdge(bounds, r);
    Flow(dc, t, s, true);
}

}  // namespace ombra_ui
