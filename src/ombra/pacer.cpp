#include "ombra/pacer.h"
#include "ombra/log.h"

#include <windows.h>
#include <mmsystem.h>  // timeBeginPeriod/timeEndPeriod (winmm) — the timer fallback path

#include <algorithm>
#include <chrono>

namespace ombra {

namespace {
int64_t QpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}
}  // namespace

void Pacer::Start(ICaptureSource& capture, IVideoEncoder& encoder, uint32_t fps, AuCallback cb,
                   PreEncodeCallback pre_encode_cb, FrameScaler* scaler) {
    running_.store(true);
    thread_ = std::thread(&Pacer::Run, this, &capture, &encoder, fps, std::move(cb),
                           std::move(pre_encode_cb), scaler);
}

void Pacer::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void Pacer::WaitUntilNextTick(std::chrono::steady_clock::time_point target) {
    using clock = std::chrono::steady_clock;
    if (timer_handle_) {
        const auto now = clock::now();
        if (target > now) {
            const auto rel_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
            LARGE_INTEGER due;
            // Negative due time = relative, in 100ns units. Must be non-zero or the timer is
            // treated as already-expired-and-cancel, which on some builds fires immediately
            // without actually waiting — round up to at least 1 tick (100ns).
            due.QuadPart = -(static_cast<LONGLONG>(rel_ns / 100));
            if (due.QuadPart == 0) due.QuadPart = -1;
            // The return value used to
            // go unchecked -- on failure the MANUAL_RESET timer stays in whatever state it was
            // already in (unsignaled on the very first tick, since it was just created), so the
            // unconditional WaitForSingleObject(..., INFINITE) below would block forever, and so
            // would Stop()'s thread_.join() waiting on this thread to notice running_ went false. No
            // log/lock on this path (this file's own per-tick rule) -- count it and fall back to a
            // plain sleep for this one tick instead, same as the no-high-res-timer branch below.
            if (SetWaitableTimerEx(static_cast<HANDLE>(timer_handle_), &due, 0, nullptr, nullptr,
                                    nullptr, 0)) {
                WaitForSingleObject(static_cast<HANDLE>(timer_handle_), INFINITE);
            } else {
                ++timer_arm_failures_;
                std::this_thread::sleep_until(target);
            }
        }
    } else {
        std::this_thread::sleep_until(target);
    }
}

