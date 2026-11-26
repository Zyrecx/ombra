// A tool's clip: the footage from `before` seconds ahead of an event to `after` seconds past it. It pins at event - before and tails live footage into a
// streaming mux, releasing the ring behind itself, so `after` is bounded by policy (the Extend cap),
// not by the buffer, and a writer that falls behind fails its own file, never grows memory.
//
// Apart from the user's hotkey save: a clip never takes save_in_flight and never drops the truncate
// cut, so neither one costs the other footage or a busy refusal.
#pragma once

#include "ombra/audio_tracks.h"
#include "ombra/reconfig.h"
#include "ombra/ring.h"
#include "ombra/save.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ombra {

// How far past its first event a clip may run, however often it's extended.
inline constexpr double kClipMaxAfterSeconds = 120.0;
// No new footage for this long ends an open clip with what it took: capture paused, Instant Replay
// switched off, the game closed. Live, the pacer appends a tick every frame while armed.
inline constexpr double kClipStallSeconds = 3.0;
// How often an open clip takes the audio encoded so far (AudioTrackHub::DrainClip), well within the
// audio rings' reach.
inline constexpr double kClipDrainSeconds = 1.0;

struct ClipRequest {
    int64_t      event_qpc      = 0;
    double       before_seconds = 0;  // capped at buffer.seconds: the ring holds no more
    double       after_seconds  = 0;  // capped at kClipMaxAfterSeconds
    std::wstring title;               // optional; sanitized into the file (or group folder) name
    std::string  app;                 // who asked, as its toast names it: the "clip saved" marker's label
    uint64_t     id = 0;              // AppClips' id for it, told in the clip_saved event; 0: none
};

// Why a clip stopped taking footage.
enum class ClipEnd {
    kReached,       // footage reached the clip's end
    kSegmentEnded,  // a resolution change or rebuild cut the stream: the file ends there
    kClosing,       // the buffer is being rebuilt
    kStalled,       // no new footage for kClipStallSeconds
    kStopped,       // Stop(): the engine is exiting
    kBroken,        // the footage was overwritten before it was written (disk too slow)
};
const char* ClipEndName(ClipEnd end);

// One monitor's open clip. Open on any thread; Run on the clip's own; Extend and Stop from any.
class ClipSession {
public:
    // Pins from the keyframe at or before event - before (TruncateFloor::kIgnore), picks the file
    // and checks the disk for the clip's estimated size. Null on refusal, with `why`: kNothingNew
    // (no footage in reach), kBusy (a buffer reallocation holds the ring), kFailed (no room).
    static std::shared_ptr<ClipSession> Open(SaveContext* ctx, const ClipRequest& request,
                                             const std::wstring* group_folder_name, SaveOutcome* why);
    ~ClipSession();
    ClipSession(const ClipSession&) = delete;
    ClipSession& operator=(const ClipSession&) = delete;

    // Moves the end to `until_qpc` when later. False, and the end unchanged, once the clip stopped
    // taking footage, past the first event + kClipMaxAfterSeconds, or when the disk can't take the
    // longer clip: the caller opens a new clip instead.
    bool Extend(int64_t until_qpc);
    // Ends the clip with the footage taken so far; Run still writes the file.
    void Stop();

    struct Result {
        SaveOutcome  outcome = SaveOutcome::kFailed;
        std::wstring path;
        bool         fell_back = false;
        double       seconds   = 0;
        ClipEnd      end       = ClipEnd::kStopped;
    };
    // Writes the file on the calling thread. `now` reads the clock; `wait` is called when the clip
    // has written everything there is and waits for more (live: a short sleep; tests: feed the ring
    // and move a synthetic clock). Never dies.
    Result Run(const std::function<int64_t()>& now, const std::function<void()>& wait);
    // Leaves ctx->open_clips, after which the engine may tear the context down: called once
    // nothing reads it any more (also by the destructor). StopClipsAndWait waits for it.
    void Close();

private:
    ClipSession() = default;
    enum class Pull { kSample, kWait, kEnd, kFail };
    Pull Step(int64_t now, MuxVideoSample& sample, std::vector<uint8_t>& bytes);
    Pull EndLocked(ClipEnd why);
    // Extend for several monitors' clips at once: all of them or none.
    static bool ExtendAll(ClipSession* const* sessions, size_t count, int64_t until_qpc);
    friend class ClipGroup;

