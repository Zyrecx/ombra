#pragma once

// The Clips screen: every saved clip under output.root as a grid of thumbnails, and a player for
// one of them with a trim timeline, Save trimmed, Copy, Show in folder and Delete. Browsing files
// is Explorer's job; this is for the clip you just saved.
//
// An isolated instance (tests, --drive) never sees the user's clips: its root is the sandbox's
// own clips folder, whatever output.root says.

#include "ombra-ui/clip_player.h"
#include "ombra/clip_file.h"
#include "ombra-ui/screens.h"
#include "ombra-ui/ui/controls.h"
#include "ombra-ui/ui/theme.h"
#include "ombra-ui/ui/widget.h"

#include <windows.h>
#include <d2d1_1.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ombra_ui {

// The player's keyboard shortcuts, one table: OnKeyDown dispatches from it and anything that
// lists the keys draws from it, so the two can't disagree.
enum class ClipAction { PlayPause, Back5, Forward5, PrevFrame, NextFrame, StartHere, EndHere, Close };
struct ClipShortcut {
    UINT        vk;
    const wchar_t* key;    // as drawn
    ClipAction  action;
    const wchar_t* label;
};
extern const ClipShortcut kClipShortcuts[];
extern const size_t kClipShortcutCount;

// Frame `index`'s time on a clip's 1/fps grid, a quarter frame in: an exact seek there shows that
// frame and not the one before (a seek onto the boundary can resolve either way).
inline double FrameSeekTime(int64_t index, double fps) { return (static_cast<double>(index) + 0.25) / fps; }
// The frame showing at `seconds`.
inline int64_t FrameAt(double seconds, double fps) { return static_cast<int64_t>(std::floor(seconds * fps + 1e-6)); }

struct ClipEntry {
    std::wstring path;
    std::wstring game;         // the per-game folder, else the file name's leading words
    FILETIME     written{};
    uint64_t     bytes = 0;
    bool         info_loaded = false;
    double       duration_s = 0.0;
    std::vector<double> keyframes_s;
    std::vector<uint8_t> thumb_bgra;  // decoded off the UI thread; becomes `thumb` in Draw
    UINT         thumb_w = 0, thumb_h = 0;
    ComPtr<ID2D1Bitmap1> thumb;
};

class ClipsScreen {
public:
    explicit ClipsScreen(ScreenHost* host);
    ~ClipsScreen();

    // Entering the screen: lists `root` (output.root, expanded) again, back on the grid.
    void Open(const std::wstring& root);
    // Leaving it (another screen, or the overlay hiding): stops playback, unloads the clip.
    void Leave();
    // Before the overlay's D3D11 device goes away.
    void ReleaseDeviceResources();
    // Blocks up to `timeout_ms` while a trim is still being written (the process is about to end).
    void WaitForWrites(DWORD timeout_ms);

