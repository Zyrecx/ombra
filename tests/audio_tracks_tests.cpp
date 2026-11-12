// AudioTrackHub on the real clock, the way the engine runs it: the background thread encoding
// behind real time while a capture delivers packets. The save tests drive the hub synchronously on
// a synthetic timeline and never Start() it, so this is the production path's coverage. No GPU.

#include <doctest/doctest.h>

#include "support/audio_analysis.h"
#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/audio_tracks.h"
#include "ombra/mux.h"
#include "ombra/util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace ombra::test {

namespace {

constexpr uint32_t kRate = 48000;

// Muxes `audio` under the fixture's video (any valid H.264 will do; only the audio is judged).
std::wstring MuxWithFixtureVideo(const std::wstring& name, const std::vector<MuxAudioTrack>& audio) {
    const H264Fixture& fx = StandardFixture();
    std::vector<MuxAu> aus;
    for (size_t i = 0; i < 120; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
    const std::wstring path = TestOutputPath(name);
    REQUIRE(MuxWriteMp4(path, fx.sps_pps, fx.width, fx.height, fx.fps, aus, audio));
    return path;
}

}  // namespace

TEST_CASE("audio hub: background encoding on the real clock keeps a paced source whole and in place") {
    const int64_t freq   = QpcFrequency();
    const int64_t origin = QpcNow();
    AudioTrackHub hub(origin, freq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"Tone", "tone"}}, false);
    hub.Start();

    // Stands in for WASAPI: 10 ms packets stamped with their first sample's QPC, each delivered
    // 10 ms after it ends. Silence, then a 1 kHz tone from 1.0 s after `start` on.
    const int64_t     start = origin + freq / 10;
    std::atomic<bool> run{true};
    std::thread       producer([&] {
        constexpr size_t     kPacket = 480;
        std::vector<int16_t> packet(kPacket * 2);
        for (uint64_t frame = 0; run.load(); frame += kPacket) {
            const int64_t packet_qpc = start + static_cast<int64_t>(frame) * freq / kRate;
            while (run.load() && QpcNow() < packet_qpc + static_cast<int64_t>(kPacket) * freq / kRate + freq / 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            for (size_t i = 0; i < kPacket; ++i) {
                const uint64_t f = frame + i;
                const int16_t  s = f < kRate ? 0
                                             : static_cast<int16_t>(std::lround(
                                                   0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * double(f) / kRate)));
                packet[i * 2] = packet[i * 2 + 1] = s;
            }
            hub.Append(0, packet.data(), packet.size(), packet_qpc);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2600));
    const int64_t floor = start + freq / 2;  // the "first video frame": 0.5 s in, 0.5 s before the tone
    const auto    tracks = hub.Collect(floor, QpcNow());
    run.store(false);
    producer.join();
    hub.Stop();

    REQUIRE(tracks.size() == 1);
    const std::wstring file = MuxWithFixtureVideo(L"hub_realclock.mp4", tracks);
    const std::string  errors = DecodeErrors(file);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);

    const Pcm  pcm   = DecodeAudio(file, 0);
    const auto onset = FirstFrameAbove(pcm, 0.05);
    REQUIRE(onset.has_value());
    CHECK(std::abs(double(*onset) - 0.5 * kRate) <= 2.0);
    // Whole: no window of the fed span dropped out (a hole would read near 0).
    REQUIRE(pcm.Seconds() > 1.8);
    const size_t window = kRate / 10;
    for (size_t f = static_cast<size_t>(0.55 * kRate); f + window < pcm.Frames() - kRate / 20; f += window) {
        CAPTURE(double(f) / kRate);
        CHECK(ToneAmplitude(pcm, 1000.0, f, window) == doctest::Approx(0.5).epsilon(0.06));
    }
}

