// PipelineSupervisor.
//
// Owns capture + encoder + pacer for one monitor and is the only thing allowed to construct or
// destroy them. On a recoverable fault (DXGI device removed/reset, WGC item closed, NVENC session
// lost — see capture.h's IsHealthy()/HealthIssue() and encoder.h's Faulted()/FaultReason(), the
// two places that made the die()-on-a-worker-thread failure mode non-fatal), it tears the
// generation down, drops a Ring::Barrier() on both rings at one shared QPC instant (one
// timeline), backs off (1/2/4/8/16/30s, capped), and rebuilds a fresh generation. It never
// die()s itself — a background recorder that can't record is still better than one that took the
// whole process down.
//
// Save-path failures (unwritable path, disk full, mux error) are NOT this class's problem — they
// belong in a separate bucket (log, clear save_in_flight, leave the ring intact) and
// main.cpp's DoSave handles that directly, without going through here. Routing a failed save
// through this class would drop a barrier on every failed save, cutting the user's buffer
// continuity for a problem that has nothing to do with the capture/encode pipeline being
// unhealthy — exactly the wrong trade.
//
// The second reason a generation ends: the monitor changed resolution.
// That is not a fault — the pipeline is healthy, it is just producing frames at a size this
// generation's encoder cannot accept — so it gets ReopenForResolution(): Capture::Recreate() plus
// a fresh Encoder, with no backoff and without closing the capture device, so the ring keeps every
// access unit it already holds (losing the user's buffer on every alt-tab is the
// wrong trade). Both paths converge on StartSegment(), the single place a barrier is dropped —
// carrying the new segment's SPS/PPS and dimensions (Ring::SegmentParams), which is what makes a
// save that follows a resolution change mux the right avcC. CurrentParams() below is no longer how
// a save picks its parameters (Ring::PinnedSegmentParams() is); it stays for diagnostics and for
// callers with no ring of their own.
#pragma once

#include "ombra/capture_source.h"
#include "ombra/config.h"
#include "ombra/encoder.h"
#include "ombra/game_library.h"
#include "ombra/flight_recorder.h"
#include "ombra/pacer.h"
#include "ombra/ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

class PipelineSupervisor {
public:
    using AuCallback = Pacer::AuCallback;

    // `ring` is not owned — it must outlive the supervisor and every rebuild, since a barrier only
    // means anything if the data on both sides of it is still in the same ring. Audio needs no
    // barrier: a save's audio starts at its first video frame (AudioTrackHub::Collect).
    //
    // `target` (default: primary monitor) is passed to Capture::Open() fresh on
    // every generation build — see BuildGeneration() — which is what makes the "re-
    // resolved on every pipeline rebuild" property true without this class doing anything extra for it.
    PipelineSupervisor(uint32_t fps, uint32_t bitrate_kbps, uint32_t keyframe_sec, Ring* ring,
                        const EncoderOptions& encoder_opts = {},
                        bool capture_cursor = true, const CaptureTarget& target = {});
    ~PipelineSupervisor();

    // Opens the first generation (Capture::Open, Encoder::Open, RegisterSlot x kSlots,
    // Pacer::Start) and starts the watchdog thread. Fatal-at-startup failures inside
    // Capture::Open()/Encoder::Open() still die() — the other fatal-at-startup bucket, unchanged by
    // this class; a machine with no NVENC-capable adapter should still refuse to start loudly.
    // This validation happens unconditionally, even when `initial_armed` is false (advisor
    // review): a machine that can't record should say so at startup, not silently
    // wait for the first game to arm before discovering it. When `initial_armed` is false the
    // freshly-validated generation is torn back down immediately (nothing was ever appended, so
    // there is nothing to preserve) and the watchdog is left to build the real one on the first
    // SetArmed(true).
    // Exception: a strict target.mode == Window with no matching window found
    // makes Capture::Open() return false *before* any GPU/adapter call — BuildGeneration() never
    // reaches Encoder::Open() at all, so this fatal validation doesn't run yet, and Start() quietly
    // leaves the watchdog to retry on backoff until the target appears. That's intentional (a
    // missing app isn't a machine-can't-record fault), but it means "no adapter" and "target not
    // launched yet" both currently look like a quiet startup from the outside — worth remembering
    // given this codebase already got burned once by an under-specified "no automatic caller"
    // assumption (capturebench.cpp).
    void Start(AuCallback cb, bool initial_armed = true);

