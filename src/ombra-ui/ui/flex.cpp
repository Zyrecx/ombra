#include "ombra-ui/ui/flex.h"

#include <algorithm>

namespace ombra_ui {

std::shared_ptr<FlexWidget> FlexWidget::MakeRow(float gap_dip, bool wrap) {
    auto f = std::make_shared<FlexWidget>();
    f->direction = FlexDirection::Row;
    f->gap_dip = gap_dip;
    f->wrap = wrap;
    return f;
}

std::shared_ptr<FlexWidget> FlexWidget::MakeColumn(float gap_dip) {
    auto f = std::make_shared<FlexWidget>();
    f->direction = FlexDirection::Column;
    f->gap_dip = gap_dip;
    f->align = FlexAlign::Stretch;
    return f;
}

FlexItem& FlexWidget::Add(std::shared_ptr<Widget> w) {
    items.push_back(FlexItem{});
    items.back().widget = std::move(w);
    return items.back();
}

const FlexItem* FlexWidget::FindItem(const Widget* w) const {
    for (const auto& it : items) {
        if (it.widget.get() == w) return &it;
    }
    return nullptr;
}

namespace {
bool Live(const FlexItem& it) { return it.widget && it.widget->visible; }
}  // namespace

FlexWidget::Solution FlexWidget::Solve(float avail_w, const LayoutContext& ctx) const {
    return direction == FlexDirection::Column ? SolveColumn(avail_w, ctx) : SolveRow(avail_w, ctx);
}

FlexWidget::Solution FlexWidget::SolveColumn(float avail_w, const LayoutContext& ctx) const {
    Solution out;
    const float s = ctx.dpi_scale;
    const bool bounded = avail_w < kUnbounded * 0.5f;
    float y = 0.0f;
    bool first = true;
    for (size_t i = 0; i < items.size(); ++i) {
        const FlexItem& it = items[i];
        if (!Live(it)) continue;
        if (!first) y += gap_dip * s;
        first = false;
        y += it.margin_before_dip * s;

        const FlexAlign a = it.align_self.value_or(align);
        float w = avail_w;
        float h = 0.0f;
        if (a == FlexAlign::Stretch && bounded) {
            h = it.basis_dip >= 0.0f ? it.basis_dip * s
                                     : it.widget->Measure(LayoutConstraints::Exact(w), ctx).height;
        } else {
            const D2D1_SIZE_F natural = it.widget->Measure(LayoutConstraints::Available(avail_w), ctx);
            w = (std::min)(natural.width, avail_w);
            h = it.basis_dip >= 0.0f ? it.basis_dip * s : natural.height;
        }
        float x = 0.0f;
        if (bounded && a == FlexAlign::Center) x = (avail_w - w) * 0.5f;
        else if (bounded && a == FlexAlign::End) x = avail_w - w;

        out.slots.push_back({i, x, y, w, h});
        out.content_w = (std::max)(out.content_w, x + w);
        y += h + it.margin_after_dip * s;
    }
    out.content_h = y;
    return out;
}

FlexWidget::Solution FlexWidget::SolveRow(float avail_w, const LayoutContext& ctx) const {
    Solution out;
    const float s = ctx.dpi_scale;
    const bool bounded = avail_w < kUnbounded * 0.5f;
    const float gap = gap_dip * s;
    const float line_gap = (line_gap_dip >= 0.0f ? line_gap_dip : gap_dip) * s;

    // Resolve every live item's main-axis (width) size first.
    struct Pending {
        size_t index;
        float  w;        // resolved width
        float  outer_w;  // width plus the item's own before/after margins
    };
    std::vector<Pending> pend;
    const int cols = (columns > 0 && bounded) ? columns : 0;
    const float col_w = cols > 0 ? (avail_w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols) : 0.0f;
    for (size_t i = 0; i < items.size(); ++i) {
        const FlexItem& it = items[i];
        if (!Live(it)) continue;
        float w;
        if (cols > 0) {
            w = col_w;
        } else if (it.fill_line && bounded) {
            w = avail_w - (it.margin_before_dip + it.margin_after_dip) * s;
        } else if (it.basis_dip >= 0.0f) {
            w = it.basis_dip * s;
        } else {
            w = it.widget->Measure(LayoutConstraints::Available(avail_w), ctx).width;
        }
        // A basis wider than the container (or a "force a full row" 100000) clamps to the line.
        if (bounded) w = (std::min)(w, avail_w - (it.margin_before_dip + it.margin_after_dip) * s);
        w = (std::max)(w, 0.0f);
        pend.push_back({i, w, w + (it.margin_before_dip + it.margin_after_dip) * s});
    }

    // Break into lines.
    struct Line {
        size_t first, last;  // [first, last) into pend
        float  used = 0.0f;
    };
    std::vector<Line> lines;
    {
        size_t start = 0;
        float used = 0.0f;
        for (size_t p = 0; p < pend.size(); ++p) {
            const float add = (p == start ? 0.0f : gap) + pend[p].outer_w;
            const bool overflow = wrap && bounded && p > start && used + add > avail_w + 0.01f;
            const bool col_break = cols > 0 && p > start && static_cast<int>(p - start) >= cols;
            if (overflow || col_break) {
                lines.push_back({start, p, used});
                start = p;
                used = pend[p].outer_w;
            } else {
                used += add;
            }
        }
        if (start < pend.size()) lines.push_back({start, pend.size(), used});
    }

    float y = 0.0f;
    for (size_t li = 0; li < lines.size(); ++li) {
        const Line& ln = lines[li];
        if (li > 0) y += line_gap;

        // Distribute leftover width to growers.
        float total_grow = 0.0f;
        for (size_t p = ln.first; p < ln.last; ++p) total_grow += items[pend[p].index].grow;
        const float leftover = bounded ? (std::max)(0.0f, avail_w - ln.used) : 0.0f;
        if (total_grow > 0.0f && leftover > 0.0f) {
            for (size_t p = ln.first; p < ln.last; ++p) {
                const float share = leftover * items[pend[p].index].grow / total_grow;
                pend[p].w += share;
                pend[p].outer_w += share;
            }
        }

        // Cross-axis sizes for this line.
        float line_h = 0.0f;
        std::vector<float> hs;
        hs.reserve(ln.last - ln.first);
        for (size_t p = ln.first; p < ln.last; ++p) {
            const float h = items[pend[p].index].widget->Measure(LayoutConstraints::Exact(pend[p].w), ctx).height;
            hs.push_back(h);
            line_h = (std::max)(line_h, h);
        }

        // Main-axis justification (only meaningful when there is slack and nothing grew).
        float used_now = 0.0f;
        for (size_t p = ln.first; p < ln.last; ++p) used_now += pend[p].outer_w + (p > ln.first ? gap : 0.0f);
        const float slack = (bounded && total_grow <= 0.0f) ? (std::max)(0.0f, avail_w - used_now) : 0.0f;
        float x = 0.0f;
        float extra_gap = 0.0f;
        switch (justify) {
            case FlexJustify::Center: x = slack * 0.5f; break;
            case FlexJustify::End: x = slack; break;
            case FlexJustify::SpaceBetween:
                if (ln.last - ln.first > 1) extra_gap = slack / static_cast<float>(ln.last - ln.first - 1);
                break;
            case FlexJustify::Start: break;
        }

        for (size_t p = ln.first; p < ln.last; ++p) {
            const FlexItem& it = items[pend[p].index];
            x += it.margin_before_dip * s;
            const FlexAlign a = it.align_self.value_or(align);
            const float h_own = hs[p - ln.first];
            float h = h_own;
            float dy = 0.0f;
            switch (a) {
                case FlexAlign::Stretch: h = line_h; break;
                case FlexAlign::Center: dy = (line_h - h_own) * 0.5f; break;
                case FlexAlign::End: dy = line_h - h_own; break;
                case FlexAlign::Start: break;
            }
            out.slots.push_back({pend[p].index, x, y + dy, pend[p].w, h});
            out.content_w = (std::max)(out.content_w, x + pend[p].w);
            x += pend[p].w + it.margin_after_dip * s + gap + extra_gap;
        }
        y += line_h;
    }
    out.content_h = y;
    return out;
}

D2D1_SIZE_F FlexWidget::Measure(const LayoutConstraints& c, const LayoutContext& ctx) {
    const float pl = Pad(padding_dip.left, ctx), pr = Pad(padding_dip.right, ctx);
    const float pt = Pad(padding_dip.top, ctx), pb = Pad(padding_dip.bottom, ctx);
    const float inner_w = c.BoundedWidth() ? (std::max)(0.0f, c.max_w - pl - pr) : kUnbounded;
    const Solution sol = Solve(inner_w, ctx);
    const float w = c.BoundedWidth() ? c.max_w : sol.content_w + pl + pr;
    return D2D1::SizeF(std::clamp(w, c.min_w, (std::max)(c.min_w, c.max_w)),
                       (std::max)(c.min_h, sol.content_h + pt + pb));
}

void FlexWidget::Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) {
    bounds = rect;
    const float pl = Pad(padding_dip.left, ctx), pr = Pad(padding_dip.right, ctx);
    const float pt = Pad(padding_dip.top, ctx);
    const float inner_w = (std::max)(0.0f, (rect.right - rect.left) - pl - pr);
    const Solution sol = Solve(inner_w, ctx);
    for (const Slot& sl : sol.slots) {
        const float l = rect.left + pl + sl.x;
        const float t = rect.top + pt + sl.y;
        items[sl.index].widget->Arrange(D2D1::RectF(l, t, l + sl.w, t + sl.h), ctx);
    }
}

void FlexWidget::Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) {
    if (!visible) return;
    for (auto& it : items) {
        if (Live(it)) it.widget->Draw(dc, theme, dpi_scale);
    }
}

bool FlexWidget::OnMouseMove(D2D1_POINT_2F pt) {
    bool used = false;
    for (auto& it : items) {
        if (it.widget) used = it.widget->OnMouseMove(pt) || used;
    }
    return used;
}

bool FlexWidget::OnMouseDown(D2D1_POINT_2F pt) {
    for (auto& it : items) {
        if (it.widget && it.widget->OnMouseDown(pt)) return true;
    }
    return false;
}

bool FlexWidget::OnMouseUp(D2D1_POINT_2F pt) {
    bool used = false;
    for (auto& it : items) {
        if (it.widget) used = it.widget->OnMouseUp(pt) || used;
    }
    return used;
}

bool FlexWidget::OnKeyDown(UINT vk) {
    for (auto& it : items) {
        if (it.widget && it.widget->OnKeyDown(vk)) return true;
    }
    return false;
}

bool FlexWidget::QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const {
    for (const auto& it : items) {
        if (it.widget && it.widget->QueryTooltip(text, anchor)) return true;
    }
    return false;
}

}  // namespace ombra_ui