// Synthetic timeline, driven synchronously (never Start()ed): the three silent-loss counters.
TEST_CASE("audio hub: loss counters stay zero when paced, and count overrun and late packets") {
    constexpr int64_t kFreq   = 10'000'000;
    constexpr int64_t kOrigin = 1'000'000;
    constexpr size_t  kPacket = 480;
    auto qpc_at = [](int64_t frame) { return kOrigin + frame * kFreq / kRate; };
    std::vector<int16_t> packet(kPacket * 2, 1000);

    SUBCASE("paced: nothing lost") {
        AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
        hub.SetSources({{"A", "a"}}, false);
        for (int64_t f = 0; f < 2 * kRate; f += kPacket) {
            hub.Append(0, packet.data(), packet.size(), qpc_at(f));
            hub.EncodeThrough(qpc_at(f + kPacket));
        }
        const auto l = hub.GetLosses();
        CHECK(l.overrun_frames == 0);
        CHECK(l.late_frames == 0);
        CHECK(l.failed_frames == 0);
        CHECK(l.encoder_errors == 0);
    }

    SUBCASE("encoder 6 s behind: what staging no longer holds counts as overrun") {
        AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
        hub.SetSources({{"A", "a"}}, false);
        for (int64_t f = 0; f < 6 * kRate; f += kPacket) hub.Append(0, packet.data(), packet.size(), qpc_at(f));
        hub.EncodeThrough(qpc_at(6 * kRate));
        const int64_t overrun = static_cast<int64_t>(hub.GetLosses().overrun_frames);
        const int64_t expect  = 6 * kRate - AudioTrackHub::kStageFrames;  // 2 s
        CHECK(std::llabs(overrun - expect) <= 2048);
        CHECK(hub.GetLosses().late_frames == 0);
    }

    SUBCASE("a save while a source is idle doesn't make its next packet late") {
        AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
        hub.SetSources({{"A", "a"}}, false);
        for (int64_t f = 0; f < kRate; f += kPacket) {
            hub.Append(0, packet.data(), packet.size(), qpc_at(f));
            hub.EncodeThrough(qpc_at(f + kPacket));
        }
        // Silent for a while, then a save; the sound that resumed just before the press arrives after it.
        const int64_t press = 3 * kRate;
        hub.Collect(qpc_at(0), qpc_at(press));
        for (int64_t f = press - kRate / 10; f < press + kRate / 10; f += kPacket)
            hub.Append(0, packet.data(), packet.size(), qpc_at(f));
        CHECK(hub.GetLosses().late_frames == 0);
    }

    SUBCASE("a packet behind the encode cursor counts as late") {
        AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
        hub.SetSources({{"A", "a"}}, false);
        for (int64_t f = 0; f < kRate; f += kPacket) {
            hub.Append(0, packet.data(), packet.size(), qpc_at(f));
            hub.EncodeThrough(qpc_at(f + kPacket));
        }
        // The source goes quiet for 2 s, so the track moves on without it; then its next packet
        // shows up stamped where it left off (a stalled capture thread delivering old audio).
        hub.EncodeThrough(qpc_at(3 * kRate));
        hub.Append(0, packet.data(), packet.size(), qpc_at(kRate));
        CHECK(hub.GetLosses().late_frames == kPacket);
        CHECK(hub.GetLosses().overrun_frames == 0);
    }
}

TEST_CASE("audio hub: suspended, it drops its encoded audio and memory; resumed, it encodes from now on") {
    constexpr int64_t kFreq   = 10'000'000;
    constexpr int64_t kOrigin = 1'000'000;
    constexpr size_t  kPacket = 480;
    auto qpc_at = [](int64_t frame) { return kOrigin + frame * kFreq / kRate; };
    std::vector<int16_t> packet(kPacket * 2, 1000);
    AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"A", "a"}}, false);
    auto feed = [&](int64_t from, int64_t to) {
        for (int64_t f = from; f < to; f += kPacket) {
            hub.Append(0, packet.data(), packet.size(), qpc_at(f));
            hub.EncodeThrough(qpc_at(f + kPacket));
        }
    };

    feed(0, 2 * kRate);
    CHECK(hub.LiveBytes() > 0);

    hub.SetSuspended(true);
    feed(2 * kRate, 4 * kRate);  // the capture keeps delivering; nothing is encoded
    CHECK(hub.LiveBytes() == 0);
    CHECK(hub.Collect(qpc_at(0), qpc_at(4 * kRate)).empty());

    hub.SetSuspended(false);
    feed(4 * kRate, 6 * kRate);
    CHECK(hub.Collect(qpc_at(0), qpc_at(4 * kRate - 2048)).empty());  // the audio from before the suspend is gone
    const auto tracks = hub.Collect(qpc_at(4 * kRate), qpc_at(6 * kRate));
    REQUIRE(tracks.size() == 1);
    const double seconds = static_cast<double>(tracks[0].frames.size()) * 1024.0 / kRate;
    CHECK(seconds > 1.8);
    CHECK(seconds < 2.1);
    CHECK(hub.GetLosses().late_frames == 0);
}

