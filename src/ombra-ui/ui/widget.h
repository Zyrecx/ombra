#pragma once

#include "ombra-ui/ui/layout.h"
#include "ombra-ui/ui/theme.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ombra_ui {

// Values are also used as an index into Theme's SVG icon cache (widget.cpp's GetOrCreateSvgIcon) --
// keep None first and don't renumber existing entries without checking that array's size.
enum class IconType {
    None,
    Save,
    Replay,  // circular arrow -- Home's Instant Replay tile (distinct from
             // Save, which is the literal floppy-disk glyph used elsewhere e.g. a "Save Replay" button)
    Camera,
    Toggle,
    Folder,
    Settings,
    Close,
    Check,
    Alert,
    Back,
    Record, // outlined ring + filled dot -- Home screen's Record tile
    Palette,  // Settings rail: Appearance & Notifications
    Keyboard, // Settings rail: Hotkeys
    Sliders,  // Settings rail: Advanced
    Bell,     // Settings rail: Notifications
    Zap,      // Settings rail: Quick Settings
    // Group icons and Capture-mode cards.
    Gauge,     // row icon: Quality group
    Volume,    // row icon: Audio group
    Monitor,   // Quick Settings' Capture-mode card: Desktop
    Gamepad2,  // Quick Settings' Capture-mode card: Game
    Eye,       // Monitors grid tile's "hide from this list" overlay button
    Mic,
    Plus,      // "Playing audio now" card: give this app its own track
    // Clips screen
    Play,
    Pause,
    Trash,
    Copy,
    Film,
    VolumeOff,
    Scissors,
    Power,     // Setup: Start with Windows
    Bug        // Setup: the Reports step
};
constexpr int kIconTypeCount = static_cast<int>(IconType::Bug) + 1;

// Mirrors ombra::ApplyTier (config.h) without pulling the engine header into the UI layer --
// screens.cpp maps the schema's "apply" string onto this.
enum class ApplyKind {
    Free,
    Rebuild,
    Realloc
};

// Shared rounded-rect/icon draw primitives (widget.cpp) -- exposed here so toast_window.cpp's
// non-interactive popup card can reuse the same visuals instead of a duplicate copy. Every other
// widget.cpp draw helper stays file-local; these are the only ones crossing the file boundary.
void DrawRoundedBox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius,
                    ID2D1Brush* fill, ID2D1Brush* border = nullptr, float border_width = 1.0f);
void DrawIcon(ID2D1DeviceContext* dc, const Theme& theme, IconType icon, float cx, float cy, float size,
             ID2D1Brush* brush, const D2D1_COLOR_F* dot_color = nullptr);
// "640 MB", "1.25 GB", "214.3 GB": the Memory card's and the disk buffer's sizes.
std::wstring FormatMemory(double bytes);
// "Today 23:29", "Yesterday 21:17", "Oct 12 21:17", "Oct 12 2025" for another year; local time.
std::wstring FormatWhen(const FILETIME& written);
std::wstring FormatWhen(int64_t unix_seconds);


// What the pointer should look like over a spot: a hand over anything a click acts on, a move
// cursor over something you drag. The overlay asks after every mouse move (OverlayHost::UpdateCursor).
enum class CursorKind { Arrow, Hand, Move, IBeam };

// What an audio source is doing right now (the engine's audio_status, AudioSourceState). None: no
// report for this card (engine down, a source not saved yet, a live-only session) -- no tag drawn.
enum class AudioSourceStatus { None, Live, Quiet, Waiting, NoGame, Failed, Off, Paused };

inline bool PointIn(const D2D1_RECT_F& r, D2D1_POINT_2F pt) {
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

class Widget {
public:
    virtual ~Widget() = default;

    std::string id;
    D2D1_RECT_F bounds = D2D1::RectF(0, 0, 0, 0);
    bool visible = true;
    bool enabled = true;
    bool hovered = false;
    bool focused = false;
    int  tab_index = -1;

    virtual void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) = 0;

    // Layout protocol (ui/layout.h). Measure() answers "how big do you want to be inside these
    // constraints" -- a widget placed in a container MUST override it (the default answers 0 tall,
    // which is never right for something that draws). Arrange() receives the final ABSOLUTE rect;
    // leaf widgets just take it as `bounds`, containers place their children inside it.
    virtual D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx);
    virtual void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) { (void)ctx; bounds = rect; }
    // Hover tooltip, if this widget (or a child) is currently showing one. `anchor` is in the same
    // absolute, unscrolled space as `bounds`; the caller adjusts for scrolling.
    virtual bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const { (void)text; (void)anchor; return false; }

    virtual bool HitTest(D2D1_POINT_2F pt) const;
    // Arrow unless the widget acts on a click at `pt`. Containers ask their children, in the same
    // order they route OnMouseDown.
    virtual CursorKind CursorAt(D2D1_POINT_2F pt) const { (void)pt; return CursorKind::Arrow; }
    virtual bool OnMouseMove(D2D1_POINT_2F pt);
    virtual bool OnMouseDown(D2D1_POINT_2F pt);
    virtual bool OnMouseUp(D2D1_POINT_2F pt);
    virtual bool OnKeyDown(UINT vk);
};

class ScrollAreaWidget : public Widget {
public:
    CursorKind CursorAt(D2D1_POINT_2F pt) const override {  // children are in unscrolled space
        if (!visible || !PointIn(bounds, pt)) return CursorKind::Arrow;
        const D2D1_POINT_2F adj = D2D1::Point2F(pt.x, pt.y + scroll_y);
        for (const auto& c : children) {
            if (c && c->CursorAt(adj) != CursorKind::Arrow) return c->CursorAt(adj);
        }
        return CursorKind::Arrow;
    }
    float scroll_y = 0.0f;
    float content_height = 0.0f;
    float scrollbar_room_dip = 14.0f;  // right strip the children leave free; the thumb is centred in it
    std::vector<std::shared_ptr<Widget>> children;

    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) override;
    bool HitTest(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    void OnMouseWheel(float delta);
};

// Button labels for a modal. An empty `cancel` makes it a one-button acknowledgment: no cancel
// button is drawn and Esc/Enter both take the confirm action.
struct ModalButtons {
    std::wstring confirm = L"Confirm";
    std::wstring cancel  = L"Cancel";
};

}  // namespace ombra_ui
