#pragma once

#include "ombra-ui/audio_levels.h"
#include "ombra-ui/pipe_client.h"
#include "ombra-ui/ui/theme.h"
#include "ombra-ui/ui/flex.h"
#include "ombra-ui/ui/game_list.h"
#include "ombra-ui/ui/containers.h"
#include "ombra-ui/ui/disk_location.h"
#include "ombra-ui/ui/navigation.h"
#include "ombra-ui/ui/widget.h"
#include "ombra/config.h"
#include "ombra/games.h"
#include "ombra/json.h"
#include "ombra/sent_records.h"

#include <algorithm>
#include <array>
#include <d2d1_1.h>
#include <d3d11.h>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ombra_ui {

enum class ActiveScreen {
    Home,
    Settings,
    Clips,
    Setup
};

// While set, the UI opens no native window (folder pickers, Explorer) and tells the sink what it
// would have opened instead. `ombra-ui.exe --drive` sets it: its clicks must never put a window on
// the user's screen.
void SetNativeWindowSink(std::function<void(const std::wstring&)> sink);
// True = don't open `what` (a window, dialog, the clipboard, the Recycle Bin): the sink was told.
bool NativeWindowBlocked(const std::wstring& what);
// The folder picker, owned by `owner` so it stacks above the overlay. "" on cancel or when blocked.
std::wstring PickFolderDialog(HWND owner, const std::wstring& initial);

// diagnostics.sharing's values in slider order, their labels, and what each sends;
// Settings and the first-run setup draw the same slider and words.
inline constexpr const char* kSharingKeys[] = {"off", "errors", "usage"};
int SharingIndex(const std::string& key);  // unknown -> errors, the default
std::vector<std::wstring> SharingLabels();
std::vector<LevelDetail::Stop> SharingStops();
// One record of what was sent, as the sharing card's list shows it: its kind's name and icon, a
// summary, when, and whether it arrived in plain words ("Not sent: no connection").
SentList::Item SentItem(const ombra::sent_records::Record& r);
// The start of a record for its open row: at most `max_lines` lines and `max_bytes` bytes, as wide
// text. `cut`: there was more.
std::wstring SentPreview(const std::string& text, size_t max_lines, size_t max_bytes, bool* cut);

// The engine's update_status (update.h), as the Updates card reads it.
struct UpdateView {
    std::string state;  // empty until the engine answered; "off" in an isolated instance
    std::string current, latest, page_url, error, kind;
    int64_t     last_check_unix = 0;
};
UpdateView ParseUpdateView(const ombra::JsonValue& resp);
// The card's status line, and whether it reports a failure. `now_unix` for "checked 2 hours ago".
std::wstring UpdateStatusLine(const UpdateView& u, bool auto_install, int64_t now_unix, bool* is_error);

// The Settings rail's pages, in order of use; Settings opens on the first.
// Tests, snapshots, deep links and the driver name a page
// by this (or SettingsRailByName), never by its number. Names, icons, sub-tabs: kSettingsPages.
enum SettingsRail : int {
    kRailCapture = 0,  // Video | Audio
    kRailReplay,       // Replay & Quality | Output
    kRailGames,
    kRailAppearance,
    kRailShortcuts,
    kRailAdvanced,     // Encoder | Diagnostics
    kRailCount
};
// A page's rail label ("Capture"), and a page by a prefix of it in any case ("rep"); -1 if none.
const wchar_t* SettingsRailLabel(int rail);
int SettingsRailByName(const std::wstring& prefix);

// The value's type, as the engine's schema reports it.
enum class FieldKind {
    Bool,
    Int,
    Double,
    String,
    Enum,
    Action
};

// Whether a setting shows in Settings. Every shown setting is drawn by its page's builder; the entry
// in kFieldForms (screens.cpp) is what the "every setting renders somewhere" test checks against.
enum class FieldForm {
    Custom,      // drawn by a page or panel builder
    Hidden,      // config.json only
};

struct FieldSpec {
    std::string              key;
    std::string              display_name;
    std::string              help;
    std::string              tradeoff;
    std::string              tier;    // "preset" | "common" | "advanced" -- ombra::TierNameStr()
    std::string              group;
    std::string              unit;
    FieldKind                kind = FieldKind::String;
    FieldForm                form = FieldForm::Hidden;
    ApplyKind                apply = ApplyKind::Free;
    bool                     implemented = true;
    double                   min_val = 0.0;
    double                   max_val = 0.0;
    double                   step_val = 0.0;
    std::vector<std::string> enum_options;
    std::vector<std::string> presets;
    bool                     hardware_bound = false;
};

std::vector<FieldSpec> ParseSchema(const ombra::JsonValue& schema_obj);
// kFieldForms' entry for `key`; false when the key has none (a test requires every implemented key to).
bool FieldFormFor(const std::string& key, FieldForm* form);

// Decodes a preview PNG the engine wrote (paths.h's MonitorThumbPath/WindowThumbPath) to BGRA.
// Needs COM initialized on the calling thread. Defined in overlay_window.cpp.
bool DecodeThumbnailPng(const std::wstring& path, UINT& out_w, UINT& out_h, std::vector<uint8_t>& out_bgra);

// One list_windows entry, kept past the request itself -- `hwnd` is a
// same-session key for a capture_window_thumbnail request only, never persisted and never matched
// against later; the committed capture.target_window value stays `title`.
struct WindowEntry {
    std::wstring title;
    std::string  process;  // exe filename ("chrome.exe"); what the Monitors card saves as the match
    uint64_t     hwnd = 0;
    std::wstring image_path;  // the exe's full path: what "Mark as game" stores
    bool         is_game = false;   // the engine's Games list says so (committed decisions)
    bool         not_game = false;
};

// One list_monitors entry -- same role WindowEntry plays for the window picker, kept past the
// request itself only for the duration of building monitor_rows_ (screens.cpp's BuildDisplaysCard).
struct MonitorEntry {
    std::string device;
    std::string label;
    int         width = 0;
    int         height = 0;
    bool        primary = false;
    int         left = 0, top = 0;     // physical pixels on the virtual desktop
    bool        positioned = false;    // the reply carried left/top
};

// One list_processes entry -- feeds both BuildAudioPage' process_display_names_ map (skips
// entries with an empty display_name) and its/BuildExclusionRows' "Add app"/"Add process" picker
// option lists (which want every name, display_name or not, falling back to the raw exe name).
// `display_name` is "" when the engine couldn't resolve one (controller.h's
// ReadVersionResourceName).
struct ProcessEntry {
    std::string name;
    std::string display_name;
};

// One list_audio_sessions entry -- a currently-running process with an active WASAPI session,
// which is a different (and narrower-in-one-way, wider-in-another) query than list_processes: it
// catches background-only processes with no top-level window (list_processes' windowed filter
// misses those), but only ones producing audio right now.
struct AudioSessionEntry {
    std::string name;
    std::string display_name;
};

// One list_audio_devices entry -- an active WASAPI endpoint, for the Desktop (outputs) and Mic
// (inputs) device pickers: which device a source records from, switchable without changing the
// system default.
struct AudioDeviceEntry {
    std::string id;    // AudioSourceSpec::device (config.h) stores exactly this
    std::string name;
    bool        is_default = false;
    bool        input = false;  // a capture endpoint (Mic's picker), else a render endpoint
};