    SaveContext*                         ctx_ = nullptr;
    RingPin                              pin_;
    std::shared_ptr<const SegmentParams> segment_;
    uint32_t                             fps_      = 60;
    int64_t                              qpc_freq_ = 1;
    int64_t                              cap_qpc_  = 0;
    double                               bytes_per_second_ = 0;
    ClipDestination                      dest_;
    std::optional<AudioTrackHub::ClipAudio> audio_;
    std::vector<MuxAudioTrack>           audio_tracks_;
    // Markers from the first frame on: the timeline keeps them though the ring lets that footage go.
    MarkerTimeline::Hold                 marker_hold_;
    std::optional<FileClock>             clock_;  // Run's thread, once open
    int64_t                              first_qpc_ = 0;

    std::mutex mutex_;  // until_, ended_, stop_, end_, end_qpc_; held across Follow and the end decision
    int64_t    until_   = 0;
    bool       ended_   = false;
    bool       stop_    = false;
    ClipEnd    end_     = ClipEnd::kStopped;
    int64_t    end_qpc_ = 0;
    std::atomic<int64_t> written_qpc_{0};  // the newest AU written, for Extend's disk estimate

    // Run's thread only.
    std::vector<AuDescriptor> queue_;  // handed out by the pin, not yet written
    size_t                    next_ = 0;
    std::optional<AuDescriptor> written_;  // the newest AU written
    int64_t                   newest_qpc_ = 0;  // the newest AU the tail has seen
    uint64_t                  first_index_ = 0;
    int64_t                   last_drain_  = 0;
    bool                      broken_ = false;
    bool                      listed_ = false;  // in ctx_->open_clips
};

// One request on every monitor, a thread each, like SaveReplay. `feedback` runs once, on the last
// clip thread, with every monitor's result.
class ClipGroup {
public:
    // Extends every monitor's clip, or none (ClipSession::Extend's false).
    bool Extend(int64_t until_qpc);
    std::shared_future<ReplaySaveResults> Done() const { return done_; }

private:
    friend std::shared_ptr<ClipGroup> StartClip(const std::vector<SaveContext*>&, const ClipRequest&,
                                                SaveFeedback);
    std::vector<std::shared_ptr<ClipSession>> sessions_;
    std::shared_future<ReplaySaveResults>     done_;
};
std::shared_ptr<ClipGroup> StartClip(const std::vector<SaveContext*>& contexts, const ClipRequest& request,
                                     SaveFeedback feedback = {});

// The clips apps asked for, one open clip per app: a request from an app whose own clip is still
// taking footage extends it (one file per combo) instead of opening a second; past the Extend cap,
// or once the clip ended, it opens a new one. The user's hotkey saves never come through here.
// Thread-safe: one per engine, shared by every pipe client.
class AppClips {
public:
    struct Started {
        uint64_t                              id       = 0;  // the clip's id for this engine run
        bool                                  extended = false;  // joined the app's open clip
        std::shared_future<ReplaySaveResults> done;
    };
    // nullopt once Close() ran. `feedback` is made only for a new clip, not for an extension.
    std::optional<Started> Request(const std::string& app, const std::vector<SaveContext*>& contexts,
                                   const ClipRequest& request, const StartSaveFeedback& feedback = {});
    // Engine exit, before StopClipsAndWait: refuses every request from now on.
    void Close();

private:
    struct Open {
        uint64_t                   id = 0;
        std::shared_ptr<ClipGroup> group;
    };
    std::mutex                       mutex_;  // held across StartClip: one app can't open two at once
    std::vector<std::pair<std::string, Open>> open_;  // by app; a finished clip is dropped lazily
    uint64_t                         next_id_ = 1;
    bool                             closed_  = false;
};

// Engine exit: ends every clip open on `ctx` with what it took and waits until their files are
// written.
void StopClipsAndWait(SaveContext* ctx);

// True while a clip is open on `ctx` (the update's idle gate).
bool ClipsOpen(SaveContext* ctx);

}  // namespace ombra
