// A tool's open clip (clip.h) on a synthetic timeline: a real H.264 stream through the engine's own
// Ring/AudioTrackHub and the streaming mux, a clock that moves only when the test feeds footage,
// ffprobe/ffmpeg to judge the file.

#include <doctest/doctest.h>

#include "support/audio_analysis.h"
#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/audio_tracks.h"
#include "ombra/clip.h"
#include "ombra/config.h"
#include "ombra/controller.h"
#include "ombra/json.h"
#include "ombra/reconfig.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/supervisor.h"
#include "ombra/util.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace ombra::test {

namespace {

constexpr uint32_t kRate         = 48000;
constexpr size_t   kPacketFrames = 480;

class ClipRig {
public:
    explicit ClipRig(const std::wstring& name, size_t capacity = 32u << 20, size_t overshoot = 8u << 20)
        : freq_(QpcFrequency()), origin_(QpcFrequency() * 1000), name_(name) {
        const H264Fixture& fx = StandardFixture();
        ring.Init(capacity, overshoot);
        auto params = std::make_shared<SegmentParams>();
        params->codec_header = fx.sps_pps;
        params->width        = fx.width;
        params->height       = fx.height;
        ring.SetBaseSegmentParams(params);
        ctx.ring           = &ring;
        ctx.fps            = fx.fps;
        ctx.buffer_seconds = 10.0;
        ctx.out_path       = TestOutputPath(L"clip\\" + name + L".mp4");
        DeleteFileW(ctx.out_path.c_str());
    }

    // Puts the timeline on the real clock, `seconds_ago` of footage ending now (StartClip's threads
    // read QpcNow). Call before feeding.
    void EndNow(double seconds_ago) { origin_ = QpcNow() - static_cast<int64_t>(seconds_ago * freq_); }

    int64_t At(double seconds) const { return origin_ + static_cast<int64_t>(std::llround(seconds * freq_)); }
    int64_t Tick(uint64_t t) const { return origin_ + static_cast<int64_t>(t) * freq_ / StandardFixture().fps; }

    // Audio: one source, a hub sized for `audio_seconds` of reach, never Start()ed.
    void AddAudio(double audio_seconds) {
        hub = std::make_unique<AudioTrackHub>(At(0), freq_, 192, AudioTrackHub::SizeFor(audio_seconds, 192, 0));
        hub->SetSources({{"Game", "game"}}, false);
        ctx.audio = hub.get();
    }

    // Video ticks [fed, until_tick) and, with audio, a 440 Hz tone over the same time.
    void FeedTo(uint64_t until_tick) {
        const H264Fixture& fx = StandardFixture();
        for (; fed < until_tick; ++fed) {
            const auto& au = fx.aus[fed % fx.aus.size()];
            ring.Append(au.annexb.data(), au.annexb.size(), Tick(fed), fed, au.idr);
        }
        if (!hub) return;
        const uint64_t last = until_tick * kRate / fx.fps;
        std::vector<int16_t> packet(kPacketFrames * 2);
        for (; audio_fed < last; audio_fed += kPacketFrames) {
            for (size_t i = 0; i < kPacketFrames; ++i) {
                const double phase = 2.0 * std::numbers::pi * 440.0 * double(audio_fed + i) / kRate;
                packet[i * 2] = packet[i * 2 + 1] = static_cast<int16_t>(std::lround(0.5 * 32767.0 * std::sin(phase)));
            }
            hub->Append(0, packet.data(), packet.size(), At(double(audio_fed) / kRate));
        }
        hub->EncodeThrough(Tick(fed));
    }

    std::shared_ptr<ClipSession> Open(double event_s, double before_s, double after_s, SaveOutcome* why = nullptr) {
        ClipRequest req;
        req.event_qpc      = At(event_s);
        req.before_seconds = before_s;
        req.after_seconds  = after_s;
        return ClipSession::Open(&ctx, req, nullptr, why);
    }

    // Runs `clip` to its end. Each wait feeds 0.1 s more footage (up to `feed_limit_s`) and moves
    // the clock 0.1 s; `on_wait` sees the clock first.
    ClipSession::Result Run(ClipSession& clip, double feed_limit_s,
                            const std::function<void(double now_s)>& on_wait = {}) {
        const uint64_t limit = static_cast<uint64_t>(std::llround(feed_limit_s * StandardFixture().fps));
        int waits = 0;
        return clip.Run([&] { return At(now_s); },
                        [&] {
                            REQUIRE(++waits < 100000);
                            if (on_wait) on_wait(now_s);
                            now_s += 0.1;
                            FeedTo((std::min)(limit, static_cast<uint64_t>(std::llround(now_s * StandardFixture().fps))));
                        });
    }

