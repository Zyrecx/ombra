#include "ombra-ui/ui/navigation.h"

#include "ombra-ui/ui/paint.h"

#include <algorithm>
#include <cmath>

namespace ombra_ui {

using namespace paint;

namespace {

// Arrow keys step a selection through `n` entries; Home/End jump. False for any other key.
bool StepSelection(UINT vk, int n, int& selected, bool horizontal) {
    if (n <= 0) return false;
    const UINT prev = horizontal ? VK_LEFT : VK_UP, next = horizontal ? VK_RIGHT : VK_DOWN;
    int to = selected;
    if (vk == prev) to = (std::max)(0, selected - 1);
    else if (vk == next) to = (std::min)(n - 1, selected + 1);
    else if (vk == VK_HOME) to = 0;
    else if (vk == VK_END) to = n - 1;
    else return false;
    selected = to;
    return true;
}

}  // namespace

// --- NavRail ------------------------------------------------------------------------------------

D2D1_SIZE_F NavRail::ItemSize(const Theme& t, float s) const {
    // Icon over the name: a 72 DIP square holds "Appearance" in Caption with room either side. Type
    // doesn't scale with density, so at Compact the item widens to its longest name (selected, in
    // Label) rather than trimming it.
    const float side = (t.size.control_lg() + t.size.space_xxl()) * s;
    return D2D1::SizeF((std::max)(side, std::ceil(label_w_)), side);
}

D2D1_SIZE_F NavRail::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    theme_ = ctx.theme;
    s_ = ctx.dpi_scale;
    if (!theme_ || items.empty()) return D2D1::SizeF(0, 0);
    label_w_ = 0.0f;
    for (const auto& item : items)
        label_w_ = (std::max)(label_w_, theme_->MeasureText(theme_->Font(TypeRole::Label), item.label).width);
    const D2D1_SIZE_F it = ItemSize(*theme_, s_);
    const float gap = theme_->size.space_xs() * s_;
    const float n = static_cast<float>(items.size());
    return horizontal ? D2D1::SizeF(n * it.width + (n - 1) * gap, it.height)
                      : D2D1::SizeF(it.width, n * it.height + (n - 1) * gap);
}

D2D1_RECT_F NavRail::ItemRect(const Theme& t, float s, int i) const {
    const D2D1_SIZE_F it = ItemSize(t, s);
    const float step = (horizontal ? it.width : it.height) + t.size.space_xs() * s;
    const float x = bounds.left + (horizontal ? step * static_cast<float>(i) : 0.0f);
    const float y = bounds.top + (horizontal ? 0.0f : step * static_cast<float>(i));
    return D2D1::RectF(x, y, x + it.width, y + it.height);
}

int NavRail::ItemAt(D2D1_POINT_2F pt) const {
    if (!theme_) return -1;
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
        if (PointIn(ItemRect(*theme_, s_, i), pt)) return i;
    return -1;
}

void NavRail::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float ic = (z.icon() + 4) * s, label_h = LineHeight(TypeRole::Caption) * s;
    const float pw = (z.control_lg() + z.space_sm()) * s, ph = z.control_sm() * s;
    TrackItems(hover_index);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const D2D1_RECT_F r = ItemRect(t, s, i);
        const bool sel = i == selected;
        const float hv = item_hover_.Amount(i);
        const D2D1_COLOR_F ink = sel ? c.accent_text : Mix(c.text2, c.text, hv);
        // The icon and name centred as one block; the pill sits behind the icon only.
        const float block = ic + z.space_sm() * s + label_h;
        const float iy = r.top + (r.bottom - r.top - block) * 0.5f + ic * 0.5f;
        const auto pill = D2D1::RectF((r.left + r.right - pw) * 0.5f, iy - ph * 0.5f, (r.left + r.right + pw) * 0.5f, iy + ph * 0.5f);
        const float prad = t.size.radius_full(ph, s);
        if (sel) p.Fill(pill, prad, c.accent_soft);
        p.Fill(pill, prad, c.HoverLayer(hv));
        DrawIcon(dc, t, items[i].icon, (r.left + r.right) * 0.5f, iy, ic, p.B(ink).Get());
        const float ly = iy + ic * 0.5f + z.space_sm() * s;
        p.Text(items[i].label, sel ? TypeRole::Label : TypeRole::Caption, D2D1::RectF(r.left, ly, r.right, ly + label_h), ink,
               {TextAlign::Center, false, true});
        if (focus_visible && sel) p.Focus(pill, prad);
    }
}

bool NavRail::OnMouseMove(D2D1_POINT_2F pt) {
    const int h = ItemAt(pt);
    const bool changed = h != hover_index;
    hover_index = h;
    hovered = h >= 0;
    return changed;
}

void NavRail::Press(D2D1_POINT_2F pt) {
    const int i = ItemAt(pt);
    if (i < 0 || i == selected) return;
    selected = i;
    if (on_change) on_change(selected);
}

bool NavRail::OnKeyDown(UINT vk) {
    if (!focused) return false;
    const int was = selected;
    if (!StepSelection(vk, static_cast<int>(items.size()), selected, horizontal)) return false;
    focus_visible = true;
    if (selected != was && on_change) on_change(selected);
    return true;
}

// --- TabBar -------------------------------------------------------------------------------------

