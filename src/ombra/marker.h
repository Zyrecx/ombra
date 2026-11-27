// Markers: named moments on the engine's timeline, not in any one file. A file covering a marker's
// moment gets it, as an MP4 chapter plus an entry in the file's "markers" tag (mux.h), when the
// marker is Ombra's or the user's, or the file is the user's or the marking app's own.
#pragma once

#include "ombra/mux.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ombra {

// What a marker marks (fixed: each type has a themed icon). Apps pick from kGeneric..kLastAppType;
// the rest only Ombra places.
enum class MarkerType : uint8_t {
    kGeneric, kKill, kDeath, kAssist, kWin, kLoss, kObjective, kHighlight, kRoundStart, kRoundEnd,
    kClipSaved, kGameStart, kGameEnd,
};
inline constexpr MarkerType kLastAppType = MarkerType::kRoundEnd;
// The wire name ("round_start"), and back for the types an app may pick; nullopt for any other name.
const char* MarkerTypeName(MarkerType type);
std::optional<MarkerType> ParseMarkerType(std::string_view name);
inline bool IsOmbraMarkerType(MarkerType type) { return type > kLastAppType; }
inline constexpr const char* kOmbraMarkerOwner = "Ombra";
// The timeline keys (MarkerTimeline::Add) whose markers every file gets: Ombra's own, and the user's
// (the bookmark hotkey, not built). Any other key is an app's, kept to its own files and the user's.
// Both start with ':', which no app's key does (the engine names those, never from what an app
// calls itself), so no app passes as either.
inline constexpr const char* kOmbraMarkerKey = ":ombra";
inline constexpr const char* kUserMarkerKey  = ":user";

inline constexpr size_t kMarkerLabelMaxChars = 64;  // code points, not bytes
inline constexpr size_t kMarkersPerApp       = 256;  // within the footage still held
inline constexpr size_t kMarkersTotal        = 16 * kMarkersPerApp;  // 16 apps at once
// How far past a file's last frame a marker still lands on it. The pipe's `marker` takes "now" as
// covered by a live ring up to the same distance past its newest frame.
inline constexpr double kMarkerTrailSeconds = 2.0;

// A label as stored: control characters dropped, spaces trimmed. Nullopt when it isn't valid
// UTF-8 or runs past kMarkerLabelMaxChars.
std::optional<std::string> CleanMarkerLabel(std::string_view label);

struct Marker {
    uint64_t    id = 0;
    int64_t     qpc = 0;
    MarkerType  type = MarkerType::kGeneric;
    std::string label;  // may be empty: the chapter is named by the type
    std::string owner;  // the app's name, as its toast shows it
    bool        important = false;
};

// The chapter a marker becomes: "Pentakill · LeagueTool", or "Kill · LeagueTool" without a label.
// Ombra's own read type then detail, without the owner: "Game start · VALORANT", "Clip saved".
std::string MarkerChapterTitle(const Marker& marker);

// Where a file's frames sit on the qpc timeline, so a marker lands on the frame at or before it.
// Fed every sample in order. Keeps an anchor only where the frames stop following the frame rate
// (a pause, a jump), so it costs a few entries, not one per frame.
class FileClock {
public:
    FileClock(int64_t qpc_freq, uint32_t fps) : qpc_freq_(qpc_freq), fps_(fps ? fps : 1) {}
    void Add(int64_t qpc, uint64_t tick);
    // Milliseconds into the file of the frame at or before `qpc` (within 1 ms); nullopt before the
    // first frame or more than kMarkerTrailSeconds past the last. Up to that, past the last frame
    // is the last frame: a marker placed "now" runs ahead of the newest frame by the encode, while
    // a file whose footage stopped long before (a paused or disarmed ring) mustn't collect it.
    std::optional<uint64_t> MsAt(int64_t qpc) const;
    // MsAt, but up to kMarkerTrailSeconds before the first frame is the first frame: a game start
    // that armed the buffer comes a moment before the footage it started.
    std::optional<uint64_t> MsAtOrFirst(int64_t qpc) const;
    bool Empty() const { return anchors_.empty(); }

private:
    struct Anchor { int64_t qpc; uint64_t tick; };
    int64_t TickQpc(const Anchor& a, uint64_t tick) const;

    int64_t             qpc_freq_;
    uint32_t            fps_;
    std::vector<Anchor> anchors_;
    int64_t             last_qpc_  = 0;
    uint64_t            last_tick_ = 0;
};

// The markers a file gets: chapters, and the tag's JSON
// ({"v":1,"markers":[{"ms","kind","label","app","important"}]}). `markers`: those from the file's
// first frame to its end (MarkerTimeline::Between); any before the first frame are left out.
MuxMarkers MarkersForFile(const std::vector<Marker>& markers, const FileClock& clock);

// The markers a cut of a file keeps (a trim, a re-encode): from `json` (the source's tag), those on
// a frame in [start_s, end_s) of the source, `ms` moved to the cut's first frame, each item's other
// fields as they were, and their chapters rebuilt. Half a frame of slack at both ends: a marker's
// `ms` is its frame's time rounded down. A tag this can't read (another version, malformed) gives
// none.
MuxMarkers CutFileMarkers(const std::string& json, double start_s, double end_s, uint32_t fps);

