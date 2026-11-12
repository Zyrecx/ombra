#include "ombra-ui/ui/controls.h"
#include "ombra/brand.h"

#include "ombra-ui/ui/color.h"
#include "ombra-ui/ui/draw_probe.h"
#include "ombra-ui/ui/layout.h"
#include "ombra-ui/ui/paint.h"
#include "ombra/hotkey_spec.h"

#include <d2d1effects.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ombra_ui {

using namespace paint;

// --- Control ------------------------------------------------------------------------------------

bool Control::OnMouseDown(D2D1_POINT_2F pt) {
    if (!HitTest(pt)) {
        focused = focus_visible = false;
        return false;
    }
    pressed = true;
    focused = true;
    focus_visible = false;
    Press(pt);
    return true;
}

bool Control::QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const {
    if (tooltip.empty() || !visible || !hovered) return false;
    if (text) *text = tooltip;
    if (anchor) *anchor = D2D1::Point2F((bounds.left + bounds.right) * 0.5f, bounds.top);
    return true;
}

bool Control::OnMouseUp(D2D1_POINT_2F) {
    const bool was = pressed;
    pressed = false;
    return was;
}

// --- Switch -------------------------------------------------------------------------------------

D2D1_SIZE_F SwitchControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    return D2D1::SizeF(z.switch_w() * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

void SwitchControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const float w = t.size.switch_w() * s, h = t.size.switch_h() * s;
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, w, h);
    const ColorRoles& c = t.color;
    const float hv = Hover(), rad = t.size.radius_full(h, s);
    if (on) {
        p.Fill(r, rad, pressed ? c.accent_pressed : Mix(c.accent, c.accent_hover, hv));
    } else {
        p.Fill(r, rad, FieldFill(t, hv, pressed));
        p.Stroke(r, rad, c.line_strong);
    }
    const float k = h * 0.7f, pad = (h - k) * 0.5f;
    const float kx = on ? r.right - pad - k : r.left + pad;
    p.Fill(D2D1::RectF(kx, r.top + pad, kx + k, r.bottom - pad), (std::max)(0.0f, rad - pad),
           on ? c.on_accent : c.text2);
    if (focus_visible) p.Focus(r, rad);
}

void SwitchControl::Press(D2D1_POINT_2F) {
    on = !on;
    if (on_change) on_change(on);
}

bool SwitchControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

// --- Button -------------------------------------------------------------------------------------

D2D1_SIZE_F ButtonControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    if (!ctx.theme) return D2D1::SizeF(96 * ctx.dpi_scale, 32 * ctx.dpi_scale);
    const Theme& t = *ctx.theme;
    if (label.empty() && icon != IconType::None) {
        const float d = (in_row ? t.size.control_sm() : t.size.control()) * ctx.dpi_scale;
        return D2D1::SizeF(d, d);
    }
    float tw = t.MeasureText(t.Font(TypeRole::Label), label).width;
    if (icon != IconType::None) tw += (t.size.icon() + t.size.space_sm()) * ctx.dpi_scale;
    return D2D1::SizeF(tw + 2 * t.size.space_lg() * ctx.dpi_scale,
                       (in_row ? t.size.control_sm() : t.size.control()) * ctx.dpi_scale);
}

void ButtonControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const D2D1_RECT_F r = in_row ? RowBox(bounds, bounds.left, bounds.right - bounds.left, t.size.control_sm() * s) : bounds;
    const float rad = t.size.control_radius(r.bottom - r.top, s);
    D2D1_COLOR_F ink = c.text;
    const float hv = Hover();
    switch (kind) {
        case Kind::Primary:
            p.Fill(r, rad, pressed ? c.accent_pressed : Mix(c.accent, c.accent_hover, hv));
            ink = c.on_accent;
            break;
        case Kind::Secondary:
            p.Fill(r, rad, FieldFill(t, hv, pressed));
            p.Stroke(r, rad, c.line_strong);
            break;
        case Kind::Danger:  // red text; hover/press a red tint, not a solid fill
            p.Fill(r, rad, Over(c.raised, D2D1::ColorF(c.danger.r, c.danger.g, c.danger.b,
                                                       pressed ? 0.24f : 0.14f * hv)));
            p.Stroke(r, rad, pressed ? c.danger : Mix(c.line_strong, c.danger, hv));
            ink = c.danger;
            break;
    }
    if (icon == IconType::None) {
        p.Text(label, TypeRole::Label, r, ink, {TextAlign::Center});
    } else if (label.empty()) {
        DrawIcon(dc, t, icon, (r.left + r.right) * 0.5f, Cy(r), t.size.icon() * s, p.B(ink).Get());
    } else {
        // Icon and label centred as one group.
        const float iw = t.size.icon() * s, gap = t.size.space_sm() * s;
        const float x = (r.left + r.right - (iw + gap + p.TextW(label, TypeRole::Label))) * 0.5f;
        DrawIcon(dc, t, icon, x + iw * 0.5f, Cy(r), iw, p.B(ink).Get());
        p.Text(label, TypeRole::Label, D2D1::RectF(x + iw + gap, r.top, r.right, r.bottom), ink);
    }
    if (focus_visible) p.Focus(r, rad);
}

void ButtonControl::Press(D2D1_POINT_2F) {
    if (on_click) on_click();
}

bool ButtonControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

// --- Split button -------------------------------------------------------------------------------

D2D1_SIZE_F SplitButtonControl::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    ButtonControl main;
    main.label = label;
    main.icon = icon;
    const D2D1_SIZE_F m = main.Measure(c, ctx);
    arrow_w_ = (ctx.theme ? ctx.theme->size.control_sm() : 28.0f) * ctx.dpi_scale;
    return D2D1::SizeF(m.width + arrow_w_, m.height);
}

int SplitButtonControl::PartAt(D2D1_POINT_2F pt) const {
    if (!HitTest(pt)) return kNone;
    return pt.x >= bounds.right - arrow_w_ ? kArrow : kAction;
}

void SplitButtonControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const D2D1_RECT_F r = bounds;
    const float rad = t.size.control_radius(r.bottom - r.top, s);
    const float sx = std::round(r.right - arrow_w_);
    TrackItems(open ? kArrow : hovered_part_);
    for (int part : {kAction, kArrow}) {
        dc->PushAxisAlignedClip(part == kArrow ? D2D1::RectF(sx, r.top, r.right, r.bottom) : D2D1::RectF(r.left, r.top, sx, r.bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        p.Fill(r, rad, FieldFill(t, item_hover_.Amount(part), pressed && pressed_part_ == part));
        dc->PopAxisAlignedClip();
    }
    p.Stroke(r, rad, c.line_strong);
    p.Line(sx, r.top + s, sx, r.bottom - s, c.line_strong, 1.0f);
    const float iw = icon == IconType::None ? 0.0f : t.size.icon() * s;
    const float gap = icon == IconType::None ? 0.0f : t.size.space_sm() * s;
    const float x = (r.left + sx - (iw + gap + p.TextW(label, TypeRole::Label))) * 0.5f;
    if (icon != IconType::None) DrawIcon(dc, t, icon, x + iw * 0.5f, Cy(r), iw, p.B(c.text).Get());
    p.Text(label, TypeRole::Label, D2D1::RectF(x + iw + gap, r.top, sx, r.bottom), c.text);
    p.Chevron((sx + r.right) * 0.5f, Cy(r), open, c.text);
    if (focus_visible) p.Focus(r, rad);
}

bool SplitButtonControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_part_;
    Widget::OnMouseMove(pt);
    hovered_part_ = PartAt(pt);
    return was != hovered_part_;
}

void SplitButtonControl::Press(D2D1_POINT_2F pt) {
    pressed_part_ = PartAt(pt);
    if (pressed_part_ == kArrow) {
        if (on_menu) on_menu();
    } else if (on_click) {
        on_click();
    }
}

bool SplitButtonControl::OnKeyDown(UINT vk) {
    if (!focused) return false;
    if (vk == VK_DOWN) {
        if (on_menu) on_menu();
        return true;
    }
    if (vk != VK_SPACE && vk != VK_RETURN) return false;
    if (on_click) on_click();
    return true;
}

// --- Icon button --------------------------------------------------------------------------------

D2D1_SIZE_F IconButtonControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const float d = (ctx.theme ? ctx.theme->size.control_sm() : 28.0f) * ctx.dpi_scale;
    return D2D1::SizeF(d, d);
}

void IconButtonControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const D2D1_RECT_F& r = bounds;
    const float rad = t.size.control_radius(r.bottom - r.top, s);
    const float hv = pressed ? 1.0f : Hover();
    D2D1_COLOR_F ink = Mix(c.text2, c.text, hv);
    if (glyph == Glyph::Remove) {
        p.Fill(r, rad, D2D1::ColorF(c.danger.r, c.danger.g, c.danger.b, pressed ? 0.24f : 0.14f * hv));
        ink = Mix(c.text2, c.danger, hv);
    } else {
        p.Fill(r, rad, pressed ? c.PressedLayer() : c.HoverLayer(hv));
    }
    const float cx = (r.left + r.right) * 0.5f, cy = Cy(r), u = t.size.glyph() * 0.5f * s;
    switch (glyph) {
        case Glyph::More:
            for (int i = -1; i <= 1; ++i) p.Circle(cx + i * u * 0.9f, cy, 1.5f * s, ink);
            break;
        case Glyph::Remove:
            p.Line(cx - u * 0.75f, cy - u * 0.75f, cx + u * 0.75f, cy + u * 0.75f, ink);
            p.Line(cx - u * 0.75f, cy + u * 0.75f, cx + u * 0.75f, cy - u * 0.75f, ink);
            break;
        case Glyph::Add:
            p.Line(cx - u, cy, cx + u, cy, ink);
            p.Line(cx, cy - u, cx, cy + u, ink);
            break;
        case Glyph::Reset:
            DrawIcon(dc, t, IconType::Replay, cx, cy, t.size.icon() * s, p.B(ink).Get());
            break;
        case Glyph::Icon:
            DrawIcon(dc, t, icon, cx, cy, t.size.icon() * s, p.B(ink).Get());
            break;
    }
    if (focus_visible) p.Focus(r, rad);
}

void IconButtonControl::Press(D2D1_POINT_2F) {
    if (on_click) on_click();
}

bool IconButtonControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

// --- Chip group -------------------------------------------------------------------------------

D2D1_SIZE_F ChipGroupControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    chip_w_.clear();
    const Theme* t = ctx.theme;
    const Sizes z = t ? t->size : Sizes{};
    gap_dip_ = z.space_sm();
    scale_ = ctx.dpi_scale;
    float w = 0;
    for (const auto& o : options) {
        const float tw = t ? t->MeasureText(t->Font(TypeRole::Body), o).width / ctx.dpi_scale : 8.0f * o.size();
        if (!chip_w_.empty()) w += gap_dip_;
        chip_w_.push_back(std::ceil(tw) + z.space_md() * 2);
        w += chip_w_.back();
    }
    return D2D1::SizeF(w * ctx.dpi_scale, z.control_xs() * ctx.dpi_scale);
}

int ChipGroupControl::IndexAt(D2D1_POINT_2F pt) const {
    if (!HitTest(pt)) return -1;
    float x = bounds.left;
    for (size_t i = 0; i < chip_w_.size(); ++i) {
        const float r = x + chip_w_[i] * scale_;
        if (pt.x >= x && pt.x <= r) return static_cast<int>(i);
        x = r + gap_dip_ * scale_;
    }
    return -1;
}

void ChipGroupControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    if (chip_w_.size() != options.size() || scale_ != s) Measure({}, LayoutContext{s, &t});
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const float h = t.size.control_xs() * s;
    TrackItems(hovered_index);
    float x = bounds.left;
    for (size_t i = 0; i < options.size(); ++i) {
        const D2D1_RECT_F r = RowBox(bounds, x, chip_w_[i] * s, h);
        const float rad = t.size.radius_full(h, s);
        const bool sel = static_cast<int>(i) == selected, hov = static_cast<int>(i) == hovered_index;
        const float hv = item_hover_.Amount(static_cast<int>(i));
        const D2D1_COLOR_F layer = pressed && hov ? c.PressedLayer() : c.HoverLayer(hv);
        D2D1_COLOR_F ink = Mix(c.text2, c.text, hv);
        if (sel) {
            p.Fill(r, rad, c.accent);
            ink = c.on_accent;
        } else {
            p.Fill(r, rad, layer);
            p.Stroke(r, rad, c.line_strong);
        }
        p.Text(options[i], TypeRole::Body, r, ink, {TextAlign::Center});
        if (focus_visible && sel) p.Focus(r, rad);
        x = r.right + gap_dip_ * s;
    }
}

bool ChipGroupControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = IndexAt(pt);
    return was != hovered_index;
}

void ChipGroupControl::Press(D2D1_POINT_2F pt) {
    const int i = IndexAt(pt);
    if (i < 0 || i == selected) return;
    selected = i;
    if (on_change) on_change(i);
}

bool ChipGroupControl::OnKeyDown(UINT vk) {
    if (!focused || options.empty()) return false;
    const int n = static_cast<int>(options.size());
    if (vk == VK_LEFT && selected > 0) selected--;
    else if (vk == VK_RIGHT && selected < n - 1) selected++;
    else return false;
    if (on_change) on_change(selected);
    return true;
}

// --- Toggle chip --------------------------------------------------------------------------------

D2D1_SIZE_F ToggleChipControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float tw = ctx.theme ? ctx.theme->MeasureText(ctx.theme->Font(TypeRole::Body), label).width / ctx.dpi_scale
                               : 8.0f * label.size();
    return D2D1::SizeF((z.space_sm() + z.icon() + z.space_xs() + std::ceil(tw) + z.space_md()) * ctx.dpi_scale,
                       z.control_xs() * ctx.dpi_scale);
}

void ToggleChipControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, z.control_xs() * s);
    const float rad = z.radius_full(r.bottom - r.top, s), hv = Hover();
    const D2D1_COLOR_F layer = pressed ? c.PressedLayer() : c.HoverLayer(hv);
    D2D1_COLOR_F ink = c.text_off;
    const float iw = z.icon() * s, ix = r.left + z.space_sm() * s;
    if (on) {
        p.Fill(r, rad, c.accent_soft);
        ink = c.accent_text;
    }
    p.Fill(r, rad, layer);
    DrawIcon(dc, t, on ? icon : off_icon, ix + iw * 0.5f, Cy(r), iw, p.B(ink).Get());
    p.Text(label, TypeRole::Body, D2D1::RectF(ix + iw + z.space_xs() * s, r.top, r.right, r.bottom), ink);
    if (focus_visible) p.Focus(r, rad);
}

void ToggleChipControl::Press(D2D1_POINT_2F) {
    on = !on;
    if (on_change) on_change(on);
}

bool ToggleChipControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN)) return false;
    Press({});
    return true;
}

// --- Volume -------------------------------------------------------------------------------------

D2D1_SIZE_F VolumeControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    side_ = z.control() * ctx.dpi_scale;
    track_ = 2.0f * z.control_lg() * ctx.dpi_scale;
    pad_ = side_ * 0.5f;  // the knob's centre as far from the column's ends as from its sides
    return D2D1::SizeF(side_, side_);
}

D2D1_RECT_F VolumeControl::Speaker() const {
    return D2D1::RectF(bounds.left, bounds.top, bounds.left + side_, bounds.top + side_);
}

D2D1_RECT_F VolumeControl::SliderBox() const {
    // Touching the speaker, so the mouse can travel up without leaving the control.
    const D2D1_RECT_F sp = Speaker();
    return D2D1::RectF(sp.left, sp.top - track_ - 2 * pad_, sp.right, sp.top);
}

bool VolumeControl::HitTest(D2D1_POINT_2F pt) const {
    if (!visible || !enabled) return false;
    if (PointIn(Speaker(), pt)) return true;
    return Shown() && PointIn(SliderBox(), pt);
}

void VolumeControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    Measure({}, LayoutContext{s, &t});
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const D2D1_RECT_F sp = Speaker();
    const float rad = t.size.radius_control() * s;
    const float reveal = reveal_.Track(Shown() && enabled, kFadeMs);
    const float hv = Hover();
    p.Fill(sp, rad, pressed && !dragging_ ? c.PressedLayer() : c.HoverLayer(hv));
    DrawIcon(dc, t, muted ? IconType::VolumeOff : IconType::Volume, (sp.left + sp.right) * 0.5f, Cy(sp), t.size.icon() * s,
             p.B(Mix(c.text2, c.text, hv)).Get());
    if (focus_visible) p.Focus(sp, rad);
    if (reveal <= 0.0f) return;

    const D2D1_RECT_F box = SliderBox();
    const float th = 2.0f * s, v = static_cast<float>(std::clamp(muted ? 0.0 : value, 0.0, 1.0));
    const D2D1_COLOR_F fill = muted ? c.text_off : c.text;
    // Fades in with the column, shadow included (a layer, so the shadow bitmap fades too).
    const bool fading = reveal < 1.0f;
    if (fading)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), reveal),
                      nullptr);
    shadow_.Draw(dc, t, s, box, rad);
    p.Fill(box, rad, c.raised);
    p.Stroke(box, rad, c.line_strong);
    const float cx = (box.left + box.right) * 0.5f, b = box.bottom - pad_, top = box.top + pad_, ky = b - (b - top) * v;
    p.Fill(D2D1::RectF(cx - th, top, cx + th, b), th, c.HoverLayer(2.0f));
    p.Fill(D2D1::RectF(cx - th, ky, cx + th, b), th, fill);
    p.Circle(cx, ky, (dragging_ ? 6.0f : 5.0f) * s, fill);
    if (fading) dc->PopLayer();
}

void VolumeControl::SetFrom(D2D1_POINT_2F pt) {
    const D2D1_RECT_F box = SliderBox();
    const float b = box.bottom - pad_, top = box.top + pad_;
    if (b <= top) return;
    value = std::clamp(static_cast<double>((b - pt.y) / (b - top)), 0.0, 1.0);
    if (on_change) on_change(value);
}

void VolumeControl::Press(D2D1_POINT_2F pt) {
    if (PointIn(Speaker(), pt)) {
        if (on_mute) on_mute();
        return;
    }
    dragging_ = true;
    SetFrom(pt);
}

bool VolumeControl::OnMouseMove(D2D1_POINT_2F pt) {
    if (dragging_) {
        SetFrom(pt);
        return true;
    }
    return Widget::OnMouseMove(pt);
}

bool VolumeControl::OnMouseUp(D2D1_POINT_2F pt) {
    const bool was = dragging_;
    dragging_ = false;
    hovered = HitTest(pt);
    return Control::OnMouseUp(pt) || was;
}

bool VolumeControl::OnKeyDown(UINT vk) {
    if (!focused) return false;
    double step = 0;
    if (vk == VK_RIGHT || vk == VK_UP) step = 0.1;
    else if (vk == VK_LEFT || vk == VK_DOWN) step = -0.1;
    else if (vk == VK_SPACE || vk == VK_RETURN) {
        if (on_mute) on_mute();
        return true;
    } else {
        return false;
    }
    value = std::clamp(value + step, 0.0, 1.0);
    if (on_change) on_change(value);
    return true;
}

// --- Segmented ----------------------------------------------------------------------------------

D2D1_SIZE_F SegmentedControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    seg_w_.clear();
    const Theme* t = ctx.theme;
    const Sizes z = t ? t->size : Sizes{};
    inset_dip_ = z.space_xxs();
    float w = inset_dip_ * 2;
    for (const auto& o : options) {
        const float tw = t ? t->MeasureText(t->Font(TypeRole::Label), o).width / ctx.dpi_scale : 8.0f * o.size();
        seg_w_.push_back(std::ceil(tw) + z.space_md() * 2);
        w += seg_w_.back();
    }
    total_dip_ = w;
    return D2D1::SizeF(w * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

int SegmentedControl::IndexAt(D2D1_POINT_2F pt) const {
    if (!HitTest(pt) || seg_w_.empty()) return -1;
    const float s = total_dip_ > 0 ? (bounds.right - bounds.left) / total_dip_ : 1.0f;
    float x = bounds.left + inset_dip_ * s;
    for (size_t i = 0; i < seg_w_.size(); ++i) {
        x += seg_w_[i] * s;
        if (pt.x < x) return static_cast<int>(i);
    }
    return static_cast<int>(seg_w_.size()) - 1;
}

void SegmentedControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    if (seg_w_.size() != options.size()) Measure({}, LayoutContext{s, &t});
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const float h = t.size.control_sm() * s, inset = t.size.space_xxs() * s;
    const float rad = t.size.control_radius(h, s);
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, h);
    p.Fill(r, rad, c.raised);
    p.Stroke(r, rad, c.line_strong);
    float x = r.left + inset;
    const float inner = (std::max)(0.0f, rad - inset);
    TrackItems(hovered_index);
    for (size_t i = 0; i < options.size(); ++i) {
        const auto seg = D2D1::RectF(x, r.top + inset, x + seg_w_[i] * s, r.bottom - inset);
        const bool sel = static_cast<int>(i) == selected, hov = static_cast<int>(i) == hovered_index;
        const float hv = item_hover_.Amount(static_cast<int>(i));
        D2D1_COLOR_F ink = Mix(c.text2, c.text, hv);
        if (sel) {
            p.Fill(seg, inner, c.accent);
            ink = c.on_accent;
        } else {
            p.Fill(seg, inner, pressed && hov ? c.PressedLayer() : c.HoverLayer(hv));
        }
        p.Text(options[i], TypeRole::Label, seg, ink, {TextAlign::Center});
        x = seg.right;
    }
    if (focus_visible) p.Focus(r, rad);
}

bool SegmentedControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = IndexAt(pt);
    return was != hovered_index;
}

void SegmentedControl::Press(D2D1_POINT_2F pt) {
    const int i = IndexAt(pt);
    if (i < 0 || i == selected) return;
    selected = i;
    if (on_change) on_change(i);
}

bool SegmentedControl::OnKeyDown(UINT vk) {
    if (!focused || options.empty()) return false;
    const int n = static_cast<int>(options.size());
    if (vk == VK_LEFT && selected > 0) selected--;
    else if (vk == VK_RIGHT && selected < n - 1) selected++;
    else return false;
    if (on_change) on_change(selected);
    return true;
}

// --- Level tiles --------------------------------------------------------------------------------

namespace {
constexpr float kRadioR = 8.5f;  // DIP: the radio's outer radius
}  // namespace

float LevelTilesControl::TileHeight(const Theme& t, int i, float tile_w, float s) const {
    const Sizes& z = t.size;
    const float pad = z.space_md() * s, tx = pad + (2 * kRadioR + z.space_sm()) * s;
    const TextOpts wrap{TextAlign::Leading, true};
    float h = (std::max)(LineHeight(TypeRole::Label) * s,
                         t.MeasureText(t.Font(TypeRole::Label, wrap), options[i], tile_w - tx - pad).height);
    if (i < static_cast<int>(lines.size()) && !lines[i].empty())
        h += z.space_xs() * s + t.MeasureText(t.Font(TypeRole::Caption, wrap), lines[i], tile_w - 2 * pad).height;
    return 2 * pad + h;
}

D2D1_SIZE_F LevelTilesControl::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    s_ = ctx.dpi_scale;
    const float w = c.BoundedWidth() ? c.max_w : width_dip * ctx.dpi_scale;
    if (ctx.theme) gap_ = ctx.theme->size.space_sm() * ctx.dpi_scale;
    float h = 0;
    const int n = static_cast<int>(options.size());
    if (ctx.theme && n > 0) {
        const float tw = (w - gap_ * (n - 1)) / n;
        for (int i = 0; i < n; ++i) h = (std::max)(h, TileHeight(*ctx.theme, i, tw, ctx.dpi_scale));
    }
    return D2D1::SizeF(w, h);
}

D2D1_RECT_F LevelTilesControl::TileRect(int i) const {
    const int n = (std::max)(1, static_cast<int>(options.size()));
    const float tw = (bounds.right - bounds.left - gap_ * (n - 1)) / n, x = bounds.left + i * (tw + gap_);
    return D2D1::RectF(x, bounds.top, x + tw, bounds.bottom);
}

int LevelTilesControl::IndexAt(D2D1_POINT_2F pt) const {
    for (int i = 0; i < static_cast<int>(options.size()); ++i)
        if (PointIn(TileRect(i), pt)) return i;
    return -1;
}

void LevelTilesControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible || options.empty()) return;
    s_ = s;
    gap_ = t.size.space_sm() * s;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float rad = z.radius_control() * s, pad = z.space_md() * s, lh = LineHeight(TypeRole::Label) * s;
    const float rr = kRadioR * s, tx = pad + (2 * kRadioR + z.space_sm()) * s;
    const TextOpts wrap{TextAlign::Leading, true};
    TrackItems(hovered_index);
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const D2D1_RECT_F r = TileRect(i);
        const bool on = i == selected;
        const float hover = item_hover_.Amount(i);
        // TileControl's chosen look: accent tint and a 1.5 DIP accent outline.
        p.Fill(r, rad, c.raised);
        if (on) p.Fill(r, rad, c.accent_soft);
        p.Fill(r, rad, pressed && i == hovered_index ? c.PressedLayer() : c.HoverLayer(hover));
        if (on) p.Stroke(r, rad, c.accent, 1.5f);
        else p.Stroke(r, rad, c.line_strong);

        const float cx = r.left + pad + rr, cy = r.top + pad + lh * 0.5f;
        p.Ring(cx, cy, rr - 0.75f * s, on ? c.accent : Mix(c.line_strong, c.text3, hover), 1.5f);
        if (on) p.Circle(cx, cy, 4.5f * s, c.accent);

        const float w = r.right - r.left;
        const float th = (std::max)(lh, t.MeasureText(t.Font(TypeRole::Label, wrap), options[i], w - tx - pad).height);
        p.Text(options[i], TypeRole::Label, D2D1::RectF(r.left + tx, r.top + pad, r.right - pad, r.top + pad + th + 1), c.text, wrap);
        if (i < static_cast<int>(lines.size()) && !lines[i].empty()) {
            const float ly = r.top + pad + th + z.space_xs() * s;
            const float bh = t.MeasureText(t.Font(TypeRole::Caption, wrap), lines[i], w - 2 * pad).height;
            p.Text(lines[i], TypeRole::Caption, D2D1::RectF(r.left + pad, ly, r.right - pad, ly + bh + 1),
                   on ? c.text2 : c.text3, wrap);
        }
    }
    if (focus_visible && selected >= 0 && selected < static_cast<int>(options.size())) p.Focus(TileRect(selected), rad);
}

void LevelTilesControl::Pick(int i) {
    if (i < 0 || i >= static_cast<int>(options.size()) || i == selected) return;
    selected = i;
    if (on_change) on_change(i);
}

void LevelTilesControl::Press(D2D1_POINT_2F pt) { Pick(IndexAt(pt)); }

bool LevelTilesControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = hovered ? IndexAt(pt) : -1;
    return was != hovered_index;
}

bool LevelTilesControl::OnKeyDown(UINT vk) {
    if (!focused || options.empty()) return false;
    if (vk == VK_LEFT && selected > 0) Pick(selected - 1);
    else if (vk == VK_RIGHT && selected + 1 < static_cast<int>(options.size())) Pick(selected + 1);
    else return false;
    return true;
}

// --- Select -------------------------------------------------------------------------------------