// A loopback endpoint with nothing playing sends no packets at all (USB earphones, live
// 2026-09-26). The capture thread used to fill that with silence ~300-800 ms behind real time,
// behind the hub's encode cursor, so every idle second counted as a "late" second. Nothing fills
// it now: the hub must skip the gap, keep the audio on either side in place, and lose nothing.
TEST_CASE("audio hub: a source that delivers nothing for 2 s, then resumes, loses nothing and stays in place") {
    constexpr int64_t kFreq   = 10'000'000;
    constexpr int64_t kOrigin = 1'000'000;
    constexpr int64_t kPacket = 480;  // 10 ms
    auto qpc_at = [](int64_t frame) { return kOrigin + frame * kFreq / kRate; };
    auto playing = [](int64_t frame) { return frame < kRate || frame >= 3 * kRate; };  // 0-1 s, 3-4 s

    AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"A", "a"}}, false);
    std::vector<int16_t> packet(kPacket * 2);
    // Every 10 ms of the timeline: the packet that ended 10 ms ago arrives (if the endpoint was
    // playing), and every 20 ms the hub encodes kLagSeconds behind "now", as its thread does.
    for (int64_t now = 2 * kPacket; now <= 4 * kRate + 2 * kPacket; now += kPacket) {
        const int64_t first = now - 2 * kPacket;
        if (playing(first) && first < 4 * kRate) {
            for (int64_t i = 0; i < kPacket; ++i) {
                const int16_t v = static_cast<int16_t>(std::lround(
                    0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * double(first + i) / kRate)));
                packet[i * 2] = packet[i * 2 + 1] = v;
            }
            hub.Append(0, packet.data(), packet.size(), qpc_at(first));
        }
        if (now % (2 * kPacket) == 0) hub.EncodeThrough(qpc_at(now - static_cast<int64_t>(AudioTrackHub::kLagSeconds * kRate)));
    }
    const auto l = hub.GetLosses();
    CHECK(l.late_frames == 0);
    CHECK(l.overrun_frames == 0);
    CHECK(l.failed_frames == 0);

    const auto tracks = hub.Collect(qpc_at(0), qpc_at(4 * kRate));
    REQUIRE(tracks.size() == 1);
    const std::wstring file = MuxWithFixtureVideo(L"hub_idle_gap.mp4", tracks);
    const std::string  errors = DecodeErrors(file);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);

    const Pcm    pcm    = DecodeAudio(file, 0);
    const size_t window = kRate / 10;
    REQUIRE(pcm.Seconds() > 3.9);
    CHECK(ToneAmplitude(pcm, 1000.0, kRate / 2, window) == doctest::Approx(0.5).epsilon(0.06));
    CHECK(Rms(pcm, 2 * kRate, window) < 0.001);
    const auto resumed = FirstFrameAbove(pcm, 0.05, static_cast<size_t>(1.5 * kRate));
    REQUIRE(resumed.has_value());
    CHECK(std::abs(double(*resumed) - 3.0 * kRate) <= 2.0);
    CHECK(ToneAmplitude(pcm, 1000.0, static_cast<size_t>(3.5 * kRate), window) == doctest::Approx(0.5).epsilon(0.06));
}

