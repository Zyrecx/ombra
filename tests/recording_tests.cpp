// A recording (recording.h) on a synthetic timeline: a real H.264 stream through the engine's own
// Ring/AudioTrackHub and the fragmented writer, a clock that moves only when the test feeds footage,
// ffprobe/ffmpeg to judge the files.

#include <doctest/doctest.h>

#include "support/audio_analysis.h"
#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/audio_tracks.h"
#include "ombra/clip_file.h"
#include "ombra/config.h"
#include "ombra/controller.h"
#include "ombra/json.h"
#include "ombra/marker.h"
#include "ombra/reconfig.h"
#include "ombra/recording.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/supervisor.h"
#include "ombra/util.h"

#include <windows.h>

#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace ombra::test {

namespace {

constexpr uint32_t kRate         = 48000;
constexpr size_t   kPacketFrames = 480;

// Removes a test's earlier output: the file, and the parts' folder a cut made beside it.
void RemoveRecording(const std::wstring& path) {
    DeleteFileW(path.c_str());
    const std::wstring folder = path.substr(0, path.size() - 4);
    WIN32_FIND_DATAW found{};
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((folder + L"\\" + found.cFileName).c_str());
        } while (FindNextFileW(h, &found));
        FindClose(h);
    }
    RemoveDirectoryW(folder.c_str());
}

std::shared_ptr<const SegmentParams> FixtureParams() {
    const H264Fixture& fx = StandardFixture();
    auto params = std::make_shared<SegmentParams>();
    params->codec_header = fx.sps_pps;
    params->width        = fx.width;
    params->height       = fx.height;
    return params;
}

class RecordRig {
public:
    explicit RecordRig(const std::wstring& name, size_t capacity = 32u << 20, size_t overshoot = 8u << 20)
        : freq_(QpcFrequency()), origin_(QpcFrequency() * 1000) {
        const H264Fixture& fx = StandardFixture();
        ring.Init(capacity, overshoot);
        ring.SetBaseSegmentParams(FixtureParams());
        ctx.ring           = &ring;
        ctx.fps            = fx.fps;
        ctx.buffer_seconds = 10.0;
        ctx.out_path       = TestOutputPath(L"recording\\" + name + L".mp4");
        RemoveRecording(ctx.out_path);
    }

    int64_t At(double seconds) const { return origin_ + static_cast<int64_t>(std::llround(seconds * freq_)); }
    int64_t Tick(uint64_t t) const { return origin_ + static_cast<int64_t>(t) * freq_ / StandardFixture().fps; }

    void AddAudio(double audio_seconds) {
        hub = std::make_unique<AudioTrackHub>(At(0), freq_, 192, AudioTrackHub::SizeFor(audio_seconds, 192, 0));
        hub->SetSources({{"Game", "game"}}, false);
        ctx.audio = hub.get();
    }

