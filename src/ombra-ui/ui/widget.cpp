#include "ombra-ui/ui/widget.h"
#include "ombra-ui/ui/icons_svg.h"
#include "ombra-ui/ui/paint.h"
#include "ombra/log.h"

#include <wrl/client.h>
#include <shlwapi.h>  // SHCreateMemStream -- wraps an embedded icon SVG's XML bytes as an IStream
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <algorithm>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

// Shared with toast_window.cpp (a short-lived popup process with no widget/hover/click machinery
// of its own) -- declared in widget.h so both draw call sites can reuse the same rounded-rect/icon
// primitives instead of a duplicate copy. Everything else in this file's helper block stays
// file-local; only these two needed to cross the file boundary.
void DrawRoundedBox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius,
                    ID2D1Brush* fill, ID2D1Brush* border, float border_width) {
    if (!dc) return;
    const float clamped_r = (std::min)(radius, (std::min)(rect.right - rect.left, rect.bottom - rect.top) * 0.5f);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, clamped_r, clamped_r);
    if (fill) dc->FillRoundedRectangle(&rr, fill);
    if (border && border_width > 0.0f) dc->DrawRoundedRectangle(&rr, border, border_width);
}

namespace {

ComPtr<ID2D1SolidColorBrush> Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& col) {
    ComPtr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(col, &b);
    return b;
}

// Lazily creates (and caches on `theme`, see theme.h's ResetIconCache comment) the SVG document
// for `icon`. Returns nullptr -- and the caller must fall back to the hand-drawn switch below --
// on anything from "pre-1709 Windows, no ID2D1DeviceContext5" to "malformed SVG"; this codebase's
// standing posture is that one bad icon shouldn't blank a tile.
ID2D1SvgDocument* GetOrCreateSvgIcon(ID2D1DeviceContext* dc, const Theme& theme, IconType icon) {
    const int idx = static_cast<int>(icon);
    if (idx < 0) return nullptr;

    if (!theme.svg_ctx5) {
        dc->QueryInterface(IID_PPV_ARGS(&theme.svg_ctx5));
        if (!theme.svg_ctx5) return nullptr;
    }
    if (static_cast<size_t>(idx) >= theme.svg_icon_cache.size()) {
        theme.svg_icon_cache.resize(idx + 1);
    }
    if (theme.svg_icon_cache[idx]) return theme.svg_icon_cache[idx].Get();

    const char* xml = IconSvgSource(icon);
    if (!xml) return nullptr;

    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(xml), static_cast<UINT>(strlen(xml))));
    if (!stream) return nullptr;

    ComPtr<ID2D1SvgDocument> doc;
    HRESULT hr = theme.svg_ctx5->CreateSvgDocument(stream.Get(), D2D1::SizeF(24.0f, 24.0f), &doc);
    if (FAILED(hr) || !doc) {
        ombra::Log::Warn("GetOrCreateSvgIcon: CreateSvgDocument failed for icon %d (hr=0x%08lX)", idx, hr);
        return nullptr;
    }
    theme.svg_icon_cache[idx] = doc;
    return doc.Get();
}

void ColorToSvgHex(const D2D1_COLOR_F& color, wchar_t (&out)[16]) {
    swprintf_s(out, L"#%02X%02X%02X",
              static_cast<int>(color.r * 255.0f + 0.5f),
              static_cast<int>(color.g * 255.0f + 0.5f),
              static_cast<int>(color.b * 255.0f + 0.5f));
}