    // The panel height that fits a 16:9 preview at `width` plus the player's controls. The grid
    // gets the same height, so opening a clip doesn't resize the panel.
    static float PanelHeightFor(float width, const Theme& theme, float dpi_scale);
    void Layout(const D2D1_RECT_F& bounds, float dpi_scale);
    void Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale);
    bool OnMouseMove(D2D1_POINT_2F pt);
    CursorKind CursorAt(D2D1_POINT_2F pt) const;
    bool OnMouseDown(D2D1_POINT_2F pt);
    bool OnMouseUp(D2D1_POINT_2F pt);
    bool OnKeyDown(UINT vk);
    void OnMouseWheel(float delta);
    // The host's frame timer (SetFrameTimer): true when the screen needs a repaint.
    bool OnFrameTimer();
    bool QueryHoverTooltip(std::wstring* text, D2D1_POINT_2F* anchor);

    // Tests and --drive.
    // Clip file names in grid order, under sort `sort` (an index into the sort menu).
    std::vector<std::wstring> GridNamesForTest(int sort);
    void SetClipFactsForTest(int index, uint64_t bytes, double duration_s, const std::wstring& game) {
        clips_[index].bytes = bytes;
        clips_[index].duration_s = duration_s;
        clips_[index].game = game;
    }
    // The split button's menu, as if its arrow was clicked.
    void OpenSaveMenuForTest() { if (save_split_.on_menu) save_split_.on_menu(); }
    const std::wstring& Root() const { return root_; }
    size_t ClipCount() const { return clips_.size(); }
    bool PlayerOpen() const { return selected_ >= 0; }
    void OpenClipForTest(int index) { OpenClip(index); }
    // A refresh's reply holding these of the current entries, in this order.
    void ApplyListForTest(const std::vector<int>& keep) {
        std::vector<ClipEntry> next;
        for (int i : keep) next.push_back(clips_[i]);
        ApplyList(std::move(next));
    }
    std::wstring OpenPathForTest() const { return selected_ >= 0 ? clips_[selected_].path : std::wstring(); }
    void SetTrimForTest(double in, double out) { trim_in_ = in; trim_out_ = out; }
    double ClipDurationForTest(int index) const { return clips_[index].duration_s; }
    const std::wstring& StatusForTest() const { return status_; }
    // A frame-exact save shown as running at done/total on the open clip (no child process).
    void SetExportForTest(uint32_t done, uint32_t total);
    // `--drive` record mode: every listed clip has its details and thumbnail, and an open clip has
    // loaded, so a filmed frame never shows a half-loaded grid.
    bool SettledForDriver() const {
        for (const ClipEntry& c : clips_)
            if (!c.info_loaded) return false;
        return !player_ || player_->GetState() != ClipPlayer::State::Loading;
    }