D2D1_SIZE_F TabBar::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float h = z.control_lg() * ctx.dpi_scale;
    return D2D1::SizeF(c.max_w < kUnbounded ? c.max_w : 480 * ctx.dpi_scale, h);
}

D2D1_RECT_F TabBar::TabRect(int i) const {
    const float n = static_cast<float>((std::max)(size_t{1}, tabs.size()));
    const float w = (bounds.right - bounds.left) / n;
    return D2D1::RectF(bounds.left + w * static_cast<float>(i), bounds.top, bounds.left + w * static_cast<float>(i + 1), bounds.bottom);
}

void TabBar::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    p.Fill(D2D1::RectF(bounds.left, bounds.bottom - s, bounds.right, bounds.bottom), 0, c.line);
    TrackItems(hover_index);
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        const D2D1_RECT_F r = TabRect(i);
        const bool sel = i == selected;
        const float hv = sel ? 0.0f : item_hover_.Amount(i);
        p.Fill(D2D1::RectF(r.left, r.top, r.right, r.bottom - s), z.radius_control() * s, c.HoverLayer(hv));
        if (sel) p.Fill(D2D1::RectF(r.left, r.bottom - 2 * s, r.right, r.bottom), s, c.accent);
        p.Text(tabs[i], sel ? TypeRole::Label : TypeRole::Body, r, sel ? c.text : Mix(c.text2, c.text, hv), {TextAlign::Center, false, true});
    }
    if (focus_visible && selected >= 0 && selected < static_cast<int>(tabs.size()))
        p.Focus(TabRect(selected), z.radius_control() * s);
}

bool TabBar::OnMouseMove(D2D1_POINT_2F pt) {
    int h = -1;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
        if (PointIn(TabRect(i), pt)) h = i;
    const bool changed = h != hover_index;
    hover_index = h;
    hovered = h >= 0;
    return changed;
}

void TabBar::Press(D2D1_POINT_2F pt) {
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (!PointIn(TabRect(i), pt) || i == selected) continue;
        selected = i;
        if (on_change) on_change(selected);
        return;
    }
}

bool TabBar::OnKeyDown(UINT vk) {
    if (!focused) return false;
    const int was = selected;
    if (!StepSelection(vk, static_cast<int>(tabs.size()), selected, true)) return false;
    focus_visible = true;
    if (selected != was && on_change) on_change(selected);
    return true;
}

// --- SaveBar ------------------------------------------------------------------------------------

SaveBar::SaveBar() {
    discard.label = L"Discard";
    save.label = L"Save";
    save.kind = ButtonControl::Kind::Primary;
}

D2D1_SIZE_F SaveBar::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float h = (z.control() + 2 * z.space_md()) * ctx.dpi_scale;
    return D2D1::SizeF(c.max_w < kUnbounded ? c.max_w : 640 * ctx.dpi_scale, h);
}

void SaveBar::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float gap = z.space_sm() * ctx.dpi_scale;
    const D2D1_SIZE_F sm = save.Measure(LayoutConstraints{}, ctx), dm = discard.Measure(LayoutConstraints{}, ctx);
    const float y = (rect.top + rect.bottom - sm.height) * 0.5f;
    const float sx = rect.right - sm.width;
    save.Arrange(D2D1::RectF(sx, y, sx + sm.width, y + sm.height), ctx);
    const float dx = sx - gap - dm.width;
    discard.Arrange(D2D1::RectF(dx, y, dx + dm.width, y + dm.height), ctx);
}

void SaveBar::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    p.Fill(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + s), 0, c.line);
    const float x = bounds.left;
    const float right = discard.bounds.left - z.space_md() * s;
    const float th = LineHeight(TypeRole::Label) * s, dh = detail.empty() ? 0.0f : LineHeight(TypeRole::Caption) * s;
    const float top = (bounds.top + bounds.bottom - th - dh) * 0.5f;
    p.Text(title, TypeRole::Label, D2D1::RectF(x, top, right, top + th), failed ? c.danger : c.text, {TextAlign::Leading, false, true});
    if (!detail.empty())
        p.Text(detail, TypeRole::Caption, D2D1::RectF(x, top + th, right, top + th + dh), c.text2, {TextAlign::Leading, false, true});
    discard.Draw(dc, t, s);
    save.Draw(dc, t, s);
}

CursorKind SaveBar::CursorAt(D2D1_POINT_2F pt) const {
    if (!visible) return CursorKind::Arrow;
    const CursorKind k = discard.CursorAt(pt);
    return k != CursorKind::Arrow ? k : save.CursorAt(pt);
}
bool SaveBar::OnMouseMove(D2D1_POINT_2F pt) {
    if (!visible) return false;
    const bool a = discard.OnMouseMove(pt);
    return save.OnMouseMove(pt) || a;
}
bool SaveBar::OnMouseDown(D2D1_POINT_2F pt) {
    if (!visible) return false;
    const bool a = discard.OnMouseDown(pt);
    return save.OnMouseDown(pt) || a;
}
bool SaveBar::OnMouseUp(D2D1_POINT_2F pt) {
    if (!visible) return false;
    const bool a = discard.OnMouseUp(pt);
    return save.OnMouseUp(pt) || a;
}

}  // namespace ombra_ui