// Draws icon's cached SVG document, tinted to `brush`'s color via the root element's `stroke`
// attribute (see icons_svg.h's provenance comment for why this can't just be baked into the SVG).
// If the icon has an `id="dot"` sub-element (Replay, Record -- icons_svg.h), it's separately tinted
// to `dot_color` (or `brush`'s color, matching the rest of the icon, if `dot_color` is null) via
// its own `fill` attribute -- this is what lets Record's center dot turn red independently of its
// ring once `dot_active` is set (ActionTileControl, containers.cpp).
// Returns false if there's no usable SVG for this icon, so the caller can fall back.
bool DrawSvgIcon(ID2D1DeviceContext* dc, const Theme& theme, IconType icon, float cx, float cy,
                 float size, ID2D1Brush* brush, const D2D1_COLOR_F* dot_color) {
    ID2D1SvgDocument* doc = GetOrCreateSvgIcon(dc, theme, icon);
    if (!doc) return false;

    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    if (!root) return false;

    D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White);
    ComPtr<ID2D1SolidColorBrush> solid;
    if (SUCCEEDED(brush->QueryInterface(IID_PPV_ARGS(&solid))) && solid) {
        color = solid->GetColor();
    }
    wchar_t hex[16];
    ColorToSvgHex(color, hex);
    root->SetAttributeValue(L"stroke", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, hex);

    ComPtr<ID2D1SvgElement> dot;
    if (SUCCEEDED(doc->FindElementById(L"dot", &dot)) && dot) {
        wchar_t dot_hex[16];
        ColorToSvgHex(dot_color ? *dot_color : color, dot_hex);
        dot->SetAttributeValue(L"fill", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, dot_hex);
    }

    D2D1_MATRIX_3X2_F old_xf;
    dc->GetTransform(&old_xf);
    const float scale = size / 24.0f;
    // The icon's corner on a whole device pixel: off the grid, a symmetric glyph anti-aliases
    // unevenly (Close's "\" came out sharp and its "/" soft, one arm a pixel longer; 2026-11-06).
    float x0 = cx - size * 0.5f, y0 = cy - size * 0.5f;
    if (old_xf._11 == 1.0f && old_xf._22 == 1.0f && old_xf._12 == 0.0f && old_xf._21 == 0.0f) {
        x0 = std::round(x0 + old_xf.dx) - old_xf.dx;
        y0 = std::round(y0 + old_xf.dy) - old_xf.dy;
    }
    D2D1_MATRIX_3X2_F icon_xf = D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x0, y0);
    dc->SetTransform(icon_xf * old_xf);
    theme.svg_ctx5->DrawSvgDocument(doc);
    dc->SetTransform(old_xf);
    return true;
}

}  // namespace