// A loopback stream that resumes after a silence hands over its whole buffer (200 ms) in one pass,
// restamped by LoopbackStampRepair to end where the pass started (32 logged events,
// each 0-46 ms "arrived behind the encoder" while the hub trailed by 150 ms).
// The hub must trail by more than that buffer, or the head of every sound after a silence is cut.
TEST_CASE("audio hub: a whole loopback buffer arriving at once after a silence lands in place") {
    constexpr int64_t kFreq   = 10'000'000;
    constexpr int64_t kOrigin = 1'000'000;
    constexpr int64_t kPacket = 480;          // 10 ms
    constexpr int64_t kBuffer = kRate / 5;    // the loopback client's 200 ms (audio.cpp)
    constexpr int64_t kResume = 3 * kRate;    // sound comes back at 3 s
    auto qpc_at = [](int64_t frame) { return kOrigin + frame * kFreq / kRate; };
    const int64_t lag = static_cast<int64_t>(AudioTrackHub::kLagSeconds * kRate);

    AudioTrackHub hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"A", "a"}}, false);
    std::vector<int16_t> packet(kPacket * 2);
    auto append = [&](int64_t first) {
        for (int64_t i = 0; i < kPacket; ++i) {
            const int16_t v = static_cast<int16_t>(std::lround(
                0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * double(first + i) / kRate)));
            packet[i * 2] = packet[i * 2 + 1] = v;
        }
        hub.Append(0, packet.data(), packet.size(), qpc_at(first));
    };
    // 0-1 s paced; 1-3 s nothing; at 3.2 s the buffer [3.0, 3.2) arrives in one pass, right after
    // an encode pass (the worst case); then paced again to 4 s.
    for (int64_t now = 2 * kPacket; now <= 4 * kRate + 2 * kPacket; now += kPacket) {
        if (now % (2 * kPacket) == 0) hub.EncodeThrough(qpc_at(now - lag));
        if (now == kResume + kBuffer) {
            for (int64_t f = kResume; f < now; f += kPacket) append(f);
        } else {
            const int64_t first = now - 2 * kPacket;
            if ((first < kRate || first >= kResume + kBuffer) && first < 4 * kRate) append(first);
        }
    }
    const auto l = hub.GetLosses();
    CHECK(l.late_frames == 0);
    CHECK(l.overrun_frames == 0);

    const auto tracks = hub.Collect(qpc_at(0), qpc_at(4 * kRate));
    REQUIRE(tracks.size() == 1);
    const Pcm pcm = DecodeAudio(MuxWithFixtureVideo(L"hub_loopback_burst.mp4", tracks), 0);
    REQUIRE(pcm.Seconds() > 3.9);
    const auto resumed = FirstFrameAbove(pcm, 0.05, static_cast<size_t>(1.5 * kRate));
    REQUIRE(resumed.has_value());
    CHECK(std::abs(double(*resumed) - double(kResume)) <= 2.0);
}

