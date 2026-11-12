// The save path end to end without a GPU: a real H.264 stream and synthetic tones go through the
// engine's own Ring/AudioTrackHub/DoSave, and ffprobe/ffmpeg judge the file.
//
// Not covered here: anything about the encoder's SPS (VUI, profile), which needs real NVENC output.

#include <doctest/doctest.h>

#include "support/audio_analysis.h"
#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/process.h"
#include "support/test_env.h"

#include "ombra/audio_tracks.h"
#include "ombra/reconfig.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/save_toast.h"
#include "ombra/telemetry.h"
#include "ombra/util.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

namespace {

constexpr uint32_t kRate          = 48000;
constexpr size_t   kPacketFrames  = 480;  // 10 ms, WASAPI's usual loopback period
constexpr double   kToneTolerance = 0.06;

// One engine timeline on the real QPC scale: video tick N and audio frame K both map to QPC from
// the same arbitrary origin, the way the pacer and WASAPI stamp the live rings.
class Timeline {
public:
    Timeline() : freq_(QpcFrequency()), origin_(QpcFrequency() * 1000) {}
    int64_t At(double seconds) const { return origin_ + static_cast<int64_t>(std::llround(seconds * freq_)); }
    int64_t Tick(uint64_t index, uint32_t fps) const {
        return origin_ + static_cast<int64_t>(index) * freq_ / fps;
    }
    int64_t Frame(uint64_t frame) const { return origin_ + static_cast<int64_t>(frame) * freq_ / kRate; }

private:
    int64_t freq_;
    int64_t origin_;
};

struct Tone {
    double freq_hz   = 440.0;
    double amplitude = 0.5;  // of full scale
};

class SaveRig {
public:
    explicit SaveRig(const std::wstring& name, size_t video_capacity = 32u << 20) {
        const H264Fixture& fx = StandardFixture();
        ring.Init(video_capacity, 8u << 20);
        auto params     = std::make_shared<SegmentParams>();
        params->codec_header = fx.sps_pps;
        params->width   = fx.width;
        params->height  = fx.height;
        ring.SetBaseSegmentParams(params);

        ctx.ring                = &ring;
        ctx.fps                 = fx.fps;
        ctx.buffer_seconds      = 3.0;
        ctx.out_path            = TestOutputPath(L"save\\" + name + L".mp4");
        ctx.save_overlap_policy = "overlap";
    }

    // Sources are registered before any audio is fed; the hub is built on first use. Never
    // Start()ed: its background thread runs on real QPC, which would see this synthetic timeline's
    // sources as idle and skip their audio.
    size_t AddSource(const std::string& name, const std::string& group_id, double gain_db = 0.0) {
        REQUIRE(!hub_);
        sources_.push_back({name, group_id, gain_db});
        return sources_.size() - 1;
    }

    AudioTrackHub& Hub() {
        if (!hub_) {
            hub_ = std::make_unique<AudioTrackHub>(timeline.At(0), QpcFrequency(), 192,
                                                   AudioTrackHub::SizeFor(70.0, 192, 0));
            hub_->SetSources(sources_, full_mix);
            ctx.audio = hub_.get();
        }
        return *hub_;
    }

    // Appends ticks [first, first + count), looping the fixture (it's a whole number of GOPs).
    void FeedVideo(uint64_t first, uint64_t count) {
        const H264Fixture& fx = StandardFixture();
        for (uint64_t t = first; t < first + count; ++t) {
            const auto& au = fx.aus[t % fx.aus.size()];
            ring.Append(au.annexb.data(), au.annexb.size(), timeline.Tick(t, fx.fps), t, au.idr);
        }
    }

    // Appends 10 ms packets covering [from_s, to_s). Phase follows the absolute frame index, so
    // separate calls join without a click.
    void FeedTone(size_t source, double from_s, double to_s, Tone tone) {
        const uint64_t first = static_cast<uint64_t>(std::llround(from_s * kRate));
        const uint64_t last  = static_cast<uint64_t>(std::llround(to_s * kRate));
        std::vector<int16_t> packet(kPacketFrames * 2);
        for (uint64_t f = first; f < last; f += kPacketFrames) {
            for (size_t i = 0; i < kPacketFrames; ++i) {
                const double phase = 2.0 * std::numbers::pi * tone.freq_hz * double(f + i) / kRate;
                const auto   s     = static_cast<int16_t>(std::lround(tone.amplitude * 32767.0 * std::sin(phase)));
                packet[i * 2]     = s;
                packet[i * 2 + 1] = s;
            }
            Hub().Append(source, packet.data(), packet.size(), timeline.Frame(f));
        }
    }

    SaveOutcome SaveAt(double t_end_s, const std::wstring* group_folder = nullptr) {
        if (!sources_.empty()) Hub();
        ctx.save_in_flight.store(true);
        StdoutSilencer quiet;
        return DoSave(&ctx, timeline.At(t_end_s), group_folder);
    }

    bool Save(double t_end_s, const std::wstring* group_folder = nullptr) {
        return SaveAt(t_end_s, group_folder) == SaveOutcome::kSaved;
    }

    std::wstring SavedPath() {
        std::lock_guard<std::mutex> lock(ctx.last_save_mutex);
        return ctx.last_save_path;
    }

    Timeline    timeline;
    Ring        ring;
    SaveContext ctx;
    bool        full_mix = false;  // audio.full_mix_track; set before feeding

private:
    std::vector<AudioTrackSource>  sources_;
    std::unique_ptr<AudioTrackHub> hub_;
};

size_t FramesAt(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

// Amplitude of `freq_hz` over a 0.5 s window starting at `from_s` into the decoded track.
double ToneAt(const Pcm& pcm, double freq_hz, double from_s) {
    return ToneAmplitude(pcm, freq_hz, FramesAt(from_s), FramesAt(0.5));
}

double FileSizeMb(const std::wstring& file) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info)) return 0.0;
    return static_cast<double>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) / (1024.0 * 1024.0);
}

void CheckDecodesCleanly(const std::wstring& file) {
    const std::string errors = DecodeErrors(file);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);
}

}  // namespace