    // Installs a Pacer::PreEncodeCallback that survives every rebuild —
    // StartSegment() (the single place a Pacer is constructed) passes whatever is set here into
    // every generation's Pacer::Start(), not just the first one. Call before Start(); changing it
    // afterward only takes effect from the next rebuild/reopen, which is fine for its one caller
    // (the screenshot hotkey, serviced inline on the tick path the same way capturebench's own
    // pre-encode hook is — see pacer.h's comment on why that's acceptable for a rare, user-
    // triggered one-shot and not for anything continuous).
    void SetPreEncodeCallback(Pacer::PreEncodeCallback cb) { pre_encode_cb_ = std::move(cb); }

    // Where each generation's frames come from. Called on every BuildGeneration (startup, fault
    // rebuild, reconfigure, arm), so a synthetic source in a test is torn down and rebuilt exactly
    // like WGC. Unset: Capture. Call before Start().
    using CaptureFactory = std::function<std::unique_ptr<ICaptureSource>()>;
    void SetCaptureFactory(CaptureFactory factory) { capture_factory_ = std::move(factory); }

    // Which adapter encode.adapter means right now, leaving out `exclude` when set (the next choice
    // after its encoder wouldn't open): unset is ResolveEncodeAdapter. Tests use it to make a GPU
    // leave and come back, and shorten how often a healthy pipeline checks. Call before Start().
    using AdapterResolver = std::function<std::optional<EncodeAdapter>(const LUID* exclude, const LUID* game_gpu)>;

    // The adapter the recorded game renders on (nullopt: no game, or not known yet). Auto records
    // on an NVIDIA one (PickEncodeAdapter), switching as soon as it's set; once cleared it keeps it
    // for the ring's max age, so the game's footage isn't cut off from a save right after it closes.
    // Any thread; the watchdog acts on it.
    void SetGameGpu(std::optional<LUID> luid);
    void SetAdapterResolver(AdapterResolver resolver, std::chrono::milliseconds recheck) {
        adapter_resolver_ = std::move(resolver);
        adapter_recheck_  = recheck;
    }

    // The same for encoders: unset is CreateVideoEncoder. Tests use it to make a hardware encoder
    // fail to open. Call before Start().
    using EncoderFactory = std::function<std::unique_ptr<IVideoEncoder>(EncoderBackend)>;
    void SetEncoderFactory(EncoderFactory factory) { encoder_factory_ = std::move(factory); }

    // Called on the watchdog thread when a build falls back to the CPU encoder because the hardware
    // one wouldn't open (OpenEncodeStage). Set before Start(). The engine shows a toast.
    using EncoderFallbackCallback = std::function<void(const std::string& reason)>;
    void SetEncoderFallbackCallback(EncoderFallbackCallback cb) { encoder_fallback_cb_ = std::move(cb); }

    // Called on the watchdog thread when the ring's file became unusable and the buffer moved to RAM
    // (Ring::TakeStorageLost). Set before Start(). The engine shows a toast.
    using RingStorageLostCallback = std::function<void()>;
    void SetRingStorageLostCallback(RingStorageLostCallback cb) { ring_storage_lost_cb_ = std::move(cb); }

    // A hardware encoder that fails to open this many builds in a row is replaced by the CPU one for
    // the build that fails next; at startup, the first failure falls back. Every later full build
    // tries the hardware encoder again first (a fault, an arm, a settings change). Exposed for tests.
    static constexpr int kHwOpenFailuresBeforeCpu = 3;