// audio.sources edited while recording (the audio target watcher's SetSources): a source removed
// from the middle of the list must not shift the others, a source added later starts a track of
// its own from now on without counting the time before it as lost, and every track that still
// exists keeps the audio it already holds.
TEST_CASE("audio hub: sources added and removed live keep every other track whole and in place") {
    constexpr int64_t kFreq   = 10'000'000;
    constexpr int64_t kOrigin = 1'000'000;
    constexpr int64_t kPacket = 480;
    auto qpc_at = [](int64_t frame) { return kOrigin + frame * kFreq / kRate; };
    auto tone   = [](double hz, int64_t first, std::vector<int16_t>& packet) {
        for (int64_t i = 0; i < kPacket; ++i) {
            const int16_t v = static_cast<int16_t>(
                std::lround(0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * hz * double(first + i) / kRate)));
            packet[static_cast<size_t>(i) * 2] = packet[static_cast<size_t>(i) * 2 + 1] = v;
        }
    };
    struct Feed {
        size_t slot;
        double hz;
    };
    std::vector<int16_t> packet(kPacket * 2);
    AudioTrackHub        hub(kOrigin, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    auto run = [&](int64_t from, int64_t to, const std::vector<Feed>& feeds) {
        for (int64_t f = from; f < to; f += kPacket) {
            for (const Feed& feed : feeds) {
                tone(feed.hz, f, packet);
                hub.Append(feed.slot, packet.data(), packet.size(), qpc_at(f));
            }
            hub.EncodeThrough(qpc_at(f + kPacket));
        }
    };
    const std::wstring name_prefix = L"hub_live_sources_";

    SUBCASE("one track per source: remove the middle one, add one at the end") {
        const auto s0 = hub.SetSources({{"A", "a", 0.0, AudioTrackSource::kAudibleLoopback, "a"},
                                        {"B", "b", 0.0, AudioTrackSource::kAudibleLoopback, "b"},
                                        {"C", "c", 0.0, AudioTrackSource::kAudibleLoopback, "c"}},
                                       false);
        REQUIRE(s0 == std::vector<size_t>{0, 1, 2});
        run(0, 6 * kRate, {{s0[0], 1000.0}, {s0[1], 500.0}, {s0[2], 1500.0}});

        const auto s1 = hub.SetSources({{"A", "a", 0.0, AudioTrackSource::kAudibleLoopback, "a"},
                                        {"C", "c", 0.0, AudioTrackSource::kAudibleLoopback, "c"},
                                        {"D", "d", 0.0, AudioTrackSource::kAudibleLoopback, "d"}});
        CHECK(s1[0] == s0[0]);
        CHECK(s1[1] == s0[2]);
        CHECK(s1[2] != s0[1]);  // a removed slot is never reused
        CHECK(hub.SourceCount() == 3);
        CHECK(hub.TrackCount() == 3);
        // B's capture is still delivering for a moment (the watcher hasn't stopped it yet): dropped.
        run(6 * kRate, 8 * kRate, {{s1[0], 1000.0}, {s0[1], 500.0}, {s1[1], 1500.0}, {s1[2], 750.0}});

        const auto l = hub.GetLosses();
        CHECK(l.overrun_frames == 0);
        CHECK(l.late_frames == 0);
        CHECK(l.failed_frames == 0);

        const auto tracks = hub.Collect(qpc_at(0), qpc_at(8 * kRate));
        REQUIRE(tracks.size() == 3);
        CHECK(tracks[0].name == "A");
        CHECK(tracks[1].name == "C");
        CHECK(tracks[2].name == "D");
        const std::wstring file = MuxWithFixtureVideo(name_prefix + L"remove_add.mp4", tracks);
        CHECK(DecodeErrors(file).empty());
        const size_t window = kRate / 10;
        for (int t = 0; t < 2; ++t) {
            const Pcm    pcm = DecodeAudio(file, t);
            const double hz  = t == 0 ? 1000.0 : 1500.0;
            CAPTURE(t);
            for (size_t f = kRate / 10; f + window < static_cast<size_t>(7.9 * kRate); f += window) {
                CAPTURE(double(f) / kRate);
                CHECK(ToneAmplitude(pcm, hz, f, window) == doctest::Approx(0.5).epsilon(0.06));
                CHECK(ToneAmplitude(pcm, 500.0, f, window) < 0.01);  // nothing of B leaked in
            }
        }
        const Pcm d = DecodeAudio(file, 2);
        CHECK(Rms(d, 3 * kRate, window) < 0.001);  // D didn't exist yet
        const auto onset = FirstFrameAbove(d, 0.05);
        REQUIRE(onset.has_value());
        // D's encoder opens 256 samples before its tone, and a fresh AAC encoder fades in over its first
        // frame (229 samples here). Placement itself is pinned to 2 samples by the idle-gap test,
        // where the reopened encoder has silence to run on first.
        CHECK_MESSAGE(*onset >= 6 * kRate, "onset " << *onset);
        CHECK_MESSAGE(*onset < 6 * kRate + 1024, "onset " << *onset);
        CHECK(ToneAmplitude(d, 750.0, static_cast<size_t>(7 * kRate), window) == doctest::Approx(0.5).epsilon(0.06));
    }

    SUBCASE("a group that loses a member keeps the audio it already holds") {
        const auto s0 = hub.SetSources({{"A", "g", 0.0, AudioTrackSource::kAudibleLoopback, "a"},
                                        {"B", "g", 0.0, AudioTrackSource::kAudibleLoopback, "b"}},
                                       false);
        run(0, 2 * kRate, {{s0[0], 1000.0}, {s0[1], 500.0}});
        hub.SetSources({{"A", "g", 0.0, AudioTrackSource::kAudibleLoopback, "a"}});
        run(2 * kRate, 4 * kRate, {{s0[0], 1000.0}});

        const auto tracks = hub.Collect(qpc_at(0), qpc_at(4 * kRate));
        REQUIRE(tracks.size() == 1);
        CHECK(tracks[0].name == "A");
        const Pcm    pcm    = DecodeAudio(MuxWithFixtureVideo(name_prefix + L"group.mp4", tracks), 0);
        const size_t window = kRate / 10;
        // Mixed at 1/2 each while both were in it, A alone at full level after.
        CHECK(ToneAmplitude(pcm, 500.0, kRate, window) == doctest::Approx(0.25).epsilon(0.06));
        CHECK(ToneAmplitude(pcm, 1000.0, kRate, window) == doctest::Approx(0.25).epsilon(0.06));
        CHECK(ToneAmplitude(pcm, 500.0, 3 * kRate, window) < 0.01);
        CHECK(ToneAmplitude(pcm, 1000.0, 3 * kRate, window) == doctest::Approx(0.5).epsilon(0.06));
        CHECK(hub.GetLosses().overrun_frames == 0);
    }
}