D2D1_SIZE_F SelectControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    float w = width_dip;
    if (fit_options && ctx.theme) {
        float text_w = 0.0f;  // the same text box Draw uses: padding both sides, then the chevron
        for (const auto& o : options)
            text_w = (std::max)(text_w, ctx.theme->MeasureText(ctx.theme->Font(TypeRole::Body), o).width);
        w = (std::max)(w, std::ceil(text_w / ctx.dpi_scale) + 2 * z.space_md() + z.icon());
    }
    return D2D1::SizeF(w * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

void SelectControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, z.control_sm() * s);
    const float rad = z.control_radius(r.bottom - r.top, s);
    p.Fill(r, rad, FieldFill(t, Hover(hovered && !open), pressed));
    p.Stroke(r, rad, open ? c.text3 : c.line_strong);
    const float px = z.space_md() * s;
    if (selected >= 0 && selected < static_cast<int>(options.size()))
        p.Text(options[selected], TypeRole::Body, D2D1::RectF(r.left + px, r.top, r.right - px - z.icon() * s, r.bottom),
               c.text, {TextAlign::Leading, false, true});
    p.Chevron(r.right - px - z.glyph() * 0.4f * s, Cy(r), open, c.text2);
    if (focus_visible) p.Focus(r, rad);
}

void SelectControl::Press(D2D1_POINT_2F) {
    if (!on_open) {  // nothing to show it: just the state (the gallery)
        open = !open;
        return;
    }
    open = true;
    on_open(*this);
}

bool SelectControl::OnKeyDown(UINT vk) {
    if (!focused || (vk != VK_SPACE && vk != VK_RETURN && vk != VK_DOWN)) return false;
    Press({});
    return true;
}

// --- Popup --------------------------------------------------------------------------------------

namespace {
int g_shadow_renders = 0;
}  // namespace

int SoftShadow::RendersForTest() { return g_shadow_renders; }

void Popup::Opened() {
    appear_.Snap(false);
    shadow_.Reset();
    closed_ = false;
    visible = true;
}

// --- Menu popup ---------------------------------------------------------------------------------

void MenuPopup::Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& t, float s) {
    const Sizes& z = t.size;
    item_h_ = z.control_sm() * s;
    pad_ = z.space_xs() * s;
    const float margin = z.space_md() * s, gap = z.space_xs() * s;
    // Label weight: the current item draws in it, and it's the wider of the two.
    float text_w = 0.0f;
    for (const auto& o : options) text_w = (std::max)(text_w, t.MeasureText(t.Font(TypeRole::Label), o).width);
    float w = (std::max)(anchor.right - anchor.left, std::ceil(text_w) + 2 * pad_ + 2 * z.space_sm() * s);
    w = (std::min)(w, (view.right - view.left) - 2 * margin);
    float left = (std::min)(anchor.left, view.right - margin - w);
    left = (std::max)(left, view.left + margin);

    const float ideal = item_h_ * static_cast<float>(options.size()) + 2 * pad_;
    const float least = item_h_ + 2 * pad_;
    const float below = view.bottom - margin - anchor.bottom - gap;
    const float above = anchor.top - gap - (view.top + margin);
    float top, h;
    if (ideal <= below || below >= above) {  // down is the usual side, and the one that scrolls when neither fits
        h = (std::min)(ideal, (std::max)(least, below));
        top = anchor.bottom + gap;
    } else {
        h = (std::min)(ideal, (std::max)(least, above));
        top = anchor.top - gap - h;
    }
    bounds = D2D1::RectF(left, top, left + w, top + h);
    hovered_index = -1;
    // The current choice in the middle of the visible list, when it scrolls.
    scroll_y = selected < 0 ? 0.0f
                            : std::clamp(pad_ + (selected + 0.5f) * item_h_ - h * 0.5f, 0.0f, MaxScroll());
    Opened();
}

float MenuPopup::MaxScroll() const {
    return (std::max)(0.0f, item_h_ * static_cast<float>(options.size()) + 2 * pad_ - (bounds.bottom - bounds.top));
}

int MenuPopup::IndexAt(D2D1_POINT_2F pt) const {
    if (!PointIn(bounds, pt) || pt.y < bounds.top + pad_ || pt.y >= bounds.bottom - pad_ || item_h_ <= 0) return -1;
    const int i = static_cast<int>((pt.y - bounds.top - pad_ + scroll_y) / item_h_);
    return i >= 0 && i < static_cast<int>(options.size()) ? i : -1;
}

CursorKind MenuPopup::CursorAt(D2D1_POINT_2F pt) const {
    return visible && IndexAt(pt) >= 0 ? CursorKind::Hand : CursorKind::Arrow;
}

void SoftShadow::Render(ID2D1DeviceContext* dc, const Theme& t, float s, float w, float h, float rad) {
    using Microsoft::WRL::ComPtr;
    const float sd = (t.mode == ThemeMode::Light ? 6.0f : 8.0f) * s;  // the blur's standard deviation
    margin_ = std::ceil(sd * 3);
    for_ = D2D1::SizeF(w, h);
    radius_ = rad;
    mode_ = t.mode;
    bitmap_.Reset();

    ComPtr<ID2D1Image> target;
    dc->GetTarget(&target);
    D2D1_MATRIX_3X2_F xf;
    dc->GetTransform(&xf);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    ComPtr<ID2D1CommandList> shape;
    ComPtr<ID2D1SolidColorBrush> black;
    ComPtr<ID2D1Effect> blur;
    const auto size = D2D1::SizeU(static_cast<UINT32>(std::ceil(w + 2 * margin_)),
                                  static_cast<UINT32>(std::ceil(h + 2 * margin_)));
    FLOAT dpi_x = 96, dpi_y = 96;
    dc->GetDpi(&dpi_x, &dpi_y);
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                               dpi_x, dpi_y);
    if (SUCCEEDED(dc->CreateCommandList(&shape)) && SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black)) &&
        SUCCEEDED(dc->CreateEffect(CLSID_D2D1Shadow, &blur)) && SUCCEEDED(dc->CreateBitmap(size, nullptr, 0, props, &bitmap_))) {
        dc->SetTarget(shape.Get());
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(margin_, margin_, margin_ + w, margin_ + h), rad, rad),
                                 black.Get());
        shape->Close();
        blur->SetInput(0, shape.Get());
        blur->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sd);
        blur->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, t.mode == ThemeMode::Light ? 0.16f : 0.5f));
        dc->SetTarget(bitmap_.Get());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(blur.Get());
        ++g_shadow_renders;
    } else {
        bitmap_.Reset();  // no shadow beats a broken frame
    }
    dc->SetTarget(target.Get());
    dc->SetTransform(xf);
}

void SoftShadow::Draw(ID2D1DeviceContext* dc, const Theme& t, float s, const D2D1_RECT_F& r, float radius) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (!bitmap_ || for_.width != w || for_.height != h || radius_ != radius || mode_ != t.mode) Render(dc, t, s, w, h, radius);
    if (bitmap_) dc->DrawImage(bitmap_.Get(), D2D1::Point2F(r.left - margin_, r.top - margin_ + t.size.space_xs() * s));
}

void DrawTooltip(ID2D1DeviceContext* dc, const Theme& t, float s, const std::wstring& text, D2D1_POINT_2F anchor) {
    if (!dc || text.empty()) return;
    const Sizes& z = t.size;
    const TextOpts wrap{TextAlign::Leading, true, false, false};
    IDWriteTextFormat* f = t.Font(TypeRole::Caption, wrap);
    if (!f) return;
    const D2D1_SIZE_F m = t.MeasureText(f, text, 260 * s);
    const float px = z.space_sm() * s, py = z.space_xs() * s;
    const float w = std::ceil(m.width) + 2 * px, h = std::ceil(m.height) + 2 * py;
    const float bottom = anchor.y - z.space_xs() * s;
    const auto r = D2D1::RectF(anchor.x - w / 2, bottom - h, anchor.x + w / 2, bottom);
    const Paint p{dc, t, s};
    const float rad = z.radius_control() * s;
    p.Fill(r, rad, t.color.raised);
    p.Stroke(r, rad, t.color.line_strong);
    if (g_draw_probe) g_draw_probe->Occlude(dc, r);  // floats above the page
    p.Text(text, TypeRole::Caption, D2D1::RectF(r.left + px, r.top + py, r.right - px, r.bottom - py), t.color.text, wrap);
}

void Popup::BeginSurface(ID2D1DeviceContext* dc, const Theme& t, float s) {
    const Sizes& z = t.size;
    const float a = appear_.Track(true, kPopupFadeMs);
    fading_ = a < 1.0f;
    if (fading_)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), a),
                      nullptr);
    shadow_.Draw(dc, t, s, bounds, z.radius_card() * s);
    const Paint p{dc, t, s};
    p.Fill(bounds, z.radius_card() * s, t.color.raised);
    p.Stroke(bounds, z.radius_card() * s, t.color.line_strong);
    if (g_draw_probe) g_draw_probe->Occlude(dc, bounds);  // text under the popup can't be seen or clicked
}

void Popup::EndSurface(ID2D1DeviceContext* dc) {
    if (fading_) dc->PopLayer();
    fading_ = false;
}

void MenuPopup::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible || options.empty()) return;
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    BeginSurface(dc, t, s);
    const Paint p{dc, t, s};
    const float rad = z.radius_control() * s;

    // Clipped to the padded inside: a scrolled list has rows partly out of it.
    const auto inner = D2D1::RectF(bounds.left + pad_, bounds.top + pad_, bounds.right - pad_, bounds.bottom - pad_);
    dc->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_ALIASED);
    TrackItems(hovered_index);
    const float irad = (std::max)(0.0f, rad - pad_ * 0.5f);
    for (size_t i = 0; i < options.size(); ++i) {
        const float y = inner.top + item_h_ * static_cast<float>(i) - scroll_y;
        if (y + item_h_ < inner.top || y > inner.bottom) continue;
        const auto item = D2D1::RectF(inner.left, y, inner.right, y + item_h_);
        const bool sel = static_cast<int>(i) == selected;
        D2D1_COLOR_F ink = c.text;
        if (sel) {
            p.Fill(item, irad, c.accent_soft);
            ink = c.accent_text;
        }
        p.Fill(item, irad, c.HoverLayer(item_hover_.Amount(static_cast<int>(i))));
        if (sel) {  // a short accent bar sets the current item apart from a hovered one
            const float bh = z.icon() * s, bw = z.space_xxs() * s;
            p.Fill(D2D1::RectF(item.left, Cy(item) - bh * 0.5f, item.left + bw, Cy(item) + bh * 0.5f), bw * 0.5f, c.accent);
        }
        p.Text(options[i], sel ? TypeRole::Label : TypeRole::Body,
               D2D1::RectF(item.left + z.space_sm() * s, item.top, item.right - z.space_sm() * s, item.bottom), ink,
               {TextAlign::Leading, false, true});
    }
    dc->PopAxisAlignedClip();
    EndSurface(dc);
}

bool MenuPopup::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    hovered_index = IndexAt(pt);
    return was != hovered_index;
}

bool MenuPopup::OnMouseDown(D2D1_POINT_2F pt) {
    // A click in the padding or outside dismisses.
    const int i = IndexAt(pt);
    if (on_select) on_select(i);
    return true;
}

bool MenuPopup::OnKeyDown(UINT vk) {
    const int n = static_cast<int>(options.size());
    if (vk == VK_ESCAPE) {
        if (on_select) on_select(-1);
        return true;
    }
    const int from = hovered_index >= 0 ? hovered_index : selected;
    auto move_to = [&](int i) {
        hovered_index = std::clamp(i, 0, n - 1);
        const float view = bounds.bottom - bounds.top - 2 * pad_;
        const float top = hovered_index * item_h_, bottom = top + item_h_;
        if (top < scroll_y) scroll_y = top;
        else if (bottom > scroll_y + view) scroll_y = bottom - view;
    };
    if (n > 0 && vk == VK_UP) move_to(from < 0 ? n - 1 : from - 1);
    else if (n > 0 && vk == VK_DOWN) move_to(from < 0 ? 0 : from + 1);
    else if ((vk == VK_RETURN || vk == VK_SPACE) && on_select) on_select(from);
    return true;  // modal: no key reaches the page under it
}

void MenuPopup::OnMouseWheel(float delta) {
    scroll_y = std::clamp(scroll_y - delta * item_h_ * 1.25f, 0.0f, MaxScroll());
}

// --- Stepper ------------------------------------------------------------------------------------

std::wstring StepperControl::Format(double v) const {
    if (!min_label.empty() && v <= min) return min_label;
    wchar_t num[32];
    std::swprintf(num, 32, step < 1 ? L"%.1f" : L"%.0f", v);
    return unit.empty() ? std::wstring(num) : std::wstring(num) + L" " + unit;
}

int LevelMeter::LitBlocks() const {
    if (!enabled || !(db > kFloorDb)) return 0;
    const float at = (std::min)(1.0f, (db - kFloorDb) / -kFloorDb);
    return static_cast<int>(std::ceil(at * kBlocks - 0.001f));
}

D2D1_SIZE_F LevelMeter::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    return D2D1::SizeF(width_dip * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

void LevelMeter::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const float gap = t.size.space_xxs() * s, h = (t.size.space_md() + t.size.space_xxs()) * s;
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, h);
    const float pitch = (r.right - r.left + gap) / kBlocks;
    const int lit = LitBlocks();
    for (int i = 0; i < kBlocks; ++i) {
        // A block's colour is the zone its top edge reaches: yellow from -20 dB, red from -9 dB.
        const float top_db = kFloorDb * (1.0f - static_cast<float>(i + 1) / kBlocks);
        const D2D1_COLOR_F on = top_db > -9.0f ? c.danger : top_db > -20.0f ? c.warning : c.success;
        const float x = r.left + i * pitch;
        p.Fill(D2D1::RectF(x, r.top, x + pitch - gap, r.bottom), 1.0f * s, i < lit ? on : c.line);
    }
}

D2D1_SIZE_F StepperControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    float vw = 64;
    if (ctx.theme)
        for (double v : {min, max})  // not `value`: an out-of-range entry must not move layout
            vw = (std::max)(vw, std::ceil(ctx.theme->MeasureText(ctx.theme->Font(TypeRole::Body), Format(v)).width / ctx.dpi_scale));
    return D2D1::SizeF((z.control_sm() * 2 + vw + 2 * z.space_md()) * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

bool StepperControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_side;
    Widget::OnMouseMove(pt);
    const float h = bounds.bottom - bounds.top;
    hovered_side = !hovered ? 0 : pt.x < bounds.left + h ? -1 : pt.x > bounds.right - h ? 1 : 0;
    return was != hovered_side;
}

void StepperControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const float h = t.size.control_sm() * s, rad = t.size.control_radius(h, s);
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, h);
    p.Fill(r, rad, c.raised);
    const auto minus = D2D1::RectF(r.left, r.top, r.left + h, r.bottom);
    const auto plus = D2D1::RectF(r.right - h, r.top, r.right, r.bottom);
    const float inset = t.size.space_xxs() * s;
    TrackItems(hovered_side == 0 ? -1 : hovered_side + 1);  // sides -1/+1 as items 0/2
    auto side = [&](const D2D1_RECT_F& b, int which, bool at_limit) {
        const auto in = D2D1::RectF(b.left + inset, b.top + inset, b.right - inset, b.bottom - inset);
        if (!at_limit)
            p.Fill(in, (std::max)(0.0f, rad - inset),
                   pressed && pressed_side == which && hovered_side == which ? c.PressedLayer()
                                                                             : c.HoverLayer(item_hover_.Amount(which + 1)));
        const D2D1_COLOR_F ink = at_limit ? c.text_off : c.text2;
        const float cx = (b.left + b.right) * 0.5f, cy = Cy(b), a = t.size.glyph() * 0.5f * s;
        p.Line(cx - a, cy, cx + a, cy, ink);
        if (which > 0) p.Line(cx, cy - a, cx, cy + a, ink);
    };
    side(minus, -1, value <= min);
    side(plus, 1, value >= max);
    p.Bar(minus.right, r, c.line);
    p.Bar(plus.left, r, c.line);
    p.Text(Format(value), TypeRole::Body, D2D1::RectF(minus.right, r.top, plus.left, r.bottom), c.text, {TextAlign::Center});
    p.Stroke(r, rad, error ? c.danger : c.line_strong);
    if (focus_visible) p.Focus(r, rad);
}

void StepperControl::Press(D2D1_POINT_2F pt) {
    const float h = bounds.bottom - bounds.top;
    pressed_side = pt.x < bounds.left + h ? -1 : pt.x > bounds.right - h ? 1 : 0;
    StepBy(pressed_side);
}

void StepperControl::StepBy(int direction) {
    if (direction == 0) return;
    const double next = direction > 0 && first_step_above_min > min && value <= min
                            ? (std::min)(first_step_above_min, max)
                            : (std::clamp)(value + direction * step, min, max);
    if (next == value) return;
    value = next;
    if (on_change) on_change(value);
}

bool StepperControl::OnKeyDown(UINT vk) {
    if (!focused) return false;
    if (vk == VK_LEFT || vk == VK_DOWN) StepBy(-1);
    else if (vk == VK_RIGHT || vk == VK_UP) StepBy(+1);
    else return false;
    return true;
}

// --- Text field ---------------------------------------------------------------------------------

D2D1_SIZE_F TextFieldControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float extra = error_text.empty() ? 0.0f : z.space_xs() + LineHeight(TypeRole::Caption);
    return D2D1::SizeF(width_dip * ctx.dpi_scale, (z.control_sm() + extra) * ctx.dpi_scale);
}

void TextFieldControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const float h = t.size.control_sm() * s, rad = t.size.control_radius(h, s), px = t.size.space_md() * s;
    const auto r = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + h);
    const float hv = Hover(hovered && !focused);
    p.Fill(r, rad, focused ? c.card : FieldFill(t, hv, false));
    const bool err = !error_text.empty();
    p.Stroke(r, rad, err ? c.danger : Mix(c.line_strong, c.text3, hv), err ? 1.5f : 1.0f);
    if (focused && !err) p.Typing(r, rad);
    const TypeRole role = mono ? TypeRole::Mono : TypeRole::Body;
    const auto tr = D2D1::RectF(r.left + px, r.top, r.right - px, r.bottom);
    text_left_ = tr.left;
    if (text.empty()) p.Text(placeholder, TypeRole::Body, tr, c.text3, {TextAlign::Leading, false, true});
    else p.Text(text, role, tr, c.text, {TextAlign::Leading, false, true});
    if (focused) {
        const std::wstring before = text.substr(0, Caret());
        p.Bar((std::min)(tr.right, tr.left + (before.empty() ? 0.0f : p.TextW(before, role)) + 1), r, c.text);
    }
    if (err) p.Note(error_text, r.left, r.bottom + t.size.space_xs() * s, c.danger);
    if (focus_visible) p.Focus(r, rad);
}

void TextFieldControl::Press(D2D1_POINT_2F pt) {
    (void)pt;
    focused = true;
    caret_ = std::wstring::npos;  // a click puts the caret at the end: fields here hold a few characters
}

bool TextFieldControl::OnKeyDown(UINT vk) {
    if (!focused) return false;
    size_t c = Caret();
    switch (vk) {
        case VK_LEFT:  if (c > 0) --c; break;
        case VK_RIGHT: if (c < text.size()) ++c; break;
        case VK_HOME:  c = 0; break;
        case VK_END:   c = text.size(); break;
        case VK_BACK:
            if (c == 0) return true;
            text.erase(--c, 1);
            caret_ = c;
            if (on_edit) on_edit(text);
            return true;
        case VK_DELETE:
            if (c >= text.size()) return true;
            text.erase(c, 1);
            if (on_edit) on_edit(text);
            return true;
        case VK_RETURN:
            if (on_commit) on_commit(text);
            return true;
        default: return false;
    }
    caret_ = c;
    return true;
}

bool TextFieldControl::OnChar(wchar_t ch) {
    if (!focused || ch < 0x20 || ch == 0x7f) return false;  // control characters come as keys
    if (accept_char && !accept_char(ch)) return true;
    if (max_length && text.size() >= max_length) return true;
    const size_t c = Caret();
    text.insert(c, 1, ch);
    caret_ = c + 1;
    if (on_edit) on_edit(text);
    return true;
}

// --- Template field -----------------------------------------------------------------------------

std::vector<TemplateFieldControl::Chip> TemplateFieldControl::Chips() const { return ChipsIn(text); }

std::vector<TemplateFieldControl::Chip> TemplateFieldControl::ChipsIn(const std::wstring& s) const {
    std::vector<Chip> out;
    for (size_t i = 0; i < s.size();) {
        int hit = -1;
        if (s[i] == L'{') {
            for (size_t t = 0; t < tokens.size(); ++t)
                if (s.compare(i, tokens[t].key.size(), tokens[t].key) == 0) { hit = static_cast<int>(t); break; }
        }
        if (hit < 0) { ++i; continue; }
        out.push_back({i, tokens[hit].key.size(), hit});
        i += tokens[hit].key.size();
    }
    return out;
}

void TemplateFieldControl::SetCaret(size_t c) {
    c = (std::min)(c, text.size());
    for (const Chip& ch : Chips())
        if (c > ch.pos && c < ch.pos + ch.len) c = ch.pos + ch.len;
    caret_ = c;
}

void TemplateFieldControl::Changed() {
    if (on_change) on_change(text);
}

void TemplateFieldControl::InsertToken(int token) {
    if (token < 0 || token >= static_cast<int>(tokens.size())) return;
    SetCaret(caret_);
    text.insert(caret_, tokens[token].key);
    caret_ += tokens[token].key.size();
    selected_chip = -1;
    Changed();
}

std::wstring TemplateFieldControl::Preview() const {
    const std::wstring& src = text.empty() ? fallback : text;
    std::wstring out;
    size_t at = 0;
    for (const Chip& ch : ChipsIn(src)) {
        out += src.substr(at, ch.pos - at) + tokens[ch.token].sample;
        at = ch.pos + ch.len;
    }
    out += src.substr(at);
    // A typed suffix isn't doubled (save.cpp's WithContainerExtension).
    if (!suffix.empty() && out.size() >= suffix.size() &&
        _wcsicmp(out.c_str() + out.size() - suffix.size(), suffix.c_str()) == 0)
        out.resize(out.size() - suffix.size());
    return out + suffix;
}

D2D1_SIZE_F TemplateFieldControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float h = z.control() + z.space_sm() + z.control_xs() + z.space_xs() + LineHeight(TypeRole::Caption);
    return D2D1::SizeF(width_dip * ctx.dpi_scale, h * ctx.dpi_scale);
}

void TemplateFieldControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float h = z.control() * s, rad = z.control_radius(h, s), px = z.space_md() * s;
    field_ = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + h);
    const D2D1_RECT_F& r = field_;
    p.Fill(r, rad, focused ? c.card : FieldFill(t, Hover(hovered && hovered_chip < 0 && hovered_insert < 0), false));
    TrackItems(hovered_chip);
    insert_hover_.Track(enabled ? hovered_insert : -1, kFadeMs);
    p.Stroke(r, rad, c.line_strong);
    if (focused) p.Typing(r, rad);

    // Walk the text: plain runs as Body text, known placeholders as chips.
    const std::vector<Chip> chips = Chips();
    chip_boxes_.assign(chips.size(), {});
    caret_x_.assign(text.size() + 1, std::nanf(""));
    const float chip_h = h - z.space_md() * s, chip_px = z.space_sm() * s, chip_gap = z.space_xxs() * s;
    float x = r.left + px;
    size_t at = 0;
    const float suffix_w = p.TextW(suffix, TypeRole::Body);
    const float limit = r.right - px - suffix_w;
    dc->PushAxisAlignedClip(D2D1::RectF(r.left + 1, r.top, limit, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    auto run = [&](size_t from, size_t to) {
        for (size_t i = from; i <= to; ++i)
            caret_x_[i] = x + (i == from ? 0.0f : p.TextW(text.substr(from, i - from), TypeRole::Body));
        if (to > from) {
            const std::wstring seg = text.substr(from, to - from);
            const float w = p.TextW(seg, TypeRole::Body);
            p.Text(seg, TypeRole::Body, D2D1::RectF(x, r.top, x + std::ceil(w) + 1, r.bottom), c.text);
            x += w;
        }
    };
    for (size_t k = 0; k < chips.size(); ++k) {
        run(at, chips[k].pos);
        const std::wstring& label = tokens[chips[k].token].label;
        x += chip_gap;
        const float w = p.TextW(label, TypeRole::Label) + 2 * chip_px;
        const auto box = D2D1::RectF(x, Cy(r) - chip_h / 2, x + w, Cy(r) + chip_h / 2);
        chip_boxes_[k] = box;
        const bool sel = static_cast<int>(k) == selected_chip, hov = static_cast<int>(k) == hovered_chip;
        // A soft pill: tint only, no outline, regular weight.
        const float pr = (box.bottom - box.top) / 2;
        p.Fill(box, pr, c.accent_soft);
        p.Fill(box, pr, pressed && hov ? c.PressedLayer() : c.HoverLayer(item_hover_.Amount(static_cast<int>(k))));
        if (sel) p.Stroke(box, pr, c.accent_text, 1.5f);
        p.Text(label, TypeRole::Body, box, c.accent_text, {TextAlign::Center});
        x = box.right + chip_gap;
        at = chips[k].pos + chips[k].len;
        caret_x_[at] = x;
    }
    run(at, text.size());
    if (focused && selected_chip < 0 && caret_ < caret_x_.size() && !std::isnan(caret_x_[caret_])) {
        p.Bar(caret_x_[caret_], r, c.text);
    }
    dc->PopAxisAlignedClip();
    p.Text(suffix, TypeRole::Body, D2D1::RectF(limit, r.top, r.right - px, r.bottom), c.text3, {TextAlign::Trailing});
    if (focus_visible) p.Focus(r, rad);

    // Insert row: "+ Game" chips, Reset at the far end, then the preview.
    const float iy = r.bottom + z.space_sm() * s, ih = z.control_xs() * s;
    const size_t n_insert = tokens.size() + (on_reset ? 1 : 0);
    insert_boxes_.assign(n_insert, {});
    float ix = r.left;
    for (size_t i = 0; i < n_insert; ++i) {
        const bool reset = i == tokens.size();
        const std::wstring label = reset ? L"Reset" : L"+ " + tokens[i].label;
        const float w = p.TextW(label, TypeRole::Caption) + 2 * chip_px;
        if (reset) ix = r.right - w;
        const auto box = D2D1::RectF(ix, iy, ix + w, iy + ih);
        insert_boxes_[i] = box;
        const bool hov = static_cast<int>(i) == hovered_insert;
        const float hv = insert_hover_.Amount(static_cast<int>(i));
        p.Fill(box, ih / 2, pressed && hov ? c.PressedLayer() : c.HoverLayer(hv));
        p.Text(label, TypeRole::Caption, box, Mix(c.text2, c.text, hv), {TextAlign::Center});
        ix = box.right + z.space_xs() * s;
    }
    p.Text(Preview(), TypeRole::Caption, D2D1::RectF(r.left, iy + ih + z.space_xs() * s, bounds.right, bounds.bottom),
           c.text3, {TextAlign::Leading, false, true});
}

bool TemplateFieldControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was_c = hovered_chip, was_i = hovered_insert;
    const bool was = hovered;
    Widget::OnMouseMove(pt);
    hovered_chip = hovered_insert = -1;
    for (size_t k = 0; k < chip_boxes_.size(); ++k) if (PointIn(chip_boxes_[k], pt)) hovered_chip = static_cast<int>(k);
    for (size_t k = 0; k < insert_boxes_.size(); ++k) if (PointIn(insert_boxes_[k], pt)) hovered_insert = static_cast<int>(k);
    return was != hovered || was_c != hovered_chip || was_i != hovered_insert;
}

CursorKind TemplateFieldControl::CursorAt(D2D1_POINT_2F pt) const {
    if (!HitTest(pt)) return CursorKind::Arrow;
    for (const auto& b : chip_boxes_) if (PointIn(b, pt)) return CursorKind::Hand;
    for (const auto& b : insert_boxes_) if (PointIn(b, pt)) return CursorKind::Hand;
    return PointIn(field_, pt) ? CursorKind::IBeam : CursorKind::Arrow;
}

