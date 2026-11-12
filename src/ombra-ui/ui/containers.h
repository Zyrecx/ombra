#pragma once

// Containers: what holds the controls. Same rules as
// controls.h: new tokens only, every state settable directly so the gallery can draw it.
//
// Anatomy follows the WinUI Community Toolkit's SettingsCard/SettingsExpander (MIT,
// github.com/CommunityToolkit/Windows, components/SettingsControls): a row is text on the left
// (title, help under it) and its control on the right, and the control wraps under the text when
// the row is too narrow for both. Sizes are ours (Theme::size), not the toolkit's.

#include "ombra-ui/ui/controls.h"
#include "ombra-ui/ui/file_icon.h"

#include <memory>
#include <string>
#include <vector>

namespace ombra_ui {

// One setting, or one item of a list (a game, an audio source): title, optional help, and its
// controls at the trailing edge (left to right, `space_sm` apart).
class SettingRow : public Widget {
public:
    enum class Tone { Neutral, Good, Warning, Error };
    std::wstring title, help;
    Tone help_tone = Tone::Neutral;  // a live help line (a shortcut refusing a key) can warn
    bool help_one_line = false;      // help that changes in place: one line, trimmed, so the row keeps its height
    bool align_top = false;          // a tall control (the file-name field): text and controls share a top line
    // The controls always go under the text, measured against the text column's full width (the
    // theme tiles), so the title and help keep the whole line.
    bool controls_below = false;
    // A small pill after the title ("Also NVIDIA"), its full sentence in a tooltip.
    std::wstring tag, tag_tooltip;
    // A status line under the help: a dot in its tone, then the text (Error and Warning in theirs).
    std::wstring note;
    Tone note_tone = Tone::Neutral;
    std::vector<std::shared_ptr<Widget>> controls;  // a switch too: right edge
    // A display under the text, at the text's left edge (an audio source's level meter): drawn,
    // never clicked; trimmed to the text column.
    std::shared_ptr<Widget> below;
    bool divider_above = false;  // set by CardContainer between rows
    // A row that can be dragged to another card (an audio source to another track): six dots before
    // the text. `lifted`: it is being dragged, so its place is drawn faded under the moving copy.
    bool grip = false;
    bool lifted = false;
    std::function<void()> on_grip;  // a press on the grip starts the drag
    D2D1_RECT_F GripRect() const;  // pixels, last Arrange; empty without a grip
    // A list item's picture before the text (a game): the program's own icon, else `glyph`; none
    // when both are empty. `faded`: the item is switched off (not a game), so the picture pales.
    std::shared_ptr<const FileIcon> icon;
    IconType glyph = IconType::None;
    bool faded = false;
    // A row waiting for an answer (an app that only looked like a game): a warning tint behind it.
    bool attention = false;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const override;

private:
    // Everything Measure and Arrange share, in pixels, for a row `w` wide.
    struct Geometry {
        float height = 0, text_x = 0, text_w = 0, title_h = 0, help_h = 0, note_h = 0, below_h = 0, controls_w = 0, controls_h = 0;
        bool stacked = false;   // controls under the text
        // Stacked: each visible control's box, x from the row's left and y from the band's top;
        // lines wrap when the controls don't fit the row's width.
        std::vector<D2D1_RECT_F> stacked_at;
        const Widget* pinned = nullptr;  // stacked: a trailing switch kept on the title's line
        D2D1_SIZE_F pinned_size{};
        float TextHeight(const Sizes& z, float s) const;
    };
    Geometry Layout(float w, const LayoutContext& ctx) const;
    bool HasPicture() const { return (icon && !icon->bgra.empty()) || glyph != IconType::None; }
    Geometry g_;
    D2D1_RECT_F tag_rect_{};  // last Draw's
    bool tag_hovered_ = false;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> icon_bitmap_;  // `icon` on this device, built on the first Draw
};

// A titled group of rows: one card, a line between rows.
// `actions` sit at the trailing end of the title line (a list's Add/Edit), right to left order kept.
class CardContainer : public Widget {
public:
    std::wstring title, help;  // empty title: no header
    std::vector<std::shared_ptr<Widget>> actions;
    std::vector<std::shared_ptr<Widget>> rows;
    bool drop_target = false;  // a dragged row is over it: accent tint and outline, as a chosen tile

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const override;

private:
    float HeaderHeight(float w, const LayoutContext& ctx) const;  // pixels, 0 without a title
    float TitleLine(const LayoutContext& ctx) const;              // title or actions, the taller
    float ActionsWidth(const LayoutContext& ctx) const;
};

// A card's rows in a box of their own that scrolls, under a header that doesn't. On Games only the
// list moves; the card's title and the Save bar stay put. Put it in a CardContainer
// as its one row. The caller sizes the box (`viewport_h_dip`) and keeps the
// offset (`scroll_y` in, `on_scroll` out): a switch stages an edit, the page rebuilds, and a fresh
// list would snap back to the top. Rows outside the box get no input.
class RowList : public Widget {
public:
    std::vector<std::shared_ptr<SettingRow>> rows;
    std::wstring empty_text;           // one row's worth of muted text when `rows` is empty
    float viewport_h_dip = -1.0f;      // < 0: every row, no scrolling
    float scroll_y = 0.0f;             // pixels, clamped by Arrange
    std::function<void(float)> on_scroll;