TEST_CASE("audio hub: LastAudibleQpc stamps audible packets only") {
    constexpr int64_t kFreq = 10'000'000;
    AudioTrackHub hub(0, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"A", "a"}, {"B", "b"}}, false);  // never Start()ed: synthetic timeline

    std::vector<int16_t> quiet(960, 0);
    for (size_t i = 0; i < quiet.size(); ++i) quiet[i] = static_cast<int16_t>(i % 2 ? 20 : -20);  // under -60 dBFS
    hub.Append(0, quiet.data(), quiet.size(), kFreq);
    CHECK(hub.LastAudibleQpc(0) == 0);

    std::vector<int16_t> loud = quiet;
    loud[500] = 4000;
    hub.Append(0, loud.data(), loud.size(), kFreq + kFreq / 100);
    CHECK(hub.LastAudibleQpc(0) == kFreq + kFreq / 100);
    hub.Append(0, quiet.data(), quiet.size(), kFreq + 2 * kFreq / 100);
    CHECK(hub.LastAudibleQpc(0) == kFreq + kFreq / 100);  // a quiet packet doesn't clear it

    CHECK(hub.LastAudibleQpc(1) == 0);
    CHECK(hub.LastAudibleQpc(9) == 0);  // out of range
}

TEST_CASE("audio hub: an input source's noise floor doesn't read as audible") {
    constexpr int64_t kFreq = 10'000'000;
    AudioTrackHub hub(0, kFreq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    AudioTrackSource mic{"Mic", "mic", 0.0, AudioTrackSource::kAudibleInput};
    hub.SetSources({mic}, false);

    std::vector<int16_t> floor(960);  // -47 dBFS peaks, this laptop mic's measured floor
    for (size_t i = 0; i < floor.size(); ++i) floor[i] = static_cast<int16_t>(i % 2 ? 146 : -146);
    hub.Append(0, floor.data(), floor.size(), kFreq);
    CHECK(hub.LastAudibleQpc(0) == 0);

    std::vector<int16_t> speech = floor;
    speech[300] = 3000;  // about -21 dBFS
    hub.Append(0, speech.data(), speech.size(), 2 * kFreq);
    CHECK(hub.LastAudibleQpc(0) == 2 * kFreq);
}

}  // namespace ombra::test