    // The Games list "The game" target (capture.target "game") records from; shared by every
    // pipeline, owned by the caller. Read on every window resolve, so a list change needs no
    // rebuild. Unset: "The game" matches nothing and records the display. Call before Start().
    void SetGameLibrary(const GameLibrary* games) { games_ = games; }
    // capture.resolution ("native"|"1440p"|...): each build encodes at ResolveOutputSize of the
    // captured size, through a FrameScaler when that differs. Call before Start(); afterwards it
    // changes through RequestReconfigure.
    void SetCaptureResolution(const std::string& resolution) { capture_resolution_ = resolution; }
    // "The game" with When = Always records the display while no game is open; with Only in games
    // it records only a game window, never the desktop (the controller disarms when none is open).
    // Read on every build and window resolve, so flipping it needs no rebuild. Default: fall back.
    void SetGameFallbackToDisplay(bool fallback) { game_fallback_.store(fallback, std::memory_order_release); }
    // The window the current generation records (HWND value); 0 for a display, or with nothing
    // built. Any thread. BufferController names the game from it so the {game} name, game_only
    // audio and the picture all follow the same window.
    uint64_t RecordedWindowId() const;

    // Final shutdown: stops the watchdog thread first (so it can't start a rebuild mid-Stop),
    // then tears the current generation down in the safe order. Not the same ordering concern as
    // a fault-rebuild — see TeardownGeneration()'s comment in the .cpp.
    void Stop();

    // Everything the *current* pipeline generation is encoding with, snapshotted by value.
    // Mutex-protected and safe to call from any thread at any time, including mid-rebuild — this
    // replaces the pre-step-8 pattern of a raw pointer/reference into a live Encoder, which a
    // rebuild would dangle or silently repoint at the wrong segment's SPS/PPS (advisor review).
    // The save path moved off this: Ring::PinnedSegmentParams() gives a save the parameters
    // of the segment it actually pinned, which is the right answer when a rebuild or a resolution
    // change happened between the buffer filling and the hotkey. This remains the answer to "what
    // is the pipeline doing right now," which is a different question.
    // width/height are the encoded size (what the SPS says); source_width/height what is captured,
    // which is what the quality preset's bitrate derivation scales by capture.resolution.
    struct PipelineParams {
        std::vector<uint8_t> codec_header;
        VideoCodec            codec = VideoCodec::H264;
        uint32_t              width  = 0;
        uint32_t              height = 0;
        uint32_t              source_width  = 0;
        uint32_t              source_height = 0;
        uint32_t              picture_width  = 0;  // the image inside the coded size
        uint32_t              picture_height = 0;
        uint32_t              fps            = 0;
    };
    PipelineParams CurrentParams() const;

    // The adapter and backend the most recent generation was built on (encode_target.h). Before the
    // first build: a default (NVENC, zero LUID). Safe from any thread.
    EncodeAdapter CurrentEncodeAdapter() const;

    // A barrier caller, with resolution change, fault-rebuild and save.overlap_policy=truncate
    // (ring.h). Disarming tears the current generation down and leaves it down — the watchdog
    // thread stops touching it entirely until this flips back to true — rather than the watchdog
    // immediately rebuilding what was just torn down, which is what happens if a caller reaches
    // into Stop()/Start() instead of this. Arming when nothing is built (first arm, or after a
    // disarm) makes the watchdog build a fresh generation, on the same capped backoff a fault
    // rebuild uses if the first attempt fails, dropping a barrier over the gap either way
    // so the buffer on both sides of an arm/disarm cycle stays separately save-able.
    // Idempotent: calling with the state already in effect is a no-op. Safe from any thread; the
    // actual teardown/build work always happens on the watchdog thread, which is the only thread
    // allowed to touch capture_/encoder_ once Start() has returned.
    void SetArmed(bool armed);
    bool Armed() const { return armed_.load(std::memory_order_acquire); }