class ScreenHost {
public:
    virtual ~ScreenHost() = default;
    virtual void RequestRepaint() = 0;
    // Owner for native modal dialogs (folder picker). The overlay is WS_EX_TOPMOST, so an
    // unowned dialog opens behind it; an owned one stays above its owner.
    virtual HWND OwnerWindow() const { return nullptr; }
    // Settings is recording a new shortcut: the engine's own hotkeys must neither fire nor swallow
    // the keys meanwhile (hotkeys_suspend). The host renews the engine's lease while this is true.
    virtual void SetHotkeysSuspended(bool suspended) { (void)suspended; }
    virtual void SwitchScreen(ActiveScreen screen) = 0;
    virtual void ShowModal(const std::wstring& title, const std::wstring& msg,
                           bool is_danger, std::function<void()> on_confirm,
                           std::function<void()> on_cancel = nullptr,
                           ModalButtons buttons = {}) = 0;
    // A menu of `options` against `anchor` (view pixels). `on_select` gets the pick (never -1);
    // `on_close` runs after any close, a pick or a dismissal (Esc, a click outside).
    virtual void ShowDropdown(const D2D1_RECT_F& anchor, const std::vector<std::wstring>& options,
                              int selected, std::function<void(int)> on_select,
                              std::function<void()> on_close = nullptr) = 0;
    // Any other popup (the colour editor) against `anchor` (view pixels): the host places it, draws
    // it above the page, routes input to it first and destroys it once it reports Closed().
    virtual void ShowPopup(std::unique_ptr<Popup> popup, const D2D1_RECT_F& anchor) = 0;
    // list_windows off the UI thread. Runs
    // on a dedicated worker thread with its own PipeClient connection (a second connection, not
    // GetPipeClient()'s -- that one belongs to the UI thread and PipeServer already accepts
    // concurrent connections); `on_result` is invoked back on the UI thread once the response
    // lands, or with an empty list on any failure/timeout. If a second call arrives before the
    // first's result lands, the first's `on_result` is simply never invoked (superseded, not an
    // error) -- callers must not assume exactly one call to `on_result` per RequestWindowListAsync
    // call, only "at most one, and only for the most recent request."
    virtual void RequestWindowListAsync(std::function<void(std::vector<WindowEntry>)> on_result) = 0;
    // Same off-UI-thread contract as RequestWindowListAsync above, for list_monitors -- an empty
    // config's Monitors tab used to pay a blocking round trip on the UI thread the moment
    // capture.target switched to "monitor" (or any other rebuild while the cache was cold).
    // `on_result` is invoked with an empty vector on any failure/timeout, and at most once per call
    // (a superseded call's `on_result` is simply never invoked, same as the window-list contract).
    virtual void RequestMonitorListAsync(std::function<void(std::vector<MonitorEntry>)> on_result) = 0;
    // Same off-UI-thread contract, for `devices.size()` capture_monitor_thumbnail round trips.
    // Deliberately one worker thread and one PipeClient connection processing `devices` in
    // sequence rather than one worker per
    // device -- each request makes the engine open a standalone D3D11 device + WGC session
    // (CaptureMonitorThumbnailToPng), so firing them concurrently would mean N simultaneous WGC
    // sessions competing for the same GPU a user may be gaming on. `on_result` is invoked once
    // per device as its result arrives (ok=false on that device's failure/timeout), never more than
    // once per device, and not necessarily for every device if the overlay closes mid-fetch.
    virtual void RequestMonitorThumbnailsAsync(
        std::vector<std::string> devices,
        std::function<void(std::string device, bool ok, UINT w, UINT h, std::vector<uint8_t> bgra)>
            on_result) = 0;
    // Same contract for capture_window_thumbnail, keyed by the list_windows hwnd. Shares the
    // monitor fetch's one-at-a-time rule: the caller never has both in flight at once.
    virtual void RequestWindowThumbnailsAsync(
        std::vector<uint64_t> hwnds,
        std::function<void(uint64_t hwnd, bool ok, UINT w, UINT h, std::vector<uint8_t> bgra)>
            on_result) = 0;
    // Same off-UI-thread contract as RequestMonitorListAsync, for list_processes -- the Audio tab's
    // BuildAudioPage (process_display_names_ prefetch, "Add app" picker) and the Exclusions
    // tab's BuildExclusionRows ("Add process" picker) all round-trip the same query; every caller
    // shares one cache (screens.h's process_list_cache_/process_display_names_). `on_result` is
    // invoked with an empty vector on any failure/timeout, at most once per call (a superseded
    // call's `on_result` is simply never invoked, same contract as the other Async methods here).
    virtual void RequestProcessListAsync(std::function<void(std::vector<ProcessEntry>)> on_result) = 0;
    // Same off-UI-thread contract, for list_audio_sessions -- BuildAudioPage' "Live now" list.
    // A genuinely empty result (nothing currently producing audio) is a normal steady state, not a
    // failure -- unlike RequestMonitorListAsync, callers of this one should NOT treat an empty reply
    // as "retry on the next rebuild" (see screens.cpp's audio_sessions_fetched_ comment).
    virtual void RequestAudioSessionListAsync(
        std::function<void(std::vector<AudioSessionEntry>)> on_result) = 0;
    // Same off-UI-thread contract, for list_audio_devices -- the Desktop track's device picker
    // (BuildAudioPage). An empty reply is a real failure (there's always at least one active
    // render endpoint on a machine with audio hardware), not a normal steady state.
    virtual void RequestAudioDeviceListAsync(
        std::function<void(std::vector<AudioDeviceEntry>)> on_result) = 0;
    // Off-UI-thread contract for set_config: own worker thread + PipeClient connection,
    // same shape as the other Async methods here. Unlike the
    // list fetches above, a caller must apply *every* reply, not just the most recent -- each one
    // corresponds to a distinct write the engine actually accepted or rejected, not a re-queryable
    // "give me current state" snapshot a superseded call could safely drop. `on_result` receives the
    // raw response (nullopt on failure/timeout/overlay-closed), same shape `PipeClient::Request`
    // already returns, so a caller's existing ok/error parsing carries over unchanged.
    void RequestSetConfigAsync(const ombra::JsonValue& patch,
                               std::function<void(std::optional<ombra::JsonValue>)> on_result) {
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("set_config"));
        req.Set("config", patch);
        RequestAsync(std::move(req), 3000, std::move(on_result));
    }
    // Any one-shot request (set_config, save_now, set_armed) off the UI thread, same every-reply
    // contract as above.
    virtual void RequestAsync(ombra::JsonValue req, DWORD timeout_ms,
                              std::function<void(std::optional<ombra::JsonValue>)> on_result) = 0;
    // Off-UI-thread contract, for FetchPreviewAsync's preview_config + get_status round trips --
    // chained right after set_config in every commit path, so fixing only set_config would leave
    // two more blocking calls on the UI thread immediately after. Both responses come back through
    // one `on_result` call so FetchPreview's existing parsing logic doesn't need to split across two
    // callbacks; either may be nullopt independently on its own failure/timeout.
    virtual void RequestPreviewAsync(
        const ombra::JsonValue& config,
        std::function<void(std::optional<ombra::JsonValue> preview_resp,
                            std::optional<ombra::JsonValue> status_resp)> on_result) = 0;
    // LoadFileIcon (file_icon.h) for every path, on one worker thread; `on_result` gets them all
    // at once on the UI thread (a failed one with empty `bgra`), so a page of icons costs one
    // rebuild. Not invoked if the overlay closes first. No pipe: the files are read in this process.
    virtual void RequestFileIconsAsync(std::vector<std::wstring> paths, UINT px,
                                       std::function<void(std::vector<FileIcon>)> on_result) = 0;
    virtual void CloseOverlay() = 0;
    // Runs `work` on a detached worker thread; it reaches the UI thread again through a UiPoster
    // taken on the UI thread before starting it. Hosts without a window (the headless tests) run
    // it inline.
    virtual void RunAsync(std::function<void()> work) { work(); }
    // Called on the UI thread; the poster it returns is safe on any thread and never touches the
    // host (it holds the window handle by value). A posted `fn` runs on the UI thread, or is
    // dropped if the overlay closed first, so it must not own anything that needs releasing.
    using UiPost = std::function<void(std::function<void()>)>;
    virtual UiPost UiPoster() { return [](std::function<void()> fn) { fn(); }; }
    // The overlay's D3D11 device if it was created with video support, else null (no preview).
    virtual ID3D11Device* VideoDevice() { return nullptr; }
    // While on, the active screen's OnFrameTimer runs once per display refresh (clip playback).
    virtual void SetFrameTimer(bool on) { (void)on; }
    // While on, Settings' OnMeterTick runs about 30 times a second (the Audio page's meters).
    virtual void SetMeterTimer(bool on) { (void)on; }
    virtual PipeClient& GetPipeClient() = 0;
    virtual Theme& GetTheme() = 0;
};

class HomeScreen {
public:
    explicit HomeScreen(ScreenHost* host);

    void MarkEngineUnavailable();  // no status reply: what Home last knew is no longer true
    void UpdateState(const ombra::JsonValue* status_resp, const ombra::JsonValue* config_resp);
    void Layout(const D2D1_RECT_F& bounds, float dpi_scale);
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale);

    bool OnMouseMove(D2D1_POINT_2F pt);
    CursorKind CursorAt(D2D1_POINT_2F pt) const;
    bool OnMouseDown(D2D1_POINT_2F pt);
    bool OnMouseUp(D2D1_POINT_2F pt);
    bool OnKeyDown(UINT vk);

    // Headless testing accessors
    size_t ActionCardCount() const { return 3; }
    // Tile geometry is assigned in Layout(); a test reads it back rather than recomputing it.
    const D2D1_RECT_F& ReplayCardBoundsForTest() const { return replay_card_.bounds; }
    const D2D1_RECT_F& OpenRecordingsCardBoundsForTest() const { return clips_card_.bounds; }
    const D2D1_RECT_F& StatsBoundsForTest() const { return stats_bounds_; }
    const D2D1_RECT_F& HintBarBoundsForTest() const { return hint_bounds_; }
    void ClickBufferToggleForTest(bool checked) { replay_card_.toggle.on = checked; replay_card_.toggle.on_change(checked); }
    // output.root, expanded: where the Clips screen looks.
    const std::wstring& OutputFolder() const { return output_folder_str_; }
    bool BufferToggleCheckedForTest() const { return replay_card_.toggle.on; }
    const std::wstring& ReplayDetailForTest() const { return replay_card_.detail; }

private:
    ScreenHost* host_ = nullptr;
    D2D1_RECT_F bounds_{};

    // Instant Replay (its foot is the on/off switch), Record (a placeholder until manual record
    // mode exists) and Clips. Screenshot was dropped from Home; its hotkey still works.
    ActionTileControl replay_card_;
    ActionTileControl record_card_;
    ActionTileControl clips_card_;
    std::wstring      hint_keys_;         // hotkeys.save_replay
    std::wstring      hint_suffix_ = L"to save your replay";
    D2D1_RECT_F       hint_bounds_{};     // "Press [keys] to save ...", under the tiles
    D2D1_RECT_F       stats_bounds_{};    // live stats line, under the hint

    std::wstring output_folder_str_;
    bool         is_armed_ = true;  // the Instant Replay switch (get_status.enabled); matches the tile's switch
    // Switched on but nothing recording: Only in games with no game open, or One window with its
    // program closed. Replaces the tile's "ACTIVE" line, which would be a lie.
    std::wstring waiting_text_;
    bool engine_unavailable_ = false;  // the last status request got no reply
    std::wstring active_text_;
    uint64_t     set_armed_generation_ = 0;
    std::wstring live_stats_str_;
    void RefreshReplayDetail();  // the Instant Replay tile's state line: off, waiting or keeping
    std::wstring RecordingSubject() const;
    std::string  status_target_;       // get_status.target
    std::wstring running_game_;        // get_status.current_game
    std::wstring window_name_;         // capture.target_window
    std::wstring display_label_;       // the one recorded display's name, once list_monitors answers
    int          display_count_ = 1;
    uint64_t     display_label_generation_ = 0;
};

class SettingsScreen {
public:
    explicit SettingsScreen(ScreenHost* host);

    void InitSchema(const std::vector<FieldSpec>& specs, const ombra::JsonValue* config_obj);
    // A SettingsRail page. Test/deep-link use.
    void SetActiveRail(int index);
    // A sub-tab of the active page, in kSettingsPages' order (Capture: Video, Audio); a no-op on a
    // page without sub-tabs. Test/deep-link use, same contract as SetActiveRail.
    void SetActiveSubTab(int index);