    // Every row's height at the last Measure (pixels), and the first `n` rows' -- the smallest box
    // worth shrinking to.
    float ContentHeight() const { return content_h_; }
    float LeadHeight(size_t n) const;
    // A wheel over the box with somewhere to go; false leaves it to the page.
    bool OnWheel(D2D1_POINT_2F pt, float delta);

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const override;

private:
    float MaxScroll() const { return (std::max)(0.0f, content_h_ - (bounds.bottom - bounds.top)); }
    D2D1_POINT_2F Inside(D2D1_POINT_2F pt) const;  // `pt`, or far away when it's outside the box
    std::vector<float> heights_;  // per row, last Measure
    float content_h_ = 0.0f;
    float empty_h_ = 0.0f;
    LayoutContext ctx_{};  // last Arrange's, so a wheel can move the rows itself
};

// What left the PC, under the sharing level: a heading with Open folder, then one row per record
// (kind icon, title, summary; time and a status dot at the trailing edge). A click on a row opens it
// in place to the start of its text, in a box that scrolls, with Copy and Open in Notepad under it;
// one row open at a time. The screen keeps the data, the open row and the box's offset, so a rebuild
// (a level tile clicked) leaves them where they were. Put it in a CardContainer as a row.
class SentList : public Widget {
public:
    struct Item {
        IconType icon = IconType::None;
        std::wstring title, summary, when, status;
        SettingRow::Tone tone = SettingRow::Tone::Neutral;  // the status dot; Warning and Error colour the text too
    };
    std::wstring heading, help, empty_text;
    std::vector<Item> items;
    int open = -1;
    std::wstring open_text;  // the open row's first lines
    std::wstring open_note;  // under its box, beside the buttons
    float text_scroll = 0.0f;  // pixels, clamped by Arrange
    std::function<void(int)> on_toggle;  // a row was clicked: open it, or close it if it's the open one
    std::function<void(float)> on_scroll;
    std::shared_ptr<ButtonControl> folder = std::make_shared<ButtonControl>();   // in the heading; hidden when empty
    std::shared_ptr<ButtonControl> copy = std::make_shared<ButtonControl>();     // under the open box
    std::shared_ptr<ButtonControl> notepad = std::make_shared<ButtonControl>();