    // Memory while disarmed: once nothing has been appended for the ring's max age
    // (buffer.seconds + 2 s, RingMaxAgeQpc), no save can reach anything the ring holds, so the
    // watchdog releases it (Ring::ReleaseIfIdle) and calls `on_released`. `on_arm` runs inside
    // SetArmed(true), after the armed flag is set, on the caller's thread: it must be cheap. main
    // uses the pair to suspend the shared audio hub once every pipeline has released and to resume
    // it on the first arm. Call before Start().
    void SetRingReleaseCallbacks(std::function<void()> on_released, std::function<void()> on_arm) {
        on_ring_released_ = std::move(on_released);
        on_arm_           = std::move(on_arm);
    }
    bool RingReleased() const { return ring_ && ring_->Released(); }
    Ring* GetRing() const { return ring_; }

    // Diagnostics for tests and startup logging.
    uint64_t RebuildCount() const { return rebuild_count_.load(std::memory_order_acquire); }
    // Resolution reopens are counted separately from rebuilds: they are not faults, and a
    // test needs to tell "reopened at the new size" from "the fault path caught it."
    uint64_t ResolutionChangeCount() const {
        return resolution_change_count_.load(std::memory_order_acquire);
    }
    bool     Rebuilding() const { return rebuilding_.load(std::memory_order_acquire); }

    struct SupervisorReconfigParams {
        ApplyTier tier = ApplyTier::Free;
        uint32_t fps = 60;
        uint32_t bitrate_kbps = 15000;
        uint32_t keyframe_sec = 2;
        EncoderOptions encoder_opts;
        bool capture_cursor = true;
        size_t ring_capacity = 0;
        size_t ring_overshoot = 0;
        // When set, a Realloc-tier reconfigure re-Inits the ring file-backed
        // at this path instead of the 2-arg RAM overload — the fix for a bug advisor
        // review found (RequestReconfigure always calling the RAM overload silently dropped a
        // file-backed ring back to RAM on any unrelated Realloc-tier change). false/empty here
        // means RAM, matching every earlier caller of this struct exactly.
        bool          ring_file_backed = false;
        std::wstring  ring_file_path;
        // capture.target/capture.target_window, applied to `target_` in place —
        // monitor_device is deliberately NOT part of this (see RequestReconfigure's .cpp comment):
        // which monitor a pipeline captures is fixed at construction, not something a live config
        // patch changes.
        std::string   capture_target_mode = "monitor";  // capture.target: "monitor"|"game"|"window"
        std::string   capture_target_window;
        std::string   capture_resolution = "native";
    };
    bool RequestReconfigure(const SupervisorReconfigParams& req);

    // The *current* generation's live tick count (0 between a teardown and the next rebuild
    // completing) — tests use this to confirm the pipeline is actually ticking again after
    // a rebuild, not just that RebuildCount() incremented.
    uint64_t CurrentTicksSubmitted() const;

    // The most recently torn-down generation's Pacer counters/latency stats, captured at teardown
    // time (either the final Stop() or a fault-triggered rebuild) since the Pacer object itself is
    // destroyed immediately after. Per-generation, not cumulative across rebuilds — a rebuild
    // starts a fresh Pacer with its own counters from zero, same as a fresh process would, and
    // nothing adds cross-generation accumulation on top of that.
    struct RunStats {
        uint64_t          ticks_submitted = 0;
        uint64_t          duplicate_ticks = 0;
        uint64_t          late_ticks      = 0;
        uint64_t          resync_count    = 0;
        PacerTimerBackend timer_backend   = PacerTimerBackend::kTimeBeginPeriod;
        PacerLatencyStats latency;
    };
    RunStats LastStats() const;

    // Pacer counters and latency histograms summed over every generation this supervisor has run:
    // retired generations are folded in at teardown, plus the live one. Safe from any thread; a
    // periodic reader diffs two calls with TickCounters::Since/KeepMax (see latency.h for why the
    // diff saturates).
    TickCounters LifetimeCounters() const;

