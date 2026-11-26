// A recording: footage from its start (up to buffer.seconds back) until it's stopped, written as it
// arrives into a hybrid MP4 (Mp4FragmentedWriter), so a crash keeps everything up to the last
// fragment. Like an open clip it tails the ring and releases behind itself, so it costs the disk
// writes and about a fragment of memory, never a second encode or a growing heap: its audio is
// streamed into the file too (AudioTrackHub::SettleClip).
//
// A cut in the stream (a barrier: resolution change, rebuild, re-arming after every display was
// off) can't continue one MP4 track, so the recording goes on in a new file, a "part". At the first
// cut a folder named after the recording takes part 1 (moved in) and every part after it; a
// multi-monitor recording's parts stay in its group folder. Footage that stops arriving (paused,
// disarmed) only waits: a recording ends when it's stopped, the disk runs low or a write fails.
#pragma once

#include "ombra/open_recording.h"
#include "ombra/reconfig.h"
#include "ombra/save.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ombra {

struct AuDescriptor;

// A recording that finds no footage for this long after it was asked for gives up (kNoFootage):
// capture never armed (no display on, the window gone).
inline constexpr double kRecordStartSeconds = 10.0;
// How often a recording asks the disk for room, and how much it wants: it stops while the disk can
// still take this many seconds more at the rate it records, the file's index included.
inline constexpr double kRecordDiskCheckSeconds = 5.0;
inline constexpr double kRecordDiskReserveSeconds = 60.0;

// The ring's age while the switch is off and only a recording keeps capture armed: enough for the
// recording's tail (RingMaxAgeQpc adds its margin), because that footage is no replay's to keep.
inline constexpr double kRecordOnlyRingSeconds = 3.0;

// What ApplyReplayReach remembers between calls.
struct ReplayReachState {
    bool off_footage = false;  // capture ran with the switch off: switching on floors the saves there
};
// Applies the Instant Replay switch and whether a recording runs to every pipeline (the controller's
// reach sink), as they change at `now`: SaveContext's replay_floor_qpc and replay_off_qpc, the video
// rings' age (kRecordOnlyRingSeconds while only a recording keeps capture), and switching off ends
// the open clips, which would otherwise go on taking the recording's footage.
void ApplyReplayReach(const std::vector<SaveContext*>& contexts, ReplayReachState& state, bool replay_on,
                      bool recording, int64_t now);

struct RecordRequest {
    int64_t      start_qpc      = 0;  // when it was asked for
    double       before_seconds = 0;  // capped at buffer.seconds: the ring holds no more
    int64_t      floor_qpc      = 0;  // nothing before it, whatever `before` says (0: no floor)
    std::wstring title;               // optional; sanitized into the file (or group folder) name
    std::string  app;                 // who asked, as its toast names it
    std::string  marker_app;          // its app's marker key (MarkerTimeline::ForFile); empty: the user's
    uint64_t     id = 0;              // its Recordings id
};

// Why a recording ended.
enum class RecordEnd {
    kStopped,      // StopAt(): asked for, its connection closed, or the engine is exiting
    kDiskLow,      // the disk can't take kRecordDiskReserveSeconds more
    kWriteFailed,  // a write failed: the file keeps what was written
    kNoFootage,    // nothing to record within kRecordStartSeconds
};
const char* RecordEndName(RecordEnd end);

// One monitor's recording. Open on any thread; Run on the recording's own; Stop and Seconds from any.
class RecordingSession {
public:
    // Picks the file and checks the disk; footage is taken in Run, which waits for it. Null on
    // refusal, with `why`: kBusy (a buffer reallocation holds the ring), kFailed (no room).
    static std::shared_ptr<RecordingSession> Open(SaveContext* ctx, const RecordRequest& request,
                                                  const std::wstring* group_folder_name, SaveOutcome* why);
    ~RecordingSession();
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    // Ends the recording at `at_qpc` (the moment the stop was asked for): Run still writes the footage
    // from before it, the `before` it hasn't caught up on included and up to half a second of what
    // the encoder hasn't delivered yet, then finishes the file.
    // A later call never moves the end later.
    void StopAt(int64_t at_qpc);
    // Seconds written so far, every part together.
    double Seconds() const { return seconds_.load(std::memory_order_relaxed); }

    struct Result {
        SaveOutcome               outcome = SaveOutcome::kFailed;  // kSaved: at least one part written
        std::vector<std::wstring> paths;  // every part, in order
        bool                      fell_back = false;
        double                    seconds   = 0;
        RecordEnd                 end       = RecordEnd::kStopped;
    };
    // Records on the calling thread until it ends. `now` reads the clock; `wait` is called when
    // everything there is was written (live: a short sleep; tests: feed the ring and move a
    // synthetic clock). `started` is called once: true when the first file opens, false when the
    // recording ends before that. Never dies.
    Result Run(const std::function<int64_t()>& now, const std::function<void()>& wait,
               const std::function<void(bool)>& started = {});
    // Leaves ctx->open_recordings, after which the engine may tear the context down: called once
    // nothing reads it any more (also by the destructor). StopClipsAndWait waits for it.
    void Close();

private:
    RecordingSession() = default;
    struct Part;
    // Opens `part`'s file at its first frame, a keyframe. False: the file couldn't be made.
    bool OpenPartFile(Part& part, const AuDescriptor& first);
    // Part 2's name onward; at the first, moves part 1 into the parts' folder.
    std::wstring NextPartPath();
    // Rewrites the crash record's markers once a fragment more is on the disk.
    void KeepRecordMarkers(Part& part);
    // Moves the audio drained so far into the file.
    void TakeAudio(Part& part);
    // Writes the rest of `part`'s audio and its markers and makes the file plain. False: a write failed.
    bool FinishPart(Part& part);
    // The disk can't take kRecordDiskReserveSeconds more of an open part.
    bool DiskLow(const Part& part) const;