    // A wheel over the open box with somewhere to go; false leaves it to the page.
    bool OnWheel(D2D1_POINT_2F pt, float delta);
    D2D1_RECT_F RowRect(int i) const { return i >= 0 && i < static_cast<int>(rows_.size()) ? rows_[i] : D2D1_RECT_F{}; }

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const override;

private:
    struct Geometry {  // pixels from the widget's top, for a width
        float head_h = 0, help_h = 0, row_h = 0, box_h = 0, text_h = 0, foot_h = 0, open_h = 0, height = 0;
    };
    Geometry Layout(float w, const LayoutContext& ctx) const;
    float MaxScroll() const { return (std::max)(0.0f, g_.text_h - (box_.bottom - box_.top - 2 * box_pad_)); }
    std::vector<Widget*> Buttons() const;
    Geometry g_;
    std::vector<D2D1_RECT_F> rows_;  // each row's clickable band, last Arrange
    D2D1_RECT_F box_{}, note_{};     // the open row's text box and the note beside its buttons
    float box_pad_ = 0.0f, s_ = 1.0f;
    int hovered_row_ = -1;
};

// Where a dragged row can land to start a card of its own ("Drop here for its own track"): a dashed
// outline with a centred caption, shown only while dragging; `active` while the row is over it.
class DropSlot : public Widget {
public:
    std::wstring label;
    bool active = false;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
};

// A selectable picture: a monitor, a GPU, a quality preset. Chosen: accent tint and outline.
// `multi` tiles belong to a set where several can be chosen (monitors) and always show
// a check circle; single-choice tiles don't.
class TileControl : public Control {
public:
    std::wstring title, detail;
    bool selected = false;
    bool multi = false;
    uint32_t thumb_from = 0, thumb_to = 0;  // 0xRRGGBB gradient standing in for a capture
                                            // thumbnail (the gallery)
    // The capture itself (a display, a window): BGRA, aspect-fit into the 16:9 box. The bitmap is
    // built on the first Draw and the bytes dropped, so a tile holds one or the other.
    std::vector<uint8_t> thumb_bgra;
    uint32_t thumb_w = 0, thumb_h = 0;
    bool picture = false;  // a 16:9 box even before its thumbnail arrives (a camera glyph waits there)
    // Something that isn't a capture, centred in the picture (a running game): `picture_icon` when
    // it has pixels, else this glyph. Needs `picture`.
    IconType picture_glyph = IconType::None;
    std::shared_ptr<const FileIcon> picture_icon;
    // Pale picture and text, the chosen outline kept: a saved window that isn't running.
    // Still clickable, unlike `enabled = false`.
    bool dimmed = false;
    IconType title_icon = IconType::None;  // before the title: a game's controller
    // Under the detail: small pills (a GPU's codecs), then a status line with a dot in its tone, as
    // SettingRow's note. Either adds no height when empty.
    std::vector<std::wstring> chips;
    std::wstring note;
    SettingRow::Tone note_tone = SettingRow::Tone::Neutral;
    // A labelled pill in the picture's top-left corner, on the hovered tile only ("Mark as game").
    std::wstring action_label;
    std::function<void()> on_action;
    // "Hide from this list": an eye button in the picture's top-left corner while hovered.
    std::function<void()> on_hide;
    std::function<void(bool)> on_change;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    bool HasPicture() const { return picture || thumb_from || thumb_to || !thumb_bgra.empty() || bitmap_; }
    static float ChipHeight(const Sizes& z, float s);
    float ExtrasHeight(const Sizes& z, float s) const;  // pixels: chips and note, 0 without them
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_, icon_bitmap_;
    D2D1_RECT_F action_rect_{}, hide_rect_{};  // last Draw's; empty when not drawn
};

// An action as a big square (Home: save a replay, open Clips): its icon on a disc, a title and a
// state line, centred. The primary one (one per screen) has an accent disc. A tile may carry a foot
// row, a label with its shortcut as keys and `toggle` at the trailing edge (Instant Replay's on/off);
// the switch takes clicks before the tile does. A disabled tile is a placeholder (Record).
class ActionTileControl : public Control {
public:
    std::wstring title, detail;
    IconType icon = IconType::None;
    bool primary = false;
    // The icon's dot (Replay, Record) in the recording red: Record's "recording now", wired ahead of
    // manual record mode. Always false until then.
    bool dot_active = false;
    std::function<void()> on_click;
    std::wstring foot_label, foot_keys;  // the foot shows when foot_label isn't empty
    SwitchControl toggle;
    bool reserve_foot = false;  // no foot, but centre as if there were, so a row's titles line up
    std::wstring live_line;     // under the detail after a green dot: what is being recorded

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// A toast (ombra-ui --toast): the result of a save or a buffer switch, opaque so the game doesn't
// bleed through. The icon's disc carries the tone;
// the card and title stay neutral. Not interactive: the toast window is click-through.
class ToastCard : public Control {
public:
    enum class Tone { Neutral, Success, Warning, Danger, Muted };
    std::wstring title;
    IconType icon = IconType::None;
    Tone tone = Tone::Neutral;
    // The brand mark (brand_mark.h, ring in the picked colour) in place of the icon's disc: a save
    // and its pending state.
    bool brand = false;
    // ui.toast_style compact: a pill as wide as its content, against the trailing edge (the leading
    // one for a toast on the left of the screen).
    bool compact = false;
    bool trailing = true;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
};

// A confirm dialog over the whole overlay (ScreenHost::ShowModal): a dimmed backdrop, a card sized
// to its title and message, Cancel (Secondary) then the confirm button (Primary, or Danger) at the
// trailing edge. An empty `cancel_text` makes it one button; Esc then confirms too.
class ModalDialog : public Widget {
public:
    std::wstring title, message;
    std::wstring confirm_text = L"Confirm";
    std::wstring cancel_text  = L"Cancel";
    bool is_danger = false;
    std::function<void()> on_confirm, on_cancel;