void TemplateFieldControl::Press(D2D1_POINT_2F pt) {
    for (size_t k = 0; k < insert_boxes_.size(); ++k) {
        if (!PointIn(insert_boxes_[k], pt)) continue;
        if (k == tokens.size()) { if (on_reset) on_reset(); }
        else InsertToken(static_cast<int>(k));
        return;
    }
    selected_chip = -1;
    const std::vector<Chip> chips = Chips();
    for (size_t k = 0; k < chip_boxes_.size() && k < chips.size(); ++k)
        if (PointIn(chip_boxes_[k], pt)) {
            selected_chip = static_cast<int>(k);
            caret_ = chips[k].pos + chips[k].len;
            return;
        }
    if (!PointIn(field_, pt)) return;
    // Nearest caret position.
    size_t best = text.size();
    float best_d = 1e9f;
    for (size_t i = 0; i < caret_x_.size(); ++i)
        if (!std::isnan(caret_x_[i]) && std::abs(caret_x_[i] - pt.x) < best_d) { best_d = std::abs(caret_x_[i] - pt.x); best = i; }
    SetCaret(best);
}

bool TemplateFieldControl::OnKeyDown(UINT vk) {
    if (!focused) return false;
    const std::vector<Chip> chips = Chips();
    auto chip_ending_at = [&](size_t pos) -> const Chip* {
        for (const Chip& ch : chips) if (ch.pos + ch.len == pos) return &ch;
        return nullptr;
    };
    auto chip_starting_at = [&](size_t pos) -> const Chip* {
        for (const Chip& ch : chips) if (ch.pos == pos) return &ch;
        return nullptr;
    };
    if (selected_chip >= 0 && selected_chip < static_cast<int>(chips.size()) && (vk == VK_BACK || vk == VK_DELETE)) {
        const Chip ch = chips[selected_chip];
        text.erase(ch.pos, ch.len);
        caret_ = ch.pos;
        selected_chip = -1;
        Changed();
        return true;
    }
    selected_chip = -1;
    switch (vk) {
        case VK_LEFT:
            if (caret_ > 0) { const Chip* ch = chip_ending_at(caret_); caret_ = ch ? ch->pos : caret_ - 1; }
            return true;
        case VK_RIGHT:
            if (caret_ < text.size()) { const Chip* ch = chip_starting_at(caret_); caret_ = ch ? ch->pos + ch->len : caret_ + 1; }
            return true;
        case VK_HOME: caret_ = 0; return true;
        case VK_END: caret_ = text.size(); return true;
        case VK_BACK:
            if (caret_ == 0) return true;
            if (const Chip* ch = chip_ending_at(caret_)) { text.erase(ch->pos, ch->len); caret_ = ch->pos; }
            else text.erase(--caret_, 1);
            Changed();
            return true;
        case VK_DELETE:
            if (caret_ >= text.size()) return true;
            if (const Chip* ch = chip_starting_at(caret_)) text.erase(ch->pos, ch->len);
            else text.erase(caret_, 1);
            Changed();
            return true;
    }
    return false;
}

bool TemplateFieldControl::OnChar(wchar_t ch) {
    if (!focused || ch < 0x20 || ch == 0x7f) return false;
    if (accept_char && !accept_char(ch)) return true;  // refused, but consumed
    SetCaret(caret_);
    text.insert(caret_, 1, ch);
    ++caret_;  // closing a "}" may have just made a chip; the caret already sits after it
    selected_chip = -1;
    Changed();
    return true;
}

// --- Path ---------------------------------------------------------------------------------------

D2D1_SIZE_F PathControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    return D2D1::SizeF(width_dip * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

void PathControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float h = z.control_sm() * s, rad = z.control_radius(h, s), px = z.space_md() * s;
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, h);
    p.Fill(r, rad, c.raised);
    p.Stroke(r, rad, c.line_strong);
    const std::wstring browse = L"Browse";
    const float bw = p.TextW(browse, TypeRole::Label) + 2 * px;
    const float inset = z.space_xxs() * s;
    const auto b = D2D1::RectF(r.right - bw - inset, r.top + inset, r.right - inset, r.bottom - inset);
    p.Fill(b, (std::max)(0.0f, rad - inset), pressed && browse_hovered ? c.PressedLayer() : c.HoverLayer(Hover(browse_hovered)));
    p.Bar(b.left, r, c.line);
    p.Text(browse, TypeRole::Label, b, c.text, {TextAlign::Center});
    ComPtr<ID2D1SolidColorBrush> icon = p.B(c.text2);
    const float is = z.icon() * s;
    DrawIcon(dc, t, IconType::Folder, r.left + px + is * 0.5f, Cy(r), is, icon.Get());
    const auto text_box = D2D1::RectF(r.left + px + is + z.space_sm() * s, r.top, b.left - z.space_sm() * s, r.bottom);
    // Too long: the drive, "…", then as many of the last folders as fit; the folder a path ends in is
    // the part that tells two apart. The whole path is the tooltip.
    std::wstring shown = path;
    const float room = text_box.right - text_box.left;
    if (p.TextW(path, TypeRole::Mono) > room) {
        const size_t head_end = path.find_first_of(L"\\/");
        if (head_end != std::wstring::npos) {
            const std::wstring head = path.substr(0, head_end + 1) + L"\x2026";
            for (size_t cut = path.find_first_of(L"\\/", head_end + 1); cut != std::wstring::npos;
                 cut = path.find_first_of(L"\\/", cut + 1)) {
                const std::wstring candidate = head + path.substr(cut);
                if (p.TextW(candidate, TypeRole::Mono) <= room) {
                    shown = candidate;
                    break;
                }
            }
        }
    }
    tooltip = shown == path ? std::wstring() : path;
    p.Text(shown, TypeRole::Mono, text_box, c.text, {TextAlign::Leading, false, true});
    if (focus_visible) p.Focus(r, rad);
}

void PathControl::Press(D2D1_POINT_2F) {
    if (on_browse) on_browse();
}

// --- Key capture --------------------------------------------------------------------------------

namespace {

UINT (*g_held_mods_source)() = nullptr;

UINT ModsHeldNow() {
    if (g_held_mods_source) return g_held_mods_source();
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
    return mods;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

}  // namespace

void SetHeldModsSourceForTest(UINT (*source)()) { g_held_mods_source = source; }

void KeyCaptureControl::BeginRecording() {
    if (recording_) return;
    recording_ = true;
    held_mods_ = 0;
    error_.clear();
    if (on_recording_changed) on_recording_changed(true);
}

void KeyCaptureControl::EndRecording() {
    if (!recording_) return;
    recording_ = false;
    held_mods_ = 0;
    error_.clear();
    if (on_recording_changed) on_recording_changed(false);
}

void KeyCaptureControl::CancelRecording() { EndRecording(); }

void KeyCaptureControl::SetRecordingStateForSnapshot(UINT held_mods, const std::wstring& error) {
    recording_ = true;
    held_mods_ = held_mods;
    error_ = error;
}

void KeyCaptureControl::OnCaptureKey(UINT vk, bool down) {
    if (!recording_) return;
    held_mods_ = ModsHeldNow();
    if (!down || ombra::IsModifierVk(vk)) return;
    if (vk == VK_ESCAPE && held_mods_ == 0) {
        EndRecording();
        return;
    }
    const ombra::HotkeyCombo combo{held_mods_, vk};
    if (ombra::HotkeyKeyName(vk).empty()) {
        error_ = L"" OMBRA_PRODUCT_NAME " can't bind that key. Use a letter, digit, F-key or navigation key.";
        return;
    }
    const std::wstring candidate = Widen(ombra::FormatHotkeySpec(combo));
    if (const ombra::HotkeyProblem p = ombra::ValidateHotkeyCombo(combo); p != ombra::HotkeyProblem::None) {
        error_ = candidate + L": " + Widen(ombra::HotkeyProblemText(p));
        return;
    }
    if (validate) {
        if (std::wstring why = validate(candidate); !why.empty()) {
            error_ = candidate + L": " + why;
            return;
        }
    }
    EndRecording();
    if (candidate != spec && on_commit) on_commit(candidate);
}

bool KeyCaptureControl::OnMouseDown(D2D1_POINT_2F pt) {
    // A click anywhere else ends recording without changing anything.
    if (!HitTest(pt) && recording_) EndRecording();
    return Control::OnMouseDown(pt);
}

void KeyCaptureControl::Press(D2D1_POINT_2F) { BeginRecording(); }

D2D1_SIZE_F KeyCaptureControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    return D2D1::SizeF(width_dip * ctx.dpi_scale, z.control_sm() * ctx.dpi_scale);
}

// Raised keys at the trailing edge, no field and no "+".
// Recording adds an accent key slot after the held modifiers.
void KeyCaptureControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float h = z.control_sm() * s, rad = z.control_radius(h, s), px = z.space_sm() * s;
    const D2D1_RECT_F r = RowBox(bounds, bounds.left, bounds.right - bounds.left, h);
    const bool err = recording_ && !error_.empty();

    std::vector<std::wstring> caps;
    if (recording_) {
        if (held_mods_ & MOD_CONTROL) caps.push_back(L"Ctrl");
        if (held_mods_ & MOD_ALT)     caps.push_back(L"Alt");
        if (held_mods_ & MOD_SHIFT)   caps.push_back(L"Shift");
        if (held_mods_ & MOD_WIN)     caps.push_back(L"Win");
    } else {
        caps = SplitKeys(spec);
    }
    const float kr = z.key_radius(h, s), gap = z.space_sm() * s;
    auto cap_w = [&](const std::wstring& k) { return p.KeyW(k, h); };
    float total = 0;
    for (size_t i = 0; i < caps.size(); ++i) total += cap_w(caps[i]) + (i ? gap : 0.0f);
    if (recording_) total += (caps.empty() ? 0.0f : gap) + h;
    float x = r.right - px - total;

    if (!caps.empty() && !recording_) {
        // Hover: a layer behind the keys only, so the row still reads as text and keys.
        const float g = z.space_xs() * s;
        p.Fill(D2D1::RectF(x - g, r.top - g, x + total + g, r.bottom + g), rad, pressed ? c.PressedLayer() : c.HoverLayer(Hover()));
    }
    auto draw_cap = [&](const std::wstring& k, float w, bool waiting) {
        const auto cap = D2D1::RectF(x, r.top, x + w, r.bottom);
        if (waiting) {
            p.Fill(cap, kr, err ? Faded(c.danger, 0.14f) : c.accent_soft);
            p.Stroke(cap, kr, err ? c.danger : c.accent_line, err ? 1.5f : 1.0f);
            p.Text(k, TypeRole::Label, cap, err ? c.danger : c.accent_text, {TextAlign::Center});
        } else {
            p.Key(k, cap);
        }
        x = cap.right + gap;
    };
    for (const auto& k : caps) draw_cap(k, cap_w(k), false);
    if (recording_) draw_cap(L"\x2026", h, true);
    else if (caps.empty())
        p.Text(L"Not set", TypeRole::Body, D2D1::RectF(r.left, r.top, r.right - px, r.bottom), c.text3, {TextAlign::Trailing});
    if (focus_visible) p.Focus(r, rad);
}

// --- Colour picker ------------------------------------------------------------------------------

D2D1_SIZE_F ColorPickerControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float n = static_cast<float>(presets.size() + 1);
    const float dividers = lead > 0 ? 2.0f : 1.0f;
    const float hex_w = ctx.theme ? ctx.theme->MeasureText(ctx.theme->Font(TypeRole::Mono), L"#000000").width / ctx.dpi_scale : 64;
    // A gap after each swatch, and one more after each divider.
    const float w = z.space_xs() + n * z.control_xs() + (n + dividers) * z.space_sm() + std::ceil(hex_w);
    const float h = ctx.theme ? HeightDip(*ctx.theme) : z.control_xs() + 2 * z.space_xs();
    return D2D1::SizeF(w * ctx.dpi_scale, h * ctx.dpi_scale);
}

bool ColorPickerControl::ShowsNote(const Theme& t) const {
    const int n = static_cast<int>(presets.size());
    if (selected < 0 || selected > n) return false;
    const uint32_t chosen = selected == n ? custom : presets[selected];
    return NearStatusHue(RgbFromHex(chosen), Rgb{t.color.danger.r, t.color.danger.g, t.color.danger.b});
}

float ColorPickerControl::HeightDip(const Theme& t) const {
    const Sizes& z = t.size;
    const float swatches = z.control_xs() + 2 * z.space_xs();
    return ShowsNote(t) ? swatches + z.space_xxs() + LineHeight(TypeRole::Caption) : swatches;
}

float ColorPickerControl::SlotLeft(int i, float s) const {
    const float d = swatch_dip_ * s, gap = gap_dip_ * s;
    float x = bounds.left + pad_dip_ * s + static_cast<float>(i) * (d + gap);
    if (lead > 0 && i >= lead) x += gap;                      // past the divider after the lead
    if (i >= static_cast<int>(presets.size())) x += gap;      // past the divider before custom
    return x;
}

int ColorPickerControl::IndexAt(D2D1_POINT_2F pt, float s) const {
    if (!HitTest(pt)) return -1;
    const float d = swatch_dip_ * s, gap = gap_dip_ * s;
    for (int i = 0; i <= static_cast<int>(presets.size()); ++i) {
        const float x = SlotLeft(i, s);
        if (pt.x >= x - gap / 2 && pt.x < x + d + gap / 2) return i;
    }
    return -1;
}

void ColorPickerControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    SetDims(z, s);
    const float d = swatch_dip_ * s, gap = gap_dip_ * s, pad = pad_dip_ * s, cy = bounds.top + pad + d / 2;
    const float ring = d / 2 + z.space_xxs() * s + 1 * s;  // a 2 DIP ring with a gap outside the swatch
    const int n = static_cast<int>(presets.size()) + 1;
    auto divider = [&](float x) { p.Line(x, cy - d / 2 + pad, x, cy + d / 2 - pad, c.line_strong, 1.0f); };
    TrackItems(hovered_index);
    for (int i = 0; i < n; ++i) {
        const bool custom_slot = i == n - 1;
        const uint32_t hex = custom_slot ? custom : presets[i];
        const Rgb rgb = RgbFromHex(hex);
        const auto col = D2D1::ColorF(rgb.r, rgb.g, rgb.b);
        const float x = SlotLeft(i, s), cx = x + d / 2;
        const bool sel = i == selected;
        if (!sel) p.Ring(cx, cy, ring, Faded(c.line_strong, item_hover_.Amount(i)), 2.0f);
        p.Circle(cx, cy, d / 2, col);
        if (i < lead && lead_backdrop) {  // the left half: the colour this one comes from
            const Rgb b = RgbFromHex(*lead_backdrop);
            dc->PushAxisAlignedClip(D2D1::RectF(cx - d / 2 - 1, cy - d / 2 - 1, cx, cy + d / 2 + 1), D2D1_ANTIALIAS_MODE_ALIASED);
            p.Circle(cx, cy, d / 2, D2D1::ColorF(b.r, b.g, b.b));
            dc->PopAxisAlignedClip();
        }
        p.Ring(cx, cy, d / 2 - 0.5f * s, D2D1::ColorF(c.text.r, c.text.g, c.text.b, 0.12f), 1.0f);  // edge on near-panel colours
        if (custom_slot && !sel) {  // the custom slot reads as "make your own"
            const auto ink = PrefersWhiteText(rgb) ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(kOnAccentDark);
            const float a = z.glyph() * 0.4f * s;
            p.Line(cx - a, cy, cx + a, cy, ink);
            p.Line(cx, cy - a, cx, cy + a, ink);
        }
        if (sel) p.Ring(cx, cy, ring, c.text, 2.0f);
        if (i + 1 == lead && lead < n - 1) divider(x + d + gap);
        if (i + 1 == n - 1) divider(x + d + gap);
    }
    const float x = SlotLeft(n - 1, s) + d + gap;
    wchar_t hexs[16];
    std::swprintf(hexs, 16, L"#%06x", custom);
    p.Text(hexs, TypeRole::Mono, D2D1::RectF(x, cy - d / 2, bounds.right, cy + d / 2),
           selected == static_cast<int>(presets.size()) ? c.text : c.text3);
    if (ShowsNote(t))
        p.Note(L"Close to the red " OMBRA_PRODUCT_NAME " uses for errors", bounds.left, cy + d / 2 + pad + z.space_xxs() * s, c.warning);
    if (focus_visible) p.Focus(D2D1::RectF(bounds.left, cy - d / 2 - pad, x - gap, cy + d / 2 + pad), d);
}

void ColorPickerControl::SetDims(const Sizes& z, float s) {
    last_s_ = s;
    gap_dip_ = z.space_sm();
    swatch_dip_ = z.control_xs();
    pad_dip_ = z.space_xs();
}

void ColorPickerControl::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    Widget::Arrange(rect, ctx);
    SetDims(ctx.theme ? ctx.theme->size : Sizes{}, ctx.dpi_scale);
}

bool ColorPickerControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = IndexAt(pt, last_s_);
    return was != hovered_index;
}

void ColorPickerControl::Press(D2D1_POINT_2F pt) {
    const int i = IndexAt(pt, last_s_);
    if (i == static_cast<int>(presets.size()) && on_edit_custom) {  // chosen or not: it's where you edit it
        on_edit_custom(CustomSwatchRect());
        return;
    }
    if (i < 0 || i == selected) return;
    selected = i;
    if (on_change) on_change(i);
}

void SetAccentSwatches(ColorPickerControl& cp, const Theme& theme) {
    cp.presets.clear();
    for (AccentId id : kAccentSwatches)
        cp.presets.push_back(id == AccentId::Theme ? theme.ThemeAccentRgb() : AccentPickedRgb(id, theme.mode));
    cp.lead = 1;
    const D2D1_COLOR_F& bg = theme.color.panel;
    auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    cp.lead_backdrop = byte(bg.r) << 16 | byte(bg.g) << 8 | byte(bg.b);
}

D2D1_RECT_F ColorPickerControl::CustomSwatchRect() const {
    const float s = last_s_, d = swatch_dip_ * s, pad = pad_dip_ * s;
    const float x = SlotLeft(static_cast<int>(presets.size()), s);
    return D2D1::RectF(x, bounds.top + pad, x + d, bounds.top + pad + d);
}

// --- Theme tiles --------------------------------------------------------------------------------

namespace {
constexpr float kThemeTileMinDip = 88.0f, kThemeTileHDip = 52.0f;
constexpr int kThemeTilesPerLine = 7;  // fourteen tiles: two even lines
constexpr float kThemeCompactAspect = 1.5f;  // a compact chip's width over its height
std::wstring HexText(uint32_t v);  // with the colour editor's helpers, below
D2D1_COLOR_F D2d(Rgb c);
}  // namespace

const wchar_t* ThemePickerControl::FieldName(int i) {
    static const wchar_t* const names[kFields] = {L"Background", L"Cards", L"Menus", L"Lines", L"Text", L"Muted text"};
    return i >= 0 && i < kFields ? names[i] : L"";
}

uint32_t& ThemePickerControl::FieldOf(ombra::UiPaletteColors& c, int i) {
    uint32_t* const f[kFields] = {&c.background, &c.cards, &c.menus, &c.lines, &c.text, &c.muted};
    return *f[std::clamp(i, 0, kFields - 1)];
}

bool ThemePickerControl::ShowsFields() const {
    return selected >= 0 && selected < static_cast<int>(tiles.size()) && !compact && tiles[selected].kind == Kind::User;
}

float ThemePickerControl::Place(float w, const Sizes& z, float s, std::vector<D2D1_RECT_F>* tr, std::vector<D2D1_RECT_F>* fr) const {
    const float gap = z.space_sm() * s, ring = z.space_xxs() * s + 2 * s;  // room for the chosen ring
    const float inner = (std::max)(0.0f, w - 2 * ring);
    const int n = static_cast<int>(tiles.size());
    if (compact) {  // one line of small chips, no names, no fields
        const float th = z.control_xs() * s, tw = std::round(th * kThemeCompactAspect);
        if (tr) tr->clear();
        if (fr) fr->clear();
        for (int i = 0; i < n; ++i) {
            const float x = ring + i * (tw + gap);
            if (tr) tr->push_back(D2D1::RectF(x, ring, x + tw, ring + th));
        }
        return th + 2 * ring;
    }
    const int fit = static_cast<int>((inner + gap) / (z.S(kThemeTileMinDip) * s + gap));
    const int cols = std::clamp(fit, 1, kThemeTilesPerLine);
    const float tw = (inner - gap * (cols - 1)) / cols, th = z.S(kThemeTileHDip) * s;
    const float line = th + (z.space_xs() + LineHeight(TypeRole::Caption)) * s;  // the tile, then its name
    if (tr) tr->clear();
    for (int i = 0; i < n; ++i) {
        const float x = ring + (i % cols) * (tw + gap), y = ring + (i / cols) * (line + gap);
        if (tr) tr->push_back(D2D1::RectF(x, y, x + tw, y + th));
    }
    const int lines = (n + cols - 1) / cols;
    float h = 2 * ring + lines * line + (lines > 1 ? (lines - 1) * gap : 0.0f);
    if (fr) fr->clear();
    if (!ShowsFields()) return h;
    h += z.space_sm() * s;
    const float fw = (w - 2 * gap) / 3, fh = z.control_sm() * s;
    for (int i = 0; i < kFields; ++i) {
        const float x = (i % 3) * (fw + gap), y = h + (i / 3) * (fh + z.space_xxs() * s);
        if (fr) fr->push_back(D2D1::RectF(x, y, x + fw, y + fh));
    }
    return h + 2 * fh + z.space_xxs() * s;
}

D2D1_SIZE_F ThemePickerControl::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float s = ctx.dpi_scale;
    if (compact) {
        const float gap = z.space_sm() * s, ring = z.space_xxs() * s + 2 * s, th = z.control_xs() * s;
        const float n = static_cast<float>(tiles.size());
        const float w = 2 * ring + n * std::round(th * kThemeCompactAspect) + (n > 0 ? (n - 1) * gap : 0.0f);
        return D2D1::SizeF(w, Place(w, z, s, nullptr, nullptr));
    }
    const float w = c.BoundedWidth() ? c.max_w : (kThemeTilesPerLine * (z.S(kThemeTileMinDip) + z.space_sm()) + z.space_xs()) * s;
    return D2D1::SizeF(w, Place(w, z, s, nullptr, nullptr));
}

void ThemePickerControl::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    Widget::Arrange(rect, ctx);
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    Place(rect.right - rect.left, z, ctx.dpi_scale, &tile_rects_, &field_rects_);
    name_band_ = compact ? 0.0f : (z.space_xs() + LineHeight(TypeRole::Caption)) * ctx.dpi_scale;
    for (auto* v : {&tile_rects_, &field_rects_})
        for (D2D1_RECT_F& r : *v) r = D2D1::RectF(r.left + rect.left, r.top + rect.top, r.right + rect.left, r.bottom + rect.top);
}

D2D1_RECT_F ThemePickerControl::FieldRect(int field) const {
    return field >= 0 && field < static_cast<int>(field_rects_.size()) ? field_rects_[field] : D2D1_RECT_F{};
}

int ThemePickerControl::IndexAt(D2D1_POINT_2F pt) const {
    if (!HitTest(pt)) return -1;
    for (size_t i = 0; i < tile_rects_.size(); ++i) {  // the tile or its name under it
        D2D1_RECT_F r = tile_rects_[i];
        r.bottom += name_band_;
        if (PointIn(r, pt)) return static_cast<int>(i);
    }
    for (size_t i = 0; i < field_rects_.size(); ++i)
        if (PointIn(field_rects_[i], pt)) return static_cast<int>(tile_rects_.size() + i);
    return -1;
}

void ThemePickerControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float rad = z.radius_control() * s, ring_gap = z.space_xxs() * s + 1 * s;
    TrackItems(hovered_index);
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    auto col = [](uint32_t hex) { return D2d(RgbFromHex(hex)); };
    // A miniature of a palette in `r`: its background, a card from the inner corner, two lines of text.
    auto mini = [&](const D2D1_RECT_F& r, const ombra::UiPaletteColors& pc) {
        const float w = r.right - r.left, h = r.bottom - r.top, bar = 3 * s;
        const D2D1_RECT_F card = D2D1::RectF(r.left + w * 0.14f, r.top + h * 0.22f, r.right, r.bottom);
        p.Fill(card, 0, col(pc.cards));
        const float tx = card.left + w * 0.1f, ty = card.top + h * 0.2f;
        p.Fill(D2D1::RectF(tx, ty, tx + (std::min)(w * 0.42f, 40 * s), ty + bar), bar / 2, col(pc.text));
        p.Fill(D2D1::RectF(tx, ty + 2.6f * bar, tx + (std::min)(w * 0.28f, 28 * s), ty + 3.6f * bar), bar / 2, col(pc.muted));
    };
    for (size_t i = 0; i < tiles.size() && i < tile_rects_.size(); ++i) {
        const Tile& tile = tiles[i];
        const D2D1_RECT_F& r = tile_rects_[i];
        const bool sel = static_cast<int>(i) == selected;
        const float hover = item_hover_.Amount(static_cast<int>(i));
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, rad, rad), &clip);
        if (clip) dc->PushLayer(D2D1::LayerParameters1(r, clip.Get()), nullptr);
        if (tile.kind == Kind::System) {
            const ombra::UiPalette& light = ombra::kUiPalettes[1];
            const ombra::UiPalette& dark = ombra::kUiPalettes[0];
            const float mid = (r.left + r.right) / 2;
            const D2D1_RECT_F lh = D2D1::RectF(r.left, r.top, mid, r.bottom), dh = D2D1::RectF(mid, r.top, r.right, r.bottom);
            p.Fill(lh, 0, col(light.colors.background));
            mini(lh, light.colors);
            p.Fill(dh, 0, col(dark.colors.background));
            mini(dh, dark.colors);
        } else if (tile.kind == Kind::New) {  // reads as "make your own"
            p.Fill(r, 0, c.raised);
            const float a = z.glyph() * 0.4f * s, cx = (r.left + r.right) * 0.5f, cy = Cy(r);
            p.Line(cx - a, cy, cx + a, cy, c.text2);
            p.Line(cx, cy - a, cx, cy + a, c.text2);
        } else {
            const ombra::UiPaletteColors& pc = tile.colors;
            p.Fill(r, 0, col(pc.background));
            mini(r, pc);
            if (!compact) p.Circle(r.right - 10 * s, r.bottom - 8 * s, 3.5f * s, c.accent);
        }
        if (clip) dc->PopLayer();
        p.Stroke(r, rad, sel ? c.line_strong : Mix(c.line_strong, c.text2, 0.5f * hover));
        if (sel) {
            const D2D1_RECT_F o = D2D1::RectF(r.left - ring_gap, r.top - ring_gap, r.right + ring_gap, r.bottom + ring_gap);
            p.Stroke(o, rad + ring_gap, c.text, 2.0f);
        }
        if (compact) continue;
        const float ny = r.bottom + z.space_xs() * s;
        p.Text(tile.name, TypeRole::Caption, D2D1::RectF(r.left, ny, r.right, ny + LineHeight(TypeRole::Caption) * s),
               sel ? c.text : c.text2, {TextAlign::Center, false, true});
    }
    for (int i = 0; i < static_cast<int>(field_rects_.size()); ++i) {
        const D2D1_RECT_F& r = field_rects_[i];
        const float hover = item_hover_.Amount(static_cast<int>(tiles.size()) + i);
        if (hover > 0) p.Fill(r, rad, c.HoverLayer(hover));
        const uint32_t rgb = FieldOf(tiles[selected].colors, i);
        const float d = z.control_xs() * s * 0.75f, pad = z.space_xs() * s, cy = paint::Cy(r);
        p.Circle(r.left + pad + d / 2, cy, d / 2, col(rgb));
        p.Ring(r.left + pad + d / 2, cy, d / 2 - 0.5f * s, c.line_strong, 1.0f);
        const float lx = r.left + pad + d + z.space_sm() * s;
        const std::wstring hex = HexText(rgb);
        const float hw = p.TextW(hex, TypeRole::Mono) + 1;
        p.Text(FieldName(i), TypeRole::Body, D2D1::RectF(lx, r.top, r.right - pad - hw - z.space_sm() * s, r.bottom), c.text,
               {TextAlign::Leading, false, true});
        p.Text(hex, TypeRole::Mono, D2D1::RectF(r.right - pad - hw, r.top, r.right - pad, r.bottom), c.text2);
    }
    if (focus_visible && selected >= 0 && selected < static_cast<int>(tile_rects_.size())) p.Focus(tile_rects_[selected], rad);
}

