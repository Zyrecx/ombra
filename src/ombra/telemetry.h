// Resource and health tracking for the engine (engine.telemetry, default on). Writes two CSV files
// per local day so a later debugging session has numbers from normal use rather than a
// recollection:
//   samples-YYYYMMDD.csv  one row per interval (30 s): memory, handles, threads, CPU, GPU, the
//                         overlay process, the foreground app, ring fill, pacer latency
//   events-YYYYMMDD.csv   one row per notable event: start/stop, saves, settings changes, restarts,
//                         arm/disarm, game changes, pipeline rebuilds, audio source resolution
// Location: <Instance().data_dir>\telemetry, normally %LOCALAPPDATA%\Ombra\telemetry. Files are
// flushed per row and the directory is pruned oldest-first past 50 MB. A same-day file written with
// other columns (an older build) is renamed to <prefix>-YYYYMMDD-N.csv first.
//
// Never call into this from the pacer's tick path: events take a mutex and do file I/O.
#pragma once

#include "ombra/latency.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace ombra {

bool GetProcessRssMb(double& ws_mb, double& private_mb);

class Telemetry {
public:
    static void SetEnabled(bool enabled);
    static bool Enabled();
    // Appends a row to today's events file. `type` is a short snake_case tag; `fmt` builds the
    // detail column, key=value pairs by convention. Always writes an INFO log line too; the row is
    // skipped while disabled.
    static void Event(const char* type, const char* fmt, ...);
    static std::wstring Directory();
    // The engine's own dedicated GPU memory (MB) from the sampler's last PDH read, up to one
    // interval old. -1 until the first sample, and always while telemetry is off.
    static double LastGpuDedicatedMb();
};

// What the engine knows about itself at sample time, filled by a callback the engine supplies so
// this module doesn't depend on the supervisor, controller or ring types.
struct EngineSample {
    bool         armed = false;
    std::string  game;  // UTF-8, empty when no game is armed
    DWORD        game_pid = 0;
    uint32_t     pipelines = 0;
    uint64_t     video_ring_bytes = 0;      // live footage
    uint64_t     video_ring_committed = 0;  // private memory the rings hold for it
    uint64_t     video_ring_capacity = 0;
    uint32_t     audio_sources = 0;
    uint64_t     audio_ring_bytes = 0;
    // AudioTrackHub::Losses, cumulative since engine start, 48 kHz frames.
    uint64_t     audio_dropped_frames = 0;  // staging overrun + encoder failures
    uint64_t     audio_late_frames = 0;
    uint64_t     audio_encoder_errors = 0;
    TickCounters ticks;  // summed across pipelines, cumulative since engine start
    uint64_t     rebuilds = 0;
    uint64_t     resolution_changes = 0;
    uint64_t     saves = 0;
};

class TelemetrySampler {
public:
    using Probe = std::function<EngineSample()>;

    // Starts the sampler thread. `probe` is called about once a second from that thread (to catch
    // transitions as events) and must be safe to call while the engine runs.
    explicit TelemetrySampler(Probe probe, std::chrono::seconds interval = std::chrono::seconds(30));
    ~TelemetrySampler();

    TelemetrySampler(const TelemetrySampler&) = delete;
    TelemetrySampler& operator=(const TelemetrySampler&) = delete;

private:
    void Run();

    Probe                probe_;
    std::chrono::seconds interval_;
    std::atomic<bool>    running_{true};
    std::thread          thread_;  // declared last so it starts after the members above exist
};

}  // namespace ombra