    // Set by OnMouseDown/OnKeyDown instead of invoking on_confirm/on_cancel directly: the modal is
    // owned by a unique_ptr the callbacks themselves can reset (ShowModal's wrapper does exactly
    // that), so resetting it from inside one of these would delete `this` mid-call.
    // OverlayHost::ProcessModalResult reads this once the call has returned.
    enum class Result { None, Confirm, Cancel };
    Result last_result = Result::None;

    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool HitTest(D2D1_POINT_2F) const override { return visible; }  // modal: takes every click
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F) override { return visible; }
    bool OnKeyDown(UINT vk) override;

private:
    ButtonControl confirm_, cancel_;  // placed by Draw
    SoftShadow shadow_;
    std::wstring measured_title_, measured_message_;  // text heights, re-measured when text or scale change
    float measured_scale_ = 0.0f, title_h_ = 0.0f, message_h_ = 0.0f;
};

// Tiles in equal columns, `columns` per line, `space_md` apart.
// A level's tiles with what the chosen one means under them: a Label line and wrapped Body text,
// swapped as the choice moves. Its height is the tallest stop's, so nothing below jumps.
// Settings' and setup's diagnostics sharing.
class LevelDetail : public Widget {
public:
    struct Stop { std::wstring lead, body, tile; };  // `tile`: the short line on its tile
    std::shared_ptr<LevelTilesControl> tiles = std::make_shared<LevelTilesControl>();
    std::vector<Stop> stops;  // one per option
    float width_dip = 440.0f; // when measured without a width (setup places it by hand)

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override { return tiles->CursorAt(pt); }
    bool OnMouseMove(D2D1_POINT_2F pt) override { return tiles->OnMouseMove(pt); }
    bool OnMouseDown(D2D1_POINT_2F pt) override { return tiles->OnMouseDown(pt); }
    bool OnMouseUp(D2D1_POINT_2F pt) override { return tiles->OnMouseUp(pt); }
    bool OnKeyDown(UINT vk) override { return tiles->OnKeyDown(vk); }
};

class TileGrid : public Widget {
public:
    std::vector<std::shared_ptr<TileControl>> tiles;
    int columns = 3;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
};

// One display in a MonitorDiagram: its capture filling the box (dimmed while not chosen), the name
// and resolution on a dark label, a check circle, and the hover eye of "Hide from this list".
class ScreenControl : public Control {
public:
    std::wstring title, detail;
    bool selected = false;
    // Where it is on the virtual desktop, physical pixels. Without `positioned` the diagram puts it
    // to the right of the previous one.
    int left = 0, top = 0, width = 1920, height = 1080;
    bool positioned = false;
    std::vector<uint8_t> thumb_bgra;  // BGRA; becomes a bitmap on the first Draw
    uint32_t thumb_w = 0, thumb_h = 0;
    std::function<void()> on_hide;
    std::function<void(bool)> on_change;

    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    D2D1_RECT_F hide_rect_{};
};

// The displays where they really are: each screen's box at its desktop position, scaled to the
// width and capped at `max_h_dip`, left-aligned. Click a screen to choose it.
class MonitorDiagram : public Widget {
public:
    std::vector<std::shared_ptr<ScreenControl>> screens;
    float max_h_dip = 150.0f;  // the card fits the panel at 1080p

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
private:
    struct Box { float l, t, r, b; };
    std::vector<Box> Desktop() const;  // each screen's desktop rect, unpositioned ones placed in a row
    float Scale(float w, float cap_h, float* out_w, float* out_h) const;
};

// What each source records and when: the game's tile under its caption, a rule, then the displays
// under theirs, on one tile width. `game` only shows state, so it takes no input. Without
// `displays` the tile stands alone with no captions.
class RecordingSources : public Widget {
public:
    std::shared_ptr<TileControl> game;
    std::shared_ptr<Widget> displays;
    std::wstring game_caption, displays_caption;
    int columns = 4;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
private:
    float TileWidth(float w, const Sizes& z, float s) const;
    float CaptionHeight(const Sizes& z, float s) const;
    float rule_x_ = 0;
};

// One line in a slim card, after a dot: green while `live`, muted otherwise.
class StatusStrip : public Widget {
public:
    std::wstring text;
    bool live = false;

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
};

// What the running config costs in RAM, as a card of its own beside the Settings panel and the rail.
// Rings commit as footage arrives, so the numbers say
// what the engine holds now and what it grows to; nothing animates with buffer fill. SettingsScreen
// fills `data`; the card only draws. Segment colours are status hues, never the accent: the mono
// accent would make three accent shades identical.
struct MemoryCardData {
    bool   valid       = false;
    bool   file_backed = false;  // video ring is a mapped file: disk, not RAM, so it stays out of the bar
    double video_bytes = 0.0;    // committed now (what the running engine holds)
    double audio_bytes = 0.0;
    double video_max_bytes = 0.0;  // what they grow to once the buffer is full (capacity + overshoot)
    double audio_max_bytes = 0.0;
    double engine_bytes = 0.0;   // private commit minus the rings: threads, encoder, WinRT, heap
    double sys_total_bytes = 0.0;
    double sys_load_pct    = 0.0;
    double vram_bytes      = -1.0;  // engine's dedicated GPU memory; < 0 = not reported
    std::wstring video_cause;    // "120 s · 1 monitor" -- the setting that buys the segment
    std::wstring audio_cause;
    bool   has_projection = false;  // staged, unsaved edits change the numbers
    double proj_video_bytes = 0.0;
    double proj_audio_bytes = 0.0;
    bool   proj_file_backed = false;  // the staged buffer.storage, which can differ from file_backed
    double TotalBytes() const { return (file_backed ? 0.0 : video_bytes) + audio_bytes + engine_bytes; }
    double MaxBytes() const { return (file_backed ? 0.0 : video_max_bytes) + audio_max_bytes + engine_bytes; }
    // Staged edits, compared with MaxBytes(): a projection is a configuration's full size too.
    double ProjectedBytes() const { return (proj_file_backed ? 0.0 : proj_video_bytes) + proj_audio_bytes + engine_bytes; }
};

// The card: the Settings panel's own surface (panel fill, line edge, panel radius), one flow for
// measuring and drawing so the two can't drift.
class MemoryCard : public Widget {
public:
    MemoryCardData data;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    static float Width(float s) { return 224.0f * s; }
    // Height the content needs, so the card ends where its content does instead of stretching to the
    // settings panel and leaving a dead band.
    float ContentHeight(ID2D1DeviceContext* dc, const Theme& theme, float s) const;

private:
    float Flow(ID2D1DeviceContext* dc, const Theme& theme, float s, bool draw) const;
};

}  // namespace ombra_ui