bool ThemePickerControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = IndexAt(pt);
    return was != hovered_index;
}

void ThemePickerControl::Press(D2D1_POINT_2F pt) {
    const int i = IndexAt(pt);
    const int n = static_cast<int>(tiles.size());
    if (i >= n) {
        if (on_edit_field) on_edit_field(i - n, field_rects_[i - n]);
        return;
    }
    if (i < 0 || i == selected) return;
    if (tiles[i].kind != Kind::New) selected = i;  // "new" makes a theme; the page selects it
    if (on_change) on_change(i);
}

// --- Colour editor ------------------------------------------------------------------------------

namespace {
// The editor's parts, in DIPs at Default density (Sizes::S scales them): the square is as wide as
// the popup's inside, the hue bar a thin strip, the handles a ring around the colour.
constexpr float kEditorInnerDip = 240, kSquareDip = 140, kHueBarDip = 12, kHandleDip = 7;

Rgb FromHsv(float h, float sat, float v) {  // h in 0..1
    const float r = std::fmod(h * 6, 6.0f), f = r - std::floor(r);
    const float p = v * (1 - sat), q = v * (1 - sat * f), u = v * (1 - sat * (1 - f));
    switch (static_cast<int>(r)) {
        case 0: return {v, u, p};
        case 1: return {q, v, p};
        case 2: return {p, v, u};
        case 3: return {p, q, v};
        case 4: return {u, p, v};
        default: return {v, p, q};
    }
}

void ToHsv(Rgb c, float* h, float* sat, float* v) {
    const float mx = (std::max)({c.r, c.g, c.b}), mn = (std::min)({c.r, c.g, c.b}), d = mx - mn;
    *v = mx;
    *sat = mx > 0 ? d / mx : 0;
    if (d <= 0) return;  // a grey: the hue stays where it was
    float hh = mx == c.r ? std::fmod((c.g - c.b) / d, 6.0f) : mx == c.g ? (c.b - c.r) / d + 2 : (c.r - c.g) / d + 4;
    *h = (hh < 0 ? hh + 6 : hh) / 6;
}

bool ParseHex(const std::wstring& s, uint32_t* out) {
    const size_t start = !s.empty() && s[0] == L'#' ? 1 : 0;
    if (s.size() - start != 6) return false;
    uint32_t v = 0;
    for (size_t i = start; i < s.size(); ++i) {
        const wchar_t ch = s[i];
        const int d = ch >= L'0' && ch <= L'9' ? ch - L'0' : ch >= L'a' && ch <= L'f' ? ch - L'a' + 10
                    : ch >= L'A' && ch <= L'F' ? ch - L'A' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    *out = v;
    return true;
}

std::wstring HexText(uint32_t v) {
    wchar_t b[16];
    std::swprintf(b, 16, L"#%06x", v);
    return b;
}

D2D1_COLOR_F D2d(Rgb c) { return D2D1::ColorF(c.r, c.g, c.b); }

// A handle on a gradient: the colour inside a white ring with a dark edge, readable on any hue.
void Handle(const Paint& p, float cx, float cy, float r, Rgb c) {
    p.Ring(cx, cy, r + 1.5f * p.s, D2D1::ColorF(0, 0, 0, 0.45f), 1.0f);
    p.Circle(cx, cy, r, D2D1::ColorF(1, 1, 1));
    p.Circle(cx, cy, r - 2 * p.s, D2d(c));
}

void FillGradient(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, const D2D1_GRADIENT_STOP* stops,
                  UINT32 n, bool vertical) {
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> sc;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> b;
    if (FAILED(dc->CreateGradientStopCollection(stops, n, &sc)) ||
        FAILED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top),
                                                vertical ? D2D1::Point2F(r.left, r.bottom) : D2D1::Point2F(r.right, r.top)),
            sc.Get(), &b)))
        return;
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b.Get());
}
}  // namespace

void ColorEditorPopup::SetColor(uint32_t rgb) {
    rgb_ = committed_ = rgb;
    ToHsv(RgbFromHex(rgb), &hue_, &sat_, &val_);
    hex_.text = HexText(rgb);
    hex_.error_text.clear();
    hex_.SetCaret(std::wstring::npos);
}

void ColorEditorPopup::Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& t, float s) {
    theme_ = &t;
    s_ = s;
    const Sizes& z = t.size;
    hex_.mono = true;
    hex_.max_length = 7;
    hex_.accept_char = [](wchar_t ch) { return ch == L'#' || iswxdigit(ch); };
    hex_.on_edit = [this](const std::wstring& txt) {
        uint32_t v = 0;
        hex_.error_text.clear();
        if (!ParseHex(txt, &v) || v == rgb_) return;
        const float keep = hue_;
        rgb_ = v;
        ToHsv(RgbFromHex(v), &hue_, &sat_, &val_);
        if (sat_ <= 0) hue_ = keep;
        if (on_preview) on_preview(rgb_);
    };
    hex_.on_commit = [this](const std::wstring& txt) {
        uint32_t v = 0;
        if (!ParseHex(txt, &v)) {
            hex_.error_text = L"Six hex digits, like #3d7eff";
            return;
        }
        Commit();
        hex_.focused = false;
    };
    const float pad = z.space_md() * s, gap = z.space_xs() * s, margin = z.space_md() * s;
    const float w = z.S(kEditorInnerDip) * s + 2 * pad;
    const float foot = accent_foot ? z.space_md() + 2 * LineHeight(TypeRole::Caption) + z.space_xxs() : 0.0f;
    const float h = 2 * pad + (z.S(kSquareDip) + z.space_md() + z.S(kHueBarDip) + z.space_md() + z.control_sm() + foot) * s;
    float left = (std::max)(view.left + margin, (std::min)(anchor.right - w, view.right - margin - w));
    float top = anchor.bottom + gap;
    if (top + h > view.bottom - margin && anchor.top - gap - h >= view.top + margin) top = anchor.top - gap - h;
    top = (std::max)(view.top + margin, (std::min)(top, view.bottom - margin - h));
    bounds = D2D1::RectF(left, top, left + w, top + h);
    drag_ = Drag::None;
    Layout(t, s);
    Opened();
}

void ColorEditorPopup::Layout(const Theme& t, float s) {
    const Sizes& z = t.size;
    const float pad = z.space_md() * s, l = bounds.left + pad, r = bounds.right - pad;
    float y = bounds.top + pad;
    square_ = D2D1::RectF(l, y, r, y + z.S(kSquareDip) * s);
    y = square_.bottom + z.space_md() * s;
    hue_bar_ = D2D1::RectF(l, y, r, y + z.S(kHueBarDip) * s);
    y = hue_bar_.bottom + z.space_md() * s;
    const LayoutContext ctx{s, &t};
    hex_.width_dip = (r - l) / s;
    hex_.Arrange(D2D1::RectF(l, y, r, y + z.control_sm() * s), ctx);
    y += (z.control_sm() + z.space_md()) * s;
    foot_ = D2D1::RectF(l, y, r, bounds.bottom - pad);
}

void ColorEditorPopup::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    theme_ = &t;
    s_ = s;
    Layout(t, s);
    BeginSurface(dc, t, s);
    const Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float rad = z.radius_inner() * s, hr = z.S(kHandleDip) * s;

    // Saturation across, brightness down: the hue under a white-to-clear and a clear-to-black ramp.
    p.Fill(square_, rad, D2d(FromHsv(hue_, 1, 1)));
    const D2D1_GRADIENT_STOP white[] = {{0, D2D1::ColorF(1, 1, 1, 1)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
    const D2D1_GRADIENT_STOP black[] = {{0, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, 1)}};
    FillGradient(dc, square_, rad, white, 2, false);
    FillGradient(dc, square_, rad, black, 2, true);
    p.Stroke(square_, rad, D2D1::ColorF(c.text.r, c.text.g, c.text.b, 0.12f));
    Handle(p, square_.left + sat_ * (square_.right - square_.left), square_.top + (1 - val_) * (square_.bottom - square_.top),
           hr, RgbFromHex(rgb_));

    D2D1_GRADIENT_STOP hues[7];
    for (int i = 0; i <= 6; ++i) hues[i] = {i / 6.0f, D2d(FromHsv(i / 6.0f, 1, 1))};
    const float bh = hue_bar_.bottom - hue_bar_.top;
    FillGradient(dc, hue_bar_, bh / 2, hues, 7, false);
    Handle(p, hue_bar_.left + bh / 2 + hue_ * (hue_bar_.right - hue_bar_.left - bh), paint::Cy(hue_bar_), hr,
           FromHsv(hue_, 1, 1));

    hex_.Draw(dc, t, s);
    if (!accent_foot) {
        EndSurface(dc);
        return;
    }

    // Picked, then used when contrast moved it (DeriveAccent), and why: theme `t` already shows
    // this colour (the host previews it), so its roles are the ones in use.
    const AccentRoles a = DeriveAccent(RgbFromHex(rgb_), Rgb{c.card.r, c.card.g, c.card.b}, Rgb{c.raised.r, c.raised.g, c.raised.b},
                                       Mix(Rgb{c.card.r, c.card.g, c.card.b}, Rgb{c.text.r, c.text.g, c.text.b}, c.hover_a),
                                       t.mode == ThemeMode::Light);
    const bool moved = a.fill_moved || a.text_moved;
    const uint32_t used = RgbToHex(a.fill_moved || !a.text_moved ? a.fill : a.text);
    const float line = LineHeight(TypeRole::Caption) * s, d = z.space_md() * s, cy = foot_.top + line / 2;
    float x = foot_.left;
    p.Circle(x + d / 2, cy, d / 2, D2d(a.picked));
    x += d + z.space_xs() * s;
    if (moved) {
        p.Text(L"\x2192", TypeRole::Caption, D2D1::RectF(x, foot_.top, x + d, foot_.top + line), c.text3);
        x += d + z.space_xxs() * s;
        p.Circle(x + d / 2, cy, d / 2, D2d(RgbFromHex(used)));
        x += d + z.space_xs() * s;
    }
    p.Text(moved ? HexText(rgb_) + L" \x2192 " + HexText(used) : HexText(rgb_), TypeRole::Caption,
           D2D1::RectF(x, foot_.top, foot_.right, foot_.top + line), c.text2, {TextAlign::Leading, false, true});
    const bool light = t.mode == ThemeMode::Light;
    const wchar_t* why = a.fill_moved ? (light ? L"Darker, so filled controls stand out." : L"Lighter, so filled controls stand out.")
                       : a.text_moved ? (light ? L"Darker as text, so it stays readable." : L"Lighter as text, so it stays readable.")
                                      : L"Used as picked.";
    p.Text(why, TypeRole::Caption, D2D1::RectF(foot_.left, foot_.top + line + z.space_xxs() * s, foot_.right, foot_.bottom),
           c.text3, {TextAlign::Leading, false, true});
    EndSurface(dc);
}

CursorKind ColorEditorPopup::CursorAt(D2D1_POINT_2F pt) const {
    if (hex_.HitTest(pt)) return CursorKind::IBeam;
    return PointIn(square_, pt) || PointIn(hue_bar_, pt) ? CursorKind::Hand : CursorKind::Arrow;
}

void ColorEditorPopup::DragTo(D2D1_POINT_2F pt) {
    auto frac = [](float v, float lo, float hi) { return hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f; };
    if (drag_ == Drag::Square) {
        sat_ = frac(pt.x, square_.left, square_.right);
        val_ = 1 - frac(pt.y, square_.top, square_.bottom);
    } else if (drag_ == Drag::Hue) {
        const float r = (hue_bar_.bottom - hue_bar_.top) / 2;
        hue_ = frac(pt.x, hue_bar_.left + r, hue_bar_.right - r);
    } else {
        return;
    }
    const uint32_t v = RgbToHex(FromHsv(hue_, sat_, val_));
    if (v == rgb_) return;
    rgb_ = v;
    hex_.text = HexText(v);
    hex_.error_text.clear();
    hex_.SetCaret(std::wstring::npos);
    if (on_preview) on_preview(rgb_);
}

bool ColorEditorPopup::OnMouseMove(D2D1_POINT_2F pt) {
    hex_.OnMouseMove(pt);
    if (drag_ == Drag::None) return false;
    DragTo(pt);
    return true;
}

bool ColorEditorPopup::OnMouseDown(D2D1_POINT_2F pt) {
    if (!PointIn(bounds, pt)) {  // outside: keep the colour, close
        Commit();
        Close();
        return true;
    }
    if (hex_.HitTest(pt)) {
        hex_.OnMouseDown(pt);
        hex_.pressed = false;
        return true;
    }
    hex_.focused = false;
    drag_ = PointIn(square_, pt) ? Drag::Square : PointIn(hue_bar_, pt) ? Drag::Hue : Drag::None;
    DragTo(pt);
    return true;
}

bool ColorEditorPopup::OnMouseUp(D2D1_POINT_2F pt) {
    if (drag_ == Drag::None) return false;
    DragTo(pt);
    drag_ = Drag::None;
    Commit();
    return true;
}

bool ColorEditorPopup::OnKeyDown(UINT vk) {
    if (vk == VK_ESCAPE) {
        if (hex_.focused && hex_.text != HexText(rgb_)) {  // first Esc: the half-typed hex
            hex_.text = HexText(rgb_);
            hex_.error_text.clear();
            hex_.focused = false;
            return true;
        }
        drag_ = Drag::None;
        if (on_cancel) on_cancel();
        Close();
        return true;
    }
    if (vk == VK_RETURN && !hex_.focused) {
        Commit();
        Close();
        return true;
    }
    hex_.OnKeyDown(vk);
    return true;  // modal
}

