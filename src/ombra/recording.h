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

#include "ombra/reconfig.h"
#include "ombra/save.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
    uint64_t     id = 0;              // its Recordings id
};

// Why a recording ended.
enum class RecordEnd {
    kStopped,      // Stop(): asked for, its connection closed, or the engine is exiting
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

    // Ends the recording with the footage taken so far; Run still finishes the file.
    void Stop() { stop_.store(true, std::memory_order_release); }
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
    double          written_seconds_ = 0;  // finished parts

    std::atomic<bool>   stop_{false};
    std::atomic<double> seconds_{0};
    bool listed_ = false;  // in ctx_->open_recordings
};

}  // namespace ombra
