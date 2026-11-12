#pragma once

// Layout protocol. Shape ported from Noctalia's
// render/scene/node.h LayoutConstraints (MIT, github.com/noctalia-dev/noctalia-shell).
//
// Two passes: Measure() asks a widget how big it wants to be inside some constraints (bottom-up),
// Arrange() then hands it its final rect (top-down). Widgets no longer position themselves from
// hardcoded per-call-site numbers, and a *container* decides where its children go -- which is what
// the old row widget's grid_* fields and RebuildGroupRows' packer were faking.
//
// Everything is in device pixels once it reaches Measure/Arrange (callers scale DIP inputs by
// LayoutContext::dpi_scale). Bounds stay ABSOLUTE, never parent-relative: ScrollAreaWidget only
// subtracts scroll_y at draw time and hit-tests the same way, and every widget's cached rects
// assume it. Arrange(rect) therefore takes an absolute rect.

namespace ombra_ui {

struct Theme;

inline constexpr float kUnbounded = 1.0e9f;

struct LayoutConstraints {
    float min_w = 0.0f;
    float max_w = kUnbounded;
    float min_h = 0.0f;
    float max_h = kUnbounded;

    // Room to grow up to (w, h); either may stay unbounded.
    static LayoutConstraints Available(float w, float h = kUnbounded) {
        LayoutConstraints c;
        c.max_w = w;
        c.max_h = h;
        return c;
    }
    // The parent has already decided this widget's size; it just wants the other axis's answer.
    static LayoutConstraints Exact(float w, float h = kUnbounded) {
        LayoutConstraints c;
        c.min_w = c.max_w = w;
        c.min_h = 0.0f;
        c.max_h = h;
        return c;
    }
    bool BoundedWidth() const { return max_w < kUnbounded * 0.5f; }
};

struct LayoutContext {
    float dpi_scale = 1.0f;
    // Only needed by widgets that size themselves off real text metrics (Theme::MeasureText).
    // Null in headless callers -- such a widget must fall back to a fixed size.
    const Theme* theme = nullptr;
};

}  // namespace ombra_ui
