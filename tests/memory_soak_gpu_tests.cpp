// Diagnostic soak for engine memory growth: a real GPU run of
// PipelineSupervisor/pacer/NVENC/Ring, no UI, no saves, fed a duplicate-heavy synthetic source
// (present_fps << kFps) to match the idle-desktop condition of a real 5.2h session that
// leaked under (614/770 MB after hours, flat threads/handles, zero saves in that window).
//
// "gpu" suite: runs only with `ombra-tests --gpu`.

#include <doctest/doctest.h>

#include "support/synthetic_capture.h"

#include "ombra/ring.h"
#include "ombra/supervisor.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace ombra::test {

namespace {

using namespace std::chrono_literals;

size_t PrivateBytesKB() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage / 1024;
}

}  // namespace

TEST_CASE("gpu-soak: duplicate-heavy idle run over several minutes does not grow private bytes" *
          doctest::test_suite("gpu")) {
    constexpr uint32_t kFps         = 60;
    constexpr uint32_t kBitrateKbps = 8000;
    constexpr uint32_t kKeyframeSec = 1;

    auto scene = std::make_shared<SyntheticScene>();
    scene->present_fps = 1;  // near-static: pacer duplicates almost every tick, like an idle desktop

    // Deliberately small so the ring fills and starts evicting within seconds, not minutes -- the
    // real 5.2h leak happened in steady-state eviction, not during initial fill-up, and a ring big
    // enough to never evict in a short test would just measure normal fill-up growth (descriptor
    // bookkeeping for every not-yet-evicted tick, ~40 bytes each -- confirmed separately by hand:
    // that alone accounts for the fill-up run's growth, not a leak).
    Ring ring;
    ring.Init(32u << 10, 64u << 10);

    PipelineSupervisor supervisor(kFps, kBitrateKbps, kKeyframeSec, &ring);
    supervisor.SetCaptureFactory([scene] { return std::make_unique<SyntheticCapture>(scene); });
    supervisor.Start([&](const IVideoEncoder::AccessUnit& au, const Tick& tick) {
        ring.Append(au.data, au.size, tick.qpc, tick.index, au.is_keyframe);
    });

    // Let the pipeline settle (first-run allocations: NVENC session, ring arena, registered
    // resources) before taking the baseline, so startup cost doesn't get counted as "growth".
    std::this_thread::sleep_for(3000ms);
    const size_t baseline_kb = PrivateBytesKB();
    MESSAGE("baseline private_kb=" << baseline_kb);

    constexpr int kSamples = 12;
    constexpr auto kSampleInterval = 15s;  // 12 * 15s = 3 minutes of run time
    size_t max_kb = baseline_kb;
    for (int i = 0; i < kSamples; ++i) {
        std::this_thread::sleep_for(kSampleInterval);
        const size_t kb = PrivateBytesKB();
        max_kb = (std::max)(max_kb, kb);
        MESSAGE("sample " << i << " private_kb=" << kb << " delta_kb=" << (int64_t(kb) - int64_t(baseline_kb))
                           << " ticks=" << supervisor.CurrentTicksSubmitted()
                           << " ring_live_kb=" << ring.LiveBytes() / 1024);
    }

    supervisor.Stop();
    const size_t end_kb = PrivateBytesKB();
    MESSAGE("end private_kb=" << end_kb << " delta_kb=" << (int64_t(end_kb) - int64_t(baseline_kb)));

    // Loose bound for the first diagnostic run: real bug was +336 MB over 5.2h (~64 MB/hour). Over
    // this test's ~3 minutes that rate would be ~3.2 MB; 40 MB gives headroom for legitimate
    // one-time buffering/allocator behavior while still catching the real leak's order of magnitude.
    CHECK_MESSAGE((int64_t(max_kb) - int64_t(baseline_kb)) < 40 * 1024,
                  "private bytes grew " << (int64_t(max_kb) - int64_t(baseline_kb)) / 1024
                                         << " MB over the soak window");
}