TEST_CASE("save: replay fans out with one timestamp, grouped files and one completion") {
    bool fail_second = false;
    SUBCASE("both monitors save") {}
    SUBCASE("one monitor fails") { fail_second = true; }
    SaveRig first(L"replay-first"), second(L"replay-second");
    for (auto* rig : {&first, &second}) {
        rig->ctx.out_path = L"out\\ombra.mp4";
        rig->ctx.output_root = NarrowUtf8(TestOutputPath(L"save\\replay-group"));
        const auto& fx = StandardFixture();
        const int64_t start = QpcNow() - 2 * QpcFrequency();
        for (uint64_t t = 0; t < 2 * fx.fps; ++t) {
            const auto& au = fx.aus[t % fx.aus.size()];
            rig->ring.Append(au.annexb.data(), au.annexb.size(), start + t * QpcFrequency() / fx.fps, t, au.idr);
        }
    }
    first.ctx.monitor_label = L"first";
    second.ctx.monitor_label = L"second";
    if (fail_second) {
        const auto blocker = TestOutputPath(L"replay-blocker");
        std::ofstream file(blocker);
        file << "this is a file, not a directory";
        file.close();
        second.ctx.out_path = blocker + L"\\clip.mp4";
    }
    int starts = 0, finishes = 0;
    SaveOutcome feedback = SaveOutcome::kFailed;
    const auto result = SaveReplay({&first.ctx, &second.ctx}, [&]() -> SaveFeedback {
        ++starts;
        // Claims must precede feedback and every worker.
        CHECK(first.ctx.save_in_flight.load());
        CHECK(second.ctx.save_in_flight.load());
        return [&](const ReplaySaveResults& r) { ++finishes; feedback = r.outcome; };
    }).get();
    CHECK(starts == 1);
    CHECK(finishes == 1);
    CHECK(feedback == (fail_second ? SaveOutcome::kFailed : SaveOutcome::kSaved));
    REQUIRE(result.outcome == feedback);
    REQUIRE(result.monitors.size() == 2);
    CHECK(result.monitors[0].outcome == SaveOutcome::kSaved);
    CHECK(result.monitors[0].seconds == doctest::Approx(2.0));  // the clip, not the 3 s buffer
    const auto& a = result.monitors[0].path;
    const auto& b = result.monitors[1].path;
    CheckDecodesCleanly(a);
    if (fail_second) {
        CHECK(result.monitors[1].outcome == SaveOutcome::kFailed);
        CHECK(b.empty());
    } else {
        CHECK(first.ctx.last_save_qpc == second.ctx.last_save_qpc);
        CHECK(a.substr(0, a.find_last_of(L'\\')) == b.substr(0, b.find_last_of(L'\\')));
        CHECK(a != b);
        CheckDecodesCleanly(b);
    }
    CHECK_FALSE(first.ctx.save_in_flight.load());
    CHECK_FALSE(second.ctx.save_in_flight.load());
}

TEST_CASE("save: the toast's title names the clip's real length") {
    CHECK(SavedToastTitle({1, 29.98f}) == L"Saved last 30 s");
    CHECK(SavedToastTitle({1, 4.2f}) == L"Saved last 4 s");
    CHECK(SavedToastTitle({1, 0.6f}) == L"Saved last 1 s");
    CHECK(SavedToastTitle({1, 120.0f}) == L"Saved last 2 min");
    CHECK(SavedToastTitle({1, 90.0f}) == L"Saved last 1 min 30 s");
    CHECK(SavedToastTitle({2, 30.0f}) == L"Saved 2 clips, last 30 s");
    CHECK(SavedToastTitle({1, 0.0f}) == L"Clip saved");  // length unknown
}

TEST_CASE("save: replay skips busy monitors and reports incomplete instead of success") {
    SaveRig first(L"busy-first"), second(L"busy-second");
    first.ctx.save_in_flight = true;
    int finishes = 0;
    SaveOutcome feedback = SaveOutcome::kSaved;
    const auto result = SaveReplay({&first.ctx, &second.ctx}, [&]() -> SaveFeedback {
        return [&](const ReplaySaveResults& r) { ++finishes; feedback = r.outcome; };
    }).get();
    CHECK(result.outcome == SaveOutcome::kFailed);
    CHECK(result.monitors[0].outcome == SaveOutcome::kBusy);
    CHECK(result.monitors[1].outcome == SaveOutcome::kNothingNew);
    CHECK(finishes == 1);
    CHECK(feedback == SaveOutcome::kFailed);
    CHECK(first.ctx.save_in_flight.load());
    CHECK_FALSE(second.ctx.save_in_flight.load());
    CHECK(SaveReplay({&first.ctx}).get().outcome == SaveOutcome::kBusy);
    first.ctx.save_in_flight = false;
}

