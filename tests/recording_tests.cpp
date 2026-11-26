// A recording (recording.h) on a synthetic timeline: a real H.264 stream through the engine's own
// Ring/AudioTrackHub and the fragmented writer, a clock that moves only when the test feeds footage,
// ffprobe/ffmpeg to judge the files.

#include <doctest/doctest.h>

#include "support/audio_analysis.h"
#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/audio_tracks.h"
#include "ombra/clip.h"
#include "ombra/clip_file.h"
#include "ombra/config.h"
#include "ombra/controller.h"
#include "ombra/events.h"
#include "ombra/json.h"
#include "ombra/marker.h"
#include "ombra/open_recording.h"
#include "ombra/reconfig.h"
#include "ombra/recording.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/supervisor.h"
#include "ombra/util.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
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
    // Puts the timeline on the real clock, `seconds_ago` of footage ending now (StartRecording's
    // threads read QpcNow). Call before feeding.
    void EndNow(double seconds_ago) { origin_ = QpcNow() - static_cast<int64_t>(seconds_ago * freq_); }
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
                               rec.StopAt(At(now_s));
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

TEST_CASE("recording: each open part has a crash record with its file's id and the markers on the disk") {
    MarkerTimeline timeline;
    RecordRig rig(L"crash_record");
    rig.ctx.markers          = &timeline;
    rig.ctx.open_records_dir = TestOutputPath(L"recording\\open_records");
    for (const LeftRecording& l : ListLeftRecordings(rig.ctx.open_records_dir)) DeleteFileW(l.record.c_str());
    rig.FeedTo(180);
    rig.now_s = 3.0;
    RecordRequest req;
    req.start_qpc      = rig.At(3.0);
    req.before_seconds = 2.0;
    req.app            = "Tool";
    req.title          = L"Run";
    auto rec = RecordingSession::Open(&rig.ctx, req, nullptr, nullptr);
    REQUIRE(rec);
    bool                       placed = false, cut = false;
    std::vector<LeftRecording> before_marker, after_marker, in_part_2;
    const auto r = rig.Run(*rec, 11.0, {}, [&](double now) {
        if (!placed && now >= 4.0) {
            before_marker = ListLeftRecordings(rig.ctx.open_records_dir);
            Marker m;
            m.qpc   = rig.At(4.0);
            m.label = "During";
            REQUIRE(timeline.Add(m, "command line", rig.At(0.0)) == MarkerTimeline::AddResult::kAdded);
            placed = true;
        }
        if (after_marker.empty() && now >= 7.0) after_marker = ListLeftRecordings(rig.ctx.open_records_dir);
        if (!cut && now >= 8.0) {
            rig.ring.Barrier(rig.Tick(rig.fed), FixtureParams());
            cut = true;
        }
        if (in_part_2.empty() && now >= 10.0) in_part_2 = ListLeftRecordings(rig.ctx.open_records_dir);
    });
    REQUIRE(r.paths.size() == 2);

    REQUIRE(before_marker.size() == 1);
    CHECK(before_marker[0].info.path == rig.ctx.out_path);
    CHECK(before_marker[0].info.app == "Tool");
    CHECK(before_marker[0].info.title == "Run");
    CHECK(before_marker[0].info.file_id == FileIdAt(r.paths[0]));  // part 1, since moved
    REQUIRE(after_marker.size() == 1);
    std::vector<std::string> titles;
    for (const MuxChapter& c : after_marker[0].info.markers.chapters) titles.push_back(c.title);
    CHECK(std::find(titles.begin(), titles.end(), "During") != titles.end());
    REQUIRE(in_part_2.size() == 1);  // part 1 finished: its record went
    CHECK(in_part_2[0].info.path == r.paths[1]);
    CHECK(in_part_2[0].info.file_id == FileIdAt(r.paths[1]));
    CHECK(ListLeftRecordings(rig.ctx.open_records_dir).empty());
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

TEST_CASE("replay reach: a recording's footage with the switch off is its own; switching on floors saves") {
    RecordRig rig(L"reach");
    rig.ctx.output_root = NarrowUtf8(DirOf(rig.ctx.out_path));
    const int64_t long_age = RingMaxAgeQpc(rig.ctx.buffer_seconds);
    rig.ring.SetMaxAge(long_age);
    rig.FeedTo(300);  // 0..5 s with the switch on
    std::vector<SaveContext*> ctxs{&rig.ctx};
    ReplayReachState state;

    // A clip open when the switch goes off ends there, and with a recording the ring keeps little.
    ClipRequest clip;
    clip.event_qpc     = rig.At(4);
    clip.after_seconds = 20;
    SaveOutcome why = SaveOutcome::kFailed;
    std::shared_ptr<ClipSession> open;
    {
        StdoutSilencer quiet;
        open = ClipSession::Open(&rig.ctx, clip, nullptr, &why);
    }
    REQUIRE(open);
    ApplyReplayReach(ctxs, state, false, true, rig.At(5));
    CHECK_FALSE(open->Extend(rig.At(10)));  // stopped
    open.reset();
    clip.event_qpc = rig.At(5.5);
    {
        StdoutSilencer quiet;
        CHECK_FALSE(ClipSession::Open(&rig.ctx, clip, nullptr, &why));  // nor does one open meanwhile
    }
    CHECK(why == SaveOutcome::kNothingNew);
    CHECK(rig.ring.MaxAge() == RingMaxAgeQpc(kRecordOnlyRingSeconds));
    CHECK(rig.ctx.replay_off_qpc.load() == rig.At(5));
    // The recording stops with the switch still off: the ceiling stays where the switch went off.
    rig.FeedTo(600);  // 5..10 s, the recording's
    ApplyReplayReach(ctxs, state, false, false, rig.At(10));
    CHECK(rig.ctx.replay_off_qpc.load() == rig.At(5));
    CHECK(rig.ring.MaxAge() == long_age);

    // On again: saves start after the recording's footage, not buffer.seconds back.
    ApplyReplayReach(ctxs, state, true, false, rig.At(10));
    CHECK(rig.ctx.replay_off_qpc.load() == 0);
    CHECK(rig.ctx.replay_floor_qpc.load() == rig.At(10));
    rig.FeedTo(900);  // 10..15 s
    std::wstring path;
    double seconds = 0;
    {
        StdoutSilencer quiet;
        rig.ctx.save_in_flight.store(true);
        REQUIRE(DoSave(&rig.ctx, rig.At(15), nullptr, &path, nullptr, &seconds) == SaveOutcome::kSaved);
    }
    CHECK(seconds <= 5.0 + 1e-6);
    CHECK(seconds >= 4.0);  // from the first keyframe at or after the floor
    DeleteFileW(path.c_str());

    // Off and on with no recording between moves no floor.
    ReplayReachState quiet_state;
    ApplyReplayReach(ctxs, quiet_state, false, false, rig.At(16));
    ApplyReplayReach(ctxs, quiet_state, true, false, rig.At(17));
    CHECK(rig.ctx.replay_floor_qpc.load() == rig.At(10));
}

TEST_CASE("recording group: every monitor in one folder; the hold goes before the last session closes") {
    EventHub events;  // before the rigs: their recordings publish into it
    Subscription heard(events, false), own(events, true);
    heard.Set({"recording_started", "recording_stopped"});
    own.Set({"recording_stopped"});
    RecordRig first(L"group-a"), second(L"group-b");
    for (RecordRig* rig : {&first, &second}) {
        rig->ctx.events        = &events;
        rig->ctx.out_path      = L"out\\ombra.mp4";  // templated, under the test's own root
        rig->ctx.output_root   = NarrowUtf8(TestOutputPath(L"recording\\group"));
        rig->ctx.monitor_label = rig == &first ? L"A" : L"B";
        rig->EndNow(3.0);
        rig->FeedTo(180);
    }
    // The hold's drop sees whether a session is still listed: the engine's exit wait must not pass
    // while a hold is out.
    std::atomic<bool> dropped{false}, listed_at_drop{false};
    std::shared_ptr<void> hold(new int(0), [&](int* p) {
        listed_at_drop = Listed(first.ctx) || Listed(second.ctx);
        dropped        = true;
        delete p;
    });
    std::atomic<int> started{0}, ended{0};
    RecordFeedback feedback;
    feedback.started = [&] { ++started; };
    feedback.ended   = [&](const RecordingResults&) { ++ended; };
    RecordRequest req;
    req.start_qpc      = QpcNow();
    req.before_seconds = 2.0;
    req.app            = "tool";
    req.id             = 7;
    auto group = StartRecording({&first.ctx, &second.ctx}, req, std::move(hold), feedback);
    REQUIRE(group);
    REQUIRE(group->Started().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(group->Started().get());
    CHECK_FALSE(dropped.load());
    CHECK(ClipsOpen(&first.ctx));

    group->Stop();
    StopClipsAndWait(&first.ctx);
    StopClipsAndWait(&second.ctx);
    CHECK(dropped.load());
    CHECK(listed_at_drop.load());
    CHECK(group->Done().wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    CHECK(started.load() == 1);
    CHECK(ended.load() == 1);
    const RecordingResults r = group->Done().get();
    CHECK(r.outcome == SaveOutcome::kSaved);
    CHECK(r.end == RecordEnd::kStopped);
    REQUIRE(r.monitors.size() == 2);
    for (const auto& m : r.monitors) {
        REQUIRE(m.paths.size() == 1);
        CHECK(ProbeVideoFrames(m.paths[0]).count >= 110);
        CHECK(DecodeErrors(m.paths[0]).empty());
    }
    CHECK(r.monitors[0].paths[0] != r.monitors[1].paths[0]);
    CHECK(DirOf(r.monitors[0].paths[0]) == DirOf(r.monitors[1].paths[0]));  // the group folder

    // Started and stopped name the app and the id; only Ombra's own client hears the paths.
    std::vector<std::string> lines, own_lines;
    REQUIRE(heard.Take(lines));
    REQUIRE(own.Take(own_lines));
    REQUIRE(lines.size() == 2);
    const JsonValue start = ParseJson(lines[0]), stop = ParseJson(lines[1]), mine = ParseJson(own_lines.at(0));
    CHECK(start.Find("event")->str == "recording_started");
    CHECK(start.Find("app")->str == "tool");
    CHECK(start.Find("recording")->num == 7);
    CHECK(stop.Find("event")->str == "recording_stopped");
    CHECK(stop.Find("end")->str == "stopped");
    CHECK(stop.Find("reason")->str == "saved");
    CHECK(stop.Find("seconds")->num >= 1.5);
    CHECK_FALSE(stop.Find("paths"));
    REQUIRE(mine.Find("paths"));
    CHECK(mine.Find("paths")->arr.size() == 2);
}

TEST_CASE("recording group: no monitor opens: it reports at once and drops the hold") {
    RecordRig rig(L"group-refused");
    {
        std::lock_guard<std::mutex> lock(rig.ctx.clip_mutex);
        rig.ctx.clips_blocked = true;
    }
    bool dropped = false;
    std::shared_ptr<void> hold(new int(0), [&](int* p) { dropped = true; delete p; });
    RecordRequest req;
    req.start_qpc = QpcNow();
    auto group = StartRecording({&rig.ctx}, req, std::move(hold));
    CHECK(dropped);
    REQUIRE(group->Started().wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    CHECK_FALSE(group->Started().get());
    REQUIRE(group->Done().wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const RecordingResults r = group->Done().get();
    CHECK(r.outcome == SaveOutcome::kFailed);
    CHECK(r.monitors.at(0).outcome == SaveOutcome::kBusy);
}

TEST_CASE("RecordEndToast: an early end outranks the outcome; a monitor that wrote nothing it should have fails it") {
    RecordingResults r;
    r.outcome = SaveOutcome::kSaved;
    CHECK(std::wstring(RecordEndToast(r)) == L"record_saved");
    r.end = RecordEnd::kDiskLow;
    CHECK(std::wstring(RecordEndToast(r)) == L"record_disk_low");
    r.outcome = SaveOutcome::kFailed;
    r.end     = RecordEnd::kWriteFailed;
    CHECK(std::wstring(RecordEndToast(r)) == L"record_write_failed");
    r.end = RecordEnd::kNoFootage;
    CHECK(std::wstring(RecordEndToast(r)) == L"record_nothing");
    r.end = RecordEnd::kStopped;
    CHECK(std::wstring(RecordEndToast(r)) == L"record_failed");
    r.outcome = SaveOutcome::kNothingNew;
    CHECK(std::wstring(RecordEndToast(r)) == L"record_nothing");
}

TEST_CASE("recordings: one per app; a second start is refused before it arms; only its app stops it") {
    RecordRig rig(L"registry");
    rig.ctx.out_path    = L"out\\ombra.mp4";
    rig.ctx.output_root = NarrowUtf8(TestOutputPath(L"recording\\registry"));
    rig.EndNow(3.0);
    rig.FeedTo(180);
    Recordings recordings;
    int arms = 0;
    const auto arm = [&](RecordRequest& r) -> std::shared_ptr<void> {
        ++arms;
        r.floor_qpc = r.start_qpc;  // as with the switch off: nothing from before the start
        return std::make_shared<int>(0);
    };
    RecordRequest req;
    req.start_qpc      = QpcNow();
    req.before_seconds = 2.0;
    const auto a = recordings.Start("tool", {&rig.ctx}, req, arm);
    REQUIRE(a.refusal == Recordings::Refusal::kNone);
    REQUIRE(a.group);
    const auto again = recordings.Start("tool", {&rig.ctx}, req, arm);
    CHECK(again.refusal == Recordings::Refusal::kAlreadyRecording);
    CHECK(again.id == a.id);
    CHECK(arms == 1);  // refused before it touched the controller
    const auto other = recordings.Start("other", {&rig.ctx}, req, arm);
    REQUIRE(other.refusal == Recordings::Refusal::kNone);
    CHECK(other.id != a.id);
    CHECK(recordings.List().size() == 2);

    CHECK_FALSE(recordings.Stop("other", a.id));  // not its own
    const auto stopped = recordings.Stop("tool", a.id);
    REQUIRE(stopped);
    REQUIRE(stopped->Done().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK_FALSE(recordings.Stop("tool", a.id));  // forgotten once stopped
    REQUIRE(recordings.List().size() == 1);
    CHECK(recordings.List()[0].app == "other");

    // The floor held it to footage from its start: nothing from the 3 s already in the ring.
    const RecordingResults ra = stopped->Done().get();
    CHECK(ra.outcome == SaveOutcome::kNothingNew);
    CHECK(ra.monitors.at(0).paths.empty());

    recordings.Close();
    CHECK(recordings.Start("tool", {&rig.ctx}, req, arm).refusal == Recordings::Refusal::kClosed);
    StopClipsAndWait(&rig.ctx);
    CHECK(other.group->Done().wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    CHECK(arms == 2);
}

TEST_CASE("recordings: an ended recording is stopped by id 0 and replaced by the app's next start") {
    RecordRig rig(L"registry-ended");
    rig.ctx.out_path    = L"out\\ombra.mp4";
    rig.ctx.output_root = NarrowUtf8(TestOutputPath(L"recording\\registry-ended"));
    rig.EndNow(3.0);
    rig.FeedTo(180);
    Recordings recordings;
    RecordRequest req;
    req.start_qpc      = QpcNow();
    req.before_seconds = 2.0;
    const auto first = recordings.Start("tool", {&rig.ctx}, req, {});
    REQUIRE(first.group);
    REQUIRE(first.group->Started().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    first.group->Stop();  // as a full disk would end it: the registry still holds its result
    REQUIRE(first.group->Done().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(recordings.List().empty());
    const auto next = recordings.Start("tool", {&rig.ctx}, req, {});
    REQUIRE(next.refusal == Recordings::Refusal::kNone);
    CHECK(next.id != first.id);
    CHECK_FALSE(recordings.Stop("tool", first.id));  // replaced
    const auto latest = recordings.Stop("tool", 0);
    REQUIRE(latest);
    REQUIRE(latest->Done().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(latest->Done().get().outcome == SaveOutcome::kSaved);
    CHECK(latest->Done().get().monitors.at(0).paths.at(0) != first.group->Done().get().monitors.at(0).paths.at(0));
    recordings.Close();
    StopClipsAndWait(&rig.ctx);
}

}  // namespace ombra::test