    // Test-only (pipeline_gpu_tests.cpp) — passthroughs to the *current*
    // generation's Capture so a test can inject a fault without reaching into supervisor
    // internals structurally. See capture.h's TestInject* comment for what these do and do not
    // prove (item-closed is a real, roughly-honest simulation; device-removed is synthetic and
    // exercises only the rebuild path, not real TDR detection).
    void TestInjectItemClosed();
    void TestInjectDeviceRemoved();
    // Test-only: makes the capture report this size in place of the
    // frame's real ContentSize(), without touching the user's display mode. See capture.h's own
    // comment for what this does and does not prove.
    void TestForceContentSize(uint32_t w, uint32_t h);

private:
    void WatchdogLoop();
    // Retries BuildGeneration() on the capped backoff. False means shutdown was requested
    // mid-backoff, not that the rebuild is impossible.
    bool RebuildWithBackoff(int attempt, int& backoff_index);
    std::optional<EncodeAdapter> ResolveAdapter(const CaptureTarget& target, const LUID* exclude = nullptr);
    // True when the GPU encode.adapter picks now has differed from this pipeline's for two checks:
    // a driver update made one vanish for a moment and a rebuild landed on the next, or (auto) the
    // recorded window moved to a monitor another GPU drives. Watchdog thread only. An explicit
    // nvidia enumerates only while off NVENC; an explicit amd or cpu, or a CPU fallback, never.
    bool ShouldReturnToPickedAdapter();
    // Moves there with one try: a GPU that's listed but can't encode yet sends this back to the adapter
    // that was working, not through the backoff to x264. False means shutdown or a reconfigure took over.
    bool ReturnToPickedAdapter(int& backoff_index);
    // Publishes the new generation's params, drops the pending barrier carrying them, then starts
    // the pacer — in that order, from both build paths. See the .cpp comment.
    void StartSegment(ICaptureSource& capture, IVideoEncoder& encoder, FrameScaler* scaler);
    // Opens an encoder at the size capture.resolution gives for `capture`'s current size, with the
    // FrameScaler that feeds it when that isn't the captured size (`scaler` stays null at native),
    // and registers its input textures. Shared by BuildGeneration and ReopenForResolution. False
    // (logged, prefixed with `context`) on any failure; Open() failures still die() if `fatal_open`.
    // A hardware encoder that wouldn't open goes to x264, unless `other_gpu` is given and auto has
    // another GPU with a hardware encoder: then it's set to that and this returns false.
    bool OpenEncodeStage(ICaptureSource& capture, bool fatal_open, const char* context,
                         std::unique_ptr<IVideoEncoder>& encoder, std::unique_ptr<FrameScaler>& scaler,
                         std::optional<EncodeAdapter>* other_gpu = nullptr);
    // Reopen at a new resolution without tearing down the capture device or the ring.
    bool ReopenForResolution(uint32_t w, uint32_t h);
    // Moves the current Pacer out, joins it, and records its per-generation counters.
    std::unique_ptr<Pacer> DetachAndStopPacer();
    // Throws winrt::hresult_error if Capture::Open()/etc. fail via a C++ exception (some WGC calls
    // do) — callers must catch it. Does NOT protect against Capture::Open()/Encoder::Open()'s own
    // die()/DieNv() calls on a genuinely fatal HRESULT; those still take the whole process down.
    // See supervisor.cpp's WatchdogLoop comment for why that gap still exists after this step.
    // Capture::Open + Encoder::Open + RegisterSlot*kSlots + StartSegment. `fatal_open` is true
    // only for the very first generation (Start()): a failing Encoder::Open at startup should
    // still take the process down loudly, while one on a rebuild must return false and be retried.
    // Returns false if the generation could not be built; throws winrt::hresult_error as above.
    bool BuildGeneration(bool fatal_open);
    // target_ with the Games list's live filter filled in for "The game".
    CaptureTarget EffectiveTarget() const;
    // Window-mode re-resolve, run from the watchdog's healthy branch. True when it tore the
    // generation down to switch windows (the caller restarts its loop).
    bool MaybeSwitchWindow(ICaptureSource& capture, int& backoff_index);
    // Pacer::Stop (joined) -> Encoder::Close -> Capture::Close. `out_reason`, if non-null, is
    // filled with why the generation was unhealthy — read here (after Pacer::Stop()'s join, before
    // the generation's objects are destroyed) because encoder.h's FaultReason() contract requires
    // it not be read while the pacer thread could still be writing it.
    // Why the torn-down generation stopped: `code` is a stable token for error reports (encoder.h
    // FaultCode), `capture` says the capture side failed rather than the encoder.
    struct FaultInfo {
        std::string reason;
        std::string code;
        bool        capture = false;
    };
    void TeardownGeneration(FaultInfo* out_fault = nullptr);