void DrawIcon(ID2D1DeviceContext* dc, const Theme& theme, IconType icon, float cx, float cy, float size,
             ID2D1Brush* brush, const D2D1_COLOR_F* dot_color) {
    if (!dc || !brush || icon == IconType::None) return;
    if (DrawSvgIcon(dc, theme, icon, cx, cy, size, brush, dot_color)) return;

    const float hs = size * 0.5f;
    switch (icon) {
        case IconType::Save: {
            D2D1_RECT_F body = D2D1::RectF(cx - hs, cy - hs, cx + hs, cy + hs);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(body, 1.5f, 1.5f), brush, 1.6f);
            D2D1_RECT_F top = D2D1::RectF(cx - hs * 0.45f, cy - hs, cx + hs * 0.45f, cy - hs * 0.15f);
            dc->FillRectangle(&top, brush);
            break;
        }
        case IconType::Replay: {
            // Fallback only (SVG unavailable) -- a ~300-degree ring standing in for a circular
            // "replay" arrow, same arc-geometry technique DrawNavBar's connected-corner uses.
            ComPtr<ID2D1Factory> factory;
            dc->GetFactory(&factory);
            ComPtr<ID2D1PathGeometry> geo;
            if (factory && SUCCEEDED(factory->CreatePathGeometry(&geo)) && geo) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(geo->Open(&sink)) && sink) {
                    const float r = hs * 0.85f;
                    const float start_ang = -0.4f, sweep = 5.0f;
                    D2D1_POINT_2F p0 = D2D1::Point2F(cx + r * std::cos(start_ang), cy + r * std::sin(start_ang));
                    D2D1_POINT_2F p1 = D2D1::Point2F(cx + r * std::cos(start_ang + sweep), cy + r * std::sin(start_ang + sweep));
                    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    dc->DrawGeometry(geo.Get(), brush, 1.6f);
                }
            }
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), hs * 0.12f, hs * 0.12f), brush);
            break;
        }
        case IconType::Camera: {
            D2D1_RECT_F body = D2D1::RectF(cx - hs, cy - hs * 0.45f, cx + hs, cy + hs * 0.85f);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.0f, 2.0f), brush, 1.6f);
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + hs * 0.2f), hs * 0.4f, hs * 0.4f), brush, 1.6f);
            break;
        }
        case IconType::Toggle: {
            D2D1_RECT_F left = D2D1::RectF(cx - hs * 0.7f, cy - hs, cx - hs * 0.2f, cy + hs);
            D2D1_RECT_F right = D2D1::RectF(cx + hs * 0.2f, cy - hs, cx + hs * 0.7f, cy + hs);
            dc->FillRectangle(&left, brush);
            dc->FillRectangle(&right, brush);
            break;
        }
        case IconType::Folder: {
            D2D1_RECT_F body = D2D1::RectF(cx - hs, cy - hs * 0.35f, cx + hs, cy + hs * 0.85f);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.0f, 2.0f), brush, 1.6f);
            D2D1_RECT_F tab = D2D1::RectF(cx - hs, cy - hs * 0.75f, cx - hs * 0.15f, cy - hs * 0.35f);
            dc->FillRectangle(&tab, brush);
            break;
        }
        case IconType::Settings: {
            float r_outer = hs * 0.55f;
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r_outer, r_outer), brush, 2.5f);
            for (int i = 0; i < 6; ++i) {
                float ang = static_cast<float>(i) * 3.14159265f / 3.0f;
                float x0 = cx + std::cos(ang) * r_outer, y0 = cy + std::sin(ang) * r_outer;
                float x1 = cx + std::cos(ang) * hs * 0.9f, y1 = cy + std::sin(ang) * hs * 0.9f;
                dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, 3.0f);
            }
            break;
        }
        case IconType::Close: {
            dc->DrawLine(D2D1::Point2F(cx - hs * 0.7f, cy - hs * 0.7f),
                         D2D1::Point2F(cx + hs * 0.7f, cy + hs * 0.7f), brush, 1.6f);
            dc->DrawLine(D2D1::Point2F(cx + hs * 0.7f, cy - hs * 0.7f),
                         D2D1::Point2F(cx - hs * 0.7f, cy + hs * 0.7f), brush, 1.6f);
            break;
        }
        case IconType::Plus: {
            dc->DrawLine(D2D1::Point2F(cx - hs * 0.8f, cy), D2D1::Point2F(cx + hs * 0.8f, cy), brush, 1.6f);
            dc->DrawLine(D2D1::Point2F(cx, cy - hs * 0.8f), D2D1::Point2F(cx, cy + hs * 0.8f), brush, 1.6f);
            break;
        }
        case IconType::Check: {
            dc->DrawLine(D2D1::Point2F(cx - hs * 0.7f, cy),
                         D2D1::Point2F(cx - hs * 0.1f, cy + hs * 0.6f), brush, 2.0f);
            dc->DrawLine(D2D1::Point2F(cx - hs * 0.1f, cy + hs * 0.6f),
                         D2D1::Point2F(cx + hs * 0.8f, cy - hs * 0.6f), brush, 2.0f);
            break;
        }
        case IconType::Alert: {
            dc->DrawLine(D2D1::Point2F(cx, cy - hs * 0.8f), D2D1::Point2F(cx + hs, cy + hs * 0.8f), brush, 1.6f);
            dc->DrawLine(D2D1::Point2F(cx + hs, cy + hs * 0.8f), D2D1::Point2F(cx - hs, cy + hs * 0.8f), brush, 1.6f);
            dc->DrawLine(D2D1::Point2F(cx - hs, cy + hs * 0.8f), D2D1::Point2F(cx, cy - hs * 0.8f), brush, 1.6f);
            break;
        }
        case IconType::Record: {
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), hs * 0.85f, hs * 0.85f), brush, 1.6f);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), hs * 0.42f, hs * 0.42f), brush);
            break;
        }
        case IconType::Back: {
            dc->DrawLine(D2D1::Point2F(cx + hs * 0.5f, cy - hs * 0.7f),
                         D2D1::Point2F(cx - hs * 0.35f, cy), brush, 1.8f);
            dc->DrawLine(D2D1::Point2F(cx - hs * 0.35f, cy),
                         D2D1::Point2F(cx + hs * 0.5f, cy + hs * 0.7f), brush, 1.8f);
            break;
        }
        default:
            break;
    }
}


// --- Widget Base ---------------------------------------------------------------------------------

D2D1_SIZE_F Widget::Measure(const LayoutConstraints& c, const LayoutContext&) {
    return D2D1::SizeF(c.BoundedWidth() ? c.max_w : c.min_w, c.min_h);
}

bool Widget::HitTest(D2D1_POINT_2F pt) const {
    if (!visible || !enabled) return false;
    return (pt.x >= bounds.left && pt.x <= bounds.right &&
            pt.y >= bounds.top  && pt.y <= bounds.bottom);
}

bool Widget::OnMouseMove(D2D1_POINT_2F pt) {
    const bool was_hover = hovered;
    hovered = HitTest(pt);
    return was_hover != hovered;
}

bool Widget::OnMouseDown(D2D1_POINT_2F pt) {
    if (HitTest(pt)) {
        focused = true;
        return true;
    }
    focused = false;
    return false;
}

bool Widget::OnMouseUp(D2D1_POINT_2F) {
    return false;
}

bool Widget::OnKeyDown(UINT) {
    return false;
}


// --- ScrollAreaWidget ----------------------------------------------------------------------------