    Ring                           ring;
    SaveContext                    ctx;
    std::unique_ptr<AudioTrackHub> hub;
    uint64_t                       fed       = 0;
    uint64_t                       audio_fed = 0;
    double                         now_s     = 0;

private:
    int64_t      freq_;
    int64_t      origin_;
    std::wstring name_;
};

}  // namespace

TEST_CASE("clip: tails live footage from before the event to after it") {
    ClipRig rig(L"tail");
    rig.FeedTo(180);  // 0..3 s
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 2.0, 2.0);
    REQUIRE(clip);
    CHECK_FALSE(rig.ctx.save_in_flight.load());  // never the user's save slot
    CHECK(ClipsOpen(&rig.ctx));
    const ClipSession::Result r = rig.Run(*clip, 10.0);
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == ClipEnd::kReached);
    CHECK(r.seconds == doctest::Approx(4.0));
    CHECK(ProbeVideoFrames(r.path).count == 240);  // ticks 60..299: 1 s .. 5 s
    CHECK(DecodeErrors(r.path).empty());
    CHECK_FALSE(clip->Extend(rig.At(9.0)));  // it ended: the caller opens a new clip
    CHECK(ClipsOpen(&rig.ctx));               // until its thread is done with the context
    clip->Close();
    CHECK_FALSE(ClipsOpen(&rig.ctx));
}

TEST_CASE("clip: Extend moves the end out while the clip records") {
    ClipRig rig(L"extend");
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 1.0, 1.0);
    REQUIRE(clip);
    bool extended = false;
    const auto r = rig.Run(*clip, 10.0, [&](double now) {
        if (!extended && now >= 3.5) {
            CHECK(clip->Extend(rig.At(6.0)));
            CHECK(clip->Extend(rig.At(5.0)));  // an earlier end never shortens it
            extended = true;
        }
    });
    CHECK(extended);
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(ProbeVideoFrames(r.path).count == 240);  // 2 s .. 6 s
}

TEST_CASE("clip: however often it's extended, a clip ends kClipMaxAfterSeconds past its event") {
    ClipRig rig(L"cap", 4u << 20, 2u << 20);  // the long tail fits only because it releases behind itself
    rig.FeedTo(120);
    rig.now_s = 2.0;
    auto clip = rig.Open(2.0, 1.0, 1000.0);
    REQUIRE(clip);
    CHECK_FALSE(clip->Extend(rig.At(5000.0)));  // past the cap: the caller opens a new clip instead
    CHECK(clip->Extend(rig.At(2.0 + kClipMaxAfterSeconds)));
    const auto r = rig.Run(*clip, 1000.0);
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == ClipEnd::kReached);
    CHECK(r.seconds == doctest::Approx(1.0 + kClipMaxAfterSeconds));
    CHECK(rig.ring.LiveBytes() <= (6u << 20));
}

TEST_CASE("clip: Stop ends the clip with what it took, as a whole file") {
    ClipRig rig(L"stop");
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 2.0, 30.0);
    REQUIRE(clip);
    const auto r = rig.Run(*clip, 60.0, [&](double now) {
        if (now >= 4.0) clip->Stop();
    });
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == ClipEnd::kStopped);
    CHECK(r.seconds == doctest::Approx(3.0).epsilon(0.05));
    CHECK(DecodeErrors(r.path).empty());
}

TEST_CASE("clip: a barrier cut while it records ends the file there") {
    ClipRig rig(L"segment");
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 2.0, 10.0);
    REQUIRE(clip);
    uint64_t cut_at = 0;
    const auto r = rig.Run(*clip, 20.0, [&](double now) {
        if (!cut_at && now >= 4.0) {
            cut_at = rig.fed;
            rig.ring.Barrier(rig.Tick(cut_at));  // a resolution change at about 4 s
        }
    });
    REQUIRE(cut_at > 0);
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == ClipEnd::kSegmentEnded);
    CHECK(ProbeVideoFrames(r.path).count == static_cast<size_t>(cut_at - 60));  // 1 s .. the cut
}

TEST_CASE("clip: footage that stops arriving ends the clip after the stall time") {
    ClipRig rig(L"stall");
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 2.0, 10.0);
    REQUIRE(clip);
    const auto r = rig.Run(*clip, 3.0);  // nothing past 3 s: capture paused, Instant Replay off
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == ClipEnd::kStalled);
    CHECK(rig.now_s == doctest::Approx(3.0 + kClipStallSeconds).epsilon(0.05));
    CHECK(ProbeVideoFrames(r.path).count == 120);
}