    uint32_t        fps_;
    uint32_t        bitrate_kbps_;
    uint32_t        keyframe_sec_;
    Ring*           ring_;
    EncoderOptions  encoder_opts_;
    std::atomic<bool> capture_cursor_{true};  // a Free-tier reconfigure writes it without reconfig_mutex_
    CaptureTarget   target_;
    std::string     capture_resolution_ = "native";  // watchdog thread / under reconfig_mutex_, like fps_
    // True once a strict Mode::Window "not found" has already been logged for
    // the current run of retries, so RebuildWithBackoff's 30s-capped loop doesn't re-log the same
    // "not launched yet" line on every attempt. Cleared the moment a generation actually builds.
    bool            target_missing_logged_ = false;
    // The last BuildGeneration failed only because a strict window target isn't running.
    // RebuildWithBackoff polls for it every second instead of backing off: nothing is broken, and a
    // game launched late would otherwise go unrecorded for up to the 30 s backoff cap.
    bool            last_build_target_missing_ = false;
    const GameLibrary* games_ = nullptr;
    std::atomic<bool>  game_fallback_{true};
    std::function<void()> on_ring_released_;
    std::function<void()> on_arm_;
    bool                  logged_commit_cap_ = false;  // watchdog thread only
    const IVideoEncoder*  late_encoder_      = nullptr;  // watchdog thread only: LateFrames() last
    uint64_t              late_logged_       = 0;        //   logged, for this encoder
    // Window re-resolve (WatchdogLoop): when the next check is due, and a switch waiting out
    // kWindowSwitchSettle. Watchdog thread only.
    std::chrono::steady_clock::time_point next_window_check_{};
    WindowSwitchState                     window_switch_;

    AuCallback              cb_;
    Pacer::PreEncodeCallback pre_encode_cb_;  // see SetPreEncodeCallback(); nullptr by default
    CaptureFactory           capture_factory_;  // see SetCaptureFactory(); unset = Capture
    EncoderFactory           encoder_factory_;   // see SetEncoderFactory(); unset = CreateVideoEncoder
    AdapterResolver          adapter_resolver_;  // see SetAdapterResolver(); unset = ResolveEncodeAdapter
    std::chrono::milliseconds adapter_recheck_{15000};
    std::chrono::steady_clock::time_point next_adapter_check_{};
    bool                     adapter_differed_ = false;  // at the last check
    int                      adapter_return_failures_ = 0;
    std::optional<EncodeAdapter> next_build_adapter_;  // the next build's adapter, once, instead of resolving
    bool                     trying_other_gpu_ = false;  // BuildGeneration's one retry on another GPU
    // Auto's pick whose encoder wouldn't open, so it went to the other GPU: later builds skip it, and
    // only the return check (ReturnToPickedAdapter, with its backoff) tries it again.
    std::optional<LUID>      refused_gpu_;
    // SetGameGpu: the latest value, and when it was cleared (QPC; 0 while set).
    mutable std::mutex       game_gpu_mutex_;
    std::optional<LUID>      game_gpu_;
    std::optional<LUID>      game_gpu_held_;  // the cleared value, kept for the hold
    int64_t                  game_gpu_cleared_qpc_ = 0;
    std::optional<LUID>      game_gpu_applied_;  // watchdog only: EffectiveGameGpu at the last check
    bool                     return_for_game_ = false;  // watchdog only: the next return is the game's
    std::optional<LUID>      EffectiveGameGpu() const;
    EncoderFallbackCallback  encoder_fallback_cb_;
    RingStorageLostCallback  ring_storage_lost_cb_;
    int                      hw_open_failures_ = 0;  // watchdog thread only; see kHwOpenFailuresBeforeCpu
    bool                     on_cpu_fallback_  = false;  // watchdog thread only

