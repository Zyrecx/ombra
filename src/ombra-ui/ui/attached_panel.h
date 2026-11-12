#pragma once

// Attached-panel + screen-frame geometry.
//
// The construction is ported from Noctalia's shell/panel/attached_panel_context.h (MIT,
// github.com/noctalia-dev/noctalia-shell): a panel docked to a screen edge has CONCAVE corners on
// the edge side and CONVEX corners on the away side, and its background overhangs its logical
// content by `bulge_radius` along the edge. That overhang is what the concave fillet is carved
// from -- without it there is nothing to scoop, and panel + edge read as two overlapping rects
// instead of one silhouette.
//
// Pure geometry, no device: everything here builds against a bare ID2D1Factory, so it is
// headless-testable (tests/ui_tests.cpp). Nothing here draws.

#include <d2d1_1.h>
#include <wrl/client.h>

namespace ombra_ui {

enum class PanelEdge { Top, Bottom, Left, Right };

enum class CornerShape { Convex, Concave };

struct CornerShapes {
    CornerShape tl, tr, br, bl;
};

// Which corners are concave for a panel docked to `edge` (Noctalia's attached_panel::cornerShapes).
CornerShapes AttachedCornerShapes(PanelEdge edge);

struct AttachedPanelGeometry {
    PanelEdge   edge = PanelEdge::Top;
    D2D1_RECT_F content{};   // the panel proper: what layout and hit-testing see
    D2D1_RECT_F bg{};        // background bounds: content plus the concave overhang on the edge side
    // Two radii on purpose (Noctalia keeps them apart so the open/close reveal can animate the
    // bulge alone). Nothing animates yet, so callers pass the same value for both -- but don't
    // collapse them into one field, that turns a later reveal into a refactor.
    float corner_radius = 0.0f;  // away-side convex corners
    float bulge_radius  = 0.0f;  // edge-side concave fillets
};

// `content` must already touch the screen edge on `edge`'s side. Radii are clamped so they can't
// exceed half the content's smaller dimension.
AttachedPanelGeometry MakeAttachedGeometry(PanelEdge edge, const D2D1_RECT_F& content,
                                           float corner_radius, float bulge_radius);

// Closed silhouette of the panel including the two concave fillets. Null on failure.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> BuildAttachedPanelPath(ID2D1Factory* factory,
                                                                 const AttachedPanelGeometry& g);

// The screen frame (Noctalia's screen_corner_node, plus an optional border): everything inside
// `screen` but outside a rounded rect inset by `thickness` with corner radius `radius`. With
// thickness 0 this is just the four corner spandrels, i.e. the physical screen corners rounded off.
// Null on failure.
Microsoft::WRL::ComPtr<ID2D1Geometry> BuildScreenFramePath(ID2D1Factory* factory,
                                                           const D2D1_RECT_F& screen,
                                                           float thickness, float radius);

}  // namespace ombra_ui
