#include "ombra-ui/ui/attached_panel.h"

#include <algorithm>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

CornerShapes AttachedCornerShapes(PanelEdge edge) {
    using S = CornerShape;
    switch (edge) {
        case PanelEdge::Top:    return {S::Concave, S::Concave, S::Convex, S::Convex};
        case PanelEdge::Bottom: return {S::Convex, S::Convex, S::Concave, S::Concave};
        case PanelEdge::Left:   return {S::Concave, S::Convex, S::Convex, S::Concave};
        case PanelEdge::Right:  return {S::Convex, S::Concave, S::Concave, S::Convex};
    }
    return {S::Convex, S::Convex, S::Convex, S::Convex};
}

AttachedPanelGeometry MakeAttachedGeometry(PanelEdge edge, const D2D1_RECT_F& content,
                                           float corner_radius, float bulge_radius) {
    AttachedPanelGeometry g;
    g.edge = edge;
    g.content = content;
    const float w = content.right - content.left;
    const float h = content.bottom - content.top;
    const float cap = (std::max)(0.0f, (std::min)(w, h) * 0.5f);
    g.corner_radius = std::clamp(corner_radius, 0.0f, cap);
    g.bulge_radius = std::clamp(bulge_radius, 0.0f, cap);
    g.bg = content;
    // The overhang runs along the edge, i.e. on the two sides perpendicular to it.
    if (edge == PanelEdge::Top || edge == PanelEdge::Bottom) {
        g.bg.left -= g.bulge_radius;
        g.bg.right += g.bulge_radius;
    } else {
        g.bg.top -= g.bulge_radius;
        g.bg.bottom += g.bulge_radius;
    }
    return g;
}

ComPtr<ID2D1PathGeometry> BuildAttachedPanelPath(ID2D1Factory* factory, const AttachedPanelGeometry& g) {
    if (!factory) return nullptr;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || !sink) return nullptr;

    // Built once in a local frame -- u runs along the docked edge, v runs away from it -- then
    // mapped to the real edge. Written for Top; the other three are reflections/rotations of it.
    const D2D1_RECT_F& c = g.content;
    const bool horiz = (g.edge == PanelEdge::Top || g.edge == PanelEdge::Bottom);
    const float a = horiz ? (c.right - c.left) : (c.bottom - c.top);  // extent along the edge
    const float d = horiz ? (c.bottom - c.top) : (c.right - c.left);  // depth away from it
    const float r = g.corner_radius;
    const float b = g.bulge_radius;

    auto map = [&](float u, float v) -> D2D1_POINT_2F {
        switch (g.edge) {
            case PanelEdge::Top:    return D2D1::Point2F(c.left + u, c.top + v);
            case PanelEdge::Bottom: return D2D1::Point2F(c.left + u, c.bottom - v);
            case PanelEdge::Left:   return D2D1::Point2F(c.left + v, c.top + u);
            case PanelEdge::Right:  return D2D1::Point2F(c.right - v, c.top + u);
        }
        return D2D1::Point2F(0, 0);
    };
    // Bottom and Left are mirror images of the local frame (determinant -1), which flips what
    // "clockwise" means for every arc. Top and Right are pure rotations.
    const bool mirrored = (g.edge == PanelEdge::Bottom || g.edge == PanelEdge::Left);
    const D2D1_SWEEP_DIRECTION cw  = mirrored ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE;
    const D2D1_SWEEP_DIRECTION ccw = mirrored ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    auto arc = [&](float u, float v, float radius, D2D1_SWEEP_DIRECTION dir) {
        sink->AddArc(D2D1::ArcSegment(map(u, v), D2D1::SizeF(radius, radius), 0.0f, dir, D2D1_ARC_SIZE_SMALL));
    };

    sink->BeginFigure(map(-b, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(map(a + b, 0.0f));         // along the screen edge, out to both fillets' feet
    arc(a, b, b, ccw);                        // concave fillet down into the panel's side
    sink->AddLine(map(a, d - r));
    arc(a - r, d, r, cw);                     // convex, away side
    sink->AddLine(map(r, d));
    arc(0.0f, d - r, r, cw);                  // convex, away side
    sink->AddLine(map(0.0f, b));
    arc(-b, 0.0f, b, ccw);                    // concave fillet back out to the screen edge
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return geo;
}

ComPtr<ID2D1Geometry> BuildScreenFramePath(ID2D1Factory* factory, const D2D1_RECT_F& screen,
                                           float thickness, float radius) {
    if (!factory) return nullptr;
    const float t = (std::max)(0.0f, thickness);
    D2D1_RECT_F inner = D2D1::RectF(screen.left + t, screen.top + t, screen.right - t, screen.bottom - t);
    const float cap = (std::min)(inner.right - inner.left, inner.bottom - inner.top) * 0.5f;
    const float r = std::clamp(radius, 0.0f, (std::max)(0.0f, cap));

    ComPtr<ID2D1RectangleGeometry> outer_geo;
    ComPtr<ID2D1RoundedRectangleGeometry> inner_geo;
    if (FAILED(factory->CreateRectangleGeometry(screen, &outer_geo)) || !outer_geo) return nullptr;
    if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(inner, r, r), &inner_geo)) || !inner_geo) {
        return nullptr;
    }
    // Even-odd over {outer, inner}: filled where exactly one contains the point, i.e. the ring.
    ID2D1Geometry* parts[2] = {outer_geo.Get(), inner_geo.Get()};
    ComPtr<ID2D1GeometryGroup> group;
    if (FAILED(factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, parts, 2, &group)) || !group) return nullptr;
    ComPtr<ID2D1Geometry> out;
    group.As(&out);
    return out;
}

}  // namespace ombra_ui
