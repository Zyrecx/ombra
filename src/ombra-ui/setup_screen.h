#pragma once

// The first-run setup: Recording, Clips, Appearance. Shown on a new
// install until it's finished or has appeared kFirstRunMaxShows times, and from Advanced > System. Nothing reaches the engine until the last step or Use
// recommended, as one set_config; appearance previews live on the overlay's own theme.
//
// Immediate style: Draw lays out and paints the current step and records what's clickable, so the
// layout and the hit tests can't disagree. Controls persist by name, keeping their hover fades.

#include "ombra-ui/screens.h"
#include "ombra-ui/ui/controls.h"
#include "ombra-ui/ui/theme.h"
#include "ombra-ui/ui/widget.h"
#include "ombra/json.h"
#include "ombra/setup_plan.h"

#include <d2d1_1.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ombra_ui {

// The marker that says the setup is over (first_run_done beside config.json). Not a config key: it
// isn't a setting anyone would flip.
std::wstring FirstRunMarkerPath();
bool FirstRunComplete();
void MarkFirstRunComplete();
// Closing the setup leaves it to come back, but the overlay has no other screen until it's over, so
// someone who won't finish it gets the panel back after this many appearances.
inline constexpr int kFirstRunMaxShows = 3;
// The setup is appearing on its own: counts it, and ends the nagging on the last one.
void NoteFirstRunShown();

class SetupScreen {
public:
    explicit SetupScreen(ScreenHost* host);

    struct GpuInfo { std::string vendor; std::wstring name; bool av1 = false; uint32_t align_w = 1, align_h = 1; };

    // Starts over on Recording, recommending from what the engine reports. Either may be null (no
    // engine): schema defaults and this PC's primary display.
    void Open(const ombra::JsonValue* status, const ombra::JsonValue* config);

    // The scale to Layout and Draw at, so the current step fits a panel `panel_height` pixels tall:
    // dpi_scale when it fits, less when it doesn't.
    float FitScale(const Theme& theme, float panel_height, float dpi_scale) const;
    void Layout(const D2D1_RECT_F& bounds, float dpi_scale);
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale);
    bool OnMouseMove(D2D1_POINT_2F pt);
    CursorKind CursorAt(D2D1_POINT_2F pt) const;
    bool QueryHoverTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const;
    bool OnMouseDown(D2D1_POINT_2F pt);
    bool OnMouseUp(D2D1_POINT_2F pt);

    // A shortcut field is recording: the overlay sends it every key first.
    bool IsRecordingShortcut() const;
    void OnShortcutKey(UINT vk, bool down);
    void CancelShortcutRecording();

    int Step() const { return step_; }
    void FinishForTest() { Finish(); }
    const ombra::SetupPlan& Plan() const { return plan_; }
    // --drive and tests: this machine's facts without an engine.
    void SetFactsForTest(const ombra::SetupFacts& facts, std::vector<GpuInfo> gpus, const std::wstring& display_owner, double ram_gb);

private:
    struct Zone { D2D1_RECT_F rect; std::function<void()> fn; };

    void Recommend(bool keep_preferences);
    void Finish();
    void SetStorage(const std::string& storage);
    void ApplyThemePreview();

    template <class T> T& Get(const std::string& name);
    // Arranges a control at its own size at (x, y) DIPs from the panel, draws it, and makes it
    // clickable. Returns its width in DIPs.
    float Put(Widget& w, float x, float y);
    float PutMid(Widget& w, float x, float row_y, float row_h);
    D2D1_RECT_F R(float x, float y, float w, float h) const;
    bool Hovering(const D2D1_RECT_F& r) const;
    SwitchControl& Switch(const std::string& name, bool on, std::function<void(bool)> fn);
    SegmentedControl& Seg(const std::string& name, std::vector<std::wstring> options, int selected, std::function<void(int)> fn);
    SelectControl& Select(const std::string& name, std::vector<std::wstring> options, int selected, float width, std::function<void(int)> fn);
    ButtonControl& Button(const std::string& name, const std::wstring& label, ButtonControl::Kind kind, std::function<void()> fn);
    KeyCaptureControl& KeyField(const std::string& name, std::string* spec, const std::string* other);

    void DrawHeader();
    void DrawTitle(const std::wstring& title, const std::wstring& sub, float y);
    void DrawCard(float x, float y, float w, float h);
    void DrawDisc(float cx, float cy, float r, IconType icon, bool accent = false);
    void DrawRow(float x, float y, float w, int index, IconType icon, const std::wstring& value, const std::wstring& desc);
    void DrawLane(float x, float y, float w, float h, std::vector<IconType> icons, const std::wstring& name, float seed, bool selected);
    void DrawTrackTile(float x, float y, float w, float h, const std::wstring& title, bool merged, bool selected, bool recommended, std::function<void()> fn);
    void DrawStoreTile(float x, float y, float w, float h, IconType icon, const std::wstring& name, const std::wstring& fact, bool selected, bool recommended, std::function<void()> fn);
    void DrawOpenRow(float x, float y, float w, int index);
    void DrawKeysFoot();
    void DrawFootButtons(const std::wstring& primary, std::function<void()> on_primary, bool back);
    void DrawRecording();
    void DrawClips();
    void DrawAppearance();
    void DrawSharing();
    void DrawNotifyPicture(float x, float y, float w, float h);
    // split: the System tile, a dark preview on the left half and a light one on the right.
    void DrawThemeTile(float x, float y, float w, float h, ThemeMode mode, const std::wstring& name, const std::string& key,
                       bool split = false);
    void DrawCornerTile(float x, float y, float w, float h, float radius, const std::wstring& name, const std::string& key);

    ScreenHost* host_ = nullptr;
    D2D1_RECT_F bounds_{};
    float s_ = 1.0f;
    ID2D1DeviceContext* dc_ = nullptr;  // only during Draw
    const Theme* theme_ = nullptr;      // only during Draw
    int step_ = 0;
    int open_row_ = -1;

    ombra::SetupFacts facts_;
    ombra::SetupPlan plan_;
    std::vector<GpuInfo> gpus_;
    std::wstring display_owner_;
    double ram_gb_ = 0.0;
    std::string output_root_raw_;  // as the config holds it (%USERPROFILE%...), sent back unchanged
    std::wstring output_root_shown_, output_free_, disk_note_;
    ombra::JsonValue existing_sources_;
    bool has_existing_sources_ = false;
    ombra::JsonValue ui_config_;  // ui.* for the live preview

    std::unordered_map<std::string, std::shared_ptr<Widget>> widgets_;
    std::vector<Widget*> live_;
    std::vector<Zone> zones_;
    D2D1_POINT_2F mouse_{-1.0f, -1.0f};
    KeyCaptureControl* recording_ = nullptr;
};

}  // namespace ombra_ui