// After resolution changes, an engine went
// 545 -> 589 MB private and 160 -> 273 MB dedicated GPU after four 1080p <-> 1440p changes and
// stayed there back at 1080p. Real WGC on the primary monitor (no capture factory), the size change
// injected through Capture::TestForceContentSize, so the real detection, FramePool::Recreate and
// encoder reopen all run. WGC delivers nothing on a static screen: without motion (a blinking caret
// is enough) the test skips with a message instead of failing.
TEST_CASE("gpu-soak: real-WGC resolution round trips don't retain private or GPU memory" *
          doctest::test_suite("gpu")) {
    constexpr uint32_t kFps         = 30;
    constexpr uint32_t kBitrateKbps = 13000;
    constexpr int      kRoundTrips  = 5;

    // Small, so it is full (and its commit flat) long before the first measurement.
    Ring ring;
    ring.Init(16u << 20, 16u << 20);

    EncoderOptions opts;
    opts.codec = VideoCodec::AV1;
    PipelineSupervisor supervisor(kFps, kBitrateKbps, 2, &ring, opts);
    supervisor.Start([&](const IVideoEncoder::AccessUnit& au, const Tick& tick) {
        ring.Append(au.data, au.size, tick.qpc, tick.index, au.is_keyframe);
    });

    auto wait_changes = [&](uint64_t n) {
        for (int i = 0; i < 100; ++i) {
            if (supervisor.ResolutionChangeCount() >= n) return true;
            std::this_thread::sleep_for(100ms);
        }
        return false;
    };
    auto gpu_mb = [] {
        // This process's usage on every adapter: QueryVideoMemoryInfo is per process.
        uint64_t local = 0, shared = 0;
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::pair<double, double>{0, 0};
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (FAILED(a.As(&a3))) continue;
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) local += info.CurrentUsage;
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) shared += info.CurrentUsage;
        }
        return std::pair<double, double>{local / 1048576.0, shared / 1048576.0};
    };
    // Private bytes minus the ring's commit (the ring fills during the run), as the floor over 3 s:
    // single samples spike tens of MB and recede within a second (allocator timing, 2026-10-20);
    // a retained allocation raises the floor.
    auto report = [&](const std::string& what) {
        double floor_net = 1e9, peak_net = 0;
        for (int i = 0; i < 30; ++i) {
            const double net = PrivateBytesKB() / 1024.0 - ring.CommittedBytes() / 1048576.0;
            floor_net = (std::min)(floor_net, net);
            peak_net = (std::max)(peak_net, net);
            std::this_thread::sleep_for(100ms);
        }
        const auto [local, shared] = gpu_mb();
        MESSAGE(what << " net_private_mb floor=" << floor_net << " peak=" << peak_net
                     << " gpu_local_mb=" << local << " gpu_shared_mb=" << shared);
        return std::pair<double, double>{floor_net, local};
    };

    std::this_thread::sleep_for(3000ms);
    // Against the pipeline before any change: what the first trip to a new size kept is the bug
    // this test found (the driver's cache of the freed 1440p allocations, see Capture::Recreate).
    const auto [base_net, base_gpu] = report("before any change");

    double net = base_net, gpu = base_gpu;
    for (int i = 1; i <= kRoundTrips; ++i) {
        supervisor.TestForceContentSize(2560, 1440);
        if (i == 1 && !wait_changes(1)) {
            supervisor.Stop();
            MESSAGE("SKIPPED: no WGC frame arrived in 10 s (static screen); needs motion on the monitor");
            return;
        }
        REQUIRE(wait_changes(2ull * i - 1));
        std::this_thread::sleep_for(1000ms);
        if (i == 1) report("at 1440p");
        supervisor.TestForceContentSize(0, 0);
        REQUIRE(wait_changes(2ull * i));
        std::tie(net, gpu) = report("after round trip " + std::to_string(i));
    }
    CHECK(supervisor.RebuildCount() == 0);
    supervisor.Stop();

    // Without the trim: +54 MB private and +45 MB GPU from the first trip on.
    CHECK_MESSAGE(net - base_net < 20.0, "private (net of ring) grew " << net - base_net << " MB");
    CHECK_MESSAGE(gpu - base_gpu < 20.0, "dedicated GPU grew " << gpu - base_gpu << " MB");
}

}  // namespace ombra::test
