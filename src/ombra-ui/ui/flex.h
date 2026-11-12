#pragma once

// FlexWidget: the one container. Mirrors the parts of Noctalia's ui/controls/flex.h (MIT) that Ombra
// needs -- direction, wrap, gap, cross-axis align, main-axis justify, padding, per-item basis/grow --
// plus two things Noctalia's flex doesn't have but every Ombra page wants: an equal-columns mode
// (Monitors) and per-item main-axis margins (section spacing).
//
// Scope, stated so nobody assumes more:
//   * Row: children flow left to right, wrapping onto new lines when `wrap` is set. `basis_dip`
//     is an item's preferred width (<0 asks the widget), `grow` shares a line's leftover width,
//     `fill_line` takes a whole line. `columns` > 0 instead forces N equal columns.
//   * Column: children stack top to bottom at the container's full width (align Stretch) or their
//     measured width. Heights are always natural: `grow` and `justify` apply to Row lines only,
//     because a Column would need a definite container height and nothing here has one (pages
//     scroll).
//   * Wrapping a Column is not supported.

#include "ombra-ui/ui/layout.h"
#include "ombra-ui/ui/widget.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ombra_ui {

enum class FlexDirection { Row, Column };
enum class FlexAlign { Start, Center, End, Stretch };
enum class FlexJustify { Start, Center, End, SpaceBetween };

struct FlexPadding {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;  // DIP
};

struct FlexItem {
    std::shared_ptr<Widget> widget;
    float basis_dip = -1.0f;   // preferred main-axis size in DIP; < 0 = ask widget->Measure()
    float grow = 0.0f;         // share of a Row line's leftover width
    bool  fill_line = false;   // take the whole main axis of its line
    // Extra space on the main axis around this item, on top of the container's gap. Lets a page
    // give a header and a card different breathing room without wrapper widgets.
    float margin_before_dip = 0.0f;
    float margin_after_dip = 0.0f;
    std::optional<FlexAlign> align_self;
};

class FlexWidget : public Widget {
public:
    FlexDirection direction = FlexDirection::Row;
    bool          wrap = false;
    float         gap_dip = 0.0f;         // between items on a line / in a column
    float         line_gap_dip = -1.0f;   // between wrapped Row lines; < 0 = same as gap_dip
    FlexAlign     align = FlexAlign::Start;   // cross axis (Column default is set by MakeColumn)
    FlexJustify   justify = FlexJustify::Start;
    FlexPadding   padding_dip;
    int           columns = 0;            // Row only: > 0 = that many equal-width columns, wrapping

    std::vector<FlexItem> items;

    static std::shared_ptr<FlexWidget> MakeRow(float gap_dip = 0.0f, bool wrap = false);
    static std::shared_ptr<FlexWidget> MakeColumn(float gap_dip = 0.0f);

    FlexItem& Add(std::shared_ptr<Widget> w);
    const FlexItem* FindItem(const Widget* w) const;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;

    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) override;
    // Same contract ScrollAreaWidget always had: events are broadcast to the children in order and
    // each child does its own hit test. Deliberately NOT gated on this container's bounds -- a
    // widget may draw (a dropdown arrow, a drag ghost) outside its own rect.
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override {
        for (const auto& it : items) {
            if (it.widget && it.widget->visible && it.widget->CursorAt(pt) != CursorKind::Arrow) return it.widget->CursorAt(pt);
        }
        return CursorKind::Arrow;
    }

    // Solved placement, relative to the content origin (inside padding). Public so tests can read
    // the geometry without a device.
    struct Slot {
        size_t index = 0;
        float x = 0, y = 0, w = 0, h = 0;
    };
    struct Solution {
        std::vector<Slot> slots;
        float content_w = 0.0f;
        float content_h = 0.0f;
    };
    Solution Solve(float avail_w, const LayoutContext& ctx) const;

private:
    Solution SolveRow(float avail_w, const LayoutContext& ctx) const;
    Solution SolveColumn(float avail_w, const LayoutContext& ctx) const;
    float Pad(float dip, const LayoutContext& ctx) const { return dip * ctx.dpi_scale; }
};

}  // namespace ombra_ui