    // Video ticks [fed, until_tick) (while `video`: else time passes with no footage, as with every
    // display off) and, with audio, a 440 Hz tone over the same time.
    void FeedTo(uint64_t until_tick) {
        const H264Fixture& fx = StandardFixture();
        for (; fed < until_tick; ++fed) {
            if (!video) continue;
            const auto& au = fx.aus[fed % fx.aus.size()];
            ring.Append(au.annexb.data(), au.annexb.size(), Tick(fed), fed, au.idr);
            ++appended;
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

    std::shared_ptr<RecordingSession> Open(double start_s, double before_s, SaveOutcome* why = nullptr) {
        RecordRequest req;
        req.start_qpc      = At(start_s);
        req.before_seconds = before_s;
        return RecordingSession::Open(&ctx, req, nullptr, why);
    }

    // Runs `rec` until `stop_s`, when it's stopped. Each wait moves the clock 0.1 s and feeds up to
    // it, video only while `video_on(now_s)` says so; `on_wait` sees the clock first.
    RecordingSession::Result Run(RecordingSession& rec, double stop_s,
                                 const std::function<bool(double now_s)>& video_on = {},
                                 const std::function<void(double now_s)>& on_wait = {}) {
        int waits = 0;
        return rec.Run([&] { return At(now_s); },
                       [&] {
                           REQUIRE(++waits < 100000);
                           if (on_wait) on_wait(now_s);
                           if (now_s >= stop_s - 1e-9 && !stopped) {
                               rec.Stop();
                               stopped = appended;
                           }
                           now_s += 0.1;
                           if (video_on) video = video_on(now_s);
                           FeedTo(static_cast<uint64_t>(std::llround(now_s * StandardFixture().fps)));
                       },
                       [&](bool ok) { started.push_back(ok); });
    }

    Ring                           ring;
    SaveContext                    ctx;
    std::unique_ptr<AudioTrackHub> hub;
    uint64_t                       fed       = 0;
    uint64_t                       audio_fed = 0;
    bool                           video     = true;
    uint64_t                       appended  = 0;  // video ticks put in the ring
    uint64_t                       stopped   = 0;  // `appended` when the recording was stopped
    double                         now_s     = 0;
    std::vector<bool>              started;

private:
    int64_t freq_;
    int64_t origin_;
};

bool Listed(SaveContext& ctx) {
    std::lock_guard<std::mutex> lock(ctx.clip_mutex);
    return !ctx.open_recordings.empty();
}

std::wstring NameOf(const std::wstring& path) { return path.substr(path.find_last_of(L'\\') + 1); }
std::wstring DirOf(const std::wstring& path) { return path.substr(0, path.find_last_of(L'\\')); }

}  // namespace

TEST_CASE("recording: from `before` until it's stopped, one plain MP4 with its audio and markers") {
    MarkerTimeline timeline;
    RecordRig rig(L"plain");
    rig.ctx.markers = &timeline;
    rig.AddAudio(10.0);
    rig.FeedTo(180);  // 0..3 s
    rig.now_s = 3.0;
    auto rec = rig.Open(3.0, 2.0);
    REQUIRE(rec);
    CHECK(Listed(rig.ctx));
    CHECK_FALSE(rig.ctx.save_in_flight.load());  // never the user's save slot
    bool placed = false;
    const auto r = rig.Run(*rec, 8.0, {}, [&](double now) {
        if (!placed && now >= 6.0) {
            Marker m;
            m.qpc   = rig.At(6.0);
            m.label = "During";
            REQUIRE(timeline.Add(m, "command line", rig.At(0.0)) == MarkerTimeline::AddResult::kAdded);
            placed = true;
        }
    });
    CHECK(rig.started == std::vector<bool>{true});
    REQUIRE(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == RecordEnd::kStopped);
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0] == rig.ctx.out_path);
    CHECK(r.seconds == doctest::Approx(7.0).epsilon(0.03));  // 1 s .. 8 s
    const VideoFrames frames = ProbeVideoFrames(r.paths[0]);
    CHECK(frames.first_is_key);
    CHECK(frames.count == static_cast<size_t>(std::llround(r.seconds * 60)));
    CHECK(DecodeErrors(r.paths[0]).empty());
    ClipInfo info;
    std::string error;
    REQUIRE_MESSAGE(ReadClipInfo(r.paths[0], info, &error), error);  // finished: the full moov
    CHECK(info.duration_s == doctest::Approx(r.seconds).epsilon(0.01));
    const Pcm pcm = DecodeAudio(r.paths[0], 0);
    CHECK(pcm.Seconds() == doctest::Approx(r.seconds).epsilon(0.02));
    CHECK(ToneAmplitude(pcm, 440.0, kRate / 2, kRate / 2) == doctest::Approx(0.5).epsilon(0.06));
    const ProbeInfo probe = Probe(r.paths[0]);
    REQUIRE_MESSAGE(probe.ok, probe.error);
    REQUIRE(probe.chapters.size() == 2);
    CHECK(probe.chapters[1].title == "During");
    CHECK(probe.chapters[1].start_s == doctest::Approx(5.0).epsilon(0.002));
    CHECK(Listed(rig.ctx));  // until its thread is done with the context
    rec->Close();
    CHECK_FALSE(Listed(rig.ctx));
}

TEST_CASE("recording: footage that stops arriving only pauses it") {
    RecordRig rig(L"pause");
    rig.FeedTo(120);
    rig.now_s = 2.0;
    auto rec = rig.Open(2.0, 1.0);
    REQUIRE(rec);
    // Footage 1..4 s, none for 30 s (longer than any clip's stall or the start timeout), then 4 s more.
    const auto r = rig.Run(*rec, 38.0, [](double now) { return now < 4.0 + 1e-9 || now > 34.0 + 1e-9; });
    CHECK(r.end == RecordEnd::kStopped);
    REQUIRE(r.paths.size() == 1);  // a pause is no cut
    CHECK(ProbeVideoFrames(r.paths[0]).count == static_cast<size_t>(rig.stopped - 60));
    CHECK(DecodeErrors(r.paths[0]).empty());
}

