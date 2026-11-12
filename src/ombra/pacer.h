// The pacer thread.
//
// WGC is variable-framerate — it delivers a frame only when content changes, and a static screen
// produces nothing at all (1 frame in 15s on an idle desktop).
// Driving the encoder from FrameArrived would give VFR output, which breaks editors, Discord, and
// anything that seeks. The pacer instead ticks at a fixed rate and submits whichever texture slot
// the source most recently published, duplicating it when nothing new arrived. The AU timestamp for
// each tick is the tick index, not the WGC frame's SystemRelativeTime — the pacer tick *is* the
// output timeline; folding capture-side timestamps back in would reintroduce the VFR it exists to
// remove. (WGC's own timestamp is relevant for audio alignment, not here.)
#pragma once

#include "ombra/capture_source.h"
#include "ombra/encoder.h"
#include "ombra/frame_scaler.h"
#include "ombra/flight_recorder.h"
#include "ombra/latency.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace ombra {

// Which wait mechanism the pacer actually used — logged once at startup (never on the tick path)
// so a fresh session can tell which path is live without re-deriving it.
enum class PacerTimerBackend {
    kHighResWaitableTimer,  // CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)
    kTimeBeginPeriod,       // fallback: timeBeginPeriod(1) + sleep_until (pre-Windows 10 1803)
};

// p50/p99 of two disjoint per-tick durations, split so a caller can tell whether a pacing problem
// is the wait mechanism overshooting its deadline or the encode itself eating the frame budget —
// not just a combined number.
struct PacerLatencyStats {
    double wait_overshoot_p50_ms = 0.0;
    double wait_overshoot_p99_ms = 0.0;
    double encode_p50_ms = 0.0;
    double encode_p99_ms = 0.0;
};

// Two timestamps travel with every tick, deliberately not collapsed into one:
//   - `tick_index` is the CFR frame count — what NVENC's inputTimeStamp and the muxer's stts need.
//   - `qpc` (QueryPerformanceCounter, raw ticks) is wall-clock — what the ring descriptor and
//     the save-window search (T_start = oldest keyframe with qpc >= T_end - buffer.seconds)
//     need. Conflating them produces a save window that's the right frame count but the wrong
//     number of seconds whenever the pacer has fallen behind and resynced.
struct Tick {
    uint64_t index = 0;
    int64_t  qpc   = 0;
};

class Pacer {
public:
    // A new source's slots are never-drawn (black) textures until its first frame lands, and a
    // segment that opens on one shows a black first frame. So the pacer skips ticks until
    // ICaptureSource::HasFrame(), but no longer than this: a minimized window delivers nothing,
    // and its segment still has to start.
    static constexpr std::chrono::milliseconds kFirstFrameWait{500};

    using AuCallback = std::function<void(const IVideoEncoder::AccessUnit& au, const Tick& tick)>;

    // Invoked with the freshly-claimed slot, after ClaimSlotForEncoding but before EncodeSlot —
    // the one place `capturebench` and the screenshot hotkey can
    // read the raw BGRA texture, since the real pipeline never surfaces a slot number to anything
    // outside Pacer/Encoder. When set, it runs inline on the tick path, so whatever it does becomes
    // part of that tick's measured latency — acceptable for a bench tool or a rare one-shot, not
    // for anything continuous.
    using PreEncodeCallback = std::function<void(ICaptureSource& capture, uint32_t slot, const Tick& tick)>;

    // Runs on its own thread until Stop(). `cb` is invoked once per tick, synchronously — the
    // caller must not hold the AccessUnit past the callback's return (see Encoder::EncodeSlot).
    // With a `scaler`, each tick draws the claimed slot into the scaler's output and encodes that
    // (registered with the encoder as its only slot, 0); the pre-encode callback still sees the
    // captured, unscaled slot.
    void Start(ICaptureSource& capture, IVideoEncoder& encoder, uint32_t fps, AuCallback cb,
               PreEncodeCallback pre_encode_cb = nullptr, FrameScaler* scaler = nullptr);
    void Stop();

    // Before Start(): every tick also goes to `recorder` (flight_recorder.h), which the caller owns
    // and keeps alive past Stop(). Null (the default) records nothing.
    void SetFlightRecorder(FlightRecorder* recorder) { recorder_ = recorder; }

    // Diagnostics: how many ticks re-submitted the same slot as the previous tick (i.e. the source
    // had nothing new — expected and harmless, not an error); how many ticks were already past
    // their deadline when picked up (a real pacing miss);
    // and how many ticks were dropped outright because the pacer had fallen more than one period
    // behind and resynced instead of burst-submitting to pay off the backlog.
    uint64_t TicksSubmitted() const { return ticks_submitted_.load(); }
    uint64_t DuplicateTicks() const { return duplicate_ticks_.load(); }
    uint64_t LateTicks() const { return late_ticks_.load(); }
    uint64_t ResyncCount() const { return resync_count_.load(); }
    // How many ticks had to fall back
    // to a plain sleep_until because SetWaitableTimerEx itself failed (rare -- resource exhaustion,
    // a handle gone bad across a sleep/resume). Expected to stay 0 forever in practice; exposed so a
    // real occurrence is visible in telemetry rather than only inferable from the hang it used to
    // cause. See WaitUntilNextTick's own comment for why this can't log on the tick path.
    uint64_t TimerArmFailures() const { return timer_arm_failures_.load(); }
    // Ticks skipped at the start waiting for the source's first frame (kFirstFrameWait).
    uint64_t FirstFrameWaitTicks() const { return first_frame_wait_ticks_.load(); }

    // Which wait mechanism Run() actually ended up using. Valid only after Start() has been
    // called — Run() picks it before entering the tick loop.
    PacerTimerBackend TimerBackend() const { return backend_used_; }

    // Both are safe from any thread, including while the pacer runs: the tick path only does
    // relaxed atomic increments. Percentiles are bucketed to LatencyHistogram::kBucketMs.
    PacerLatencyStats ComputeLatencyStats() const;
    TickCounters ReadCounters() const;

private:
    void Run(ICaptureSource* capture, IVideoEncoder* encoder, uint32_t fps, AuCallback cb,
              PreEncodeCallback pre_encode_cb, FrameScaler* scaler);
    void WaitUntilNextTick(std::chrono::steady_clock::time_point target);

    FlightRecorder*   recorder_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> ticks_submitted_{0};
    std::atomic<uint64_t> duplicate_ticks_{0};
    std::atomic<uint64_t> late_ticks_{0};
    std::atomic<uint64_t> resync_count_{0};
    std::atomic<uint64_t> timer_arm_failures_{0};
    std::atomic<uint64_t> first_frame_wait_ticks_{0};

    // Windows HANDLE, kept as void* so this header doesn't have to pull in <windows.h> for
    // consumers that only want the counters (gputest, faulttest, etc. don't need it).
    void*             timer_handle_ = nullptr;
    PacerTimerBackend backend_used_ = PacerTimerBackend::kTimeBeginPeriod;

    LatencyHistogram wait_overshoot_ms_;
    LatencyHistogram encode_ms_;
};

}  // namespace ombra