TEST_CASE("clip: a writer that falls past the headroom fails its own file, not the ring") {
    ClipRig rig(L"broken", 1u << 20, 128u << 10);
    rig.FeedTo(120);
    rig.now_s = 2.0;
    auto clip = rig.Open(2.0, 0.5, 30.0);
    REQUIRE(clip);
    const auto r = rig.Run(*clip, 200.0, [&](double) { rig.FeedTo(rig.fed + 6000); });  // a stalled disk
    CHECK(r.outcome == SaveOutcome::kFailed);
    CHECK(r.end == ClipEnd::kBroken);
    CHECK(GetFileAttributesW(r.path.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(rig.ring.LiveBytes() <= (1u << 20) + (128u << 10));
}

TEST_CASE("clip: the user's hotkey save during an open clip is its own file, neither costs the other") {
    ClipRig rig(L"alongside");
    rig.ctx.save_overlap_policy = "truncate";
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 2.0, 2.0);
    REQUIRE(clip);
    bool saved = false;
    uint64_t fed_at_save = 0;
    const std::wstring hotkey = TestOutputPath(L"clip\\alongside-hotkey.mp4");
    DeleteFileW(hotkey.c_str());
    const auto r = rig.Run(*clip, 10.0, [&](double now) {
        if (saved || now < 4.0) return;
        const std::wstring clip_path = rig.ctx.out_path;
        rig.ctx.out_path = hotkey;
        rig.ctx.save_in_flight.store(true);
        StdoutSilencer quiet;
        CHECK(DoSave(&rig.ctx, rig.At(now)) == SaveOutcome::kSaved);  // not busy
        rig.ctx.out_path = clip_path;
        fed_at_save = rig.fed;
        saved = true;
    });
    REQUIRE(saved);
    CHECK(ProbeVideoFrames(hotkey).count == static_cast<size_t>(fed_at_save));  // from 0 s up to the press
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(ProbeVideoFrames(r.path).count == 240);  // the truncate cut doesn't touch the clip
}

TEST_CASE("clip: an open clip refuses a buffer reallocation, and one blocked by it can't open") {
    constexpr size_t kMb = 1024 * 1024;
    ClipRig rig(L"realloc");
    rig.AddAudio(10.0);
    PipelineSupervisor sup(60, 10000, 2, &rig.ring);
    sup.SetArmed(false);  // no generation to build: nothing touches the GPU
    BufferController controller(&sup);
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto clip = rig.Open(3.0, 2.0, 2.0);
    REQUIRE(clip);

    Config cfg{};
    cfg.buffer_seconds = 10.0;
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("buffer.seconds", JsonValue::MakeNum(30));
    const size_t capacity = rig.ring.Capacity();
    const LiveApplyResult refused = ApplyLiveConfig(cfg, patch, sup, controller, rig.ctx, rig.ring);
    CHECK_FALSE(refused.ok);
    CHECK(rig.ring.Capacity() == capacity);
    CHECK(rig.hub->TrackRingCapacity() == AudioTrackHub::SizeFor(10.0, 192, 0).ring_capacity);
    CHECK_FALSE(rig.ctx.save_in_flight.load());  // the refusal let go of the save slot

    const auto r = rig.Run(*clip, 10.0);
    CHECK(r.outcome == SaveOutcome::kSaved);
    clip.reset();
    CHECK(ApplyLiveConfig(cfg, patch, sup, controller, rig.ctx, rig.ring).ok);
    CHECK(rig.ring.Capacity() != capacity);

    {
        std::lock_guard<std::mutex> lock(rig.ctx.clip_mutex);
        rig.ctx.clips_blocked = true;  // a reallocation between its check and the rebuild
    }
    SaveOutcome why = SaveOutcome::kSaved;
    CHECK_FALSE(rig.Open(3.0, 1.0, 1.0, &why));
    CHECK(why == SaveOutcome::kBusy);
    CHECK_FALSE(ClipsOpen(&rig.ctx));
    (void)kMb;
}

TEST_CASE("clip: a clip longer than the audio rings' reach keeps its opening audio") {
    ClipRig rig(L"audio");
    rig.AddAudio(0.0);  // the track rings hold only their slack, about 20 s at 192 kbps: the clip runs 42 s
    rig.FeedTo(120);
    rig.now_s = 2.0;
    auto clip = rig.Open(2.0, 2.0, 40.0);
    REQUIRE(clip);
    const auto r = rig.Run(*clip, 60.0);
    REQUIRE(r.outcome == SaveOutcome::kSaved);
    CHECK(r.seconds == doctest::Approx(42.0));
    const Pcm pcm = DecodeAudio(r.path, 0);
    CHECK(pcm.Seconds() == doctest::Approx(42.0).epsilon(0.01));
    CHECK(ToneAmplitude(pcm, 440.0, 0, kRate / 2) == doctest::Approx(0.5).epsilon(0.06));
    CHECK(ToneAmplitude(pcm, 440.0, 41 * kRate, kRate / 2) == doctest::Approx(0.5).epsilon(0.06));
}

TEST_CASE("clip: two clips of one moment get their own files, a long title cut to fit") {
    ClipRig rig(L"names");
    rig.ctx.out_path    = L"out\\ombra.mp4";  // the output.filename template, as live
    rig.ctx.output_root = NarrowUtf8(TestOutputPath(L"clip\\names"));
    rig.ctx.output_filename = "{game} {date} {time}.mp4";
    rig.FeedTo(180);
    rig.now_s = 3.0;
    ClipRequest req;
    req.event_qpc      = rig.At(3.0);
    req.before_seconds = 2.0;
    req.after_seconds  = 1.0;
    req.title          = std::wstring(300, L'x');
    SaveOutcome why = SaveOutcome::kFailed;
    auto first  = ClipSession::Open(&rig.ctx, req, nullptr, &why);
    auto second = ClipSession::Open(&rig.ctx, req, nullptr, &why);  // before the first's file exists
    REQUIRE(first);
    REQUIRE(second);
    const auto a = rig.Run(*first, 10.0);
    const auto b = rig.Run(*second, 10.0);
    CHECK(a.outcome == SaveOutcome::kSaved);
    CHECK(b.outcome == SaveOutcome::kSaved);
    CHECK(a.path != b.path);
    CHECK(a.path.find(L".mp4 - ") == std::wstring::npos);  // the title goes before the extension
    CHECK(a.path.find(std::wstring(64, L'x') + L".mp4") != std::wstring::npos);
    CHECK(a.path.find(std::wstring(65, L'x')) == std::wstring::npos);
    CHECK(ProbeVideoFrames(a.path).count == 180);
    CHECK(ProbeVideoFrames(b.path).count == 180);
}

TEST_CASE("clip: on engine exit every monitor's clip ends, is written and reported before the wait returns") {
    ClipRig first(L"exit-a"), second(L"exit-b");
    for (ClipRig* rig : {&first, &second}) {
        rig->ctx.out_path      = L"out\\ombra.mp4";  // templated, under the test's own root
        rig->ctx.output_root   = NarrowUtf8(TestOutputPath(L"clip\\exit"));
        rig->ctx.monitor_label = rig == &first ? L"A" : L"B";
        rig->EndNow(3.0);
        rig->FeedTo(180);
    }
    std::atomic<int> reported{0};
    ClipRequest req;
    req.event_qpc      = QpcNow();
    req.before_seconds = 2.0;
    req.after_seconds  = 30.0;
    auto group = StartClip({&first.ctx, &second.ctx}, req, [&](const ReplaySaveResults&) { ++reported; });
    REQUIRE(group);
    CHECK(ClipsOpen(&first.ctx));
    StopClipsAndWait(&first.ctx);
    StopClipsAndWait(&second.ctx);

    CHECK(group->Done().wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    CHECK(reported.load() == 1);
    CHECK_FALSE(ClipsOpen(&first.ctx));
    CHECK_FALSE(ClipsOpen(&second.ctx));
    CHECK_FALSE(group->Extend(QpcNow() + QpcFrequency()));
    const ReplaySaveResults results = group->Done().get();
    CHECK(results.outcome == SaveOutcome::kSaved);
    REQUIRE(results.monitors.size() == 2);
    for (const auto& m : results.monitors) {
        CHECK(m.outcome == SaveOutcome::kSaved);
        CHECK(ProbeVideoFrames(m.path).count >= 120);
        CHECK(DecodeErrors(m.path).empty());
    }
    CHECK(results.monitors[0].path != results.monitors[1].path);
}

TEST_CASE("clip: with no footage in reach a clip doesn't open") {
    ClipRig rig(L"empty");
    SaveOutcome why = SaveOutcome::kSaved;
    CHECK_FALSE(rig.Open(3.0, 2.0, 2.0, &why));
    CHECK(why == SaveOutcome::kNothingNew);
    CHECK_FALSE(ClipsOpen(&rig.ctx));
}

}  // namespace ombra::test
