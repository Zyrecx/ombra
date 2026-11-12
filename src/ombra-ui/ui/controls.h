#pragma once

// Controls: one control per setting value shape, drawn only from the new tokens
// (Theme::color, Theme::size, Theme::Font). Nothing in the working UI uses them yet.
//
// States are the Widget fields plus `pressed` and `focus_visible`, so a gallery (or a test) can set
// any state directly and the real input path sets the same fields. Like the existing widgets they
// act on mouse down; `pressed` lasts until mouse up.

#include "ombra-ui/ui/motion.h"
#include "ombra-ui/ui/widget.h"
#include "ombra/ui_palette.h"

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ombra_ui {

class Control : public Widget {
public:
    bool pressed = false;
    bool focus_visible = false;  // keyboard focus: draws the ring (a click focuses without one)
    int variant = 0;             // A/B candidates on the gallery --choices sheet; 0 = current pick
    std::wstring tooltip;        // shown while hovered, anchored above the control
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override { return HitTest(pt) ? CursorKind::Hand : CursorKind::Arrow; }
    bool QueryTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const override;

protected:
    // Called from OnMouseDown when the press lands on the control.
    virtual void Press(D2D1_POINT_2F pt) { (void)pt; }
    // Hover as drawn, 0..1: fades in kFadeMs (ui/motion.h); `on` defaults to `hovered`. Press is instant.
    float Hover() { return Hover(hovered); }
    float Hover(bool on) { return hover_fade_.Track(on && enabled, kFadeMs); }
    // The same for a control's parts (segments, menu rows, rail pages): once per Draw, then Amount(i).
    void TrackItems(int hovered_item) { item_hover_.Track(enabled ? hovered_item : -1, kFadeMs); }
    ItemFade item_hover_;

private:
    Fade hover_fade_;
};

// Bool.
class SwitchControl : public Control {
public:
    bool on = false;
    std::function<void(bool)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// Action: a labelled button. Primary is the accent fill (one per surface), Secondary the raised
// outline, Danger red text with a red tint on hover. An icon, if any, leads the label; with no
// label the button is a square holding the icon (give it a tooltip).
class ButtonControl : public Control {
public:
    enum class Kind { Primary, Secondary, Danger };
    std::wstring label;
    IconType icon = IconType::None;
    Kind kind = Kind::Secondary;
    bool in_row = false;  // control_sm tall, like the Selects and steppers in a setting row
    std::function<void()> on_click;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// A Secondary button with a second hit area at its right end: the main area runs `on_click`, the
// arrow calls `on_menu`, whose owner shows the other actions and clears `open` when the menu closes.
class SplitButtonControl : public Control {
public:
    std::wstring label;
    IconType icon = IconType::None;
    bool open = false;
    std::function<void()> on_click;
    std::function<void()> on_menu;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;  // Space/Enter the action, Down the menu
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    enum Part { kNone = -1, kAction = 0, kArrow = 1 };
    float arrow_w_ = 28.0f;  // pixels, from the last Measure
    int hovered_part_ = kNone, pressed_part_ = kNone;
    int PartAt(D2D1_POINT_2F pt) const;
};

// Action as a glyph in a square: no fill or outline at rest, the hover layer under the mouse.
// Remove turns red on hover like a danger button. Icon draws `icon` (play, close, a speaker).
class IconButtonControl : public Control {
public:
    enum class Glyph { More, Remove, Add, Reset, Icon };
    Glyph glyph = Glyph::More;
    IconType icon = IconType::None;
    std::function<void()> on_click;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// Small enum (2-5 short options). The chosen option is a solid accent fill.
class SegmentedControl : public Control {
public:
    std::vector<std::wstring> options;
    int selected = 0;
    int hovered_index = -1;   // set by OnMouseMove; a gallery may set it directly
    std::function<void(int)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    std::vector<float> seg_w_;  // DIPs, from the last Measure
    float total_dip_ = 0, inset_dip_ = 2;
    int IndexAt(D2D1_POINT_2F pt) const;
};

// An ordered level (diagnostics sharing: Off, Error reports, + Usage): one tile per
// level in equal columns, further right means more. A tile: a radio and its label, a short line
// under them; both wrap and every tile takes the tallest one's height. Chosen: TileControl's accent
// tint and outline. Click picks; Left/Right step.
class LevelTilesControl : public Control {
public:
    std::vector<std::wstring> options;
    std::vector<std::wstring> lines;  // one per option, under its label; may be shorter
    int selected = 0;
    int hovered_index = -1;
    float width_dip = 440.0f;         // when measured without a width
    std::function<void(int)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    float s_ = 1.0f;   // DPI scale from the last Measure/Draw
    float gap_ = 8.0f; // px between tiles
    float TileHeight(const Theme& t, int i, float tile_w, float s) const;  // px, for a tile `tile_w` px wide
    D2D1_RECT_F TileRect(int i) const;
    int IndexAt(D2D1_POINT_2F pt) const;
    void Pick(int i);
};

// A floating surface's shadow (menus, the volume column): its rounded shape blurred by D2D's
// shadow effect into a bitmap, redone only when the size, corner or theme changes, so a fade or a
// hover just draws the bitmap.
class SoftShadow {
public:
    // Under `r` (pixels), dropped by space_xs.
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s, const D2D1_RECT_F& r, float radius);
    void Reset() { bitmap_.Reset(); }
    static int RendersForTest();

private:
    void Render(ID2D1DeviceContext* dc, const Theme& theme, float s, float w, float h, float radius);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    D2D1_SIZE_F for_{};  // the size it was blurred for
    float radius_ = 0.0f, margin_ = 0.0f;
    ThemeMode mode_ = ThemeMode::Dark;
};

// A hover label above `anchor` (a control's QueryTooltip): Caption text on the raised surface,
// wrapping past 260 DIP. OverlayHost draws it last, above everything; no delay of its own.
void DrawTooltip(ID2D1DeviceContext* dc, const Theme& theme, float s, const std::wstring& text, D2D1_POINT_2F anchor);

// One of an open-ended set of short labels (Clips' game filter): pills in a row, one chosen.
// Unlike a segmented control the set grows with the user's data, so each pill stands alone. The
// chosen one is a solid accent fill, like a segmented pick.
class ChipGroupControl : public Control {
public:
    std::vector<std::wstring> options;
    int selected = 0;
    int hovered_index = -1;
    std::function<void(int)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    std::vector<float> chip_w_;  // DIPs, from the last Measure
    float gap_dip_ = 8, scale_ = 1;
    int IndexAt(D2D1_POINT_2F pt) const;
};

// Bool as a labelled pill with an icon (a clip's audio track: heard and kept, or left out). On is
// an accent tint in accent text, off bare and faded with `off_icon`.
class ToggleChipControl : public Control {
public:
    std::wstring label;
    IconType icon = IconType::Volume, off_icon = IconType::VolumeOff;
    bool on = true;
    std::function<void(bool)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// A clip's playback volume: a speaker button that mutes, and its level (0..1) on a slider shown
// while the mouse is over either or the knob is held; only the icon shows at rest.
// The slider is a raised column above the speaker; nothing beside it moves.
class VolumeControl : public Control {
public:
    double value = 1.0;
    bool muted = false;
    std::function<void(double)> on_change;  // a drag; the host unmutes
    std::function<void()> on_mute;          // the speaker clicked
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool HitTest(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;  // Left/Right (Down/Up) step the level while focused
    bool Shown() const { return hovered || dragging_ || focus_visible; }
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    bool dragging_ = false;
    Fade reveal_;
    SoftShadow shadow_;
    float side_ = 0, track_ = 0, pad_ = 0;  // px from the last Measure: speaker square, track length, track inset
    D2D1_RECT_F Speaker() const;
    D2D1_RECT_F SliderBox() const;  // the column above the speaker
    void SetFrom(D2D1_POINT_2F pt);
};

// Long enum: a closed field with the value and a chevron. A click calls `on_open`, whose owner shows
// the options in a MenuPopup through its host and clears `open` when the menu closes; the chevron
// points up meanwhile.
class SelectControl : public Control {
public:
    std::vector<std::wstring> options;
    int selected = 0;
    bool open = false;
    float width_dip = 200.0f;
    bool fit_options = false;  // widen past width_dip so the widest option shows untrimmed
    std::function<void(int)> on_change;
    std::function<void(SelectControl&)> on_open;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnKeyDown(UINT vk) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// A floating surface above everything else: a menu, the colour editor. The host (OverlayHost) keeps
// one at a time, draws it last and gives it every input first; it's modal, so a click outside goes
// to it too (and dismisses it). Floating, so it has a shadow, blurred once per open into a cached
// bitmap, and fades in over kPopupFadeMs.
class Popup : public Control {
public:
    // Against `anchor` inside `view`, pixels; once per open (it ends with Opened()).
    virtual void Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& theme, float s) = 0;
    bool HitTest(D2D1_POINT_2F pt) const override { (void)pt; return visible; }
    virtual bool OnChar(wchar_t ch) { (void)ch; return true; }  // modal: no key reaches the page under it
    virtual void OnMouseWheel(float delta) { (void)delta; }
    // A popup that finished (a pick, Esc, a click outside) says so here; the host destroys it after
    // the event that closed it, never from inside its own callback.
    bool Closed() const { return closed_; }

    static int ShadowRendersForTest() { return SoftShadow::RendersForTest(); }

protected:
    void Close() { closed_ = true; visible = false; }
    // Once per open, after `bounds` is set: restarts the fade-in and drops the cached shadow.
    void Opened();
    // The shadow, the raised fill and its outline, inside the fade-in's layer; EndSurface pops it.
    void BeginSurface(ID2D1DeviceContext* dc, const Theme& theme, float s);
    void EndSurface(ID2D1DeviceContext* dc);

private:
    bool closed_ = false, fading_ = false;
    Fade appear_;
    SoftShadow shadow_;
};

// A floating list of choices: a Select's menu, a list's ⋯ menu. The host (OverlayHost::ShowDropdown)
// and the gallery both place it with Place and draw it with Draw. The current item is a tinted row
// in accent text with a short accent bar.
class MenuPopup : public Popup {
public:
    std::vector<std::wstring> options;
    int selected = -1;       // the current choice; -1 for a menu of actions
    int hovered_index = -1;  // mouse or arrow keys
    float scroll_y = 0.0f;   // pixels; a list taller than the room scrolls
    std::function<void(int)> on_select;  // the picked index, or -1 when dismissed

    // Below `anchor` (above when that side has more room and the list doesn't fit below), at least
    // as wide as it and wide enough for every option untrimmed, inside `view`. Pixels. Call once
    // per open.
    void Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& theme, float s) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    void OnMouseWheel(float delta) override;

private:
    float item_h_ = 32.0f, pad_ = 4.0f;  // pixels, from Place
    int IndexAt(D2D1_POINT_2F pt) const;
    float MaxScroll() const;
};

// Number with a range and a unit: a stepper (- value unit +), not a slider.
class StepperControl : public Control {
public:
    double value = 0, min = 0, max = 100, step = 1;
    std::wstring unit;
    int pressed_side = 0;   // -1 minus, +1 plus (with `pressed`)
    int hovered_side = 0;
    bool error = false;     // the typed value is out of range
    // Shown instead of the number at `min` ("Auto": a max bitrate whose 0 means derived), and where
    // + goes from there when min + one step would mean nothing (Auto -> 1.25x the target, not 1).
    std::wstring min_label;
    double first_step_above_min = 0;
    std::function<void(double)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;     // Left/Down and Right/Up step while focused
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    void StepBy(int direction);
    std::wstring Format(double v) const;  // "40 Mbps"
};

// A live level: a row of blocks, lit up to `db` on a -60..0 dBFS scale, in OBS's zones (green, yellow
// from -20 dB, red from -9 dB). Shows only; `enabled` false draws it unlit.
class LevelMeter : public Widget {
public:
    static constexpr int kBlocks = 24;
    static constexpr float kFloorDb = -60.0f;
    float db = -INFINITY;
    float width_dip = 168.0f;
    int LitBlocks() const;  // what Draw lights: a repaint is needed only when this changes
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
};

// Free text. `focused` shows the caret and an accent underline; `error` outlines it red with the
// message under it.
class TextFieldControl : public Control {
public:
    std::wstring text, placeholder, error_text;
    bool mono = false;
    float width_dip = 240.0f;
    size_t max_length = 0;                          // 0: no limit
    std::function<bool(wchar_t)> accept_char;       // null accepts any printable character
    std::function<void(const std::wstring&)> on_edit;    // every change while typing
    std::function<void(const std::wstring&)> on_commit;  // Enter
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override { return HitTest(pt) ? CursorKind::IBeam : CursorKind::Arrow; }
    // While `focused`: Backspace, Delete, Left/Right, Home/End and Enter; Esc is the owner's.
    bool OnKeyDown(UINT vk) override;
    bool OnChar(wchar_t ch);
    size_t Caret() const { return (std::min)(caret_, text.size()); }
    void SetCaret(size_t c) { caret_ = c; }
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    size_t caret_ = std::wstring::npos;  // npos = end
    float text_left_ = 0.0f;             // pixels, from Draw: where the text starts, for a click
};

// A name template (output.filename): known placeholders show as chips that the caret and Backspace
// treat as one character; a row of "+ Game" chips inserts them at the caret; a preview line shows a
// real name. The value stays the plain string ("{game} {date}"); an unknown {x} stays text, silently.
// Friendly chip names, insert row under the field, live preview.
class TemplateFieldControl : public Control {
public:
    struct Token { std::wstring key, label, sample; };  // {L"{game}", L"Game", L"Example"}
    std::vector<Token> tokens;
    std::wstring text;
    std::wstring suffix = L".mp4";  // drawn at the end of the field and the preview
    std::wstring fallback;          // what an empty text stands for, in the preview
    std::function<bool(wchar_t)> accept_char;  // null accepts any printable character
    float width_dip = 420.0f;
    int hovered_chip = -1;          // index into Chips() (in-field), or -1
    int hovered_insert = -1;        // index into `tokens` (insert row), or -1
    int selected_chip = -1;         // clicked chip; Backspace/Delete removes it
    std::function<void(const std::wstring&)> on_change;
    std::function<void()> on_reset;  // set: a "Reset" chip ends the insert row (index tokens.size())

    struct Chip { size_t pos, len; int token; };
    std::vector<Chip> Chips() const;     // known placeholders in `text`, in order
    size_t Caret() const { return caret_; }
    void SetCaret(size_t c);             // snapped out of any chip
    void InsertToken(int token);         // at the caret
    std::wstring Preview() const;        // samples substituted, plus suffix (once, like the engine)

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool OnChar(wchar_t ch);
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;

protected:
    void Press(D2D1_POINT_2F pt) override;

private:
    size_t caret_ = 0;
    // From the last Draw, for hit tests: each in-field chip's box, each insert chip's box, and the
    // x of every caret position in the field (index = position in `text`, NaN inside a chip).
    std::vector<D2D1_RECT_F> chip_boxes_, insert_boxes_;
    std::vector<float> caret_x_;
    D2D1_RECT_F field_ = {};
    ItemFade insert_hover_;  // the insert row; item_hover_ has the in-field chips
    std::vector<Chip> ChipsIn(const std::wstring& s) const;
    void Changed();
};

// Folder: the path (mono; too long, it keeps the drive and the last folders, the whole path in the
// tooltip) and Browse.
class PathControl : public Control {
public:
    std::wstring path;
    float width_dip = 300.0f;
    bool browse_hovered = false;
    std::function<void()> on_browse;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
};

// Hotkey: keycaps. A click records a new combo in place: held modifiers show live as keycaps, the
// first non-modifier key commits, Esc cancels, a click elsewhere cancels. Ported from the old
// ShortcutRowWidget (itself modelled on OBS's UI/hotkey-edit.cpp @30.2.3). Keys don't come through
// OnKeyDown: while it records, the overlay routes every key down/up (WM_SYSKEYDOWN too: Alt combos
// never arrive as WM_KEYDOWN) to SettingsScreen, which calls OnCaptureKey.
class KeyCaptureControl : public Control {
public:
    std::wstring spec;   // canonical "Ctrl+Alt+Key" (hotkey_spec.h); empty = not set
    float width_dip = 180.0f;
    // Why `candidate` can't be bound here ("already used by Screenshot."), or "" if it can.
    std::function<std::wstring(const std::wstring& candidate)> validate;
    std::function<void(const std::wstring& spec)> on_commit;
    // Recording started (true) or ended (false), by any path: commit, Esc, a click elsewhere.
    std::function<void(bool recording)> on_recording_changed;

    bool Recording() const { return recording_; }
    void BeginRecording();
    void CancelRecording();
    // A key went down or up while recording. Always consumed.
    void OnCaptureKey(UINT vk, bool down);
    // While recording: the last refusal ("Ctrl+A: ..."), shown by the row; empty otherwise.
    const std::wstring& Refusal() const { return error_; }
    // Refuse `candidate` as if it had been pressed (the reset button found the default taken).
    void Refuse(const std::wstring& why) { error_ = why; }

    UINT HeldModsForTest() const { return held_mods_; }
    void SetRecordingStateForSnapshot(UINT held_mods, const std::wstring& error);

    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    void EndRecording();
    bool recording_ = false;
    UINT held_mods_ = 0;  // MOD_* while recording
    std::wstring error_;
};

// Where KeyCaptureControl reads the held modifiers (MOD_*). Unset: the keyboard state. The ui tests
// set it, so a key held on the machine running them can't change what they record.
void SetHeldModsSourceForTest(UINT (*source)());

// Colour: preset swatches plus a custom one with its hex; the chosen one has a ring.
// A colour near the danger red gets the note under it.
class ColorPickerControl : public Control {
public:
    std::vector<uint32_t> presets;   // 0xRRGGBB
    int selected = 0;                // index into presets, or presets.size() for custom
    uint32_t custom = 0xf97316;
    // The first `lead` presets stand apart, a divider after them (the theme's own accent).
    int lead = 0;
    // A lead swatch shows `lead_backdrop` on its left half: the colour it comes from.
    std::optional<uint32_t> lead_backdrop;
    int hovered_index = -1;
    std::function<void(int)> on_change;
    // The custom swatch, chosen or not, opens the editor when this is set (its rect, pixels);
    // without it the swatch is picked like a preset.
    std::function<void(const D2D1_RECT_F&)> on_edit_custom;
    D2D1_RECT_F CustomSwatchRect() const;
    // The chosen colour is near the danger red, so the note shows under the swatches; the height
    // (DIPs) counts its line only then.
    bool ShowsNote(const Theme& theme) const;
    float HeightDip(const Theme& theme) const;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    int IndexAt(D2D1_POINT_2F pt, float s) const;
    float SlotLeft(int i, float s) const;  // swatch i's left edge, pixels (presets.size() is custom)
    void SetDims(const Sizes& z, float s);
    float last_s_ = 1.0f, gap_dip_ = 8.0f, swatch_dip_ = 24.0f, pad_dip_ = 4.0f;  // from Arrange/Draw, for hit tests
};

// The accent picker's presets for this theme (kAccentSwatches; the theme's own one set apart,
// over the panel colour it comes with).
void SetAccentSwatches(ColorPickerControl& cp, const Theme& theme);

// ui.theme: a tile per theme (its background, a card with two lines of text, the accent) with its
// name under it; System shows Light and Dark halves, New a "+". The chosen one has a ring. The
// tiles fill the width they are measured at, at most seven a line. With a user theme chosen its
// six colours follow, three a line: swatch, name and hex, each opening the editor. `compact`: one
// line of small tiles without names or colours, for a row beside its label.
class ThemePickerControl : public Control {
public:
    enum class Kind { Palette, System, User, New };
    struct Tile {
        Kind kind = Kind::Palette;
        std::wstring name;
        ombra::UiPaletteColors colors{};  // a palette's or a user theme's
    };
    static constexpr int kFields = 6;     // background, cards, menus, lines, text, muted
    static const wchar_t* FieldName(int i);
    static uint32_t& FieldOf(ombra::UiPaletteColors& c, int i);

    bool compact = false;
    std::vector<Tile> tiles;
    int selected = -1;  // -1: the theme in use is in another picker
    int hovered_index = -1;               // tiles, then the fields after them
    std::function<void(int)> on_change;   // a tile index
    std::function<void(int field, const D2D1_RECT_F&)> on_edit_field;  // pixels
    bool ShowsFields() const;
    D2D1_RECT_F FieldRect(int field) const;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    void Arrange(const D2D1_RECT_F& rect, const LayoutContext& ctx) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    // Every part's rect for width `w` (pixels) from (0, 0); returns the height.
    float Place(float w, const Sizes& z, float s, std::vector<D2D1_RECT_F>* tiles, std::vector<D2D1_RECT_F>* fields) const;
    int IndexAt(D2D1_POINT_2F pt) const;
    std::vector<D2D1_RECT_F> tile_rects_, field_rects_;  // pixels, from Arrange
    float name_band_ = 0;  // a tile's name under it, pixels
};

// A one-line text prompt under its anchor: a caption, the field and what's wrong with it. Enter or
// a click outside keeps the text when `validate` accepts it (an empty answer), Esc drops it.
class TextPromptPopup : public Popup {
public:
    std::wstring caption;
    std::function<std::wstring(const std::wstring&)> validate;  // what's wrong, or empty
    std::function<void(const std::wstring&)> on_commit;
    void SetText(const std::wstring& text);
    TextFieldControl& Field() { return field_; }
    void Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& theme, float s) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool OnChar(wchar_t ch) override;

private:
    bool TryCommit();  // false (and the reason under the field) when `validate` refuses
    void Layout(const Theme& t, float s);
    TextFieldControl field_;
    D2D1_RECT_F caption_{};
};
// Your own colour: a saturation/brightness square, a hue bar and the hex, under the custom swatch.
// Dragging previews (on_preview) and letting go
// commits (on_commit), so a drag stages once; a typed hex previews once it parses and commits on
// Enter; Esc puts back what was there when it opened (on_cancel); a click outside keeps the colour
// and closes. The foot says which colour Ombra uses when contrast moved it (DeriveAccent).
class ColorEditorPopup : public Popup {
public:
    std::function<void(uint32_t)> on_preview, on_commit;
    std::function<void()> on_cancel;
    // The foot: which colour Ombra uses when contrast moved it, for the accent; off for colours
    // used as they are (Custom theme).
    bool accent_foot = true;

    // Under `anchor` (above when it doesn't fit), right edges aligned, inside `view`. Pixels.
    void Place(const D2D1_RECT_F& anchor, const D2D1_RECT_F& view, const Theme& theme, float s) override;
    void SetColor(uint32_t rgb);  // also resets the hex field
    uint32_t Color() const { return rgb_; }
    TextFieldControl& HexField() { return hex_; }
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    bool OnMouseDown(D2D1_POINT_2F pt) override;
    bool OnMouseUp(D2D1_POINT_2F pt) override;
    bool OnKeyDown(UINT vk) override;
    bool OnChar(wchar_t ch) override;

private:
    enum class Drag { None, Square, Hue };
    void Layout(const Theme& t, float s);  // the parts' rects from `bounds`
    void DragTo(D2D1_POINT_2F pt);
    void Commit();
    float hue_ = 0, sat_ = 0, val_ = 0;   // kept apart from rgb_ so a grey keeps its hue
    uint32_t rgb_ = 0, committed_ = 0;
    Drag drag_ = Drag::None;
    TextFieldControl hex_;
    D2D1_RECT_F square_{}, hue_bar_{}, foot_{};  // pixels
    float s_ = 1.0f;
    const Theme* theme_ = nullptr;  // for Layout during input; set by Place and Draw
};

// Position on screen (toast corner, panel edge): a miniature screen with selectable spots.
class PositionPickerControl : public Control {
public:
    struct Spot { float fx, fy; };   // 0..1 within the miniature
    std::vector<Spot> spots;
    std::vector<std::wstring> names; // read under the miniature
    int selected = 0;
    int hovered_index = -1;
    std::function<void(int)> on_change;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    int IndexAt(D2D1_POINT_2F pt) const;
};

// ui.panel_position as a picture (Appearance > Layout): a small screen with the overlay panel where
// it would sit, fused to the edge when `attached`, every other slot a dashed outline so it reads as
// a choice. Five click zones: the middle and a band along each edge, each option's own text picking
// its zone. Ported from the old ScreenPositionPickerWidget (same geometry), minus its toast corners:
// Notifications has its own PositionPickerControl.
class PanelPositionControl : public Control {
public:
    std::vector<std::wstring> options;  // "center", "top", "bottom", "left", "right"
    int selected = 0;
    int hovered_index = -1;
    bool attached = false;
    // Shown, not picked here: which side of the panel the tab rail sits on, and the screen frame.
    std::wstring rail_position = L"left";
    bool screen_frame = false;
    float max_width_dip = 224.0f;  // at most; narrower if the column is
    std::function<void(int)> on_change;

    // Which option a point falls in, or -1 outside the screen. Public for tests.
    int ZoneAt(D2D1_POINT_2F pt) const;
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext& ctx) override;
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float s) override;
    bool OnMouseMove(D2D1_POINT_2F pt) override;
    CursorKind CursorAt(D2D1_POINT_2F pt) const override { return ZoneAt(pt) >= 0 ? CursorKind::Hand : CursorKind::Arrow; }
protected:
    void Press(D2D1_POINT_2F pt) override;
private:
    D2D1_RECT_F ScreenRect() const;
    D2D1_RECT_F PanelRect(const std::wstring& pos, bool fused) const;
};

}  // namespace ombra_ui