// The type's name as people read it ("Round start").
const char* MarkerTypeTitle(MarkerType type);

// A marker as a saved file's tag holds it, for showing (the Clips timeline).
struct FileMarker {
    double      at_s = 0.0;  // from the file's first video sample (add ClipInfo::video_offset_s for presentation time)
    MarkerType  type = MarkerType::kGeneric;  // a kind this build doesn't know reads as generic
    std::string label;
    std::string app;
    bool        important = false;
};
// The markers in `json` (ClipInfo::markers_json), oldest first; none for a tag this can't read,
// as CutFileMarkers.
std::vector<FileMarker> ReadFileMarkers(const std::string& json);

// Every marker on the engine's timeline. Keeps a marker while it's within the buffer's reach or a
// file being written (a Hold) still covers it. Thread-safe.
class MarkerTimeline {
public:
    // `lead_qpc`: how far before a file's first frame a marker can still land on it (MsAtOrFirst),
    // and so how much longer than the buffer's reach markers are kept. The engine's is
    // kMarkerTrailSeconds.
    explicit MarkerTimeline(int64_t lead_qpc = 0) : lead_qpc_(lead_qpc) {}

    // Keeps markers from `from_qpc` on while alive: a save or open clip, from its first frame,
    // since a writer releases the ring behind itself before it collects its markers.
    class Hold {
    public:
        Hold() = default;
        Hold(Hold&& other) noexcept { *this = std::move(other); }
        Hold& operator=(Hold&& other) noexcept;
        ~Hold() { Reset(); }
        void Reset();

    private:
        friend class MarkerTimeline;
        MarkerTimeline* timeline_ = nullptr;
        uint64_t        id_ = 0;
    };

    enum class AddResult { kAdded, kAppFull /* its kMarkersPerApp */, kFull /* kMarkersTotal */ };
    // `app_key` counts against kMarkersPerApp (every command-line script is one app, whatever it
    // calls itself). `oldest_qpc`: the buffer's reach now; anything older and unheld is dropped first.
    AddResult Add(Marker& marker, const std::string& app_key, int64_t oldest_qpc);
    Hold HoldFrom(int64_t from_qpc);
    // Takes back a marker that turned out wrong (a game that came back); false if it's gone.
    bool Remove(uint64_t id);
    // Markers with `from_qpc` <= qpc <= `to_qpc`, oldest first.
    std::vector<Marker> Between(int64_t from_qpc, int64_t to_qpc) const;
    // What a file from `first_qpc` (its first frame) to `end_qpc` may get: Between, reaching back
    // the lead, kept to the markers its owner may see. `file_app`: the key (as Add's) of the app
    // whose file it is, which gets its own and Ombra's and the user's; empty: the user's file, which
    // gets every app's too. MarkersForFile decides which land.
    std::vector<Marker> ForFile(int64_t first_qpc, int64_t end_qpc, std::string_view file_app = {}) const;
    int64_t LeadQpc() const { return lead_qpc_; }
    size_t Size() const;

private:
    struct Entry { Marker marker; std::string app_key; };
    struct HoldRec { uint64_t id; int64_t from_qpc; };
    void Release(uint64_t hold_id);

    const int64_t        lead_qpc_;
    mutable std::mutex   mutex_;
    std::vector<Entry>   entries_;  // ascending qpc
    std::vector<HoldRec> holds_;
    uint64_t             next_id_ = 1;
    uint64_t             next_hold_ = 1;
};

// A game session shorter than this leaves no markers: a launcher or client window flashing past
// (League's client shows for 10-16 s at a time while it starts).
inline constexpr double kGameMarkerMinSeconds  = 30.0;
// The same game back within this of its end continues the session: its end marker is taken back
// and no new start placed (a client handing over to the match, a window the game recreates).
inline constexpr double kGameMarkerJoinSeconds = 120.0;

// Turns the buffer controller's game changes into Ombra's game start and end markers, labelled
// with the game's name. Not thread-safe: one caller at a time (the controller's sink).
class GameMarkers {
public:
    GameMarkers(MarkerTimeline& timeline, int64_t qpc_freq) : timeline_(timeline), qpc_freq_(qpc_freq) {}
    // The game is now `name` (empty: none) since `qpc`. `oldest_qpc` as MarkerTimeline::Add's.
    void Update(const std::string& name, int64_t qpc, int64_t oldest_qpc);

private:
    void End(int64_t qpc, int64_t oldest_qpc);
    uint64_t Place(MarkerType type, int64_t qpc, int64_t oldest_qpc);
    int64_t Seconds(double s) const { return static_cast<int64_t>(s * static_cast<double>(qpc_freq_)); }

    MarkerTimeline& timeline_;
    int64_t         qpc_freq_;
    std::string     game_;            // the session's game; empty: none since the last forgotten one
    bool            ended_    = false;  // game_ ended at end_qpc_ and may yet come back
    int64_t         start_qpc_ = 0;
    int64_t         end_qpc_   = 0;
    uint64_t        start_id_  = 0;   // 0: not placed (the timeline was full)
    uint64_t        end_id_    = 0;
};

}  // namespace ombra