    SaveContext*    ctx_ = nullptr;
    int64_t         qpc_freq_ = 1;
    int64_t         start_qpc_ = 0;
    int64_t         first_floor_qpc_ = 0;
    bool            floored_ = false;  // first_floor_qpc_ is RecordRequest::floor_qpc
    bool            grouped_ = false;  // in a multi-monitor group folder
    ClipDestination dest_;  // part 1's file
    // After the first cut: where the parts go and the name they share ("<stem> part N.mp4").
    std::wstring    parts_dir_;
    std::wstring    parts_stem_;
    std::vector<std::shared_ptr<void>> reservations_;  // the later parts' names
    std::vector<std::wstring>          paths_;
    std::string     app_;  // RecordRequest's, for the crash record
    std::string     title_;
    std::string     marker_app_;  // RecordRequest's
    std::vector<OpenRecording> kept_records_;  // of parts whose Finish failed: the next start's
    double          written_seconds_ = 0;  // finished parts

    std::atomic<int64_t> stop_qpc_{(std::numeric_limits<int64_t>::max)()};  // max: not stopped
    std::atomic<double> seconds_{0};
    bool listed_ = false;  // in ctx_->open_recordings
};

// Every monitor's end of one recording, in input order. A monitor that refused at Open has that
// outcome (kBusy, kFailed) and no paths.
struct RecordingResults {
    // kSaved: every monitor that opened wrote its files; kNothingNew: none had footage; kFailed:
    // a monitor refused or a write failed (what was written is kept).
    SaveOutcome outcome = SaveOutcome::kNothingNew;
    RecordEnd   end     = RecordEnd::kStopped;  // the first monitor's that wasn't stopped
    std::vector<RecordingSession::Result> monitors;
    double Seconds() const;  // the longest monitor's
};

// The toast for a recording's end, as ombra-ui's LookupToastInfo knows it: record_saved,
// record_nothing (no footage), record_disk_low or record_write_failed (stopped early, files kept),
// record_failed (a monitor wrote nothing it should have).
const wchar_t* RecordEndToast(const RecordingResults& result);

// What the engine tells the user about a recording. Both run on a recording thread.
struct RecordFeedback {
    std::function<void()>                        started;  // once, when the first file opens
    std::function<void(const RecordingResults&)> ended;    // once, when every file is finished
};

// One recording on every monitor, a thread each, like StartClip.
class RecordingGroup {
public:
    void   Stop();  // every monitor's StopAt now
    double Seconds() const;  // the longest monitor's so far
    // True once a monitor wrote its first frame; false when every one ended before that.
    std::shared_future<bool>             Started() const { return started_; }
    std::shared_future<RecordingResults> Done() const { return done_; }
    uint64_t Id() const { return id_; }  // RecordRequest::id

private:
    friend std::shared_ptr<RecordingGroup> StartRecording(const std::vector<SaveContext*>&, const RecordRequest&,
                                                          std::shared_ptr<void>, RecordFeedback);
    uint64_t                                       id_ = 0;
    std::vector<std::shared_ptr<RecordingSession>> sessions_;
    std::shared_future<bool>                       started_;
    std::shared_future<RecordingResults>           done_;
};
// `hold` (BufferController::HoldRecording, taken before this so capture arms in time) keeps capture
// armed until the last monitor's Run returns; that thread drops it before it reports and before its
// session leaves open_recordings, so once StopClipsAndWait returned for every context no hold is left.
// Raises recording_started and recording_stopped (events.h) when request.id is set.
std::shared_ptr<RecordingGroup> StartRecording(const std::vector<SaveContext*>& contexts, const RecordRequest& request,
                                               std::shared_ptr<void> hold, RecordFeedback feedback = {});

// The recordings apps asked for: one per app at a time (a second start is refused while its own
// runs), each stopped only by the app that started it. Thread-safe: one per engine, every pipe client.
class Recordings {
public:
    enum class Refusal { kNone, kAlreadyRecording, kClosed };
    struct Started {
        Refusal                         refusal = Refusal::kNone;
        uint64_t                        id      = 0;  // the new recording's, or the one already running
        std::shared_ptr<RecordingGroup> group;
    };
    // `arm` runs once the request is accepted, before any monitor opens: it takes the controller's
    // hold and sets request.floor_qpc from the Instant Replay switch as it stands with that hold.
    // When no monitor opens, `feedback.ended` runs inside this call, under the registry's lock: it
    // must not call back into Recordings.
    Started Start(const std::string& app, const std::vector<SaveContext*>& contexts, RecordRequest request,
                  const std::function<std::shared_ptr<void>(RecordRequest&)>& arm, RecordFeedback feedback = {});
    // Stops `app`'s recording `id` (0: its latest, running or ended) and forgets it: the caller waits
    // on Done. Null when the app has none by that id.
    std::shared_ptr<RecordingGroup> Stop(const std::string& app, uint64_t id);

    struct Running {
        uint64_t    id = 0;
        std::string app;
        double      seconds = 0;
    };
    // The recordings running now: `app`'s, or with no app every one.
    std::vector<Running> List(const std::string& app = {});
    // Engine exit, before StopClipsAndWait: refuses every start and stops every recording.
    void Close();

private:
    struct Entry {
        std::string                     app;
        uint64_t                        id = 0;
        std::shared_ptr<RecordingGroup> group;
    };
    std::mutex         mutex_;  // held across StartRecording: an app can't start two at once
    std::vector<Entry> entries_;  // the latest per app, kept after it ends until stopped or replaced
    uint64_t           next_id_ = 1;
    bool               closed_  = false;
};

}  // namespace ombra