TEST_CASE("save: one H.264 track plus one named AAC track per audio source") {
    SaveRig rig(L"tracks");
    const size_t desktop = rig.AddSource("Desktop", "desktop");
    const size_t discord = rig.AddSource("Discord", "discord");
    const size_t mic = rig.AddSource("Mic", "mic");
    rig.FeedVideo(0, 240);  // 0..4 s
    rig.FeedTone(desktop, 0.0, 4.0, {440.0, 0.5});
    rig.FeedTone(discord, 0.0, 4.0, {1000.0, 0.5});
    rig.FeedTone(mic, 0.0, 4.0, {2500.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();

    const ProbeInfo probe = Probe(file);
    REQUIRE_MESSAGE(probe.ok, probe.error);
    const auto video = probe.Of("video");
    const auto audio = probe.Of("audio");
    REQUIRE(video.size() == 1);
    CHECK(video[0].codec_name == "h264");
    CHECK(video[0].width == 320);
    CHECK(video[0].height == 180);
    REQUIRE(audio.size() == 3);
    const char* names[] = {"Desktop", "Discord", "Mic"};
    for (size_t i = 0; i < audio.size(); ++i) {
        CAPTURE(i);
        CHECK(audio[i].codec_name == "aac");
        CHECK(audio[i].sample_rate == 48000);
        CHECK(audio[i].channels == 2);
        CHECK(audio[i].title == names[i]);
    }
    CheckDecodesCleanly(file);

    // Window is [1 s, 4 s): ticks 60..239, starting on the IDR at tick 60.
    const VideoFrames frames = ProbeVideoFrames(file);
    CHECK(frames.count == 180);
    CHECK(frames.first_is_key);

    const double tones[] = {440.0, 1000.0, 2500.0};
    for (int track = 0; track < 3; ++track) {
        CAPTURE(track);
        const Pcm pcm = DecodeAudio(file, track);
        CHECK(pcm.Seconds() == doctest::Approx(3.0).epsilon(0.02));
        for (int tone = 0; tone < 3; ++tone) {
            CAPTURE(tone);
            const double amp = ToneAt(pcm, tones[tone], 1.25);
            if (tone == track) CHECK(amp == doctest::Approx(0.5).epsilon(kToneTolerance));
            else CHECK(amp < 0.02);
        }
    }
}

TEST_CASE("save: an overloaded encoder's clip passes the minimum by its length, not its sample count") {
    // 40 samples, one every 3 ticks at 60 fps: 2 s of footage, under one second's worth of samples.
    SaveRig rig(L"sparse_ticks");
    const H264Fixture& fx = StandardFixture();
    for (uint64_t i = 0; i < 40; ++i) {
        const auto& au = fx.aus[i];
        rig.ring.Append(au.annexb.data(), au.annexb.size(), rig.timeline.Tick(i * 3, fx.fps), i * 3, au.idr);
    }
    CHECK(rig.SaveAt(2.5) == SaveOutcome::kSaved);
}

TEST_CASE("save: audio is placed by QPC across a capture gap and a late start") {
    SaveRig rig(L"placement");
    const size_t game = rig.AddSource("Game", "game");
    const size_t late = rig.AddSource("Late", "late");
    rig.FeedVideo(0, 240);
    // Game's capture stops for 1.5..2.5 s (a per-app target exiting and relaunching): no packets.
    rig.FeedTone(game, 0.0, 1.5, {440.0, 0.5});
    rig.FeedTone(game, 2.5, 4.0, {440.0, 0.5});
    // Late only starts capturing at 2.0 s.
    rig.FeedTone(late, 2.0, 4.0, {1000.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    CheckDecodesCleanly(file);

    // File time 0 is engine time 1.0 s.
    const Pcm g = DecodeAudio(file, 0);
    CHECK(g.Seconds() == doctest::Approx(3.0).epsilon(0.02));
    CHECK(ToneAt(g, 440.0, 0.0) == doctest::Approx(0.5).epsilon(kToneTolerance));
    CHECK(Rms(g, FramesAt(0.6), FramesAt(0.8)) < 0.01);
    const auto resumed = FirstFrameAbove(g, 0.1, FramesAt(0.6));
    REQUIRE(resumed.has_value());
    CHECK(double(*resumed) / kRate == doctest::Approx(1.5).epsilon(0.02));

    const Pcm l = DecodeAudio(file, 1);
    CHECK(l.Seconds() == doctest::Approx(3.0).epsilon(0.02));
    CHECK(Rms(l, 0, FramesAt(0.9)) < 0.01);
    const auto started = FirstFrameAbove(l, 0.1);
    REQUIRE(started.has_value());
    CHECK(double(*started) / kRate == doctest::Approx(1.0).epsilon(0.02));
}

// Continuous encoding puts AAC frames on a fixed 1024-sample grid, so a save rarely starts on a
// frame boundary: MP4 trims the first frame with an edit list, and the audio must still land on
// the video to the sample. 1.0 s is timeline frame 48000, 896 samples into grid frame 46.
TEST_CASE("save: audio lines up with video to the sample across the grid offset") {
    SaveRig rig(L"grid");
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    rig.FeedTone(game, 2.5, 4.0, {1000.0, 0.8});  // onset 1.5 s into the [1 s, 4 s) window

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    CheckDecodesCleanly(file);
    const Pcm pcm = DecodeAudio(file, 0);
    const auto onset = FirstFrameAbove(pcm, 0.05);
    REQUIRE(onset.has_value());
    CHECK(std::abs(double(*onset) - 1.5 * kRate) <= 2.0);
}

// Gain is applied while mixing, so a change takes effect from when it's made: audio already in
// the buffer keeps the gain it was recorded with.
TEST_CASE("save: a gain change applies from the moment it's made, not to buffered audio") {
    SaveRig rig(L"gain_live");
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    rig.FeedTone(game, 0.0, 2.5, {440.0, 0.5});
    rig.Hub().EncodeThrough(rig.timeline.At(2.5));
    rig.Hub().SetSources({{"Game", "game", -6.0206}}, false);  // x0.5 from here on
    rig.FeedTone(game, 2.5, 4.0, {440.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const Pcm pcm = DecodeAudio(rig.SavedPath(), 0);
    CHECK(ToneAt(pcm, 440.0, 0.5) == doctest::Approx(0.5).epsilon(kToneTolerance));   // 1.5..2.0 s
    CHECK(ToneAt(pcm, 440.0, 2.0) == doctest::Approx(0.25).epsilon(kToneTolerance));  // 3.0..3.5 s
}

// Encoding the way the background thread does, a little at a time: a source that goes quiet for
// longer than kIdleSeconds pauses its track's encoder, and the save fills that span with silence
// without moving what comes after it.
TEST_CASE("save: a track paused while its source was idle keeps later audio in place") {
    SaveRig rig(L"idle_pause");
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    for (int step = 0; step < 80; ++step) {  // 50 ms steps; integer so 3.0 s is exactly 3.0
        const double t       = step * 0.05;
        const bool   playing = step < 30 || step >= 60;  // capture stops for 1.5..3.0 s
        if (playing) rig.FeedTone(game, t, t + 0.05, {440.0, 0.5});
        rig.Hub().EncodeThrough(rig.timeline.At(t + 0.05 - AudioTrackHub::kLagSeconds));
    }

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    CheckDecodesCleanly(file);
    const Pcm pcm = DecodeAudio(file, 0);
    CHECK(ToneAt(pcm, 440.0, 0.0) == doctest::Approx(0.5).epsilon(kToneTolerance));
    CHECK(Rms(pcm, FramesAt(0.6), FramesAt(1.3)) < 0.01);
    const auto resumed = FirstFrameAbove(pcm, 0.1, FramesAt(0.6));
    REQUIRE(resumed.has_value());
    CHECK(std::abs(double(*resumed) - 2.0 * kRate) <= 48.0);
    CHECK(ToneAt(pcm, 440.0, 2.2) == doctest::Approx(0.5).epsilon(kToneTolerance));
}

// A source that's behind (its packets arrive late) holds its track back instead of being encoded
// over with silence.
TEST_CASE("save: a lagging source's late audio is waited for, not replaced with silence") {
    SaveRig rig(L"late_packets");
    const size_t desktop = rig.AddSource("Desktop", "voice");
    const size_t mic     = rig.AddSource("Mic", "voice");
    rig.FeedVideo(0, 240);
    rig.FeedTone(desktop, 0.0, 2.0, {440.0, 0.5});
    rig.FeedTone(mic, 0.0, 1.8, {1000.0, 0.5});
    rig.Hub().EncodeThrough(rig.timeline.At(2.0));  // mic is 200 ms behind
    rig.FeedTone(desktop, 2.0, 4.0, {440.0, 0.5});
    rig.FeedTone(mic, 1.8, 4.0, {1000.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const Pcm pcm = DecodeAudio(rig.SavedPath(), 0);
    // Mixed tracks halve each member: the mic's tone runs straight through 1.8..2.0 s.
    CHECK(ToneAmplitude(pcm, 1000.0, FramesAt(0.75), FramesAt(0.3)) == doctest::Approx(0.25).epsilon(kToneTolerance));
}

TEST_CASE("save: sources sharing a group_id are mixed into one track") {
    SaveRig rig(L"groups");
    const size_t a = rig.AddSource("Discord", "voice");
    const size_t b = rig.AddSource("Mic", "voice");
    const size_t c = rig.AddSource("Desktop", "desktop");
    rig.FeedVideo(0, 240);
    rig.FeedTone(a, 0.0, 4.0, {440.0, 0.5});
    rig.FeedTone(b, 0.0, 4.0, {1000.0, 0.5});
    rig.FeedTone(c, 0.0, 4.0, {2500.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 2);
    CHECK(audio[0].title == "Discord + Mic");
    CHECK(audio[1].title == "Desktop");

    // A group is scaled by its member count so simultaneous peaks can't clip.
    const Pcm mixed = DecodeAudio(file, 0);
    CHECK(ToneAt(mixed, 440.0, 1.0) == doctest::Approx(0.25).epsilon(kToneTolerance));
    CHECK(ToneAt(mixed, 1000.0, 1.0) == doctest::Approx(0.25).epsilon(kToneTolerance));
    CHECK(ToneAt(mixed, 2500.0, 1.0) < 0.02);

    const Pcm desktop = DecodeAudio(file, 1);
    CHECK(ToneAt(desktop, 2500.0, 1.0) == doctest::Approx(0.5).epsilon(kToneTolerance));
    CHECK(ToneAt(desktop, 440.0, 1.0) < 0.02);
}

TEST_CASE("save: a source with no packets in the window is skipped, not encoded as silence") {
    // Models a per-app source whose target process isn't running: the source is configured but
    // nothing was ever fed to it, so its track never encodes a frame.
    SaveRig rig(L"dead_source");
    const size_t desktop = rig.AddSource("Desktop", "desktop");
    rig.AddSource("NotRunning", "notrunning");  // never fed
    rig.FeedVideo(0, 240);
    rig.FeedTone(desktop, 0.0, 4.0, {440.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 1);
    CHECK(audio[0].title == "Desktop");
}

TEST_CASE("save: a mixed group with every member silent is skipped entirely") {
    SaveRig rig(L"dead_group");
    rig.AddSource("Discord", "voice");   // never fed
    rig.AddSource("Mic", "voice");       // never fed
    const size_t desktop = rig.AddSource("Desktop", "desktop");
    rig.FeedVideo(0, 240);
    rig.FeedTone(desktop, 0.0, 4.0, {440.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 1);
    CHECK(audio[0].title == "Desktop");
}

TEST_CASE("save: every source switched off writes a video-only file") {
    // Simple mode with Desktop and Mic both switched off: the watcher captures nothing, so no source
    // is ever fed.
    SaveRig rig(L"all_off");
    rig.AddSource("Desktop", "desktop");
    rig.AddSource("Mic", "mic");
    rig.FeedVideo(0, 240);

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    CHECK(probe.Of("audio").empty());
    CHECK(probe.Of("video").size() == 1);
    CheckDecodesCleanly(file);
}

TEST_CASE("save: a mixed group with one live member is still encoded") {
    SaveRig rig(L"half_dead_group");
    rig.AddSource("Discord", "voice");  // never fed
    const size_t mic = rig.AddSource("Mic", "voice");
    rig.FeedVideo(0, 240);
    rig.FeedTone(mic, 0.0, 4.0, {1000.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 1);
    CHECK(audio[0].title == "Discord + Mic");
    // Still divided by the group's full member count (2), same as any other mixed group -- a
    // silent member isn't dropped from the mix divisor, only from the all-silent skip decision.
    CHECK(ToneAt(DecodeAudio(file, 0), 1000.0, 1.0) == doctest::Approx(0.25).epsilon(kToneTolerance));
}

TEST_CASE("save: gain_db scales only its own source") {
    SaveRig rig(L"gain");
    const size_t quiet = rig.AddSource("Quiet", "quiet", -6.0206);  // x0.5
    const size_t unity = rig.AddSource("Unity", "unity");
    rig.FeedVideo(0, 240);
    rig.FeedTone(quiet, 0.0, 4.0, {440.0, 0.5});
    rig.FeedTone(unity, 0.0, 4.0, {440.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    CHECK(ToneAt(DecodeAudio(file, 0), 440.0, 1.0) == doctest::Approx(0.25).epsilon(kToneTolerance));
    CHECK(ToneAt(DecodeAudio(file, 1), 440.0, 1.0) == doctest::Approx(0.5).epsilon(kToneTolerance));
}

TEST_CASE("save: full_mix_track appends a Full Mix of every source") {
    SaveRig rig(L"fullmix");
    const size_t a = rig.AddSource("Game", "game");
    const size_t b = rig.AddSource("Mic", "mic");
    rig.full_mix = true;
    rig.FeedVideo(0, 240);
    rig.FeedTone(a, 0.0, 4.0, {440.0, 0.5});
    rig.FeedTone(b, 0.0, 4.0, {1000.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const std::wstring file = rig.SavedPath();
    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 3);
    CHECK(audio[2].title == "Full Mix");
    const Pcm mix = DecodeAudio(file, 2);
    CHECK(ToneAt(mix, 440.0, 1.0) == doctest::Approx(0.25).epsilon(kToneTolerance));
    CHECK(ToneAt(mix, 1000.0, 1.0) == doctest::Approx(0.25).epsilon(kToneTolerance));
}

TEST_CASE("save: a wrapped ring saves whole GOPs from a keyframe and decodes cleanly") {
    const H264Fixture& fx = StandardFixture();
    size_t gop_bytes = 0;
    for (size_t g = 0; g < fx.aus.size(); g += fx.gop) {
        size_t bytes = 0;
        for (size_t i = g; i < g + fx.gop; ++i) bytes += fx.aus[i].annexb.size();
        gop_bytes = (std::max)(gop_bytes, bytes);
    }

    SaveRig rig(L"wrap", gop_bytes * 3);
    rig.ctx.buffer_seconds = 60.0;
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 1800);  // 30 s into a ring that holds ~3 s
    rig.FeedTone(game, 0.0, 30.0, {440.0, 0.5});

    REQUIRE(rig.Save(30.0));
    const std::wstring file = rig.SavedPath();
    CheckDecodesCleanly(file);

    const VideoFrames frames = ProbeVideoFrames(file);
    CHECK(frames.first_is_key);
    CHECK(frames.count % fx.gop == 0);
    CHECK(frames.count >= 2 * fx.gop);
    CHECK(frames.count <= 3 * fx.gop);

    // Audio follows the video floor, not its own (much longer) ring.
    const Pcm pcm = DecodeAudio(file, 0);
    CHECK(pcm.Seconds() == doctest::Approx(double(frames.count) / fx.fps).epsilon(0.03));
}

TEST_CASE("save: truncate policy stops the next save from repeating footage") {
    SaveRig rig(L"truncate");
    rig.ctx.save_overlap_policy = "truncate";
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    rig.FeedTone(game, 0.0, 4.0, {440.0, 0.5});

    REQUIRE(rig.Save(4.0));
    CHECK(ProbeVideoFrames(rig.SavedPath()).count == 180);

    // Nothing new since the barrier: an immediate second save has nothing to write.
    CHECK(rig.SaveAt(4.0) == SaveOutcome::kNothingNew);

    rig.ctx.out_path = TestOutputPath(L"save\\truncate-second.mp4");
    rig.FeedVideo(240, 120);  // 4..6 s
    rig.FeedTone(game, 4.0, 6.0, {1000.0, 0.5});
    REQUIRE(rig.Save(6.0));
    const std::wstring second = rig.SavedPath();
    CHECK(ProbeVideoFrames(second).count == 120);  // 4..6 s, not the 3 s window
    const Pcm pcm = DecodeAudio(second, 0);
    CHECK(pcm.Seconds() == doctest::Approx(2.0).epsilon(0.03));
    CHECK(ToneAt(pcm, 440.0, 0.0) < 0.02);  // no audio from before the barrier either
    CHECK(ToneAt(pcm, 1000.0, 0.5) == doctest::Approx(0.5).epsilon(kToneTolerance));
}

TEST_CASE("save: under a second of new footage writes nothing and doesn't cut the next save") {
    SaveRig rig(L"short");
    rig.ctx.save_overlap_policy = "truncate";
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    rig.FeedTone(game, 0.0, 4.0, {440.0, 0.5});
    REQUIRE(rig.Save(4.0));

    // 0.25 s later, a double press. Live (2026-09-13) this wrote a 1-frame clip and reported success.
    rig.FeedVideo(240, 15);
    rig.FeedTone(game, 4.0, 4.25, {1000.0, 0.5});
    const std::wstring skipped = TestOutputPath(L"save\\short-skipped.mp4");
    rig.ctx.out_path = skipped;
    CHECK(rig.SaveAt(4.25) == SaveOutcome::kNothingNew);
    CHECK(GetFileAttributesW(skipped.c_str()) == INVALID_FILE_ATTRIBUTES);

    // A skipped save must not drop a barrier, or the next real clip loses footage: still 4..6 s.
    rig.ctx.out_path = TestOutputPath(L"save\\short-next.mp4");
    rig.FeedVideo(255, 105);
    rig.FeedTone(game, 4.25, 6.0, {1000.0, 0.5});
    REQUIRE(rig.Save(6.0));
    CHECK(ProbeVideoFrames(rig.SavedPath()).count == 120);
}

// '?' is reserved on NTFS regardless of which drives exist, so CreateDirectoryW fails for real (a
// control character, tried first in the old faulttest, is accepted and creates the folder).
TEST_CASE("save: an output folder that can't be created fails the save and leaves the buffer intact") {
    SaveRig rig(L"bad-folder");
    rig.ctx.save_overlap_policy = "truncate";  // a failed save must not drop its barrier either
    rig.FeedVideo(0, 240);
    const size_t barriers = rig.ring.BarrierCount();

    rig.ctx.out_path = TestOutputPath(L"save\\bad?folder\\clip.mp4");
    CHECK(rig.SaveAt(4.0) == SaveOutcome::kFailed);
    CHECK(rig.ring.BarrierCount() == barriers);

    rig.ctx.out_path = TestOutputPath(L"save\\bad-folder-retry.mp4");
    REQUIRE(rig.Save(4.0));
    CHECK(ProbeVideoFrames(rig.SavedPath()).count == 180);
}

// Instant Replay off, then a frame-rate change: the old footage stays and SaveContext has moved on.
TEST_CASE("save: a clip plays at its footage's frame rate, not the one configured since") {
    SaveRig rig(L"segment-fps");
    const H264Fixture& fx = StandardFixture();
    auto params = std::make_shared<SegmentParams>();
    params->codec_header = fx.sps_pps;
    params->width  = fx.width;
    params->height = fx.height;
    params->fps    = fx.fps;
    rig.ring.SetBaseSegmentParams(params);
    rig.ctx.fps = fx.fps * 2;
    rig.FeedVideo(0, 240);

    REQUIRE(rig.Save(4.0));
    const VideoFrames frames = ProbeVideoFrames(rig.SavedPath());
    REQUIRE(frames.count > 1);
    CHECK(frames.last_pts_s - frames.first_pts_s == doctest::Approx(double(frames.count - 1) / fx.fps).epsilon(0.01));
}

TEST_CASE("save: frames the pacer dropped keep their time, so video stays as long as its audio") {
    SaveRig rig(L"dropped-ticks");
    const size_t game = rig.AddSource("Game", "game");
    const H264Fixture& fx = StandardFixture();
    // An encoder managing half the frame rate: every second tick, indexed as the pacer does.
    for (uint64_t t = 0; t < 240; t += 2) {
        const auto& au = fx.aus[(t / 2) % fx.aus.size()];
        rig.ring.Append(au.annexb.data(), au.annexb.size(), rig.timeline.Tick(t, fx.fps), t, au.idr);
    }
    rig.FeedTone(game, 0.0, 4.0, {440.0, 0.5});

    REQUIRE(rig.Save(4.0));
    const VideoFrames frames = ProbeVideoFrames(rig.SavedPath());
    const double video_s = frames.last_pts_s - frames.first_pts_s + 2.0 / fx.fps;
    // The 3 s window starts at 1 s, inside the GOP that starts at 0 (the fixture's 60-AU GOP, every
    // second tick), so the save starts at 0: 120 frames that last 4 s. Counted as consecutive ticks
    // they would last 2 s.
    CHECK(frames.count == 120);
    CHECK(video_s == doctest::Approx(4.0).epsilon(0.03));
    CHECK(DecodeAudio(rig.SavedPath(), 0).Seconds() == doctest::Approx(video_s).epsilon(0.03));
}

// A save starts at the keyframe at or before its floor (Ring::PinCovering), up to a keyframe
// interval early. The audio rings keep that far back too (AudioRingMaxAgeQpc), or the clip's first
// seconds would be silence.
TEST_CASE("save: a long keyframe interval still gets audio from the clip's first frame") {
    SaveRig rig(L"long-gop-audio");
    rig.ring.SetMaxAge(RingMaxAgeQpc(rig.ctx.buffer_seconds));
    const size_t game = rig.AddSource("Game", "game");
    rig.Hub().Resize(AudioTrackHub::SizeFor(rig.ctx.buffer_seconds, 192, AudioRingMaxAgeQpc(rig.ctx.buffer_seconds)));
    const H264Fixture& fx = StandardFixture();
    // Every fourth tick: the fixture's 60-AU GOP lasts 4 s, keyframes at 0, 4, 8, ... s.
    REQUIRE(fx.fps == 60);
    for (uint64_t t = 0; t < 1200; t += 4) {
        const auto& au = fx.aus[(t / 4) % fx.aus.size()];
        rig.ring.Append(au.annexb.data(), au.annexb.size(), rig.timeline.Tick(t, fx.fps), t, au.idr);
    }
    for (double from = 0.0; from < 20.0; from += 2.0) {  // the hub stages only 4 s un-encoded
        rig.FeedTone(game, from, from + 2.0, {440.0, 0.5});
        rig.Hub().EncodeThrough(rig.timeline.At(from + 2.0));
    }

    // Floor 14.5 s: the keyframe at 12 s is 2.5 s before it, past the video's 2 s age slack.
    REQUIRE(rig.Save(17.5));
    const VideoFrames frames = ProbeVideoFrames(rig.SavedPath());
    const double video_s = frames.last_pts_s - frames.first_pts_s + 4.0 / fx.fps;
    CHECK(video_s == doctest::Approx(5.5).epsilon(0.03));
    const Pcm audio = DecodeAudio(rig.SavedPath(), 0);
    CHECK(audio.Seconds() == doctest::Approx(video_s).epsilon(0.03));
    CHECK(ToneAt(audio, 440.0, 0.1) > 0.3);  // tone from the start, not padded silence
}

TEST_CASE("save: a clip that fails to write leaves its footage for a retry under truncate") {
    SaveRig rig(L"failed-write");
    rig.ctx.save_overlap_policy = "truncate";
    rig.FeedVideo(0, 240);
    const size_t barriers = rig.ring.BarrierCount();

    // A folder at the clip's own path: the folder check passes and the file can't be opened, the
    // same path through the mux as a disk that fills up.
    const std::wstring blocked = TestOutputPath(L"save\\failed-write-blocked.mp4");
    CreateDirectoryW(blocked.c_str(), nullptr);
    rig.ctx.out_path = blocked;
    CHECK(rig.SaveAt(4.0) == SaveOutcome::kFailed);
    CHECK(rig.ring.BarrierCount() == barriers);

    rig.ctx.out_path = TestOutputPath(L"save\\failed-write-retry.mp4");
    REQUIRE(rig.Save(4.0));
    CHECK(ProbeVideoFrames(rig.SavedPath()).count == 180);
    CHECK(rig.ring.BarrierCount() == barriers + 1);  // a written clip still cuts the next one
    CHECK(rig.SaveAt(4.0) == SaveOutcome::kNothingNew);
}

namespace {
// An output.root that can't be created, as a drive letter with nothing on it: a file in its path.
std::string UnreachableRoot(const wchar_t* name) {
    const std::wstring blocker = TestOutputPath(name);
    std::ofstream(blocker) << "a file, not a folder";
    return NarrowUtf8(blocker + L"\\clips");
}

void FeedRecent(SaveRig& rig) {
    const auto& fx = StandardFixture();
    const int64_t start = QpcNow() - 2 * QpcFrequency();
    for (uint64_t t = 0; t < 2 * fx.fps; ++t) {
        const auto& au = fx.aus[t % fx.aus.size()];
        rig.ring.Append(au.annexb.data(), au.annexb.size(), start + t * QpcFrequency() / fx.fps, t, au.idr);
    }
}
}  // namespace

TEST_CASE("EnsureParentDir: a network share's root is never created, what's under it is") {
    CHECK(FirstCreatableDirOffset(L"C:\\Videos\\Ombra") == 2);
    CHECK(FirstCreatableDirOffset(L"Videos\\Ombra") == 0);
    CHECK(FirstCreatableDirOffset(L"\\\\nas\\clips") == std::wstring::npos);
    CHECK(FirstCreatableDirOffset(L"\\\\nas") == std::wstring::npos);
    CHECK(FirstCreatableDirOffset(L"\\\\nas\\clips\\Game") == 11);
    CHECK(FirstCreatableDirOffset(L"//nas/clips/Game") == 11);
    CHECK(FirstCreatableDirOffset(L"\\\\?\\UNC\\nas\\clips") == std::wstring::npos);
    CHECK(FirstCreatableDirOffset(L"\\\\?\\UNC\\nas\\clips\\Game") == 17);
    CHECK(FirstCreatableDirOffset(L"\\\\?\\C:\\Videos") == 6);

    // For real where the admin share answers, always under %TEMP%.
    const std::wstring local = TestOutputPath(L"save\\unc\\sub\\clip.mp4");
    if (local.size() > 3 && local[1] == L':') {
        const std::wstring share = std::wstring(L"\\\\localhost\\") + local[0] + L"$";
        if (GetFileAttributesW(share.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CHECK(EnsureParentDir(share + local.substr(2)));
            CHECK(GetFileAttributesW((share + local.substr(2, local.find_last_of(L'\\') - 2)).c_str()) != INVALID_FILE_ATTRIBUTES);
        } else {
            MESSAGE("no admin share here: only the offsets were checked");
        }
    }
}

TEST_CASE("PickWritablePath: the wanted folder, else the fallback, else nothing") {
    const std::wstring good = TestOutputPath(L"pick\\good\\shot.png");
    const std::wstring other = TestOutputPath(L"pick\\other\\shot.png");
    const std::string  dead_root = UnreachableRoot(L"pick-blocker");
    const std::wstring dead = std::wstring(dead_root.begin(), dead_root.end()) + L"\\shot.png";

    CHECK(PickWritablePath(good, other, 1024) == good);
    CHECK(PickWritablePath(dead, other, 1024) == other);
    CHECK(PickWritablePath(dead, L"", 1024).empty());
    CHECK(PickWritablePath(dead, dead, 1024).empty());
    // No disk has this much room, so a full disk falls through the same way.
    CHECK(PickWritablePath(good, L"", UINT64_MAX).empty());
    CHECK_FALSE(std::filesystem::exists(good + L".probe"));
}

TEST_CASE("save: an output folder that can't take the clip writes it to the fallback and says so") {
    SaveRig rig(L"fallback");
    FeedRecent(rig);
    rig.ctx.out_path = L"out\\ombra.mp4";
    rig.ctx.output_root = UnreachableRoot(L"save-fallback-blocker");
    rig.ctx.fallback_root = TestOutputPath(L"save\\fallback-root");

    bool feedback_fell_back = false;
    const auto result = SaveReplay({&rig.ctx}, [&]() -> SaveFeedback {
        return [&](const ReplaySaveResults& r) { feedback_fell_back = r.monitors[0].fell_back; };
    }).get();

    REQUIRE(result.outcome == SaveOutcome::kSaved);
    CHECK(feedback_fell_back);
    CHECK(result.monitors[0].fell_back);
    CHECK(result.monitors[0].path.rfind(rig.ctx.fallback_root, 0) == 0);
    CheckDecodesCleanly(result.monitors[0].path);
    // The probe leaves nothing beside the clip.
    CHECK(GetFileAttributesW((result.monitors[0].path + L".probe").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("save: a reachable output folder doesn't fall back") {
    SaveRig rig(L"no-fallback");
    FeedRecent(rig);
    rig.ctx.out_path = L"out\\ombra.mp4";
    rig.ctx.output_root = NarrowUtf8(TestOutputPath(L"save\\no-fallback-root"));
    rig.ctx.fallback_root = TestOutputPath(L"save\\no-fallback-unused");

    const auto result = SaveReplay({&rig.ctx}).get();
    REQUIRE(result.outcome == SaveOutcome::kSaved);
    CHECK_FALSE(result.monitors[0].fell_back);
    CHECK(result.monitors[0].path.rfind(TestOutputPath(L"save\\no-fallback-root"), 0) == 0);
}

TEST_CASE("save: with no usable fallback the save fails and keeps its footage") {
    SaveRig rig(L"no-usable-fallback");
    FeedRecent(rig);
    rig.ctx.out_path = L"out\\ombra.mp4";
    rig.ctx.output_root = UnreachableRoot(L"save-nofallback-blocker");
    SUBCASE("no fallback set") {}
    SUBCASE("the fallback is the folder that failed") {
        rig.ctx.fallback_root = std::wstring(rig.ctx.output_root.begin(), rig.ctx.output_root.end());
    }
    SUBCASE("the fallback can't take it either") {
        const std::string other = UnreachableRoot(L"save-nofallback-blocker2");
        rig.ctx.fallback_root = std::wstring(other.begin(), other.end());
    }
    const auto result = SaveReplay({&rig.ctx}).get();
    CHECK(result.outcome == SaveOutcome::kFailed);
    CHECK_FALSE(result.monitors[0].fell_back);

    rig.ctx.output_root = NarrowUtf8(TestOutputPath(L"save\\no-usable-fallback-retry"));
    CHECK(SaveReplay({&rig.ctx}).get().outcome == SaveOutcome::kSaved);
}

TEST_CASE("save: pressed after the game closed and the buffer disarmed, it still has the game") {
    // Only in games disarms once the game is gone (controller.h). Nothing is appended after that,
    // so the save window [press - buffer, press] reaches back into the footage the ring kept, until
    // Ring::ReleaseIfIdle frees it a whole max age later.
    SaveRig rig(L"after-close");
    rig.ctx.buffer_seconds = 10.0;
    rig.ring.SetMaxAge(RingMaxAgeQpc(rig.ctx.buffer_seconds));
    rig.FeedVideo(0, 360);  // the game: 0..6 s, then it closes and the pipeline disarms

    CHECK_FALSE(rig.ring.ReleaseIfIdle(rig.timeline.At(12.0)));  // not idle a whole max age yet
    REQUIRE(rig.Save(12.0));                                     // pressed 6 s after it closed
    CHECK(ProbeVideoFrames(rig.SavedPath()).count == 240);        // 2..6 s: all of it in the window
    CheckDecodesCleanly(rig.SavedPath());

    CHECK(rig.ring.ReleaseIfIdle(rig.timeline.At(6.0 + 12.5)));  // gone once no save could reach it
}

namespace {
// controller.h's GameFolderSource is the seam that makes
// this test possible without a real BufferController (which needs a real PipelineSupervisor and
// unconditionally opens a GpuLoadWatcher/PDH query in its constructor).
class FakeGameFolderSource : public GameFolderSource {
public:
    explicit FakeGameFolderSource(std::wstring folder) : folder_(std::move(folder)) {}
    std::wstring CurrentGameFolder() const override { return folder_; }

private:
    std::wstring folder_;
};
}  // namespace

// The real engine hands DoSave the "out\ombra.mp4" sentinel, which switches on output_root +
// output_filename templating.
TEST_CASE("save: per_game_folders nests the save under the controller's current game folder") {
    SaveRig rig(L"unused");
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    rig.FeedTone(game, 0.0, 4.0, {440.0, 0.5});

    // Each SUBCASE re-enters this whole TEST_CASE from the top (doctest), so a shared root/filename
    // across subcases would have the second write collide with the first's leftover file and pick
    // up a "_001" suffix from the never-overwrite path below -- same reason the templated-paths test
    // above uses a fresh folder per pass. One subfolder per subcase instead.
    static int pass = 0;
    const std::wstring root = TestOutputPath(L"pgf" + std::to_wstring(pass++));
    rig.ctx.out_path        = L"out\\ombra.mp4";
    rig.ctx.output_root     = NarrowUtf8(root);
    rig.ctx.output_filename = "clip.mp4";

    SUBCASE("per_game_folders on, a game is armed: nests under the sanitized game name") {
        FakeGameFolderSource folder(L"League of Legends");
        rig.ctx.controller       = &folder;
        rig.ctx.per_game_folders = true;
        REQUIRE(rig.Save(4.0));
        CHECK(NarrowUtf8(rig.SavedPath()) == NarrowUtf8(root) + "\\League of Legends\\clip.mp4");
    }
    SUBCASE("per_game_folders on, no game armed (empty folder): saves flat, not into a stray folder") {
        FakeGameFolderSource folder(L"");
        rig.ctx.controller       = &folder;
        rig.ctx.per_game_folders = true;
        REQUIRE(rig.Save(4.0));
        CHECK(NarrowUtf8(rig.SavedPath()) == NarrowUtf8(root) + "\\clip.mp4");
    }
    SUBCASE("per_game_folders off: saves flat even with a game armed") {
        FakeGameFolderSource folder(L"League of Legends");
        rig.ctx.controller       = &folder;
        rig.ctx.per_game_folders = false;
        REQUIRE(rig.Save(4.0));
        CHECK(NarrowUtf8(rig.SavedPath()) == NarrowUtf8(root) + "\\clip.mp4");
    }
    CheckDecodesCleanly(rig.SavedPath());
}

TEST_CASE("save: templated output paths create missing folders and never overwrite") {
    SaveRig rig(L"unused");
    const size_t game = rig.AddSource("Game", "game");
    rig.FeedVideo(0, 240);
    rig.FeedTone(game, 0.0, 4.0, {440.0, 0.5});

    static int pass = 0;  // doctest re-enters the case once per SUBCASE; each needs fresh folders
    const std::wstring root = TestOutputPath(L"paths" + std::to_wstring(pass++) + L"\\missing1\\missing2");
    REQUIRE(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES);
    rig.ctx.out_path         = L"out\\ombra.mp4";
    rig.ctx.output_root      = NarrowUtf8(root);
    rig.ctx.output_filename  = "clip{monitor_suffix}.mp4";
    rig.ctx.monitor_suffix   = L" - DISPLAY2";

    SUBCASE("flat file, a repeat gets a suffix") {
        const char* expected[] = {"clip - DISPLAY2.mp4", "clip - DISPLAY2_001.mp4", "clip - DISPLAY2_002.mp4"};
        for (const char* name : expected) {
            REQUIRE(rig.Save(4.0));
            CHECK(NarrowUtf8(rig.SavedPath()) == NarrowUtf8(root) + "\\" + name);
        }
        for (const char* name : expected) {
            CAPTURE(name);
            CHECK(GetFileAttributesW((root + L"\\" + WideFromUtf8(name)).c_str()) != INVALID_FILE_ATTRIBUTES);
        }
    }
    SUBCASE("N>1 monitors: one folder per press, file named by monitor label") {
        rig.ctx.monitor_label = L"DISPLAY2";
        const std::wstring group = L"press 2026-09-13 22-00-00";
        REQUIRE(rig.Save(4.0, &group));
        CHECK(NarrowUtf8(rig.SavedPath()) == NarrowUtf8(root + L"\\" + group + L"\\DISPLAY2.mp4"));
    }
    CheckDecodesCleanly(rig.SavedPath());
}

TEST_CASE("save: repeated saves do not grow private bytes") {
    SaveRig rig(L"soak");
    std::vector<size_t> sources = {rig.AddSource("A", "a"), rig.AddSource("B", "b"), rig.AddSource("C", "c")};
    rig.FeedVideo(0, 240);
    for (size_t i = 0; i < sources.size(); ++i) rig.FeedTone(sources[i], 0.0, 4.0, {440.0 * (i + 1), 0.3});

    auto private_mb = [] {
        double ws = 0, priv = 0;
        REQUIRE(GetProcessRssMb(ws, priv));
        return priv;
    };

    constexpr int kWarmup = 20, kBatch = 100, kBatches = 4;
    for (int i = 0; i < kWarmup; ++i) REQUIRE(rig.Save(4.0));
    std::vector<double> mb = {private_mb()};
    for (int b = 0; b < kBatches; ++b) {
        for (int i = 0; i < kBatch; ++i) REQUIRE(rig.Save(4.0));
        mb.push_back(private_mb());
    }
    std::string curve;
    for (double v : mb) curve += std::to_string(v).substr(0, 6) + " ";
    MESSAGE("private MB after warm-up then every " << kBatch << " saves: " << curve);
    // Samples jitter by ~3 MB between batches (heap/Media Foundation), so a per-batch delta is noise.
    // 400 saves leaking 20 KB each would add 8 MB, above the jitter. Not under ASan: its quarantine
    // holds freed memory back by design.
#ifndef __SANITIZE_ADDRESS__
    CHECK(*std::max_element(mb.begin(), mb.end()) - mb.front() < 8.0);
#endif
}

TEST_CASE("save: a large clip is streamed from the ring, never copied whole") {
    // ~120 MB of footage in the window: each fixture AU carries a 1 MB filler NAL (type 12, which
    // the mux passes through and decoders skip). The save used to copy every AU out of the ring
    // before muxing, so private bytes rose by the clip's size.
    const H264Fixture& fx = StandardFixture();
    SaveRig rig(L"streamed", 256u << 20);
    rig.ctx.buffer_seconds = 10.0;  // the whole feed
    std::vector<uint8_t> au;
    for (uint64_t t = 0; t < 150; ++t) {
        const auto& src = fx.aus[t % fx.aus.size()];
        au = src.annexb;
        const uint8_t filler_hdr[5] = {0, 0, 0, 1, 12};  // nal_unit_type 12: filler data
        au.insert(au.end(), filler_hdr, filler_hdr + 5);
        au.insert(au.end(), 1u << 20, 0xFF);
        au.push_back(0x80);  // rbsp_trailing_bits
        rig.ring.Append(au.data(), au.size(), rig.timeline.Tick(t, fx.fps), t, src.idr);
    }

    std::atomic<bool>   done{false};
    std::atomic<double> peak{0.0};
    double ws = 0, before = 0;
    REQUIRE(GetProcessRssMb(ws, before));
    std::thread sampler([&] {
        while (!done) {
            double w = 0, p = 0;
            if (GetProcessRssMb(w, p) && p > peak) peak = p;
            Sleep(1);
        }
    });
    const bool saved = rig.Save(5.0);
    done = true;
    sampler.join();
    REQUIRE(saved);
    MESSAGE("private MB before the save " << before << ", peak during it " << peak.load());
    CHECK(peak.load() - before < 24.0);
    CHECK(FileSizeMb(rig.SavedPath()) > 100.0);
    CheckDecodesCleanly(rig.SavedPath());
}

// No other test exercises Telemetry::Event with Telemetry::Enabled() true, so a format-string/
// argument mismatch in DoSave's call (variadic, unchecked by the compiler) would only show up on
// the real save path.
TEST_CASE("save: telemetry event carries mux/pin timing and a real output size") {
    const bool was_enabled = Telemetry::Enabled();
    Telemetry::SetEnabled(true);
    struct RestoreEnabled {
        bool was;
        ~RestoreEnabled() { Telemetry::SetEnabled(was); }
    } restore_enabled{was_enabled};

    SaveRig rig(L"telemetry");
    rig.FeedVideo(0, 240);
    REQUIRE(rig.Save(4.0));

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t day[16];
    swprintf_s(day, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    const std::wstring csv_path = Telemetry::Directory() + L"\\events-" + day + L".csv";

    std::ifstream file(csv_path);
    REQUIRE_MESSAGE(file.good(), "could not open " << NarrowUtf8(csv_path));
    std::string line, last_save_line;
    while (std::getline(file, line)) {
        if (line.find(",save,") != std::string::npos) last_save_line = line;
    }
    REQUIRE_MESSAGE(!last_save_line.empty(), "no 'save' event found in " << NarrowUtf8(csv_path));

    CHECK(last_save_line.find("duration_ms=") != std::string::npos);
    CHECK(last_save_line.find("audio_ms=") != std::string::npos);
    CHECK(last_save_line.find("mux_ms=") != std::string::npos);
    CHECK(last_save_line.find("pin_ms=") != std::string::npos);
    CHECK(last_save_line.find("private_mb_delta_mux=") != std::string::npos);

    const size_t output_pos = last_save_line.find("output_mb=");
    REQUIRE(output_pos != std::string::npos);
    // A real value, not the -1 "size unavailable" sentinel: this save wrote a real file that
    // GetFileAttributesExW can read back.
    const double output_mb = std::stod(last_save_line.substr(output_pos + std::strlen("output_mb=")));
    CHECK(output_mb > 0.0);
}

// The extension used to be cut at the last '.' anywhere in the path, so a template without one
// truncated a dotted folder name (C:\Users\john.doe) or a dotted game name.
TEST_CASE("save: only a trailing .mp4 on the file name is replaced") {
    CHECK(WithContainerExtension(L"C:\\Users\\john.doe\\Clips\\Game 2026-09-25", L".mp4") ==
          L"C:\\Users\\john.doe\\Clips\\Game 2026-09-25.mp4");
    CHECK(WithContainerExtension(L"C:\\v\\Some.Game clip", L".mp4") == L"C:\\v\\Some.Game clip.mp4");
    CHECK(WithContainerExtension(L"C:\\v\\old template.MP4", L".mp4") == L"C:\\v\\old template.mp4");
}

TEST_CASE("save: a typed file name loses only the characters Windows rejects") {
    CHECK(SanitizeFileName(L"a:b/c\\d|e?f*g<h>i\"j") == L"a_b_c_d_e_f_g_h_i_j");
    CHECK(SanitizeFileName(L"Valorant 2026-09-25 (1)") == L"Valorant 2026-09-25 (1)");
}

}  // namespace ombra::test