    // --snapshots only (no engine to answer list_monitors): fills the Monitors grid with `count`
    // fake monitors, each with a synthetic gradient preview (unless !thumbs). `layout` places the
    // first three as a real desktop would (negative coordinates, a portrait screen).
    void SeedMonitorsForSnapshot(int count, bool layout = false, bool thumbs = true);
    // Window-mode grid with `count` fake windows; `saved` is the staged capture.target_window.
    void SeedWindowsForSnapshot(int count, const char* saved);
    // Same for the Audio tab: per-app mode with Desktop, Mic, and three app sources (two grouped),
    // then scrolled `scroll_y` px so the cards below the fold are in frame.
    void SeedAudioForSnapshot(float scroll_y, bool per_app = true);
    // The overlay's 1 s timer: while the Audio page is built, asks the engine for audio_status and
    // updates each card's tag in place (never a rebuild: that would break a drag in progress).
    void OnStatusTick();
    // The meter timer: reads each source's level and returns whether a meter changed (repaint).
    bool OnMeterTick();
    // The overlay hides or leaves Settings: stop the timer and let go of the endpoints.
    void StopMeters();
    // Settings shows again: if the Audio page's meters are built, read and tick them.
    void SyncMeters();
    // audio_status's reply, applied to the source rows' status lines. Public for tests.
    void ApplyAudioStatus(const ombra::JsonValue* resp);  // null: engine unreachable, tags cleared
    // The host widens the settings row by MemoryCardExtraWidth() and says which side the card takes
    // (opposite the rail); inside=false hangs it outside `bounds`; visible=false when ui.memory_card is off or the view is too narrow.
    void SetMemoryCardSlot(bool visible, bool on_left, bool inside) {
        memory_card_slot_ = visible; memory_card_left_ = on_left; memory_card_inside_ = inside;
    }
    static float MemoryCardExtraWidth(float dpi_scale);
    // --snapshots only (no engine): a typical 120 s / 1 monitor / 2 source breakdown, optionally with a staged edit.
    void SeedMemoryForSnapshot(bool with_projection);
    // --snapshots only: this laptop's two GPUs, the RTX driving the display and recording (`panel`: the
    // 780M does both, the laptop's own screen; `game_on_rtx`: and a game on the RTX records there).
    void SeedRecordingGpusForSnapshot(bool cpu_fallback = false, bool panel = false, bool game_on_rtx = false);
    // --snapshots only: a list_games reply like this laptop's (17 entries, some not games, two
    // added by hand including a javaw.exe) plus two "looked like a game" apps.
    void SeedGamesForSnapshot();
    // get_status's run fields (current_game, target, mode, enabled), for --drive and tests.
    void SeedRunStatusForSnapshot(const ombra::JsonValue& status);
    // The Updates card in one state (update_status's fields), for --drive and tests.
    void SeedUpdateForSnapshot(const std::string& state, const std::string& kind);
    // A list_games reply, as FetchGamesAsync applies it. Public for tests.
    void ApplyGamesReply(const ombra::JsonValue* resp);
    // The Games page's list as last built (empty when the page isn't showing).
    const std::vector<GameRow>& GameRowsForTest() const { return game_rows_; }
    std::shared_ptr<RowList> GamesListForTest() const { return games_list_.lock(); }
    void OpenAddGamePickerForTest() { OpenAddGamePicker(D2D1::RectF(0, 0, 0, 0)); }
    // Scrolls the games list (clamped when it's next arranged), as the wheel over it would.
    void ScrollGamesListForTest(float y) { games_list_scroll_ = y; rows_dirty_ = true; }
    // Reads sent_reports\ again when Diagnostics is next built (a fixture just wrote records).
    void ReloadSentRecords() { sent_loaded_ = false; sent_open_.clear(); rows_dirty_ = true; }
    std::shared_ptr<SentList> SentListForTest() const { return sent_list_.lock(); }
    // games.list as it would be saved: the staged value, else the committed one.
    std::vector<ombra::GameEntry> StagedGameDecisions() const;
    // --snapshots only: the rate "match display" resolves to, as preview_config would report it.
    void SeedDisplayFpsForSnapshot(int fps) { preview_display_fps_ = fps; }
    void SeedSourceSizeForSnapshot(uint32_t w, uint32_t h) { preview_source_w_ = w; preview_source_h_ = h; }
    void UpdateConfigValue(const std::string& key, const ombra::JsonValue& val);
    void Layout(const D2D1_RECT_F& bounds, float dpi_scale);
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale);

    bool OnMouseMove(D2D1_POINT_2F pt);
    CursorKind CursorAt(D2D1_POINT_2F pt) const;
    bool OnMouseDown(D2D1_POINT_2F pt);
    bool OnMouseUp(D2D1_POINT_2F pt);
    bool OnKeyDown(UINT vk);
    // WM_CHAR, for the text field being edited (if any).
    bool OnChar(wchar_t ch);
    // A text field has an edit in progress: Esc cancels that edit instead of closing the overlay.
    bool IsEditingText() const {
        return TemplateFieldFocused();
    }
    // A shortcut row is recording: the overlay sends it every key down/up (WM_SYSKEY* too) instead
    // of its usual handling, Esc and Alt+F4 included.
    bool IsRecordingShortcut() const {
        auto r = recording_row_.lock();
        return r && r->Recording();
    }
    void OnShortcutKey(UINT vk, bool down);
    // Overlay hidden or lost focus.
    void CancelShortcutRecording();
    // --snapshots only: what get_status would report for hotkeys; `failed_key` is left unregistered.
    void SeedHotkeyStatusForSnapshot(const char* backend, const char* failed_key = nullptr);
    // --snapshots only: the row for `key` shows recording with `held_mods` down and `error` under it.
    void SeedShortcutRecordingForSnapshot(const char* key, UINT held_mods, const std::wstring& error);
    struct ShortcutRowView {
        std::shared_ptr<SettingRow> row;
        std::shared_ptr<KeyCaptureControl> capture;
        std::shared_ptr<IconButtonControl> reset;  // shown while the combo isn't the default
        std::wstring detail, default_spec;         // what the row's help says while not recording
    };
    ShortcutRowView ShortcutRowForTest(const std::string& key) const;
    void OnMouseWheel(float delta);

    // True if some row's severity dot is currently hovered -- fills *text_out/*pos_out (device
    // pixels, same space as mouse events) with its tooltip text and anchor point. OverlayHost
    // queries this once per repaint and draws the tooltip last, above everything else.
    bool QueryHoverTooltip(std::wstring* text_out, D2D1_POINT_2F* pos_out) const;

    // Headless testing accessors
    size_t TotalWiredControls() const;
    size_t GroupHeadingCount() const;
    const std::vector<FieldSpec>& Specs() const { return specs_; }
    // Lets selftest confirm BuildAudioPage actually
    // produced rows in the currently-built group list, not just that the build compiles -- the
    // Advanced tab's Audio card is below the fold in the fixed-size offscreen snapshot, so a visual
    // check alone wouldn't catch it silently rendering zero rows.
    size_t RowCountForGroup(const std::wstring& group_name) const;
    bool AudioPerAppModeForTest() const { return audio_per_app_mode_; }
    // Staging contract (audio_sources_dirty_): stages a gain_db change on
    // audio_sources_ui_[0] (Desktop) exactly the way a live volume-slider drop would
    // (CommitAudioSourcesNoRealloc), without simulating a real mouse drag. Requires
    // BuildAudioPage to have already run at least once (e.g. via SetActiveSubTab onto Audio)
    // so audio_sources_ui_ is populated.
    void StageAudioGainForTest(double gain_db);
    double FirstAudioGainForTest() const {
        if (audio_sources_ui_.empty()) return 0.0;
        const auto* v = audio_sources_ui_.front().Find("gain_db");
        return v ? v->num : 0.0;
    }
    void DiscardAudioSourcesForTest() { DiscardAudioSources(); }
    // Drives the real SaveAll commit path (audio's own dirty flag included) -- same
    // "no widget-click simulation" rationale as ApplySettingChangeForTest above.
    void SaveAudioSourcesForTest() { SaveAll(); }
    bool AudioSourcesDirtyForTest() const { return audio_sources_dirty_; }
    // Reads back gain_db from the currently-committed config (current_config_), not the staged
    // audio_sources_ui_ -- lets a test tell "staged but not yet saved" apart from "saved."
    double CommittedDesktopGainForTest() const;
    // Simple mode used to leave Mic at its normal 220dip card width once every per-app card was
    // hidden, with nothing left to wrap next to -- an orphaned narrow card beside a large dead
    // gutter. Returns -1 if no card with this id was built.
    // Returns the card's arranged bounds (empty rect if no such card was built).
    std::shared_ptr<SettingRow> AudioRowForTest(const std::string& id) const {
        for (const auto& r : audio_rows_)
            if (r.id == id) return r.row;
        return nullptr;
    }
    // The track cards' titles, in page order.
    std::vector<std::wstring> AudioTrackTitlesForTest() const {
        std::vector<std::wstring> out;
        for (const auto& t : audio_track_cards_) out.push_back(t.card->title);
        return out;
    }
    std::shared_ptr<CardContainer> AudioTrackCardForTest(const std::string& member_id) const {
        for (const auto& t : audio_track_cards_)
            if (t.member_id == member_id) return t.card;
        return nullptr;
    }
    std::shared_ptr<DropSlot> AudioDropSlotForTest() const { return audio_drop_slot_; }
    bool DraggingAudioForTest() const { return dragging_audio_; }
    // A string field of the staged (unsaved) audio source `id`; empty if absent.
    std::string StagedAudioFieldForTest(const std::string& id, const char* key) const {
        for (const auto& s : audio_sources_ui_) {
            const auto* idv = s.Find("id");
            if (!idv || idv->str != id) continue;
            const auto* v = s.Find(key);
            return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string();
        }
        return {};
    }
    // Arranged bounds of every Monitors tile, in flow order (empty if the tab isn't showing them).
    std::vector<D2D1_RECT_F> MonitorCardBoundsForTest() const {
        std::vector<D2D1_RECT_F> out;
        for (const auto& child : scroll_area_.children) CollectMonitorCards(child, out);
        return out;
    }
    // Width of the preview a display tile would draw (0: placeholder).
    UINT MonitorThumbWidthForTest(const std::string& device) const {
        auto it = monitor_thumb_cache_.find(device);
        return it == monitor_thumb_cache_.end() ? 0 : it->second.w;
    }
    // Layout()'s rows_dirty_/bounds-changed gate exists so a same-bounds repaint doesn't destroy
    // and recreate every row widget (screens.h comment above rows_dirty_: that used to wipe
    // transient state like a text field's edit state). Counts real RebuildGroupRows() calls so a
    // test can pin "same bounds twice -> one rebuild" directly, instead of through a pipe-call
    // proxy that a cache elsewhere could satisfy even if the gate itself broke.
    size_t RebuildCountForTest() const { return rebuild_count_for_test_; }
    // Peak nesting depth RebuildGroupRows' real body has reached -- the re-entrancy guard (screens.h
    // comment above rebuilding_group_rows_) exists specifically to keep this at 1.
    size_t MaxRebuildBodyDepthForTest() const { return max_rebuild_body_depth_for_test_; }
    // Layout()'s real geometry: the rail's measured box inside the panel.
    const D2D1_RECT_F& RailBoundsForTest() const { return rail_.bounds; }
    // Draw-then-click tests: the widgets a real click lands on.
    const NavRail& RailForTest() const { return rail_; }
    const TabBar*  SubTabBarForTest() const { return tabs_.tabs.empty() ? nullptr : &tabs_; }
    const SaveBar& SettingsBarForTest() const { return save_bar_; }
    int ActiveSubTabForTest() const { return subtab_[static_cast<size_t>(rail_.selected)]; }
    // The page's view: below the sub-tabs, above the Save bar.
    const D2D1_RECT_F& ViewBoundsForTest() const { return scroll_area_.bounds; }
    float                    ScrollYForTest() const { return scroll_area_.scroll_y; }
    // The control last built for a setting key, wherever the page put it (a row, a section card,
    // a feature card); null if the current page has none.
    std::shared_ptr<Widget> ControlForTest(const std::string& key) const {
        const auto it = controls_by_key_.find(key);
        return it == controls_by_key_.end() ? nullptr : it->second.lock();
    }
    const D2D1_RECT_F& CardBoundsForTest() const { return card_bounds_; }
    // The active page's top-level widgets in draw order -- lets a test assert what a page is made
    // OF (a card vs a bare row), not just how many controls it wired.
    const std::vector<std::shared_ptr<Widget>>& PageChildrenForTest() const { return scroll_area_.children; }
    // The staged (not yet saved) value for a key, or nullptr if nothing is staged for it.
    const ombra::JsonValue* StagedValueForTest(const std::string& key) const {
        auto it = pending_changes_.find(key);
        return (it == pending_changes_.end()) ? nullptr : &it->second;
    }
    // "track" (AudioSourceSpec::track, config.h) was deleted -- BuildAudioPage's
    // materialized-default entry used to stamp it in; this
    // confirms that write is actually gone from the synthesized JsonValue, not just unread.
    bool AudioSourceHasKeyForTest(size_t index, const char* key) const {
        return index < audio_sources_ui_.size() && audio_sources_ui_[index].Find(key) != nullptr;
    }
    // Drives OpenFreshProcessPicker directly instead of simulating a click on the "Add app"/"Add
    // process" buttons -- those widgets live inside GroupSection's private row list, not exposed to
    // tests, same reasoning as ApplySettingChangeForTest above.
    void OpenFreshProcessPickerForTest(std::function<void(const std::vector<ProcessEntry>&)> open_picker) {
        OpenFreshProcessPicker(D2D1::RectF(0, 0, 0, 0), std::move(open_picker));
    }
    // Drives the real ApplySettingChange commit path (RequestSetConfigAsync -> current_config_.Set
    // on ok, FetchPreviewAsync, RebuildGroupRows) by key instead of by simulating a widget click --
    // no existing test accessor exposes row control widgets to click programmatically, and adding
    // one just to reach this would be more surface area than this one narrow hook. No-op if `key`
    // isn't in the loaded schema.
    // Scrolls the page (clamped on the next rebuild/layout); snapshots use it to reach lower cards.
    void ScrollForTest(float y) {
        const float view_h = scroll_area_.bounds.bottom - scroll_area_.bounds.top;
        scroll_area_.scroll_y = std::clamp(y, 0.0f, (std::max)(0.0f, scroll_area_.content_height - view_h));
    }
    float MaxScrollForTest() const {
        return (std::max)(0.0f, scroll_area_.content_height - (scroll_area_.bounds.bottom - scroll_area_.bounds.top));
    }
    void ApplySettingChangeForTest(const std::string& key, const ombra::JsonValue& val) {
        auto it = std::find_if(specs_.begin(), specs_.end(), [&](const FieldSpec& s) { return s.key == key; });
        if (it != specs_.end()) ApplySettingChange(*it, val);
    }
    // The theme page's actions, as its tiles and buttons run them.
    void OpenThemeGalleryForTest(bool open) { OpenThemeGallery(open); }
    bool ThemeGalleryOpenForTest() const { return theme_gallery_open_; }
    void NewUserThemeForTest() { NewUserTheme(); }
    void RenameUserThemeForTest(const std::string& id, const std::string& name) { RenameUserTheme(id, name); }
    void DeleteUserThemeForTest(const std::string& id) { DeleteUserTheme(id); }
    std::string StagedThemeKeyForTest() const { return StagedThemeKey(); }
    std::vector<ombra::UserTheme> StagedUserThemesForTest() const { return StagedUserThemes(); }
    // What a segmented preset control's click does: stage the choice, then the derived bitrate.
    void ChoosePresetForTest(const std::string& key, const std::string& choice) {
        auto it = std::find_if(specs_.begin(), specs_.end(), [&](const FieldSpec& s) { return s.key == key; });
        if (it != specs_.end()) ChoosePreset(*it, choice);
    }
    ApplyKind PendingWorstApplyForTest() const { return pending_worst_apply_; }
    const ombra::JsonValue* CurrentConfigValueForTest(const std::string& key) const {
        return current_config_.Find(key);
    }
    void SaveAllForTest() { SaveAll(); }
    const MemoryCardData& MemoryCardDataForTest() { UpdateMemoryCard(); return memory_card_.data; }
    void DiscardAllForTest() { DiscardAll(); }
    bool HasPendingChangesForTest() const { return HasPendingChanges(); }

    // preview_config + get_status for the staged config, off the UI thread
    // (ScreenHost::RequestPreviewAsync); only the newest request's reply is applied.
    void FetchPreviewAsync();

