// Markers: named moments on the engine's timeline, not in any one file. Every clip covering a
// marker's moment gets it, as an MP4 chapter plus an entry in the file's "markers" tag (mux.h).
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

// What an app may mark (fixed: each type has a themed icon). Ombra's own types come later.
enum class MarkerType : uint8_t {
    kGeneric, kKill, kDeath, kAssist, kWin, kLoss, kObjective, kHighlight, kRoundStart, kRoundEnd,
};
// The wire name ("round_start"), and back; nullopt for an unknown name.
const char* MarkerTypeName(MarkerType type);
std::optional<MarkerType> ParseMarkerType(std::string_view name);

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

// Every marker on the engine's timeline. Keeps a marker while it's within the buffer's reach or a
// file being written (a Hold) still covers it. Thread-safe.
class MarkerTimeline {
public:
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
    // Markers with `from_qpc` <= qpc <= `to_qpc`, oldest first.
    std::vector<Marker> Between(int64_t from_qpc, int64_t to_qpc) const;
    size_t Size() const;

private:
    struct Entry { Marker marker; std::string app_key; };
    struct HoldRec { uint64_t id; int64_t from_qpc; };
    void Release(uint64_t hold_id);

    mutable std::mutex   mutex_;
    std::vector<Entry>   entries_;  // ascending qpc
    std::vector<HoldRec> holds_;
    uint64_t             next_id_ = 1;
    uint64_t             next_hold_ = 1;
};

}  // namespace ombra
