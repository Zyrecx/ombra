// Encode cost of a static screen vs a moving one. Real NVENC on synthetic
// capture, no WGC, nothing on screen. Skipped unless run explicitly:
//   ombra-tests --gpu --no-skip -tc="encode bench*"
// Log `nvidia-smi --query-gpu=timestamp,pstate,clocks.video,power.draw,utilization.encoder
// --format=csv -l 1` alongside: the phase lines below carry wall-clock times to line up with it.
#include <doctest/doctest.h>

#include "support/amf_caps.h"
#include "support/synthetic_capture.h"
#include "ombra/encode_target.h"
#include "ombra/latency.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace ombra::test {
namespace {

void Stamp(char* buf, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

}  // namespace

TEST_CASE("encode bench: static vs moving vs same-content at 1080p" * doctest::test_suite("gpu") *
          doctest::skip()) {
    using namespace std::chrono_literals;
    for (uint32_t fps : {30u, 60u, 144u}) {
        auto scene = std::make_shared<SyntheticScene>();
        scene->width       = 1920;
        scene->height      = 1080;
        scene->present_fps = fps;
        Ring ring;
        ring.Init(128u << 20, 32u << 20);
        PipelineSupervisor sup(fps, 13258, 1, &ring);
        sup.SetCaptureFactory([scene] { return std::make_unique<SyntheticCapture>(scene); });
        sup.Start([&ring](const IVideoEncoder::AccessUnit& au, const Tick& t) {
            ring.Append(au.data, au.size, t.qpc, t.index, au.is_keyframe);
        });

        struct Mode { const char* name; bool frozen; bool constant; };
        for (Mode m : {Mode{"moving", false, false}, Mode{"same-content", false, true},
                       Mode{"frozen", true, false}, Mode{"moving-again", false, false}}) {
            scene->constant = m.constant;
            scene->frozen   = m.frozen;
            std::this_thread::sleep_for(8s);  // P-state hysteresis
            char t0[16], t1[16];
            Stamp(t0, sizeof t0);
            const TickCounters a = sup.LifetimeCounters();
            std::this_thread::sleep_for(12s);
            const TickCounters d = sup.LifetimeCounters().Since(a);
            Stamp(t1, sizeof t1);
            MESSAGE(fps << " fps  " << std::string(m.name) << "  [" << t0 << "-" << t1 << "]  ticks " << d.ticks
                        << "  dup " << (d.ticks ? 100.0 * d.duplicates / d.ticks : 0.0) << "%  late "
                        << d.late << "  resyncs " << d.resyncs << "  encode p50 "
                        << d.encode.Percentile(0.5) << "  p99 " << d.encode.Percentile(0.99)
                        << "  max " << d.encode.Max());
        }
        sup.Stop();
    }
}

// AV1 against H.264 on each GPU, moving 1080p content, same bitrate. AMF AV1 runs padded (1082
// lines), the cost a user picking "Pad" or "Scale" pays. Skipped unless run explicitly:
//   ombra-tests --gpu --no-skip -tc="encode bench: AV1*"
TEST_CASE("encode bench: AV1 vs H.264 per GPU at 1080p" * doctest::test_suite("gpu") * doctest::skip()) {
    using namespace std::chrono_literals;
    SeedAmfCapsInProcess();
    for (const char* adapter : {"nvidia", "amd"}) {
        for (uint32_t fps : {30u, 60u}) {
            for (VideoCodec codec : {VideoCodec::H264, VideoCodec::AV1}) {
                auto scene = std::make_shared<SyntheticScene>();
                scene->width       = 1920;
                scene->height      = 1080;
                scene->present_fps = fps;
                Ring ring;
                ring.Init(128u << 20, 32u << 20);
                EncoderOptions opts;
                opts.adapter       = adapter;
                opts.codec         = codec;
                opts.av1_unaligned = "pad";
                PipelineSupervisor sup(fps, 13258, 1, &ring, opts);
                sup.SetCaptureFactory([scene] { return std::make_unique<SyntheticCapture>(scene); });
                std::atomic<uint64_t> bytes{0};
                sup.Start([&](const IVideoEncoder::AccessUnit& au, const Tick& t) {
                    ring.Append(au.data, au.size, t.qpc, t.index, au.is_keyframe);
                    bytes.fetch_add(au.size, std::memory_order_relaxed);
                });
                std::this_thread::sleep_for(6s);  // P-state hysteresis
                const TickCounters a  = sup.LifetimeCounters();
                const uint64_t     b0 = bytes.load();
                std::this_thread::sleep_for(12s);
                const TickCounters d = sup.LifetimeCounters().Since(a);
                const double       kbps = (bytes.load() - b0) * 8.0 / 12.0 / 1000.0;
                const auto         p    = sup.CurrentParams();
                MESSAGE(std::string(adapter) << " " << fps << " fps  asked " << std::string(VideoCodecName(codec)) << " got "
                                << std::string(VideoCodecName(p.codec)) << " " << p.width << "x" << p.height << "  late " << d.late
                                << "/" << d.ticks << "  resyncs " << d.resyncs << "  encode p50 "
                                << d.encode.Percentile(0.5) << "  p99 " << d.encode.Percentile(0.99) << "  max "
                                << d.encode.Max() << "  " << kbps << " kbps");
                sup.Stop();
            }
        }
    }
    SetAmfAdapterCaps({});
}

}  // namespace ombra::test