private:
    struct Hit;  // what a point is over
    Hit HitAt(D2D1_POINT_2F pt) const;

    void LoadList();
    void ApplyList(std::vector<ClipEntry> next);
    void LoadDetails(uint64_t generation, std::vector<std::wstring> paths);
    std::vector<int> Visible() const;  // clips_ indices under the current filter
    void OpenClip(int index);
    void CloseClip();
    void SetStatus(const std::wstring& text);

    // Player actions
    void RunAction(ClipAction a);
    void StepFrame(int dir);
    int64_t CurrentFrame() const;  // the frame on screen (or the one a step is heading to)
    void SetStartHere();
    void SetEndHere();
    void ReplaceOriginal();
    bool ReplaceWithTrim(const std::wstring& src, const std::wstring& trim);
    void TogglePlay();
    void SeekTo(double seconds);
    double SnapToKeyframe(double seconds) const;
    // The kept range starts between keyframes: saving re-encodes it (`ombra.exe export`).
    bool NeedsReencode() const;
    // A frame-exact save, run by `ombra.exe export` (clip_reencode.h) so it outlives the overlay.
    struct SaveDone {
        ombra::TrimResult r;
        std::wstring      src, dst;
        double            a = 0.0, out_saved = 0.0;
        std::vector<bool> tracks;
        bool              then_copy = false, replace = false;
        uint64_t          gen = 0;
        int               replaced = -1;  // the export child's replace: 1 done, 0 kept both, -1 the UI's to do
    };
    void StartExport(const SaveDone& d, double end_s);
    void CancelExport();
    void SaveFinished(const SaveDone& d);
    bool Trimmed() const;
    bool TracksOff() const;  // some audio track switched off: left out of saves and copies
    std::wstring OffTrackNames() const;
    void UpdateActionLabels();
    // Replace: the trim takes the original's name and times, and the original goes to the Recycle Bin.
    enum class SaveMode { Copy, CopyThenClipboard, Replace };
    void SaveTrimmed(SaveMode mode);
    void CopyToClipboard(const std::wstring& path);
    void ShowInFolder();
    void DeleteClip();

    double TimelineTime(float x) const;
    float  TimelineX(double seconds) const;

    ScreenHost* host_ = nullptr;
    std::wstring root_;
    uint64_t     generation_ = 0;  // bumped per LoadList; stale worker results are dropped
    bool         loading_ = false;
    std::vector<ClipEntry> clips_;
    std::vector<std::wstring> games_;  // filter chips, in first-seen (newest clip) order
    int          filter_ = -1;         // index into games_, -1 = all
    enum class Sort { Newest, Oldest, Largest, Longest, Game } sort_ = Sort::Newest;
    float        scroll_y_ = 0.0f;
    float        content_h_ = 0.0f;
    int          hover_tile_ = -1;
    ItemFade     tile_fade_;

    // Player view
    int          selected_ = -1;       // index into clips_, -1 = grid
    std::unique_ptr<ClipPlayer> player_;
    bool         player_failed_init_ = false;
    bool         needs_av1_extension_ = false;
    double       trim_in_ = 0.0, trim_out_ = 0.0;
    double       fps_ = 0.0;           // the open clip's, 0 unknown
    int64_t      step_frame_ = -1;     // the frame the last step went to, -1 after any other seek or play
    enum class Drag { None, Scrub, In, Out } drag_ = Drag::None;
    // The open clip's audio tracks (file order) and which are on. Off = not heard, and left out
    // of Save and Copy.
    std::vector<std::wstring> track_names_;
    std::vector<bool>         track_on_;
    std::vector<std::unique_ptr<ToggleChipControl>> track_chips_;  // one per track that fits
    VolumeControl             volume_;  // mute and level
    bool         resume_after_scrub_ = false;
    bool         busy_ = false;        // a trim is being written
    // The running frame-exact save, if any. The child keeps going when the overlay closes; a
    // reopened overlay sees its source as busy (ombra::ClipBusy) and leaves it alone.
    struct Export {
        bool         active = false;
        bool         finishing = false;  // muxing or replacing: too late to cancel
        bool         replace = false;
        uint32_t     done = 0, total = 0;
        std::wstring src, dst;
        HANDLE       cancel = nullptr;   // inherited by the child; set = stop
        uint64_t     id = 0;             // a finished export's replies are dropped if this moved on
    } export_;
    uint64_t     export_ids_ = 0;
    // Trims in flight, shared with their workers (which may outlive a timed-out wait).
    std::shared_ptr<std::atomic<int>> writes_ = std::make_shared<std::atomic<int>>(0);
    struct { std::wstring src; double in = 0, out = 0; std::vector<bool> tracks; std::wstring dst; } last_trim_;  // Copy reuses it
    std::wstring status_;
    double       status_until_ = 0;  // motion::NowMs(), so --drive's record mode films it on its clock
    ComPtr<ID2D1Bitmap1> frame_bitmap_;
    uint64_t     frame_bitmap_generation_ = 0;

    // Geometry from Layout (device pixels).
    float        s_ = 1.0f;
    D2D1_RECT_F  card_rect_{};
    D2D1_RECT_F  bounds_{};  // inside the card's padding
    D2D1_RECT_F  grid_rect_{};
    D2D1_RECT_F  video_rect_{};
    D2D1_RECT_F  timeline_rect_{};
    ChipGroupControl filter_chips_;  // [0] = All, then games_
    std::vector<D2D1_RECT_F> tile_rects_;  // per Visible() entry, unscrolled

    ButtonControl     open_folder_btn_;
    IconButtonControl close_btn_;
    IconButtonControl play_btn_;
    SelectControl     sort_select_;
    SelectControl     speed_select_;
    SplitButtonControl save_split_;  // Save trimmed; its arrow also offers Replace original
    ButtonControl     copy_btn_;
    ButtonControl     show_btn_;
    ButtonControl     delete_btn_;
    ButtonControl     store_btn_;  // "Get the AV1 Video Extension"
    ButtonControl     cancel_export_btn_;

    // Shortcuts card beside the player: each row shows its keys and runs the action on click.
    struct KeyRow { std::vector<std::wstring> keys; std::wstring label; ClipAction action; bool group_start; std::wstring group; };
    static const std::vector<KeyRow>& KeyRows();
    D2D1_RECT_F  keys_card_{};
    std::vector<D2D1_RECT_F> key_row_rects_;
    int          key_hover_ = -1;
    float KeysCardFlow(ID2D1DeviceContext* dc, const Theme& t, float s, bool draw);
    std::vector<Control*> Buttons();
};

}  // namespace ombra_ui