void ScrollAreaWidget::Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) {
    if (!visible) return;

    D2D1_RECT_F clip = bounds;
    dc->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const size_t probe_first = g_draw_probe ? g_draw_probe->texts.size() : 0;

    D2D1_MATRIX_3X2_F old_tf;
    dc->GetTransform(&old_tf);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -scroll_y) * old_tf);

    for (auto& child : children) {
        if (child && child->visible) {
            if (child->bounds.bottom - scroll_y >= bounds.top && child->bounds.top - scroll_y <= bounds.bottom) {
                child->Draw(dc, theme, dpi_scale);
            }
        }
    }

    dc->SetTransform(old_tf);
    dc->PopAxisAlignedClip();
    if (g_draw_probe) g_draw_probe->ClipSince(probe_first, dc, clip);

    // Centred in the strip the page leaves free on its right.
    const paint::Paint p{dc, theme, dpi_scale};
    p.ScrollThumb(bounds.right - scrollbar_room_dip * 0.5f * dpi_scale, bounds.top, bounds.bottom - bounds.top, content_height,
                  scroll_y);
}

bool ScrollAreaWidget::HitTest(D2D1_POINT_2F pt) const {
    return Widget::HitTest(pt);
}

bool ScrollAreaWidget::OnMouseMove(D2D1_POINT_2F pt) {
    D2D1_POINT_2F adj_pt = pt;
    adj_pt.y += scroll_y;
    for (auto& child : children) {
        if (child) child->OnMouseMove(adj_pt);
    }
    return true;
}

// Mouse and key routing walks a copy: a control's change can rebuild the page under it (every
// staged settings edit does), which would free the card mid-call and invalidate the loop.
bool ScrollAreaWidget::OnMouseDown(D2D1_POINT_2F pt) {
    D2D1_POINT_2F adj_pt = pt;
    adj_pt.y += scroll_y;
    const auto kids = children;
    for (auto& child : kids) {
        if (child && child->OnMouseDown(adj_pt)) return true;
    }
    return false;
}

bool ScrollAreaWidget::OnMouseUp(D2D1_POINT_2F pt) {
    D2D1_POINT_2F adj_pt = pt;
    adj_pt.y += scroll_y;
    const auto kids = children;
    for (auto& child : kids) {
        if (child) child->OnMouseUp(adj_pt);
    }
    return false;
}

bool ScrollAreaWidget::OnKeyDown(UINT vk) {
    const auto kids = children;
    for (auto& child : kids) {
        if (child && child->OnKeyDown(vk)) return true;
    }
    return false;
}

void ScrollAreaWidget::OnMouseWheel(float delta) {
    const float view_h = bounds.bottom - bounds.top;
    const float max_scroll = (std::max)(0.0f, content_height - view_h);
    scroll_y = std::clamp(scroll_y - delta * 40.0f, 0.0f, max_scroll);
}


std::wstring FormatMemory(double bytes) {
    wchar_t buf[32];
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 10240.0) swprintf(buf, std::size(buf), L"%.1f GB", mb / 1024.0);
    else if (mb >= 1024.0) swprintf(buf, std::size(buf), L"%.2f GB", mb / 1024.0);
    else swprintf(buf, std::size(buf), L"%.0f MB", mb);
    return buf;
}

// "Today 23:29", "Yesterday 21:17", "Oct 12 21:17", "Oct 12 2025" for another year.
std::wstring FormatWhen(const FILETIME& written) {
    FILETIME local_ft;
    SYSTEMTIME t, now;
    FileTimeToLocalFileTime(&written, &local_ft);
    FileTimeToSystemTime(&local_ft, &t);
    GetLocalTime(&now);
    auto day_number = [](const SYSTEMTIME& st) {
        FILETIME ft;
        SYSTEMTIME d = st;
        d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
        SystemTimeToFileTime(&d, &ft);
        return ((uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime) / (10'000'000ull * 86400);
    };
    static const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                       L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    const uint64_t days_ago = day_number(now) - day_number(t);
    wchar_t buf[48];
    if (days_ago == 0) swprintf_s(buf, L"Today %02u:%02u", t.wHour, t.wMinute);
    else if (days_ago == 1) swprintf_s(buf, L"Yesterday %02u:%02u", t.wHour, t.wMinute);
    else if (t.wYear == now.wYear) swprintf_s(buf, L"%ls %u %02u:%02u", kMonths[(t.wMonth + 11) % 12], t.wDay, t.wHour, t.wMinute);
    else swprintf_s(buf, L"%ls %u %u", kMonths[(t.wMonth + 11) % 12], t.wDay, t.wYear);
    return buf;
}

std::wstring FormatWhen(int64_t unix_seconds) {
    const ULONGLONG ticks = static_cast<ULONGLONG>(unix_seconds) * 10000000ull + 116444736000000000ull;
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    return FormatWhen(ft);
}

}  // namespace ombra_ui