private:
    // MonitorCardBoundsForTest's walk: the Displays card's screens.
    static void CollectMonitorCards(const std::shared_ptr<Widget>& w, std::vector<D2D1_RECT_F>& out) {
        auto c = std::dynamic_pointer_cast<CardContainer>(w);
        if (!c) return;
        for (const auto& r : c->rows) {
            auto d = std::dynamic_pointer_cast<MonitorDiagram>(r);
            if (auto s = std::dynamic_pointer_cast<RecordingSources>(r)) d = std::dynamic_pointer_cast<MonitorDiagram>(s->displays);
            if (d) for (const auto& sc : d->screens) out.push_back(sc->bounds);
        }
    }

    // Parses preview_config + get_status replies into preview_*.
    void ApplyPreviewReplies(const ombra::JsonValue* resp, const ombra::JsonValue* status_resp);
    void RebuildGroupRows(float dpi_scale);
    // Appearance & Notifications: four section cards -- Look,
    // Panel, Screen frame, Notifications -- each with its dependents inside it.
    void BuildAppearanceLookPage();
    void BuildAppearanceLayoutPage();
    void BuildAppearanceNotificationsPage();
    std::shared_ptr<ColorPickerControl> BuildAccentPicker();
    void OpenAccentEditor(const D2D1_RECT_F& anchor, uint32_t custom);
    // ui.theme's tiles (BuildThemePicker in screens.cpp says which); a user theme's six colours
    // open the same editor as the accent. Themes edits stage ui.themes and ui.theme together.
    enum class ThemeTiles { Compact, Presets, Mine };
    std::shared_ptr<ThemePickerControl> BuildThemePicker(ThemeTiles which);
    void BuildThemeGalleryPage();  // Appearance > Look's "All themes"
    void OpenThemeGallery(bool open);
    std::string StagedThemeKey() const;
    std::vector<ombra::UserTheme> StagedUserThemes() const;
    ombra::UiPaletteColors ThemeColorsFor(const std::string& key) const;
    void StageThemes(const std::vector<ombra::UserTheme>& themes, const std::string& select);
    void NewUserTheme();
    void DeleteUserTheme(const std::string& id);
    void RenameUserTheme(const std::string& id, const std::string& name);  // a name UserThemeNameOk takes
    void OpenThemeNamePrompt(const std::string& id, const D2D1_RECT_F& anchor);
    void OpenThemeFieldEditor(int field, const D2D1_RECT_F& anchor);
    bool theme_gallery_open_ = false;  // Look shows every theme; a rail or tab change closes it
    const FieldSpec* FindSpec(const std::string& key) const;  // implemented specs only
    // Replay & Quality (Replay + Video cards), Output (Saving + File names), and
    // the Advanced sub-tabs: section cards that hold each setting's dependents.
    void BuildReplayQualityPage();
    // Replay -> Buffer storage, file mode: the folder and whether the file fits there; `on_browse`
    // gets the picker that stages a new folder.
    std::optional<DiskLocation> RingFileLocation(std::function<void()>* on_browse);
    // The values a preset or enum key offers, plus the staged one when it's off that list (165 fps on
    // a 165 Hz panel, a hand-edited 90 s), and which one is staged (-1: none).
    struct Choices { std::vector<std::string> values; int selected = -1; };
    // What the staged replay takes ("Uses up to 1608 MB RAM once full (video ..."), from the
    // engine's projection; empty until there is one.
    std::wstring RingUseText() const;
    Choices ChoicesFor(const FieldSpec& spec);
    // Controls for a preset or enum key, staged through ChoosePreset (so a resolution,
    // framerate or preset change re-derives the bitrate); `label` names each value.
    using ChoiceLabel = std::function<std::wstring(const std::string& value)>;
    std::shared_ptr<SelectControl> MakeSelect(const FieldSpec& spec, const ChoiceLabel& label);
    std::shared_ptr<SegmentedControl> MakeSegmented(const FieldSpec& spec, const ChoiceLabel& label);
    std::shared_ptr<SwitchControl> MakeSwitch(const FieldSpec& spec);
    void ConfirmAutotune(const FieldSpec& spec);  // engine.autotune: a modal, then the run
    void BuildOutputPage();
    void BuildAdvancedEncoderPage();
    void BuildAdvancedSystemPage();
    // Get logs: the bundle on a worker, then Explorer with it selected. `logs_status_` is the
    // row's note while it runs and after ("Saving…", an error).
    void ExportLogs();
    bool logs_busy_ = false;
    std::wstring logs_status_;
    // Send to developer: the same bundle into data_dir, then `ombra.exe send-logs` (crash_upload.h
    // RunSendLogs), detached so it finishes if the overlay closes; this waits for it while open.
    // The id names the upload before it starts: it is the reference the user quotes in an issue,
    // and the record under "What was sent" keeps it after the overlay closes.
    enum class SendState { Idle, Busy, StillSending, Sent, Failed, NoKey, Unreadable, TooMany };
    void SendLogs();
    void OpenBugReport();  // a new GitHub issue, prefilled, with the reference if one was sent
    SendState   send_state_ = SendState::Idle;
    std::string send_id_;

    // What was sent (the sharing card): sent_reports\ read once each time the page is shown
    // (ShowPage, InitSchema), not on every rebuild. The open row is kept by file name with its
    // text, read when it opened, and the box's offset, so a rebuild (a tile click) keeps it.
    std::shared_ptr<SentList> BuildSentList();
    void ToggleSentRecord(int index);
    void CopySentRecord();
    void OpenSentRecord();
    std::vector<ombra::sent_records::Record> sent_;
    bool sent_loaded_ = false;
    std::wstring sent_open_;       // file name; empty: none open
    std::wstring sent_open_text_;  // its first lines
    bool sent_open_cut_ = false;
    bool sent_copied_ = false;
    float sent_scroll_ = 0.0f;
    std::weak_ptr<SentList> sent_list_;

    // Updates card (Advanced > Diagnostics): the engine's update_status, asked for when the page is
    // built and every status tick while a check or download runs. Kept across rebuilds.
    void RequestUpdateStatus();
    void ApplyUpdateStatus(const ombra::JsonValue* resp);
    void SendUpdateCommand(const char* type);
    UpdateView update_;
    bool update_pending_    = false;
    bool system_page_built_ = false;
    bool update_seeded_     = false;  // a snapshot seed: never overwritten by the engine
    // Shortcuts: a card of KeyCaptureControl rows and a Troubleshooting card with the
    // registration method and what the engine reports is actually live.
    void BuildShortcutsPage();
    // Games: one card whose rows -- led by any apps that only looked like a game -- scroll in a
    // RowList under its fixed title.
    void BuildGamesPage();
    // list_games off the UI thread; `rescan` asks the engine to re-read the launchers first.
    void FetchGamesAsync(bool rescan);
    // Stages games.list with `entry`'s decision set to `is_game`. A detected entry set back to a
    // game (what every launcher scan reports) drops its decision instead of storing a redundant one.
    void StageGameDecision(const ombra::GameEntry& entry, bool is_game);
    void StageGameRemoval(const std::wstring& path);
    void StageGameDecisions(const std::vector<ombra::GameEntry>& decisions);
    // "+ Add a game": running programs (by full exe path) and "Browse for a folder…".
    void OpenAddGamePicker(const D2D1_RECT_F& anchor);
    // list_games: launcher-detected entries only (each at the scan's own default, a game) -- the
    // user's decisions come from the staged/committed games.list, so a staged edit shows at once
    // and Discard reverts it without a refetch. Once per overlay open; Rescan fetches again.
    struct SeenGame { std::wstring name, path; };
    std::vector<ombra::GameEntry> games_detected_;
    std::vector<SeenGame> games_seen_;
    bool     games_fetched_ = false;
    bool     games_pending_ = false;
    bool     games_failed_  = false;  // the engine didn't answer: only the user's own entries show
    uint64_t games_generation_ = 0;   // only the newest list_games reply applies
    // Row icons by source file (the scan's `icon`, else an exe entry's own path). Failures are kept
    // too (empty `bgra`), or every rebuild would ask for them again; Rescan drops those. At most one
    // fetch in flight; rows that miss it set `game_icons_skipped_`, and its reply rebuilds the page
    // so they are asked for then.
    std::map<std::wstring, std::shared_ptr<const FileIcon>> game_icons_;
    bool game_icons_pending_ = false;
    bool game_icons_skipped_ = false;
    std::shared_ptr<const FileIcon> GameIcon(const std::wstring& source, std::vector<std::wstring>& missing) const;
    // The list's scroll offset, kept here because every toggle rebuilds the page and its widget.
    float games_list_scroll_ = 0.0f;
    std::weak_ptr<RowList> games_list_;
    std::vector<GameRow> game_rows_;  // what games_list_ shows, as data
    bool games_edit_list_ = false;    // ⋯ "Edit list": a × on the entries the user added
    // Last pointer position (OnMouseMove): the wheel scrolls the games list when it's over it.
    D2D1_POINT_2F last_mouse_pt_{};
    void OnShortcutRecordingChanged(const std::shared_ptr<KeyCaptureControl>& capture, const std::string& key,
                                    bool recording);
    // The row's help line follows its capture: the action's detail, "Press the new shortcut", or why
    // the last key was refused.
    void RefreshShortcutLine(const std::string& key);
    struct ShortcutRowRefs {
        std::weak_ptr<SettingRow> row;
        std::weak_ptr<KeyCaptureControl> capture;
        std::weak_ptr<IconButtonControl> reset;
        std::wstring detail, default_spec;
    };
    std::map<std::string, ShortcutRowRefs> shortcut_rows_;
    std::weak_ptr<KeyCaptureControl> recording_row_;
    std::string recording_key_;  // survives a rebuild: the new row for this key picks recording back up
    bool recording_restored_ = false;  // this rebuild put recording_key_'s row back
    UINT recording_restore_mods_ = 0;  // --snapshots only: what that row shows once rebuilt
    std::wstring recording_restore_error_;
    // get_status "hotkeys": what's registered right now, for the committed config.
    struct HotkeyBindingView {
        std::string key, spec, backend;
        bool ok = false;
    };
    struct HotkeyStatusView {
        bool valid = false;
        std::string policy;
        std::vector<HotkeyBindingView> bindings;
    };
    HotkeyStatusView hotkey_status_;
    void ApplyHotkeyStatus(const ombra::JsonValue& status);
    // The file-name field (Output). It stages every keystroke, and every staged edit rebuilds the
    // page, so focus and caret ride across the rebuild here; `original` is what Esc puts back.
    std::weak_ptr<TemplateFieldControl> template_field_;
    struct TemplateEdit { std::wstring original; size_t caret = 0; };
    std::optional<TemplateEdit> template_edit_;
    bool TemplateFieldFocused() const {
        auto f = template_field_.lock();
        return f && f->focused;
    }
    void EndTemplateEdit(bool revert);
    // Stages new_val for spec.key instead of committing it -- see pending_changes_'s comment.
    // Re-derives the overlay's appearance (accent, panel placement, screen frame) from saved +
    // staged config, so a staged edit previews live and Discard reverts it.
    void SyncUiTheme();