TEST_CASE("recording: every display off pauses it; the next part keeps the first's ending audio") {
    RecordRig rig(L"display_off");
    rig.AddAudio(0.0);  // the audio rings reach about 20 s: the pause lasts 35
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto rec = rig.Open(3.0, 2.0);
    REQUIRE(rec);
    bool resumed = false;
    const auto r = rig.Run(*rec, 45.0, [&](double now) {
        if (now < 5.0 + 1e-9) return true;
        if (now < 40.0) return false;
        if (!resumed) {
            rig.ring.Barrier(rig.Tick(rig.fed), FixtureParams());  // re-armed: a new segment
            resumed = true;
        }
        return true;
    });
    REQUIRE(resumed);
    CHECK(r.end == RecordEnd::kStopped);
    REQUIRE(r.paths.size() == 2);
    CHECK(ProbeVideoFrames(r.paths[0]).count == 240u);  // 1 s .. 5 s
    const Pcm first = DecodeAudio(r.paths[0], 0);
    CHECK(first.Seconds() == doctest::Approx(4.0).epsilon(0.02));
    CHECK(ToneAmplitude(first, 440.0, static_cast<size_t>(3.4 * kRate), kRate / 2) == doctest::Approx(0.5).epsilon(0.06));
    const VideoFrames second = ProbeVideoFrames(r.paths[1]);
    CHECK(second.first_is_key);
    CHECK(second.count > 0);
    CHECK(DecodeErrors(r.paths[1]).empty());
}

TEST_CASE("recording: an audio format change goes on in a new part at the next keyframe, losing no frame") {
    RecordRig rig(L"audio_format");
    rig.AddAudio(10.0);
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto rec = rig.Open(3.0, 2.0);
    REQUIRE(rec);
    bool changed = false;
    const auto r = rig.Run(*rec, 9.0, {}, [&](double now) {
        if (!changed && now >= 5.0) {
            rig.hub->SetBitrate(96);  // AAC-LC to HE-AAC: another stream format
            changed = true;
        }
    });
    REQUIRE(changed);
    CHECK(r.end == RecordEnd::kStopped);
    REQUIRE(r.paths.size() == 2);
    const VideoFrames a = ProbeVideoFrames(r.paths[0]);
    const VideoFrames b = ProbeVideoFrames(r.paths[1]);
    CHECK(b.first_is_key);
    CHECK(a.count + b.count == static_cast<size_t>(rig.stopped - 60));
    CHECK(DecodeErrors(r.paths[0]).empty());
    CHECK(DecodeErrors(r.paths[1]).empty());
    CHECK(Probe(r.paths[1]).Of("audio").size() == 1);
}

TEST_CASE("recording: a floor holds it to footage at or after it, whatever `before` asks") {
    RecordRig rig(L"floor");
    rig.FeedTo(180);
    rig.now_s = 3.0;
    RecordRequest req;
    req.start_qpc      = rig.At(3.0);
    req.before_seconds = 2.5;
    req.floor_qpc      = rig.At(1.5);  // replay came on at 1.5 s
    auto rec = RecordingSession::Open(&rig.ctx, req, nullptr, nullptr);
    REQUIRE(rec);
    const auto r = rig.Run(*rec, 6.0);
    REQUIRE(r.paths.size() == 1);
    const uint64_t gop   = StandardFixture().gop;
    const uint64_t first = (90 + gop - 1) / gop * gop;  // the first keyframe at or after 1.5 s
    CHECK(ProbeVideoFrames(r.paths[0]).count == static_cast<size_t>(rig.stopped - first));
}

TEST_CASE("recording: a cut goes on in part 2, both parts in a folder named after the recording") {
    RecordRig rig(L"parts");
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto rec = rig.Open(3.0, 2.0);
    REQUIRE(rec);
    uint64_t cut_at = 0;
    const auto r = rig.Run(*rec, 8.0, {}, [&](double now) {
        if (!cut_at && now >= 4.0) {
            cut_at = rig.fed;
            rig.ring.Barrier(rig.Tick(cut_at), FixtureParams());  // a resolution change at about 4 s
        }
    });
    REQUIRE(cut_at > 0);
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == RecordEnd::kStopped);
    REQUIRE(r.paths.size() == 2);
    const std::wstring stem   = NameOf(rig.ctx.out_path).substr(0, NameOf(rig.ctx.out_path).size() - 4);
    const std::wstring folder = rig.ctx.out_path.substr(0, rig.ctx.out_path.size() - 4);
    CHECK(DirOf(r.paths[0]) == folder);
    CHECK(DirOf(r.paths[1]) == folder);
    CHECK(NameOf(r.paths[0]) == stem + L" part 1.mp4");
    CHECK(NameOf(r.paths[1]) == stem + L" part 2.mp4");
    CHECK(GetFileAttributesW(rig.ctx.out_path.c_str()) == INVALID_FILE_ATTRIBUTES);  // moved, not copied
    CHECK(ProbeVideoFrames(r.paths[0]).count == static_cast<size_t>(cut_at - 60));  // 1 s .. the cut
    const VideoFrames second = ProbeVideoFrames(r.paths[1]);
    CHECK(second.first_is_key);
    CHECK(second.count > 0);
    CHECK(second.count <= static_cast<size_t>(rig.fed - cut_at));
    CHECK(DecodeErrors(r.paths[1]).empty());
    const double total = static_cast<double>(ProbeVideoFrames(r.paths[0]).count + second.count) / 60.0;
    CHECK(r.seconds == doctest::Approx(total));
}