    mutable std::mutex              reconfig_mutex_;
    mutable std::mutex              gen_mutex_;
    std::unique_ptr<ICaptureSource> capture_;
    // Null at native. Declared before encoder_ so it is destroyed after it: the encoder holds its
    // output registered.
    std::unique_ptr<FrameScaler>    scaler_;
    std::unique_ptr<IVideoEncoder>  encoder_;
    std::unique_ptr<Pacer>    pacer_;
    // Every generation's ticks, kept across rebuilds so a fault's summary covers the ticks before it.
    FlightRecorder            flight_;

    mutable std::mutex   params_mutex_;
    PipelineParams        params_;
    EncodeAdapter         encode_adapter_;  // guarded by params_mutex_

    mutable std::mutex   stats_mutex_;
    RunStats              last_stats_;
    TickCounters          retired_counters_;  // guarded by stats_mutex_

    // The QPC instant of the cut between two segments, latched when a generation is torn down and
    // consumed by StartSegment() when the next one's parameters exist to attach to it. 0 = none
    // pending. Touched only by the watchdog thread after Start() returns (and by Start() itself
    // before that thread exists), so it needs no lock — do not read it from a save.
    int64_t               pending_barrier_qpc_ = 0;

    // Debounce for the resolution-reopen reaction in WatchdogLoop. A
    // window drag-resize delivers a new ContentSize many times a second; reopening on every one of
    // those cuts a ring segment barrier (Ring::Pin() can't cross one), which was collapsing the
    // replayable buffer to "since the last barrier" -- effectively "since the last mouse movement"
    // -- for the whole duration of a resize. Only reopen once the pending size has looked the same
    // for kResizeSettleDelay (see supervisor.cpp). Touched only by the watchdog thread, same as
    // pending_barrier_qpc_ above -- no lock needed.
    uint32_t                              resize_debounce_w_ = 0;
    uint32_t                              resize_debounce_h_ = 0;
    uint32_t                              resize_debounce_revision_ = 0;
    std::chrono::steady_clock::time_point resize_debounce_since_{};

    std::thread           watchdog_;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> rebuild_count_{0};
    std::atomic<bool>     rebuilding_{false};
    // Set by RequestReconfigure() while it is waiting for (or holding) reconfig_mutex_, cleared
    // when that call returns. RebuildWithBackoff's retry loop polls this so a stale rebuild -- e.g.
    // capture.target=window retrying forever against a window that closed, backoff capped at 30s,
    // holding reconfig_mutex_ for the whole sleep -- abandons the attempt promptly instead of
    // making a fresh set_config (like switching capture.target back to monitor) block behind it
    // past the UI's pipe request timeout. See RequestReconfigure/RebuildWithBackoff in supervisor.cpp.
    std::atomic<bool>     reconfig_requested_{false};
    // Coalesces back-to-back Realloc-tier RequestReconfigure calls: each Realloc-tier call bumps this and captures its own value before
    // waiting on reconfig_mutex_; if a fresher call has bumped it further by the time this one
    // actually acquires the lock, this one is stale (its ring reinit would just be immediately
    // discarded by the fresher call anyway) and skips the real work rather than reallocating twice.
    std::atomic<uint64_t> realloc_seq_{0};
    std::atomic<uint64_t> resolution_change_count_{0};
    // Polled by the watchdog thread, written by SetArmed() from any thread — the watchdog is the
    // only thing that acts on a transition (see SetArmed's comment above and WatchdogLoop's body).
    std::atomic<bool>     armed_{true};
};

}  // namespace ombra