void Pacer::Run(ICaptureSource* capture, IVideoEncoder* encoder, uint32_t fps, AuCallback cb,
                 PreEncodeCallback pre_encode_cb, FrameScaler* scaler) {
    Log::SetThreadName("pacer");
    using clock  = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1'000'000'000ULL / fps);

    // A wake-up that waits for a core shows as a held frame in the clip, at the moments a game is
    // busiest. 1080p60, normal-priority load on all 16 threads, 30 s, three runs each (2026-11-03):
    // late ticks 559/597/96 -> 1/5/1, wait overshoot p99 ~15 -> 0.75 ms, no measurable loss of the
    // load's own throughput. The thread sleeps most of each tick.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Timer setup happens once, before the tick loop — this is startup cost, not per-tick cost,
    // so logging here is fine even though nothing in the loop below may log (the hot-loop rule).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    bool used_time_begin_period = false;
    if (timer) {
        timer_handle_ = timer;
        backend_used_ = PacerTimerBackend::kHighResWaitableTimer;
        Log::Info("pacer timer backend: high-resolution waitable timer");
    } else {
        // Pre-Windows 10 1803, or the flag is otherwise unsupported. timeBeginPeriod(1) raises
        // the *global* system timer resolution to ~1ms, which sleep_until's underlying wait then
        // inherits — coarser than the waitable timer path and it affects the whole system for as
        // long as this process holds it, but it's a real fallback, not a silent no-op.
        timeBeginPeriod(1);
        used_time_begin_period = true;
        backend_used_ = PacerTimerBackend::kTimeBeginPeriod;
        Log::Info("pacer timer backend: timeBeginPeriod(1) fallback "
                  "(CreateWaitableTimerExW high-res flag unsupported)");
    }

    uint64_t   last_frame = 0;  // SlotClaim::frame of the previous claim; 0 = nothing published
    uint64_t   tick_index = 0;
    clock::time_point next_tick = clock::now();
    const clock::time_point first_frame_deadline = next_tick + kFirstFrameWait;
    bool       waiting_for_first_frame = true;
    bool       first_record = true;

    while (running_.load(std::memory_order_relaxed)) {
        FlightRecord rec;  // this tick's line in the flight recorder, pushed once it's encoded
        const auto now = clock::now();
        if (now > next_tick) {
            ++late_ticks_;  // already past deadline before we even waited
            rec.flags |= FlightRecord::kLate;
        }

        // If the pacer has fallen more than a full period behind (a stall — GPU contention, a
        // debugger pause, whatever), waiting for next_tick would return instantly for every
        // backlogged tick and burst-submit them to "catch up." That bunches frames instead of
        // dropping them, which breaks the frames<->wall-time relationship the ring's save window
        // depends on. Resync instead: drop the backlog, count it, and resume on the next tick of the
        // same grid. tick_index skips the dropped ticks too, so the frame before the gap plays
        // through it: the mux times video by index and audio by QPC, and an index that only counted
        // submitted frames made an overloaded encoder's clips sped up and out of sync.
        if (now > next_tick + period) {
            const auto skipped = (now - next_tick) / period + 1;
            next_tick += skipped * period;
            if (!waiting_for_first_frame) tick_index += static_cast<uint64_t>(skipped);
            ++resync_count_;
            rec.flags |= FlightRecord::kResync;
            rec.skipped = static_cast<uint8_t>(std::min<int64_t>(skipped, 255));
        }
        WaitUntilNextTick(next_tick);

        const auto woke = clock::now();
        const double overshoot_ms = std::chrono::duration<double, std::milli>(woke - next_tick).count();
        wait_overshoot_ms_.Record(overshoot_ms);
        rec.overshoot_us = static_cast<uint16_t>(std::clamp(overshoot_ms * 1000.0, 0.0, 65535.0));

        // DXGI_ERROR_DEVICE_REMOVED/RESET and "WGC item closed" are recoverable at
        // runtime, never a die() here. Checked once per tick (cheap — see Capture::IsHealthy's
        // comment) so a fault is caught before this tick claims a slot from a capture session
        // that's already gone. Stopping the loop and returning is this thread's whole
        // responsibility; PipelineSupervisor's watchdog is what notices
        // TicksSubmitted() has stopped advancing / capture->IsHealthy() is false and rebuilds —
        // this function does not know it is being supervised.
        if (!capture->IsHealthy()) {
            running_.store(false, std::memory_order_release);
            break;
        }

        // Before the claim, so a skipped tick leaves no slot marked as encoding, and before
        // tick_index moves, so the first AU is still index 0 and the timeline has no gap.
        if (waiting_for_first_frame) {
            if (!capture->HasFrame() && woke < first_frame_deadline) {
                ++first_frame_wait_ticks_;
                next_tick += period;
                continue;
            }
            waiting_for_first_frame = false;
        }

        const ICaptureSource::SlotClaim claim = capture->ClaimSlotForEncoding();
        const uint32_t                  slot  = claim.slot;
        if (claim.frame == last_frame) {
            ++duplicate_ticks_;
            rec.flags |= FlightRecord::kDuplicate;
        }
        last_frame = claim.frame;

        Tick tick{tick_index, QpcNow()};
        if (pre_encode_cb) pre_encode_cb(*capture, slot, tick);
        const auto encode_start = clock::now();
        IVideoEncoder::AccessUnit au;
        if (scaler) {
            scaler->Scale(slot);
            au = encoder->EncodeSlot(0, scaler->Width(), scaler->Height(), tick_index);
        } else {
            au = encoder->EncodeSlot(slot, capture->Width(), capture->Height(), tick_index);
        }
        const double encode_ms = std::chrono::duration<double, std::milli>(clock::now() - encode_start).count();
        encode_ms_.Record(encode_ms);
        if (recorder_) {
            rec.qpc       = tick.qpc;
            rec.encode_us = static_cast<uint32_t>(std::min(encode_ms * 1000.0, 4.0e9));
            rec.au_bytes  = au.ok ? static_cast<uint32_t>(std::min<size_t>(au.size, UINT32_MAX)) : 0;
            if (au.ok && au.is_keyframe) rec.flags |= FlightRecord::kKeyframe;
            if (!au.ok) rec.flags |= FlightRecord::kEncodeFail;
            if (first_record) rec.flags |= FlightRecord::kFirst;
            first_record = false;
            recorder_->Push(rec);
        }
        if (!au.ok) {
            // encoder->FaultReason() already logged once from inside EncodeSlot (Encoder::
            // ReportFault) — nothing more to do here than stop cleanly. `cb` must not run: `au`
            // carries no valid data (Encoder::AccessUnit's contract, encoder.h).
            capture->ReleaseEncodingSlot();
            running_.store(false, std::memory_order_release);
            break;
        }
        cb(au, tick);
        encoder->UnlockLast();
        capture->ReleaseEncodingSlot();
        if (encoder->Faulted()) {
            // UnlockLast() failed after a successful EncodeSlot — this tick's AU was still valid
            // and already handed to `cb` (already appended to the ring), so finish the loop body
            // normally and stop only *after* counting this tick, rather than discarding a tick
            // that was genuinely good.
            ++ticks_submitted_;
            ++tick_index;
            running_.store(false, std::memory_order_release);
            break;
        }

        ++ticks_submitted_;
        ++tick_index;
        next_tick += period;
    }

    // Off the tick path: the loop has ended.
    Log::Info("pacer stopped  first-frame wait %llu ticks%s",
              static_cast<unsigned long long>(first_frame_wait_ticks_.load()),
              waiting_for_first_frame ? " (no frame yet)" : "");
    if (timer) CloseHandle(timer);
    if (used_time_begin_period) timeEndPeriod(1);
    timer_handle_ = nullptr;
}

PacerLatencyStats Pacer::ComputeLatencyStats() const {
    const auto overshoot = wait_overshoot_ms_.Read();
    const auto encode    = encode_ms_.Read();
    PacerLatencyStats stats;
    stats.wait_overshoot_p50_ms = overshoot.Percentile(0.50);
    stats.wait_overshoot_p99_ms = overshoot.Percentile(0.99);
    stats.encode_p50_ms         = encode.Percentile(0.50);
    stats.encode_p99_ms         = encode.Percentile(0.99);
    return stats;
}

TickCounters Pacer::ReadCounters() const {
    TickCounters c;
    c.ticks          = ticks_submitted_.load(std::memory_order_relaxed);
    c.duplicates     = duplicate_ticks_.load(std::memory_order_relaxed);
    c.late           = late_ticks_.load(std::memory_order_relaxed);
    c.resyncs        = resync_count_.load(std::memory_order_relaxed);
    c.wait_overshoot = wait_overshoot_ms_.Read();
    c.encode         = encode_ms_.Read();
    return c;
}

}  // namespace ombra