public:
    // Windows' light/dark changed (WM_SETTINGCHANGE "ImmersiveColorSet"): re-resolves ui.theme
    // "system" from saved + staged config.
    void OnSystemThemeChanged() { SyncUiTheme(); }
private:
    void ApplySettingChange(const FieldSpec& spec, const ombra::JsonValue& new_val);
    // Returns the value a row should currently display: the staged edit if one is pending for
    // `key`, otherwise the last actually-committed value from current_config_. Every row builder
    // reads through this instead of current_config_.Find directly, so a staged-but-unsaved edit
    // shows immediately and survives a RebuildGroupRows (tab switch, another field's edit) the same
    // way audio_sources_ui_ already does for the Audio tab.
    std::map<std::string, int> keys_in_flight_;  // staged keys of Saves whose reply hasn't landed
    const ombra::JsonValue* EffectiveConfigValue(const std::string& key) const;
    // True while any field (pending_changes_) or the audio panel (audio_sources_dirty_) has a
    // staged, not-yet-saved edit -- the one condition the Save bar's visibility and Layout()'s
    // space reservation for it both key off.
    bool HasPendingChanges() const { return !pending_changes_.empty() || audio_sources_dirty_; }
    // pending_worst_apply_ from scratch over pending_changes_: an edit can also leave staging.
    void RecomputePendingWorstApply();
    // Stages audio_sources_ui_/audio_per_app_mode_, or clears the staging when they match the
    // saved config again.
    void StageAudioSources();
    // Commits every staged field plus (if dirty) the staged audio_sources_ui_ as one set_config
    // patch -- the shared replacement for ApplySettingChange/ApplySettingChanges/SaveAudioSources
    // each separately reaching the engine per edit. Confirms once via a Realloc modal if any staged
    // key's ApplyKind is Realloc (pending_worst_apply_), not once per key.
    void SaveAll();
    // Throws away every staged field edit and the staged audio panel edit, reloading rows from
    // current_config_ -- the shared replacement for a per-tab Discard.
    void DiscardAll();
    // encode.quality_preset and capture.framerate both feed the engine's fps-scaled bitrate
    // derivation (config.cpp's QualityPresetToBitrate, which needs the capture resolution only the
    // engine knows). After either is staged while a real preset (not "custom") is selected, this
    // asks preview_config for the derived encode.bitrate_kbps off the UI thread and stages it when
    // the reply lands. set_config doesn't derive, so without this the preset would change a label
    // and nothing else. A newer edit or a Discard/reopen drops an older reply
    // (bitrate_sync_generation_).
    void SyncDerivedBitrateAsync(const FieldSpec& spec);
    void ChoosePreset(const FieldSpec& spec, const std::string& choice);
    uint64_t bitrate_sync_generation_ = 0;
    uint64_t preview_generation_ = 0;  // FetchPreviewAsync: only the newest reply applies
    // Commits several keys as one set_config patch/round-trip -- for settings that only take
    // effect together (e.g. picking a capture.target_window must also flip capture.target to
    // "window", or the pick is silently inert).
    void ApplySettingChanges(const std::vector<std::pair<FieldSpec, ombra::JsonValue>>& changes);

    std::wstring preview_mb_per_min_;
    std::wstring preview_bitrate_mbps_;  // e.g. "~40 Mbps" -- feeds the Quality
                                         // preset / Capture framerate rows' combined consequence line
    int preview_display_fps_ = 0;  // preview_config's display_fps: what "match display" records at; 0 unknown
    // preview_config's source_width/height: the captured size capture.resolution scales; 0 unknown.
    uint32_t preview_source_w_ = 0;
    uint32_t preview_source_h_ = 0;
    // Ring memory: per-ring sizes for the *edited* config (preview_config) and what the running engine
    // actually holds (get_status). A picture of a configuration, not a live meter -- ring memory is
    // committed at engine start; see ApplyPreviewReplies.
    struct PreviewMemory {
        bool   valid            = false;  // preview_config answered
        double video_ring_bytes = 0.0;    // per pipeline, capacity + overshoot
        double audio_ring_bytes = 0.0;    // per audio source
        bool   engine_valid     = false;  // get_status.memory answered
        double video_pipelines  = 1.0;    // what the engine allocated
        double audio_sources    = 1.0;
        bool   file_backed      = false;  // video rings are mapped files: disk, not RAM
        double max_video_bytes  = 0.0;    // all pipelines, capacity + overshoot: the most they take
        double max_audio_bytes  = 0.0;
        double held_video_bytes = 0.0;    // committed now (get_status.memory.*_committed_bytes)
        double held_audio_bytes = 0.0;
        double engine_bytes     = 0.0;    // private commit minus the rings
        double sys_total_bytes  = 0.0;
        double sys_load_pct     = 0.0;
        double vram_bytes       = -1.0;   // get_status.gpu_dedicated_mb; absent with telemetry off
    } preview_memory_;
    // get_status.recording_gpus / encoder_adapter / gpu_display_owner, for Encoder's Recording GPU
    // card. The engine probes once at startup; the UI never probes a GPU itself.
    struct RecordingGpu {
        std::string vendor;   // encode.adapter value: "nvidia" | "amd"
        std::string backend;  // "NVENC" | "AMF"
        std::string name;
        bool        hevc = false, av1 = false;
        uint32_t    av1_align_w = 1, av1_align_h = 1;  // sizes its AV1 codes exactly (ombra::PlanEncode)
        bool        drives_display = false;  // owns the recorded monitor: auto's first choice
    };
    std::vector<RecordingGpu> recording_gpus_;
    bool                      recording_on_cpu_ = false;  // get_status.encoder_backend == "x264"
    std::string               recording_gpu_now_;  // the adapter the engine records on right now
    std::string               codec_now_;          // get_status encoder_codec: "av1"/"h264", "" before a pipeline runs
    std::string               display_owner_gpu_;
    void ApplyRecordingGpus(const ombra::JsonValue& status);
    // The GPU the staged encode.adapter resolves to (encode_target.cpp's rule: an explicit vendor if
    // present, else the one that drives the display, else NVIDIA, else AMD). nullptr until the engine has reported its GPUs.
    const RecordingGpu* StagedRecordingGpu() const;
    std::vector<ombra::RecordingGpuCaps> RecordingGpuCapsNow() const;  // recording_gpus_ for config.h's helpers
    // Counts the *staged* config would run with (capture.monitors / audio.sources edits included).
    void ProjectedRingCounts(double& pipelines, double& sources) const;
    // buffer.storage as staged. preview_memory_.file_backed is what the running engine has.
    bool StagedRingIsFile() const {
        const auto* v = EffectiveConfigValue("buffer.storage");
        return v && v->type == ombra::JsonValue::Type::String && v->str == "file";
    }
    // Fills memory_card_.data from preview_memory_ and the staged edits.
    void UpdateMemoryCard();
    MemoryCard memory_card_;
    bool memory_card_slot_ = false;   // host reserved a slot beside the rail + card this frame
    bool memory_card_left_ = false;   // ...on the left (rail is on the right)
    bool memory_card_inside_ = false; // slice carved from the row (attached panel) vs. hung outside it

    ScreenHost* host_ = nullptr;
    D2D1_RECT_F bounds_{};

    // The shell: one panel holding the rail, the active page's
    // sub-tabs, the page and the Save bar.
    NavRail          rail_;
    ScrollAreaWidget scroll_area_;
    D2D1_RECT_F      card_bounds_{};  // the panel: rail and content column inside it
    // One TabBar, its tabs swapped with the page; each page keeps its own sub-tab across switches.
    TabBar tabs_;
    std::array<int, kRailCount> subtab_{};
    int  ActiveSubTab() const { return subtab_[static_cast<size_t>(rail_.selected)]; }
    void ShowPage(int rail, int subtab);  // select, reset scroll, rebuild
    // Opens another page from a click inside the page (a "Change GPU" link). The switch rebuilds
    // the page, which would free the widget whose handler asked for it, so it is queued and
    // OnMouseDown applies it after dispatch returns.
    void QueueNavigate(int rail, int subtab) { queued_nav_ = {rail, subtab}; }
    void ApplyQueuedNavigate();
    std::pair<int, int> queued_nav_{-1, -1};

    // Save/Discard, at the content column's foot while anything is staged. Layout() reserves its
    // height (scroll_area_ ends above it), so it never covers the page.
    SaveBar save_bar_;

    std::vector<FieldSpec> specs_;
    ombra::JsonValue current_config_;

    // Layout() runs every RenderFrame (every repaint), not just on real geometry changes -- these
    // let it skip re-running RebuildGroupRows (which throws away and recreates every row/control
    // widget, wiping transient state like a text field's edit state or a dropdown's open flyout)
    // unless the panel bounds/DPI actually changed since the last rebuild.
    D2D1_RECT_F last_rebuild_bounds_{ -1.0f, -1.0f, -1.0f, -1.0f };
    float       last_rebuild_dpi_scale_ = -1.0f;
    bool        rows_dirty_ = true;
    std::map<std::string, std::weak_ptr<Widget>> controls_by_key_;  // ControlForTest; the page builders fill it
    size_t      rebuild_count_for_test_ = 0;
    float       current_dpi_scale_ = 1.0f;  // last dpi_scale seen from Layout(); explicit
                                             // RebuildGroupRows() call sites (tab switch, apply,
                                             // browse) reuse this instead of a hardcoded 1.0f.

    // RebuildGroupRows hands out a live reference into groups_ to row-panel builders
    // (BuildDisplaysCard/BuildAudioPage/BuildExclusionRows) while it's still running. Several
    // of those fire an async host request whose callback calls RebuildGroupRows() again on receipt
    // -- with a host that resolves synchronously (FakeScreenHost, and in principle a real one that
    // happens to answer instantly), that reentrant call would groups_.clear() and repopulate while
    // the outer call is still writing into its now-dangling reference. Instead of recursing, a
    // reentrant call defers: it flags the request and returns, and the outer call loops once more
    // after it finishes normally, so RebuildGroupRows never runs inside itself.
    bool        rebuilding_group_rows_ = false;
    bool        rebuild_requested_while_rebuilding_ = false;
    float       deferred_rebuild_dpi_scale_ = 1.0f;
    // Test-only: how deep RebuildGroupRows' actual body (not a deferred, immediately-returning
    // call) ever nested. Should never exceed 1 with the guard above in place -- see
    // "ui: RebuildGroupRows never re-enters its own body" in tests/ui_tests.cpp.
    size_t      rebuild_body_depth_for_test_ = 0;
    size_t      max_rebuild_body_depth_for_test_ = 0;

    // One card of settings. `items` are the top-level layout items in order -- a full-width row, or a
    // FlexWidget of tiles (Monitors' equal-column grid, Audio's wrapping card flow). Builders use
    // AddRow/AddTile rather than touching `items`, so consecutive tiles share one flow the way the old
    // packer's "run of consecutive rows" did.
    struct GroupSection {
        struct Item {
            std::shared_ptr<Widget> widget;
            float margin_after_dip = 10.0f;
        };
        std::wstring name;
        std::string  tier;
        std::vector<Item> items;
        size_t row_count = 0;  // leaf rows/tiles added (what the tests and the empty-page check count)

        void AddRow(std::shared_ptr<Widget> w) {
            open_flow_.reset();
            items.push_back({std::move(w), 10.0f});
            ++row_count;
        }
        // A panel's card that heads its section. Panels are built last (Raw keys come last in
        // AllSettings()), so without this the thing a page is about lands under its minor rows.
        void AddLead(std::shared_ptr<Widget> w) {
            items.insert(items.begin(), {std::move(w), 16.0f});
            ++row_count;
        }
        // Tiles pack side by side and wrap; `columns` > 0 forces that many equal columns. Returns the
        // FlexItem so the caller can set its preferred width (basis_dip) or fill_line.
        FlexItem& AddTile(std::shared_ptr<Widget> w, float gap_dip, int columns = 0) {
            if (!open_flow_ || open_gap_ != gap_dip || open_columns_ != columns) {
                open_flow_ = FlexWidget::MakeRow(gap_dip, /*wrap=*/true);
                open_flow_->columns = columns;
                open_gap_ = gap_dip;
                open_columns_ = columns;
                items.push_back({open_flow_, gap_dip});
            }
            ++row_count;
            return open_flow_->Add(std::move(w));
        }

    private:
        std::shared_ptr<FlexWidget> open_flow_;
        float open_gap_ = 0.0f;
        int   open_columns_ = 0;
    };
    std::vector<GroupSection> groups_;


    // Capture > Video. capture.monitors has no live add/remove (a change self-restarts the engine).
    void BuildCaptureVideoPage();
    // The Video page's second card: the display tiles (Displays, The game), or the windows to pick
    // from, the saved one first (One window).
    // With `game_tile`, the game beside the displays recorded while it's closed.
    std::shared_ptr<CardContainer> BuildDisplaysCard(std::shared_ptr<TileControl> game_tile);
    std::shared_ptr<TileControl> MakeGameTile(bool recording_now);
    void ApplyRunStatus(const ombra::JsonValue& status);
    // What the engine is running, from get_status; empty until a reply lands.
    std::wstring running_game_;
    std::string run_target_, run_mode_;
    bool run_enabled_ = false;
    // Its icon, picture-sized, so kept apart from game_icons_ (list-row-sized).
    std::shared_ptr<const FileIcon> running_game_icon_;
    std::wstring running_game_icon_source_;
    std::shared_ptr<CardContainer> BuildWindowCard();
    // Fetches list_processes once per overlay open for friendly app names (process_display_names_).
    void PrefetchProcessNames();
    // Shared by BuildAudioPage' process_display_names_ prefetch and both "Add app"/"Add
    // process" on-click pickers (BuildAudioPage, BuildExclusionRows) -- applies one
    // RequestProcessListAsync reply to process_list_cache_/process_display_names_/
    // process_list_fetched_ exactly once, so the three call sites can't drift into three different
    // ideas of what a successful fetch does to the cache.
    void ApplyProcessListResult(std::vector<ProcessEntry> processes);
    // Used by all three "Add app"/"Add process" on-click pickers instead of trusting
    // process_list_cache_ directly: a picker showing whichever process list happened to be cached
    // at overlay-open (or at the last click) can show an app that's since closed.
    // Always re-fetches on click (a fresh
    // CreateToolhelp32Snapshot is cheap, and a picker click is not a hot path), showing "Loading…"
    // meanwhile; falls back to process_list_cache_ only if the fresh reply itself comes back empty
    // (a real transient failure -- there's always at least one process running, same reasoning as
    // the audio-device list's "empty IS a failure" case below).
    void OpenFreshProcessPicker(const D2D1_RECT_F& anchor,
                                 std::function<void(const std::vector<ProcessEntry>&)> open_picker);
    // Capture > Audio: a Sources card (App tracks,
    // ⋯ to add an app or the mic), a card per track of the clip with its sources as rows, Playing
    // now (per-app) and Saved audio. Edits stage into audio_sources_ui_ until Save.
    void BuildAudioPage();
    // Every member of the group `from_gid` joins `to_member`'s track; ungroups none.
    void MixAudioTrackInto(const std::string& from_gid, const std::string& to_member);
    // `exe` (playing, no track yet) gets its own track and Desktop leaves it out (one at a time).
    void PopOutAudioApp(const std::string& exe);
    // The group id `audio_sources_ui_[idx]` mixes under, minting one distinct from every member's id
    // the first time (so splitting a member back out always changes something).
    std::string EnsureAudioGroup(int idx);
    // Starts dragging the row of source `id` (or "live:<exe>") by its grip; OnMouseUp drops it.
    void StartAudioDrag(const std::string& id);
    // A source row's status line from audio_status_ (Off while switched off).
    void StatusNote(const std::string& id, bool enabled, SettingRow* row) const;
    // Re-finds a source's index in `audio_sources_ui_` by kind, at call time rather than at
    // row-build time. Used only for the mic (`kind: "input"`), where at most one entry ever
    // exists so kind alone disambiguates. See FindAudioSourceIndexById for `output_loopback`
    // sources, where kind alone is ambiguous the moment a second one exists.
    int FindAudioSourceIndexByKind(const std::string& kind) const;
    // Re-finds a source's index in `audio_sources_ui_` by its `id` field, at call time rather than
    // at row-build time. Every per-source row control captures the source's `id` (not a
    // pre-resolved index, and not `kind` -- once a per-app source exists, kind:"output_loopback"
    // matches more than one entry) because a control's callback can fire after a *later* rebuild
    // has already cleared and repopulated audio_sources_ui_ (e.g. the list_processes flyout's
    // on-pick callback, which resolves asynchronously) -- an index or kind-based lookup captured
    // at row-build time would then be stale or resolve to the wrong entry, the same class of bug
    // BuildExclusionRows guards against with its `if (i < exclusion_list_.size())` check.
    int FindAudioSourceIndexById(const std::string& id) const;
    // A new mode:"only" app track for `exe`, with an id no other source has and the app's display
    // name as `name` (kept so the card and the saved file's track still read "Riot Client" while
    // the app isn't running). `group_id` empty: its own group.
    ombra::JsonValue NewAppSource(const std::string& exe, const std::string& group_id = {}) const;

    // Drag-to-group + volume slider on top
    // of the group_id dropdown above. Despite the name, no longer commits anywhere -- it just marks
    // audio_sources_dirty_ and rebuilds so the pending edit shows immediately. The name is kept
    // because every caller (a group_id reassignment, an ungroup, a gain_db commit) is still the same
    // "no realloc needed" set of edits, now staged rather than committed; SaveAll() is what actually
    // reaches the engine, for these and for adds/removes alike.
    void CommitAudioSourcesNoRealloc();
    // A drop on a track card: `dragged_id` is a source's `id`, or "live:<exe_name>" for a playing
    // app with no source yet (then a new mode:"only" source is made in the target's group).
    // `target_id` is any source on the target track. Stages like every audio edit.
    void CommitAudioGroupDrop(const std::string& dragged_id, const std::string& target_id);
    // Throws away the staged audio_sources_ui_ and reloads it from current_config_ (the last
    // actually-committed state), clearing audio_sources_dirty_.
    void DiscardAudioSources();

    // capture.monitors picker state -- refetched from list_monitors each time BuildDisplaysCard runs
    // (i.e. on a rail switch or after an apply), cross-referenced against current_config_'s
    // capture.monitors array for which toggles start checked.
    struct MonitorRow {
        std::string  device;
        std::wstring label;
        std::wstring resolution;  // "" if unknown; the screen's detail line
        int          left = 0, top = 0, width = 0, height = 0;  // where the diagram draws it
        bool         positioned = false;
        bool         checked = false;
        bool         primary = false;  // capture.monitors may name it as "primary" instead of its device
        bool         hidden = false;  // decluttered out of the card grid into a "N hidden" chip row
                                       // below -- purely a local Settings-UI display preference
                                       // (hidden_monitor_devices_), NOT capture.monitors/enable
                                       // state; a hidden monitor can still be checked/capturing.
    };
    std::vector<MonitorRow> monitor_rows_;

    // Which monitors are currently hidden from the card grid, by device id -- survives
    // BuildDisplaysCard' monitor_rows_.clear()+refetch (unlike monitor_rows_ itself, which is
    // rebuilt from a fresh list_monitors round-trip on every RebuildGroupRows call). Session-local
    // UI state only, not persisted to config.json -- there's no engine-side concept of "hidden."
    std::set<std::string> hidden_monitor_devices_;

    // list_monitors/capture_monitor_thumbnail/
    // list_audio_sessions round trips are expensive (up to 3s budget each for a thumbnail) and,
    // before this cache, were redone unconditionally on *every* RebuildGroupRows call -- a volume-
    // slider drop or a checkbox toggle re-fetched every monitor's thumbnail even though nothing
    // about the monitor list changed (Finding 3: "clunky, takes a while"). Read from thereafter,
    // cleared only in InitSchema (i.e. an overlay close/reopen), matching the rule: never show a
    // blank/spinner gap, keep showing the last-fetched thumbnail while stale rather than refetching
    // every commit.
    //
    // Fetched off the UI thread via RequestMonitorListAsync/RequestMonitorThumbnailAsync to avoid
    // a visible stall picking "Desktop" as capture.target. `monitor_list_pending_`/
    // `monitor_thumb_pending_` are this function's own re-entrancy guards, since a rebuild fired by
    // the async result landing (RebuildGroupRows re-running this same code) must not fire a second
    // request for data that's already in flight or already cached.
    bool monitor_list_fetched_ = false;
    bool monitor_list_pending_ = false;
    struct MonitorThumbCache {
        UINT w = 0, h = 0;
        std::vector<uint8_t> bgra;
    };
    // An entry first holds the engine's previous grab, read from disk at page build (the UI process
    // is spawned per overlay open, so nothing survives in memory), then the fresh grab once it lands.
    // `_fresh_` marks keys grabbed this session: requested once per open, never retried.
    std::map<std::string, MonitorThumbCache> monitor_thumb_cache_;  // keyed by monitor device id
    std::set<std::string> monitor_thumb_pending_;                   // devices with a fetch in flight
    std::set<std::string> monitor_thumb_fresh_;

    // Window mode's inline grid (BuildWindowChoice): same once-per-overlay-open contract as the
    // monitor list/thumbnails above, cleared in InitSchema. Thumbnails are fetched only for tiles on
    // screen, and never while a monitor thumbnail fetch is in flight (one WGC session at a time).
    std::vector<WindowEntry> window_list_;
    bool window_list_fetched_ = false;
    bool window_list_pending_ = false;
    std::map<uint64_t, MonitorThumbCache> window_thumb_cache_;
    std::set<uint64_t> window_thumb_pending_;
    std::set<uint64_t> window_thumb_fresh_;
    bool windows_show_all_ = false;  // "Show all N windows" expanded

    // Same cache contract as monitor_thumb_cache_ above, for BuildAudioPage' "Live now" list
    // (list_audio_sessions) -- exe names only, since that's all the "Live now" cards need.
    //
    // Fetched off the UI thread via RequestAudioSessionListAsync.
    // `audio_sessions_pending_` is this function's own re-entrancy guard, same role as
    // monitor_list_pending_. Unlike monitor_list_fetched_, `audio_sessions_fetched_` is set true on
    // *every* reply, including an empty one -- "nothing is currently producing audio" is a normal
    // steady state here (unlike "zero monitors"), so retrying on empty the way Monitors does would
    // just re-pay this round trip on every rebuild when nothing is playing. The tradeoff:
    // a transient pipe failure also latches fetched_ true, so a real failure only clears on overlay
    // close/reopen (InitSchema) rather than self-healing on the next rebuild.
    bool audio_sessions_fetched_ = false;
    bool audio_sessions_pending_ = false;
    std::vector<std::string> audio_sessions_cache_;

    // Per-source state by audio.sources id (audio_status). Kept across rebuilds so a rebuilt card
    // starts with the last known tag; cleared with the other caches in InitSchema. A card whose id
    // isn't here draws no tag.
    std::unordered_map<std::string, AudioSourceStatus> audio_status_;
    bool audio_page_built_     = false;  // BuildAudioPage ran in the latest rebuild
    // The Audio page's level meters, rebound on every rebuild. The reader and each source's shown
    // level (by id) outlive rebuilds, so a staged edit doesn't reopen endpoints or reset a meter.
    struct MeterBinding {
        LevelTarget target;
        float gain_db = 0.0f;
        std::weak_ptr<LevelMeter> meter;
    };
    std::vector<MeterBinding> meters_;
    std::unique_ptr<LevelReader> level_reader_;
    std::unordered_map<std::string, float> meter_db_;
    ULONGLONG meter_last_ms_ = 0;
    bool meter_timer_on_ = false;
    bool audio_status_pending_ = false;

    // Friendly display names (controller.h's ReadVersionResourceName, e.g. "Discord" for
    // discord.exe) keyed by bare exe name -- same cache contract as the two above (filled once per
    // overlay open via list_processes, cleared in InitSchema), used for every card title in the
    // Audio panel: already-configured sources, "Live now" cards, and the Add-app/target pickers'
    // option labels. A name with no entry here (not currently running, so nothing to resolve
    // against) falls back to the raw exe name everywhere that reads this map -- same "only names
    // what it can currently see" limitation Windows' own Volume Mixer has.
    //
    // Fetched off the UI thread via RequestProcessListAsync, same shape/tradeoff as
    // audio_sessions_fetched_ above (fetched_ latches true on every reply, not just a non-empty
    // one -- a running process list is realistically never empty, but the failure-vs-legitimately-
    // empty ambiguity is the same for every one of these Async methods, see screens.h's comment on
    // RequestWindowListAsync). `process_list_cache_` is the raw list (every running process, not
    // just ones with a resolved display name), used passively for display-name resolution above. The
    // three on-click pickers (BuildAudioPage' "Add app", BuildExclusionRows'/
    // BuildGameOverrideRows' "Add process") do NOT read this cache directly -- see
    // OpenFreshProcessPicker's comment: a picker showing whichever snapshot was cached at
    // overlay-open (or at an earlier click) can show an app closed long before, so every click
    // re-fetches. `process_list_pending_` only guards this passive prefetch against itself.
    bool process_list_fetched_ = false;
    bool process_list_pending_ = false;
    std::vector<ProcessEntry> process_list_cache_;
    std::map<std::string, std::string> process_display_names_;

    // Desktop card's device picker -- fetched once per overlay open via
    // RequestAudioDeviceListAsync, same shape as the caches above. An empty reply IS a real failure
    // here (unlike audio_sessions_fetched_'s "nothing playing" steady state -- there's always at
    // least one active render endpoint on a machine with audio hardware), so this follows
    // monitor_list_fetched_'s pattern instead: only latches true on a non-empty reply, so a
    // transient failure retries on the next rebuild rather than getting stuck showing nothing.
    bool audio_device_list_fetched_ = false;
    bool audio_device_list_pending_ = false;
    std::vector<AudioDeviceEntry> audio_device_list_cache_;


    // ApplySettingChange/ApplySettingChanges/SaveAudioSources' async set_config commit (RequestSet-
    // ConfigAsync) no longer blocks the UI thread, which means two in-flight writes to the *same*
    // key can now resolve out of order (impossible before -- the synchronous call meant only one
    // set_config round trip was ever outstanding at a time). Bumped once per key at request time;
    // a reply only applies its `current_config_.Set(key, ...)` if the key's counter still matches
    // the value it captured when issued, so an older reply landing after a newer request for the
    // same key can't stomp the newer one back to a stale value. std::unordered_map::operator[]
    // default-constructs to 0 for a key never seen before, so no separate init is needed.
    std::unordered_map<std::string, uint64_t> set_config_seq_by_key_;

    // Settings Save/Discard bar: every field edit used to commit immediately
    // (ApplySettingChange's own RequestSetConfigAsync call, one Realloc confirm modal per Realloc
    // key touched). Now stages here instead -- keyed by config key, same "no separate fetch, no
    // round trip until Save" contract audio_sources_ui_/audio_sources_dirty_ already established for
    // the Audio tab. EffectiveConfigValue() is what every row builder reads through so a staged
    // value shows immediately and survives a rebuild; SaveAll()/DiscardAll() are the only two places
    // that clear it.
    std::unordered_map<std::string, ombra::JsonValue> pending_changes_;
    // Why the last Save didn't take, shown in the Save bar; the edits stay staged for a retry. Empty
    // once a new Save, edit or Discard moves on.
    std::wstring save_error_;
    // Worst (per ApplyKind's Free < Rebuild < Realloc ordering) ApplyKind among pending_changes_'
    // keys -- tracked incrementally as ApplySettingChange/ApplySettingChanges stage, so SaveAll()
    // knows whether to show one Realloc confirm modal for the whole batch without re-deriving it by
    // re-scanning specs_ against pending_changes_' keys.
    ApplyKind pending_worst_apply_ = ApplyKind::Free;

    // SaveAudioSources' async commit has the same out-of-order hazard as set_config_seq_by_key_
    // above, but for audio_sources_dirty_ specifically: the old synchronous call meant no further
    // edit could be staged while a Save was in flight, so clearing the flag on reply always matched
    // "what just got saved is exactly the current staged state." Async breaks that -- a user can
    // drag another gain slider (CommitAudioSourcesNoRealloc), pop an app out of a group, or drop a
    // new per-app source while the first Save's reply is still in flight. Bumped by every one of
    // those staging sites; SaveAudioSources captures it before firing the request and only clears
    // audio_sources_dirty_ if it still matches on reply, so a newer staged edit isn't silently
    // marked saved just because an older Save happened to resolve after it.
    uint64_t audio_sources_generation_ = 0;

    // Staged value of audio.per_app_mode. Reloaded from current_config_ whenever the audio panel
    // isn't dirty, saved together with audio.sources. Simple hides the per-app cards; the engine,
    // not this panel, decides that Simple captures only Desktop + Mic.
    bool audio_per_app_mode_ = false;

    // audio.sources picker state -- read from current_config_ each time BuildAudioPage runs,
    // *unless* audio_sources_dirty_ is set (see below), same "no separate fetch" contract as
    // exclusion_list_ above otherwise. Index 0 is always kept as the output_loopback (desktop)
    // source and any input (mic) source is kept after it, matching main.cpp's "index 0 is the
    // primary/legacy-wired source" convention -- this
    // panel never reorders entries it didn't add itself.
    std::vector<ombra::JsonValue> audio_sources_ui_;
    // Every audio edit (drag-to-group, volume, add/remove app or mic, Desktop mode)
    // used to commit to the engine immediately -- including a live set_config round trip on every
    // single drag drop, and a Realloc confirm modal on every add/remove. Design choice: audio edits
    // now stage locally (this flag) and only reach the engine when the Save button below is
    // clicked, so a user can freely rearrange groups/volumes/sources without a round trip or a
    // modal per gesture. While true, BuildAudioPage skips reloading audio_sources_ui_ from
    // current_config_ so the pending edit survives a rebuild (tab switch, other settings changes);
    // SaveAudioSources() clears it on a successful commit, DiscardAudioSources() clears it and
    // reloads from current_config_ to throw the pending edit away.
    bool audio_sources_dirty_ = false;

    // The Audio page as last built: each source's row (and each playing app's, "live:<exe>") for
    // status updates and the drag, each track's card with one of its sources' ids as drop target.
    struct AudioRowRef { std::string id; std::shared_ptr<SettingRow> row; bool enabled = true; };
    std::vector<AudioRowRef> audio_rows_;
    struct AudioTrackCard { std::string member_id; std::shared_ptr<CardContainer> card; };
    std::vector<AudioTrackCard> audio_track_cards_;
    std::shared_ptr<DropSlot> audio_drop_slot_;  // only while dragging
    bool audio_edit_list_ = false;               // Sources' ⋯ "Edit list": a × on removable rows
    // A drag between StartAudioDrag and OnMouseUp. The hover is a member id of the card under the
    // pointer, "new" over the drop slot, or "" over neither.
    bool          dragging_audio_ = false;
    std::string   drag_audio_id_;
    std::string   drag_audio_hover_id_;
    D2D1_POINT_2F drag_audio_pt_{};             // screen space, for the ghost
    std::shared_ptr<SettingRow> drag_audio_ghost_;
    SoftShadow drag_audio_shadow_;
};

}  // namespace ombra_ui