TEST_CASE("recording: longer than the ring and the audio rings reach, it keeps every frame and its opening audio") {
    RecordRig rig(L"long", 2u << 20, 1u << 20);
    rig.AddAudio(0.0);  // the track rings hold about 20 s at 192 kbps: the recording runs 60 s
    rig.FeedTo(120);
    rig.now_s = 2.0;
    auto rec = rig.Open(2.0, 1.0);
    REQUIRE(rec);
    const auto r = rig.Run(*rec, 61.0);
    REQUIRE(r.outcome == SaveOutcome::kSaved);
    REQUIRE(r.paths.size() == 1);
    CHECK(r.seconds == doctest::Approx(60.0).epsilon(0.01));
    CHECK(ProbeVideoFrames(r.paths[0]).count == static_cast<size_t>(std::llround(r.seconds * 60)));
    CHECK(rig.ring.LiveBytes() <= (2u << 20) + (1u << 20));
    const Pcm pcm = DecodeAudio(r.paths[0], 0);
    CHECK(pcm.Seconds() == doctest::Approx(r.seconds).epsilon(0.01));
    CHECK(ToneAmplitude(pcm, 440.0, 0, kRate / 2) == doctest::Approx(0.5).epsilon(0.06));
    CHECK(ToneAmplitude(pcm, 440.0, 59 * kRate, kRate / 2) == doctest::Approx(0.5).epsilon(0.06));
}

TEST_CASE("recording: with no footage within the start time it gives up and writes nothing") {
    RecordRig rig(L"nothing");
    auto rec = rig.Open(0.0, 2.0);
    REQUIRE(rec);
    const auto r = rig.Run(*rec, 1000.0, [](double) { return false; });
    CHECK(r.end == RecordEnd::kNoFootage);
    CHECK(r.outcome == SaveOutcome::kNothingNew);
    CHECK(r.paths.empty());
    CHECK(rig.now_s == doctest::Approx(kRecordStartSeconds).epsilon(0.02));
    CHECK(rig.started == std::vector<bool>{false});
    CHECK(GetFileAttributesW(rig.ctx.out_path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("recording: a buffer reallocation is refused until it stops, and one in progress refuses it") {
    RecordRig rig(L"realloc");
    PipelineSupervisor sup(60, 10000, 2, &rig.ring);
    sup.SetArmed(false);  // no generation to build: nothing touches the GPU
    BufferController controller(&sup);
    rig.FeedTo(180);
    rig.now_s = 3.0;
    auto rec = rig.Open(3.0, 1.0);
    REQUIRE(rec);

    Config cfg{};
    cfg.buffer_seconds = 10.0;
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("buffer.seconds", JsonValue::MakeNum(30));
    const size_t capacity = rig.ring.Capacity();
    const LiveApplyResult refused = ApplyLiveConfig(cfg, patch, sup, controller, rig.ctx, rig.ring);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error == "stop the recording first");
    CHECK(rig.ring.Capacity() == capacity);
    CHECK_FALSE(rig.ctx.save_in_flight.load());

    CHECK(rig.Run(*rec, 5.0).outcome == SaveOutcome::kSaved);
    rec.reset();
    CHECK(ApplyLiveConfig(cfg, patch, sup, controller, rig.ctx, rig.ring).ok);

    {
        std::lock_guard<std::mutex> lock(rig.ctx.clip_mutex);
        rig.ctx.clips_blocked = true;
    }
    SaveOutcome why = SaveOutcome::kSaved;
    CHECK_FALSE(rig.Open(3.0, 1.0, &why));
    CHECK(why == SaveOutcome::kBusy);
    CHECK_FALSE(Listed(rig.ctx));
}

}  // namespace ombra::test
