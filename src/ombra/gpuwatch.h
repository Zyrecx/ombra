#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ombra {

// Background per-process GPU 3D-engine utilization sampler.
// QueryProcessAdapter (gputopo.h) is a one-shot diagnostic with a mandatory 1s blocking Sleep and a
// full wildcard PDH re-expansion per call -- explicitly documented as never to be called per-frame,
// and it's also unusable as a per-foreground-change signal since ProcessEvent
// (controller.cpp) runs on every alt-tab, not on a slow timer. GpuLoadWatcher instead keeps its own
// background thread that periodically re-expands/collects once and publishes a pid -> 3D
// utilization% snapshot that ProcessEvent can read without blocking or touching PDH itself.
class GpuLoadWatcher {
public:
    GpuLoadWatcher();
    ~GpuLoadWatcher();

    GpuLoadWatcher(const GpuLoadWatcher&) = delete;
    GpuLoadWatcher& operator=(const GpuLoadWatcher&) = delete;

    // Most recently sampled 3D-engine utilization percent for `pid`, or 0.0 if the process
    // currently holds no GPU Engine instances, or no sample has completed yet.
    double Utilization3D(DWORD pid) const;

    // The adapter `pid`'s 3D engines were busiest on at the last sample; false with no 3D load.
    bool RenderAdapter(DWORD pid, LUID* luid) const;

private:
    void Run();

    struct Sample {
        double util_3d = 0;      // summed over every adapter
        LUID   busiest{};        // the adapter with the most of it
        double busiest_util = 0;
    };

    std::thread       thread_;
    std::atomic<bool> running_{true};

    mutable std::mutex                snapshot_mutex_;
    std::unordered_map<DWORD, Sample> snapshot_;
};

}  // namespace ombra