bool ColorEditorPopup::OnChar(wchar_t ch) {
    if (!hex_.focused) {  // typing anywhere goes to the hex, starting over
        hex_.focused = true;
        if (ch == L'#' || iswxdigit(ch)) hex_.text.clear();
        hex_.SetCaret(std::wstring::npos);
    }
    hex_.OnChar(ch);
    return true;
}

void ColorEditorPopup::Commit() {
    if (rgb_ == committed_) return;
    committed_ = rgb_;
    if (on_commit) on_commit(rgb_);
}

// --- Text prompt --------------------------------------------------------------------------------

namespace {
constexpr float kPromptInnerDip = 240;
}

void TextPromptPopup::SetText(const std::wstring& text) {
    field_.text = text;
    field_.error_text.clear();
    field_.SetCaret(std::wstring::npos);
}

void TextPromptPopup::Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& t, float s) {
    const Sizes& z = t.size;
    field_.on_edit = [this](const std::wstring&) { field_.error_text.clear(); };
    field_.on_commit = [this](const std::wstring&) {
        if (TryCommit()) Close();
    };
    field_.focused = true;
    const float pad = z.space_md() * s, gap = z.space_xs() * s, margin = z.space_md() * s;
    const float w = z.S(kPromptInnerDip) * s + 2 * pad;
    // Caption, field, and room for the reason under it so the popup doesn't grow while typing.
    const float h = 2 * pad + (LineHeight(TypeRole::Caption) + z.space_xs() + z.control_sm() + z.space_xs() +
                               LineHeight(TypeRole::Caption)) * s;
    const float left = (std::max)(view.left + margin, (std::min)(anchor.left, view.right - margin - w));
    float top = anchor.bottom + gap;
    if (top + h > view.bottom - margin && anchor.top - gap - h >= view.top + margin) top = anchor.top - gap - h;
    top = (std::max)(view.top + margin, (std::min)(top, view.bottom - margin - h));
    bounds = D2D1::RectF(left, top, left + w, top + h);
    Layout(t, s);
    Opened();
}

void TextPromptPopup::Layout(const Theme& t, float s) {
    const Sizes& z = t.size;
    const float pad = z.space_md() * s, l = bounds.left + pad, r = bounds.right - pad;
    float y = bounds.top + pad;
    caption_ = D2D1::RectF(l, y, r, y + LineHeight(TypeRole::Caption) * s);
    y = caption_.bottom + z.space_xs() * s;
    field_.width_dip = (r - l) / s;
    field_.Arrange(D2D1::RectF(l, y, r, y + z.control_sm() * s), LayoutContext{s, &t});
}

void TextPromptPopup::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    Layout(t, s);
    BeginSurface(dc, t, s);
    const Paint p{dc, t, s};
    p.Text(caption, TypeRole::Caption, caption_, t.color.text2, {TextAlign::Leading, false, true});
    field_.Draw(dc, t, s);
    EndSurface(dc);
}

CursorKind TextPromptPopup::CursorAt(D2D1_POINT_2F pt) const {
    return field_.HitTest(pt) ? CursorKind::IBeam : CursorKind::Arrow;
}

bool TextPromptPopup::OnMouseDown(D2D1_POINT_2F pt) {
    if (!PointIn(bounds, pt)) {  // outside: keep a good name, drop a bad one
        TryCommit();
        Close();
        return true;
    }
    if (field_.HitTest(pt)) {
        field_.OnMouseDown(pt);
        field_.pressed = false;
    }
    field_.focused = true;
    return true;
}

bool TextPromptPopup::OnKeyDown(UINT vk) {
    if (vk == VK_ESCAPE) {
        Close();
        return true;
    }
    field_.focused = true;
    field_.OnKeyDown(vk);  // Enter reaches field_.on_commit
    return true;
}

bool TextPromptPopup::OnChar(wchar_t ch) {
    field_.focused = true;
    field_.OnChar(ch);
    return true;
}

bool TextPromptPopup::TryCommit() {
    const std::wstring why = validate ? validate(field_.text) : std::wstring{};
    if (!why.empty()) {
        field_.error_text = why;
        return false;
    }
    if (on_commit) on_commit(field_.text);
    return true;
}

// --- Position picker ----------------------------------------------------------------------------

D2D1_SIZE_F PositionPickerControl::Measure(const LayoutConstraints&, const LayoutContext& ctx) {
    const Sizes z = ctx.theme ? ctx.theme->size : Sizes{};
    const float mh = z.miniature_h();
    return D2D1::SizeF(std::round(mh * 16 / 9) * ctx.dpi_scale,
                       (mh + z.space_xs() + LineHeight(TypeRole::Caption)) * ctx.dpi_scale);
}

namespace {
// The miniature screen: the full width of the control, 16:9.
D2D1_RECT_F Miniature(const D2D1_RECT_F& b) {
    return D2D1::RectF(b.left, b.top, b.right, b.top + (b.right - b.left) * 9 / 16);
}
// The thing that would appear at a spot (a toast, the panel): a bar in proportion to the screen.
D2D1_RECT_F SpotBar(const D2D1_RECT_F& m, float x, float y) {
    const float hw = (m.right - m.left) * 0.11f, hh = (m.bottom - m.top) * 0.07f;
    return D2D1::RectF(x - hw, y - hh, x + hw, y + hh);
}
}  // namespace

int PositionPickerControl::IndexAt(D2D1_POINT_2F pt) const {
    if (!HitTest(pt)) return -1;
    const D2D1_RECT_F m = Miniature(bounds);
    for (size_t i = 0; i < spots.size(); ++i) {
        const float x = m.left + (m.right - m.left) * spots[i].fx, y = m.top + (m.bottom - m.top) * spots[i].fy;
        // The hit box is the bar plus a quarter of the screen's height around it.
        const D2D1_RECT_F b = SpotBar(m, x, y);
        const float slop = (m.bottom - m.top) * 0.25f;
        if (pt.x > b.left - slop && pt.x < b.right + slop && pt.y > b.top - slop && pt.y < b.bottom + slop)
            return static_cast<int>(i);
    }
    return -1;
}

void PositionPickerControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const D2D1_RECT_F m = Miniature(bounds);
    const float rad = t.size.radius_control() * s;
    p.Fill(m, rad, c.panel);
    p.Stroke(m, rad, c.line_strong);
    TrackItems(hovered_index);
    for (size_t i = 0; i < spots.size(); ++i) {
        const float x = m.left + (m.right - m.left) * spots[i].fx, y = m.top + (m.bottom - m.top) * spots[i].fy;
        const bool sel = static_cast<int>(i) == selected, hov = static_cast<int>(i) == hovered_index;
        const auto bar = SpotBar(m, x, y);
        const float br = (bar.bottom - bar.top) * 0.3f, halo = t.size.space_xxs() * s + 1 * s;
        if (!sel)
            p.Fill(D2D1::RectF(bar.left - halo, bar.top - halo, bar.right + halo, bar.bottom + halo), br + halo,
                   pressed && hov ? c.PressedLayer() : c.HoverLayer(item_hover_.Amount(static_cast<int>(i))));
        if (sel) p.Fill(bar, br, c.accent);
        else {
            p.Fill(bar, br, c.raised);
            p.Stroke(bar, br, c.line_strong);
        }
    }
    if (selected >= 0 && selected < static_cast<int>(names.size()))
        p.Text(names[selected], TypeRole::Caption, D2D1::RectF(m.left, m.bottom + t.size.space_xs() * s, bounds.right, bounds.bottom),
               c.text2, {TextAlign::Center});
    if (focus_visible) p.Focus(m, rad);
}

bool PositionPickerControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = IndexAt(pt);
    return was != hovered_index;
}

void PositionPickerControl::Press(D2D1_POINT_2F pt) {
    const int i = IndexAt(pt);
    if (i < 0 || i == selected) return;
    selected = i;
    if (on_change) on_change(i);
}

// --- Panel position -----------------------------------------------------------------------------

D2D1_SIZE_F PanelPositionControl::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float w = c.BoundedWidth() ? (std::min)(c.max_w, max_width_dip * ctx.dpi_scale) : max_width_dip * ctx.dpi_scale;
    return D2D1::SizeF(w, w * 9.0f / 16.0f);
}

D2D1_RECT_F PanelPositionControl::ScreenRect() const {
    // A 16:9 screen inside bounds, centred.
    const float bw = bounds.right - bounds.left, bh = bounds.bottom - bounds.top;
    const float w = (std::min)(bw, bh * 16.0f / 9.0f), h = w * 9.0f / 16.0f;
    const float left = bounds.left + (bw - w) * 0.5f, top = bounds.top + (bh - h) * 0.5f;
    return D2D1::RectF(left, top, left + w, top + h);
}

D2D1_RECT_F PanelPositionControl::PanelRect(const std::wstring& pos, bool fused) const {
    const D2D1_RECT_F sr = ScreenRect();
    const float w = sr.right - sr.left, h = sr.bottom - sr.top;
    const float inset = fused ? 0.0f : (std::min)(w, h) * 0.07f;
    const float cx = (sr.left + sr.right) * 0.5f, cy = (sr.top + sr.bottom) * 0.5f;
    // Sized so all five slots fit on the screen at once without touching.
    if (pos == L"top")    return D2D1::RectF(cx - w * 0.26f, sr.top + inset, cx + w * 0.26f, sr.top + inset + h * 0.2f);
    if (pos == L"bottom") return D2D1::RectF(cx - w * 0.26f, sr.bottom - inset - h * 0.2f, cx + w * 0.26f, sr.bottom - inset);
    if (pos == L"left")   return D2D1::RectF(sr.left + inset, cy - h * 0.3f, sr.left + inset + w * 0.15f, cy + h * 0.3f);
    if (pos == L"right")  return D2D1::RectF(sr.right - inset - w * 0.15f, cy - h * 0.3f, sr.right - inset, cy + h * 0.3f);
    return D2D1::RectF(cx - w * 0.14f, cy - h * 0.17f, cx + w * 0.14f, cy + h * 0.17f);  // center
}

int PanelPositionControl::ZoneAt(D2D1_POINT_2F pt) const {
    if (!visible) return -1;
    const D2D1_RECT_F sr = ScreenRect();
    if (pt.x < sr.left || pt.x > sr.right || pt.y < sr.top || pt.y > sr.bottom) return -1;
    // Normalised distance to each edge; the nearest edge wins unless the point is well inside.
    const float fx = (pt.x - sr.left) / (sr.right - sr.left), fy = (pt.y - sr.top) / (sr.bottom - sr.top);
    std::wstring zone = L"center";
    const float edges[4] = {fy, 1.0f - fy, fx, 1.0f - fx};
    const wchar_t* names[4] = {L"top", L"bottom", L"left", L"right"};
    int nearest = 0;
    for (int i = 1; i < 4; ++i) if (edges[i] < edges[nearest]) nearest = i;
    if (edges[nearest] < 0.25f) zone = names[nearest];
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i] == zone) return static_cast<int>(i);
    return -1;
}

void PanelPositionControl::Draw(ID2D1DeviceContext* dc, const Theme& t, float s) {
    if (!visible) return;
    const Paint p{dc, t, s, enabled ? 1.0f : 0.4f};
    const ColorRoles& c = t.color;
    const D2D1_RECT_F sr = ScreenRect();
    // The screen frame, when on, reads as a thicker, rounder rim.
    const float rad = (screen_frame ? t.size.radius_card() : t.size.radius_control()) * s;
    p.Fill(sr, rad, c.panel);
    p.Stroke(sr, rad, screen_frame ? c.text2 : c.line_strong, screen_frame ? 3.0f : 1.0f);

    // The other slots: dashed outlines like a DropSlot; the hovered one fades to the accent tint.
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1StrokeStyle> dash;
    const float dashes[] = {4.0f, 3.0f};
    if (factory)
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                               D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM),
                                   dashes, 2, &dash);
    TrackItems(hovered_index);
    const float r = t.size.radius_inner() * s, i = 0.5f * s;
    for (int k = 0; k < static_cast<int>(options.size()); ++k) {
        if (k == selected) continue;
        const D2D1_RECT_F slot = PanelRect(options[k], attached && options[k] != L"center");
        const float hov = item_hover_.Amount(k);
        if (hov > 0.0f) p.Fill(slot, r, Faded(pressed && k == hovered_index ? c.accent_line : c.accent_soft, hov));
        const auto in = D2D1::RectF(slot.left + i, slot.top + i, slot.right - i, slot.bottom - i);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(in, r - i, r - i), p.B(Mix(c.line_strong, c.accent, hov)).Get(), 1.0f * s,
                                 hov > 0.5f ? nullptr : dash.Get());
    }
    if (selected >= 0 && selected < static_cast<int>(options.size())) {
        const std::wstring& pos = options[selected];
        const D2D1_RECT_F pr = PanelRect(pos, attached && pos != L"center");
        p.Fill(pr, r, c.accent);
        // The tab rail: a band in the on-accent colour along its side of the panel.
        const float band = (std::min)(pr.right - pr.left, pr.bottom - pr.top) * 0.28f;
        D2D1_RECT_F rail = pr;
        if (rail_position == L"right") rail.left = rail.right - band;
        else if (rail_position == L"top") rail.bottom = rail.top + band;
        else if (rail_position == L"bottom") rail.top = rail.bottom - band;
        else rail.right = rail.left + band;
        dc->FillRectangle(rail, p.B(Faded(c.on_accent, 0.35f)).Get());
    }
    if (focus_visible) p.Focus(sr, rad);
}

bool PanelPositionControl::OnMouseMove(D2D1_POINT_2F pt) {
    const int was = hovered_index;
    Widget::OnMouseMove(pt);
    hovered_index = ZoneAt(pt);
    return was != hovered_index;
}

void PanelPositionControl::Press(D2D1_POINT_2F pt) {
    const int z = ZoneAt(pt);
    if (z < 0 || z == selected) return;
    selected = z;
    if (on_change) on_change(z);
}

}  // namespace ombra_ui
