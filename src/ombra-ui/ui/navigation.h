#pragma once

// Navigation: how Settings moves between pages, and the
// Save/Discard bar every page shares. Same rules as controls.h: new tokens only, every state
// settable directly so the gallery can draw it.

#include "ombra-ui/ui/controls.h"

#include <string>
#include <vector>

namespace ombra_ui {

// The Settings rail: one entry per page, in order of use. Vertical (ui.rail_position Left/Right)
// or horizontal (Top/Bottom). Icon over the name, no box; the chosen page has a tinted pill.
class NavRail : public Control {
public:
    struct Item { IconType icon; std::wstring label; };
    std::vector<Item> items;
    int selected = 0;
    int hover_index = -1;
    bool horizontal = false;
    std::function<void(int)> on_change;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    D2D1_RECT_F ItemRect(const Theme& theme, float s, int i) const;
    D2D1_RECT_F ItemRect(int i) const { return theme_ ? ItemRect(*theme_, s_, i) : D2D1::RectF(); }  // after Measure

protected:
    void Press(D2D1_POINT_2F pt) override;

private:
    D2D1_SIZE_F ItemSize(const Theme& theme, float s) const;  // pixels
    int ItemAt(D2D1_POINT_2F pt) const;
    const Theme* theme_ = nullptr;  // from the last Measure, for hit tests
    float s_ = 1.0f;
    float label_w_ = 0.0f;  // the widest name in Label, pixels, from the last Measure
};

// A page's sub-tabs (Video | Audio), equal widths across the content column. The chosen one is
// primary text over a 2 DIP accent line on the bar's rule.
class TabBar : public Control {
public:
    std::vector<std::wstring> tabs;
    int selected = 0;
    int hover_index = -1;
    std::function<void(int)> on_change;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    D2D1_RECT_F TabRect(int i) const;

protected:
    void Press(D2D1_POINT_2F pt) override;
};

// Staged edits waiting: what Save will do, then Discard and Save at the bottom-right.
// A flat footer under a hairline, flush with the page's edges. Hidden while nothing is staged.
class SaveBar : public Widget {
public:
    std::wstring title = L"Unsaved changes";
    std::wstring detail;  // what Save will do beyond writing the config ("Reallocates the replay buffer.")
    bool failed = false;  // the last Save didn't work: the title takes the danger colour, detail says why
    ButtonControl discard, save;

    SaveBar();
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
};

}  // namespace ombra_ui
