// BuildAvcC (mux.cpp), headless, no GPU: real SPS/PPS bytes from the x264 fixture, not synthetic
// data, so the avcC fields assert against what an actual encoder emits. save_tests.cpp only
// exercises this transitively (ffprobe on a finished file); these check the AVCDecoderConfiguration
// Record's bytes directly. Also MuxWriteMp4Stream, the pull interface an open clip writes through.

#include <doctest/doctest.h>

#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/clip_file.h"
#include "ombra/mux.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ombra::test {

namespace {

// Locates the next 4-byte Annex-B start code (0x00 0x00 0x00 0x01) at or after `from`. The fixture
// is documented (h264_fixture.h) to use 4-byte start codes throughout, and real SPS/PPS NALs use
// emulation prevention specifically so this raw byte sequence can't recur inside NAL payload — so a
// plain scan is enough to split the fixture's `sps_pps` blob for building test inputs, independent
// of mux.cpp's own SplitAnnexB (which is what's under test via BuildAvcC).
size_t FindStartCode4(const std::vector<uint8_t>& data, size_t from) {
    for (size_t p = from; p + 4 <= data.size(); ++p) {
        if (data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1) return p;
    }
    return std::string::npos;
}

struct SplitSpsPps {
    std::vector<uint8_t> sps;  // NAL header byte included, start code excluded
    std::vector<uint8_t> pps;
};

SplitSpsPps SplitFixture(const std::vector<uint8_t>& sps_pps_4byte) {
    REQUIRE(sps_pps_4byte.size() > 4);
    REQUIRE(sps_pps_4byte[0] == 0);
    REQUIRE(sps_pps_4byte[1] == 0);
    REQUIRE(sps_pps_4byte[2] == 0);
    REQUIRE(sps_pps_4byte[3] == 1);
    const size_t second_start = FindStartCode4(sps_pps_4byte, 5);
    REQUIRE(second_start != std::string::npos);

    SplitSpsPps out;
    out.sps.assign(sps_pps_4byte.begin() + 4, sps_pps_4byte.begin() + second_start);
    out.pps.assign(sps_pps_4byte.begin() + second_start + 4, sps_pps_4byte.end());
    return out;
}

std::vector<uint8_t> WithStartCodes(const std::vector<uint8_t>& sps, bool sps_3byte,
                                    const std::vector<uint8_t>& pps, bool pps_3byte) {
    std::vector<uint8_t> out;
    auto append_start_code = [&](bool three_byte) {
        out.push_back(0);
        out.push_back(0);
        if (!three_byte) out.push_back(0);
        out.push_back(1);
    };
    append_start_code(sps_3byte);
    out.insert(out.end(), sps.begin(), sps.end());
    append_start_code(pps_3byte);
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

}  // namespace

TEST_CASE("mux: BuildAvcC fields match the fixture's real SPS/PPS byte for byte") {
    const H264Fixture&   fx    = StandardFixture();
    const SplitSpsPps     parts = SplitFixture(fx.sps_pps);
    const std::vector<uint8_t> avcc = BuildAvcC(fx.sps_pps);

    REQUIRE(avcc.size() >= 6);
    CHECK(avcc[0] == 1);              // configurationVersion
    CHECK(avcc[1] == parts.sps[1]);   // AVCProfileIndication
    CHECK(avcc[2] == parts.sps[2]);   // profile_compatibility
    CHECK(avcc[3] == parts.sps[3]);   // AVCLevelIndication
    CHECK(avcc[4] == 0xFF);
    CHECK(avcc[5] == 0xE1);

    size_t p = 6;
    REQUIRE(p + 2 <= avcc.size());
    const size_t sps_len = (size_t(avcc[p]) << 8) | avcc[p + 1];
    p += 2;
    REQUIRE(sps_len == parts.sps.size());
    REQUIRE(p + sps_len <= avcc.size());
    CHECK(std::vector<uint8_t>(avcc.begin() + p, avcc.begin() + p + sps_len) == parts.sps);
    p += sps_len;

    REQUIRE(p < avcc.size());
    CHECK(avcc[p] == 1);  // numOfPictureParameterSets
    ++p;

    REQUIRE(p + 2 <= avcc.size());
    const size_t pps_len = (size_t(avcc[p]) << 8) | avcc[p + 1];
    p += 2;
    REQUIRE(pps_len == parts.pps.size());
    REQUIRE(p + pps_len <= avcc.size());
    CHECK(std::vector<uint8_t>(avcc.begin() + p, avcc.begin() + p + pps_len) == parts.pps);
    p += pps_len;
    CHECK(p == avcc.size());
}

// mux.cpp:105-107 (SplitAnnexB) claims to handle 3- and 4-byte start codes mixed in the same
// buffer. Nothing exercised that claim: every other caller (Encoder::SequenceParams(), NVENC's
// per-frame output) happens to use 4-byte codes consistently. BuildAvcC's output must not depend
// on which start-code length separated the NALs it was given — same NAL bytes in, same avcC out.
TEST_CASE("mux: BuildAvcC is unaffected by 3-byte vs 4-byte start codes, mixed in one buffer") {
    const H264Fixture& fx       = StandardFixture();
    const SplitSpsPps   parts    = SplitFixture(fx.sps_pps);
    const std::vector<uint8_t> baseline = BuildAvcC(fx.sps_pps);

    const std::vector<uint8_t> sps3_pps4 =
        WithStartCodes(parts.sps, /*sps_3byte=*/true, parts.pps, /*pps_3byte=*/false);
    CHECK(BuildAvcC(sps3_pps4) == baseline);

    const std::vector<uint8_t> sps4_pps3 =
        WithStartCodes(parts.sps, /*sps_3byte=*/false, parts.pps, /*pps_3byte=*/true);
    CHECK(BuildAvcC(sps4_pps3) == baseline);

    const std::vector<uint8_t> sps3_pps3 =
        WithStartCodes(parts.sps, /*sps_3byte=*/true, parts.pps, /*pps_3byte=*/true);
    CHECK(BuildAvcC(sps3_pps3) == baseline);
}

// The byte-at-a-time splitter ConvertAnnexBSample used before the memchr one, kept as the oracle.
std::vector<uint8_t> ReferenceConvert(const std::vector<uint8_t>& d) {
    const size_t size = d.size();
    auto sc = [&](size_t p) -> size_t {
        if (p + 3 <= size && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1) return 3;
        if (p + 4 <= size && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1) return 4;
        return 0;
    };
    std::vector<std::pair<size_t, size_t>> nals;
    size_t p = 0, start = SIZE_MAX;
    while (p < size) { if (size_t l = sc(p)) { start = p + l; p = start; break; } ++p; }
    if (start != SIZE_MAX) {
        while (p < size) {
            if (size_t l = sc(p)) { nals.push_back({start, p - start}); p += l; start = p; }
            else ++p;
        }
        if (start < size) nals.push_back({start, size - start});
    }
    std::vector<uint8_t> out;
    for (auto [off, len] : nals) {
        const uint8_t t = d[off] & 0x1F;
        if (t == 7 || t == 8) continue;
        const uint8_t be[4] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)};
        out.insert(out.end(), be, be + 4);
        out.insert(out.end(), d.begin() + off, d.begin() + off + len);
    }
    return out;
}

TEST_CASE("mux: ConvertAnnexBSample splits exactly like the byte-at-a-time scan") {
    uint64_t rng = 7;
    auto next = [&] { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(rng >> 33); };
    for (int iter = 0; iter < 2000; ++iter) {
        std::vector<uint8_t> d;
        const int pieces = 1 + next() % 6;
        for (int k = 0; k < pieces; ++k) {
            switch (next() % 6) {  // 3/4-byte codes, extra zeros, back-to-back codes, SPS/PPS types
                case 0: d.insert(d.end(), {0, 0, 1}); break;
                case 1: d.insert(d.end(), {0, 0, 0, 1}); break;
                case 2: d.insert(d.end(), {0, 0, 0, 0, 1}); break;
                case 3: d.insert(d.end(), {0, 0, 1, 0x67}); break;
                case 4: d.insert(d.end(), {0, 0, 1, 0x68, 0, 0, 1}); break;
                default: break;
            }
            const size_t n = next() % 40;
            for (size_t i = 0; i < n; ++i) d.push_back(uint8_t(next() % 4 == 0 ? 0 : next()));
        }
        std::vector<uint8_t> got;
        ConvertAnnexBSample(d, got);
        INFO("iteration ", iter);
        REQUIRE(got == ReferenceConvert(d));
    }
}

TEST_CASE("mux: a streamed write matches the list write and takes audio after the last video sample") {
    const H264Fixture& fx = StandardFixture();
    std::vector<MuxAu> aus;
    // A skipped tick at 60 (a duplicate the pacer dropped) must survive as a longer duration.
    for (uint64_t i = 0; i < 90; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i < 60 ? i : i + 1});
    MuxAudioTrack track;
    track.name = "Game";
    track.asc  = {0x11, 0x90};  // AAC-LC, 48 kHz, stereo
    for (int k = 0; k < 10; ++k) track.frames.push_back({std::vector<uint8_t>(32, static_cast<uint8_t>(k))});

    const std::wstring listed = TestOutputPath(L"mux_listed.mp4");
    REQUIRE(MuxWriteMp4(listed, fx.sps_pps, fx.width, fx.height, fx.fps, aus, {track}));

    const std::wstring streamed = TestOutputPath(L"mux_streamed.mp4");
    size_t pulled = 0, pulled_at_audio = 0;
    REQUIRE(MuxWriteMp4Stream(
        streamed, fx.sps_pps, fx.width, fx.height, fx.fps,
        [&](MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
            if (pulled == aus.size()) return MuxPull::kEnd;
            sample = {aus[pulled].is_keyframe, aus[pulled].tick_index};
            bytes  = aus[pulled++].annexb;
            return MuxPull::kSample;
        },
        [&, tracks = std::vector<MuxAudioTrack>{track}](MuxTail& tail) {
            pulled_at_audio = pulled;
            tail.audio = &tracks;
            return true;
        },
        VideoCodec::H264));
    CHECK(pulled_at_audio == aus.size());

    ClipInfo a, b;
    REQUIRE(ReadClipInfo(listed, a));
    REQUIRE(ReadClipInfo(streamed, b));
    CHECK(b.duration_s == doctest::Approx(91.0 / fx.fps));
    CHECK(b.duration_s == a.duration_s);
    CHECK(b.video_bytes == a.video_bytes);
    CHECK(b.keyframes_s == a.keyframes_s);
    CHECK(b.audio_track_names == std::vector<std::string>{"Game"});
}

TEST_CASE("mux: a streamed write that fails, or holds nothing, leaves no file") {
    const H264Fixture& fx = StandardFixture();
    auto exists = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };

    const std::wstring failed = TestOutputPath(L"mux_stream_failed.mp4");
    size_t i = 0;
    CHECK_FALSE(MuxWriteMp4Stream(
        failed, fx.sps_pps, fx.width, fx.height, fx.fps,
        [&](MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
            if (i == 10) return MuxPull::kFail;  // the clip's pin broke midway
            sample = {fx.aus[i].idr, i};
            bytes  = fx.aus[i++].annexb;
            return MuxPull::kSample;
        },
        nullptr, VideoCodec::H264));
    CHECK_FALSE(exists(failed));

    const std::wstring empty = TestOutputPath(L"mux_stream_empty.mp4");
    CHECK_FALSE(MuxWriteMp4Stream(
        empty, fx.sps_pps, fx.width, fx.height, fx.fps,
        [](MuxVideoSample&, std::vector<uint8_t>&) { return MuxPull::kEnd; }, nullptr, VideoCodec::H264));
    CHECK_FALSE(exists(empty));

    const std::wstring backwards = TestOutputPath(L"mux_stream_backwards.mp4");
    i = 0;
    CHECK_FALSE(MuxWriteMp4Stream(
        backwards, fx.sps_pps, fx.width, fx.height, fx.fps,
        [&](MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
            if (i == 3) return MuxPull::kEnd;
            sample = {fx.aus[i].idr, i == 2 ? 0u : i + 1};  // a tick going back
            bytes  = fx.aus[i++].annexb;
            return MuxPull::kSample;
        },
        nullptr, VideoCodec::H264));
    CHECK_FALSE(exists(backwards));

    const std::wstring no_audio = TestOutputPath(L"mux_stream_audio_failed.mp4");
    i = 0;
    CHECK_FALSE(MuxWriteMp4Stream(
        no_audio, fx.sps_pps, fx.width, fx.height, fx.fps,
        [&](MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
            if (i == 5) return MuxPull::kEnd;
            sample = {fx.aus[i].idr, i};
            bytes  = fx.aus[i++].annexb;
            return MuxPull::kSample;
        },
        [](MuxTail&) { return false; }, VideoCodec::H264));
    CHECK_FALSE(exists(no_audio));

    const std::wstring no_key = TestOutputPath(L"mux_stream_no_keyframe.mp4");
    CHECK_FALSE(MuxWriteMp4Stream(
        no_key, fx.sps_pps, fx.width, fx.height, fx.fps,
        [&](MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
            sample = {false, 0};  // a stream that starts mid-GOP fails the file, not the engine
            bytes  = fx.aus[1].annexb;
            return MuxPull::kSample;
        },
        nullptr, VideoCodec::H264));
    CHECK_FALSE(exists(no_key));
}

TEST_CASE("mux: equal ticks are a zero-duration sample, as before") {
    const H264Fixture& fx = StandardFixture();
    std::vector<MuxAu> aus;
    for (uint64_t i = 0; i < 30; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i == 10 ? 9 : i});
    const std::wstring path = TestOutputPath(L"mux_equal_ticks.mp4");
    REQUIRE(MuxWriteMp4(path, fx.sps_pps, fx.width, fx.height, fx.fps, aus));
    ClipInfo info;
    REQUIRE(ReadClipInfo(path, info));
    CHECK(info.duration_s == doctest::Approx(30.0 / fx.fps));
}

TEST_CASE("mux: markers become chapters and a metadata tag that ffprobe reads") {
    const H264Fixture& fx = StandardFixture();
    std::vector<MuxAu> aus;
    for (uint64_t i = 0; i < 90; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
    const uint64_t video_ms = 90 * 1000 / fx.fps;
    MuxAudioTrack track;
    track.name = "Game";
    track.asc  = {0x11, 0x90};
    for (int k = 0; k < 10; ++k) track.frames.push_back({std::vector<uint8_t>(32, static_cast<uint8_t>(k))});

    MuxMarkers markers;
    // Out of order, one past the end (dropped), a UTF-8 title.
    markers.chapters = {{video_ms / 2, "Death \xC2\xB7 Tool"}, {video_ms + 5, "Too late"}, {500, "Kill \xC2\xB7 Tool"}};
    markers.json     = R"({"v":1,"markers":[{"type":"kill"}]})";
    const std::wstring path = TestOutputPath(L"mux_chapters.mp4");
    REQUIRE(MuxWriteMp4(path, fx.sps_pps, fx.width, fx.height, fx.fps, aus, {track}, VideoCodec::H264, {}, markers));

    const ProbeInfo info = Probe(path);
    REQUIRE_MESSAGE(info.ok, info.error);
    REQUIRE(info.chapters.size() == 3);
    CHECK(info.chapters[0].title == kStartChapter);  // nothing at 0, so the forced first chapter
    CHECK(info.chapters[0].start_s == doctest::Approx(0.0));
    CHECK(info.chapters[1].title == "Kill \xC2\xB7 Tool");
    CHECK(info.chapters[1].start_s == doctest::Approx(0.5));
    CHECK(info.chapters[1].end_s == doctest::Approx(static_cast<double>(video_ms / 2) / 1000.0));
    CHECK(info.chapters[2].title == "Death \xC2\xB7 Tool");
    CHECK(info.chapters[2].end_s == doctest::Approx(static_cast<double>(video_ms) / 1000.0));
    REQUIRE(info.format_tags.count("markers") == 1);
    CHECK(info.format_tags.at("markers") == markers.json);
    // The chapter track isn't a playable stream: one video, one audio, the rest data.
    CHECK(info.Of("video").size() == 1);
    CHECK(info.Of("audio").size() == 1);

    // Ombra's own reader still sees the same clip.
    ClipInfo clip;
    REQUIRE(ReadClipInfo(path, clip));
    CHECK(clip.duration_s == doctest::Approx(90.0 / fx.fps));
    CHECK(clip.audio_track_names == std::vector<std::string>{"Game"});

    // A chapter at 0 is the first one; no markers writes neither track nor tag.
    MuxMarkers at_zero;
    at_zero.chapters = {{0, "Round start"}};
    const std::wstring zero = TestOutputPath(L"mux_chapter_zero.mp4");
    REQUIRE(MuxWriteMp4(zero, fx.sps_pps, fx.width, fx.height, fx.fps, aus, {}, VideoCodec::H264, {}, at_zero));
    const ProbeInfo zero_info = Probe(zero);
    REQUIRE(zero_info.chapters.size() == 1);
    CHECK(zero_info.chapters[0].title == "Round start");
    CHECK(DecodeErrors(zero).empty());  // the placeholder AAC above wouldn't decode, so checked here
    const std::wstring none = TestOutputPath(L"mux_no_chapters.mp4");
    REQUIRE(MuxWriteMp4(none, fx.sps_pps, fx.width, fx.height, fx.fps, aus));
    const ProbeInfo none_info = Probe(none);
    CHECK(none_info.chapters.empty());
    CHECK(none_info.streams.size() == 1);
    CHECK(none_info.format_tags.count("markers") == 0);
}

// --- Mp4FragmentedWriter ------------------------------------------------------------------------

// The file's top-level box types, in order (a 64-bit largesize understood).
std::vector<std::string> TopLevelBoxes(const std::wstring& path) {
    std::vector<std::string> boxes;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return boxes;
    uint64_t pos = 0;
    for (;;) {
        uint8_t h[16];
        if (_fseeki64(f, static_cast<long long>(pos), SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) break;
        uint64_t size = (uint64_t(h[0]) << 24) | (uint64_t(h[1]) << 16) | (uint64_t(h[2]) << 8) | h[3];
        if (size == 1) {
            if (fread(h + 8, 1, 8, f) != 8) break;
            size = 0;
            for (int k = 8; k < 16; ++k) size = (size << 8) | h[k];
        }
        boxes.emplace_back(reinterpret_cast<const char*>(h + 4), 4);
        if (size < 8) break;
        pos += size;
    }
    fclose(f);
    return boxes;
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

MuxAudioTrack FakeAacTrack(const char* name) {
    MuxAudioTrack track;
    track.name = name;
    track.asc  = {0x11, 0x90};  // AAC-LC, 48 kHz, stereo
    return track;
}

// Not AAC that decodes: bytes that tell frames apart, sizes varying so a slot mixup shows.
MuxAudioFrame FakeAacFrame(size_t i) {
    MuxAudioFrame f;
    f.data.assign(32 + i % 7, static_cast<uint8_t>(i));
    f.data.front() = 0xFF;  // never all zeros: Recover takes that for a sample the disk never got
    return f;
}

// Feeds the fixture's first `frames` AUs (ticks from `ticks`, or the index), plus one fake AAC
// frame to audio track 0 every other video frame when the writer has audio.
void Feed(Mp4FragmentedWriter& w, const H264Fixture& fx, size_t frames, bool audio,
          const std::vector<uint64_t>& ticks = {}) {
    for (size_t i = 0; i < frames; ++i) {
        const uint64_t tick = ticks.empty() ? i : ticks[i];
        REQUIRE(w.AddVideo({fx.aus[i].idr, tick}, fx.aus[i].annexb));
        if (audio && i % 2 == 0)
            REQUIRE(w.AddAudio(0, FakeAacFrame(i)));
    }
}

TEST_CASE("mux: a finished fragmented recording is the plain file a save writes") {
    const H264Fixture& fx = StandardFixture();
    REQUIRE(fx.aus.size() >= 200);
    std::vector<uint64_t> ticks;
    for (uint64_t i = 0; i < 200; ++i) ticks.push_back(i < 130 ? i : i + 2);  // a gap mid-fragment
    std::vector<MuxAu> aus;
    MuxAudioTrack listed_track = FakeAacTrack("Game");
    for (size_t i = 0; i < 200; ++i) {
        aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, ticks[i]});
        if (i % 2 == 0) listed_track.frames.push_back(FakeAacFrame(i));
    }
    const std::wstring listed = TestOutputPath(L"mux_frag_listed.mp4");
    REQUIRE(MuxWriteMp4(listed, fx.sps_pps, fx.width, fx.height, fx.fps, aus, {listed_track}));

    const std::wstring path = TestOutputPath(L"mux_frag_finished.mp4");
    {
        Mp4FragmentedWriter w;
        // A second track that never gets a frame is left out of the finished file.
        REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {},
                       {FakeAacTrack("Game"), FakeAacTrack("Mic")}));
        Feed(w, fx, 200, true, ticks);
        CHECK(w.VideoTicks() == ticks.back());
        CHECK(w.FileBytes() > 0);
        REQUIRE(w.Finish(1, {}));
    }
    CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});

    ClipInfo a, b;
    REQUIRE(ReadClipInfo(listed, a));
    REQUIRE(ReadClipInfo(path, b));
    CHECK(b.duration_s == doctest::Approx(202.0 / fx.fps));
    CHECK(b.duration_s == a.duration_s);
    CHECK(b.video_bytes == a.video_bytes);
    CHECK(b.keyframes_s == a.keyframes_s);
    CHECK(b.audio_track_names == std::vector<std::string>{"Game"});

    const ProbeInfo info = Probe(path);
    REQUIRE_MESSAGE(info.ok, info.error);
    CHECK(info.Of("video").size() == 1);
    CHECK(info.Of("audio").size() == 1);
    // Every packet's bytes and time, read through the final moov's chunk tables.
    const std::vector<std::string> video_packets = PacketHashes(listed, L"v:0");
    const std::vector<std::string> audio_packets = PacketHashes(listed, L"a:0");
    CHECK(video_packets.size() == 200);
    CHECK(audio_packets.size() == 100);
    CHECK(PacketHashes(path, L"v:0") == video_packets);
    CHECK(PacketHashes(path, L"a:0") == audio_packets);

    // Discard after a Finish that worked leaves the file alone.
    Mp4FragmentedWriter again;
    const std::wstring kept = TestOutputPath(L"mux_frag_kept.mp4");
    REQUIRE(again.Open(kept, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
    Feed(again, fx, 30, false);
    REQUIRE(again.Finish(1, {}));
    again.Discard();
    CHECK(FileExists(kept));
}

TEST_CASE("mux: an abandoned fragmented recording's audio is the start of the finished one's") {
    const H264Fixture& fx = StandardFixture();
    const std::wstring finished = TestOutputPath(L"mux_frag_av_finished.mp4");
    const std::wstring abandoned = TestOutputPath(L"mux_frag_av_abandoned.mp4");
    for (const std::wstring* path : {&finished, &abandoned}) {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(*path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {FakeAacTrack("Game")}));
        Feed(w, fx, 150, true);
        if (path == &finished) REQUIRE(w.Finish(1, {}));
    }
    const std::vector<std::string> video_all = PacketHashes(finished, L"v:0");
    const std::vector<std::string> audio_all = PacketHashes(finished, L"a:0");
    const std::vector<std::string> video_part = PacketHashes(abandoned, L"v:0");
    const std::vector<std::string> audio_part = PacketHashes(abandoned, L"a:0");
    REQUIRE(video_all.size() == 150);
    REQUIRE(audio_all.size() == 75);
    // Two fragments of 60 frames went out, each with the 30 audio frames that came with it.
    REQUIRE(video_part.size() == 120);
    REQUIRE(audio_part.size() == 60);
    CHECK(std::equal(video_part.begin(), video_part.end(), video_all.begin()));
    CHECK(std::equal(audio_part.begin(), audio_part.end(), audio_all.begin()));
}

TEST_CASE("mux: a fragmented recording writes audio on its own when video stops") {
    const H264Fixture& fx = StandardFixture();
    const std::wstring path = TestOutputPath(L"mux_frag_stalled.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {FakeAacTrack("Game")}));
        Feed(w, fx, 61, false);  // one fragment out; frame 60 waits for its duration
        // Two seconds at 48 kHz is 93.75 frames: the 94th goes out with the 93 before it.
        for (size_t i = 0; i < 100; ++i) REQUIRE(w.AddAudio(0, FakeAacFrame(i)));
    }
    CHECK(PacketHashes(path, L"a:0").size() == 94);
    CHECK(PacketHashes(path, L"v:0").size() == 60);
}

TEST_CASE("mux: a fragmented recording's per-fragment sync to disk (timing)") {
    // About 6 MB a second, a high-bitrate 1080p60 recording: fake 100 KB slices, never decoded.
    const H264Fixture& fx = StandardFixture();
    std::vector<uint8_t> key = {0, 0, 0, 1, 0x65}, delta = {0, 0, 0, 1, 0x41};
    key.resize(100 * 1024, 0x5A);
    delta.resize(100 * 1024, 0x5A);
    const std::wstring path = TestOutputPath(L"mux_frag_sync.mp4");
    Mp4FragmentedWriter w;
    REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
    std::vector<double> flush_ms;
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    for (uint64_t i = 0; i < 600; ++i) {
        const bool idr = i % 120 == 0;
        QueryPerformanceCounter(&a);
        REQUIRE(w.AddVideo({idr, i}, idr ? key : delta));
        QueryPerformanceCounter(&b);
        if (i > 0 && i % 60 == 0) flush_ms.push_back(1000.0 * double(b.QuadPart - a.QuadPart) / double(freq.QuadPart));
    }
    REQUIRE(w.Finish(1, {}));
    std::sort(flush_ms.begin(), flush_ms.end());
    MESSAGE("fragment write + FlushFileBuffers of ~6 MB: p50 " << flush_ms[flush_ms.size() / 2] << " ms, worst "
                                                               << flush_ms.back() << " ms (" << flush_ms.size() << ")");
    CHECK(flush_ms.back() < 1000.0);  // well inside the second the next fragment takes to fill
}

TEST_CASE("mux: a fragmented recording decodes, finished or abandoned mid-write") {
    const H264Fixture& fx = StandardFixture();
    REQUIRE(fx.aus.size() >= 150);

    const std::wstring finished = TestOutputPath(L"mux_frag_video.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(finished, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
        Feed(w, fx, 150, false);
        REQUIRE(w.Finish(1, {}));
    }
    CHECK(DecodeErrors(finished).empty());
    const VideoFrames all = ProbeVideoFrames(finished);
    CHECK(all.count == 150);
    CHECK(all.first_is_key);

    // A crash: the writer goes away without Finish. Fragments end before each IDR (every 60
    // frames), so the last 30 frames were never written; the rest plays.
    const std::wstring abandoned = TestOutputPath(L"mux_frag_abandoned.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(abandoned, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
        Feed(w, fx, 150, false);
    }
    REQUIRE(FileExists(abandoned));
    const std::vector<std::string> boxes = TopLevelBoxes(abandoned);
    CHECK(boxes == std::vector<std::string>{"ftyp", "free", "moov", "moof", "mdat", "moof", "mdat"});
    CHECK(DecodeErrors(abandoned).empty());
    const VideoFrames part = ProbeVideoFrames(abandoned);
    CHECK(part.count == 120);
    CHECK(part.first_is_key);
    CHECK(part.last_pts_s == doctest::Approx(119.0 / fx.fps));
}

TEST_CASE("mux: a fragmented recording keeps chapters and the markers tag") {
    const H264Fixture& fx = StandardFixture();
    const std::wstring path = TestOutputPath(L"mux_frag_chapters.mp4");
    MuxMarkers markers;
    markers.chapters = {{0, "Start"}, {1500, "Kill"}};
    markers.json     = R"({"v":1})";
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {FakeAacTrack("Game")}));
        Feed(w, fx, 180, true);
        REQUIRE(w.Finish(1, markers));
    }
    const ProbeInfo info = Probe(path);
    REQUIRE_MESSAGE(info.ok, info.error);
    REQUIRE(info.chapters.size() == 2);
    CHECK(info.chapters[1].title == "Kill");
    CHECK(info.chapters[1].start_s == doctest::Approx(1.5));
    CHECK(info.chapters[1].end_s == doctest::Approx(3.0));
    REQUIRE(info.format_tags.count("markers") == 1);
    CHECK(info.format_tags.at("markers") == markers.json);
    CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
}

TEST_CASE("mux: a fragmented recording that can't go on keeps what it wrote") {
    const H264Fixture& fx = StandardFixture();
    const std::wstring path = TestOutputPath(L"mux_frag_back.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
        Feed(w, fx, 70, false);
        CHECK_FALSE(w.AddVideo({false, 10}, fx.aus[70].annexb));  // a tick going back
        CHECK_FALSE(w.AddVideo({false, 71}, fx.aus[71].annexb));  // and nothing after it
        CHECK_FALSE(w.Finish(1, {}));
    }
    REQUIRE(FileExists(path));
    CHECK(ProbeVideoFrames(path).count == 60);  // the fragment before the IDR at 60

    // The first sample must be a keyframe.
    const std::wstring nokey = TestOutputPath(L"mux_frag_nokey.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(nokey, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
        CHECK_FALSE(w.AddVideo({false, 0}, fx.aus[1].annexb));
        CHECK_FALSE(w.Finish(1, {}));
    }
    CHECK_FALSE(FileExists(nokey));
}

TEST_CASE("mux: a fragmented recording with no video, or discarded, leaves no file") {
    const H264Fixture& fx = StandardFixture();
    const std::wstring empty = TestOutputPath(L"mux_frag_empty.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(empty, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {FakeAacTrack("Game")}));
        REQUIRE(w.AddAudio(0, {std::vector<uint8_t>(32, 1)}));
        CHECK_FALSE(w.Finish(1, {}));
    }
    CHECK_FALSE(FileExists(empty));

    const std::wstring dropped = TestOutputPath(L"mux_frag_dropped.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(dropped, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
        Feed(w, fx, 90, false);
        w.Discard();
        CHECK_FALSE(w.AddVideo({false, 90}, fx.aus[90].annexb));
    }
    CHECK_FALSE(FileExists(dropped));

    Mp4FragmentedWriter bad;
    CHECK_FALSE(bad.Open(TestOutputPath(L"mux_frag_bad.mp4"), {0, 0, 0, 1}, fx.width, fx.height, fx.fps,
                         VideoCodec::H264, {}, {}));
}

// --- Past 4 GB ------------------------------------------------------------------------------------

namespace {

constexpr uint64_t kFourGb = uint64_t{1} << 32;

// Deletes the file when the test leaves, a failed REQUIRE included: each is several GB.
struct DeleteOnExit {
    std::wstring path;
    ~DeleteOnExit() { DeleteFileW(path.c_str()); }
};

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (uint64_t{a.nFileSizeHigh} << 32) | a.nFileSizeLow;
}

std::vector<uint8_t> ReadAt(const std::wstring& path, uint64_t pos, size_t n) {
    std::vector<uint8_t> bytes(n);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    const bool ok = _fseeki64(f, static_cast<long long>(pos), SEEK_SET) == 0 && fread(bytes.data(), 1, n, f) == n;
    fclose(f);
    return ok ? bytes : std::vector<uint8_t>{};
}

uint32_t Be32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t{b[at]} << 24) | (uint32_t{b[at + 1]} << 16) | (uint32_t{b[at + 2]} << 8) | b[at + 3];
}

// What a file finished with ftyp, mdat, moov says about its size fields: the mdat's 32-bit size
// field (1: a 64-bit largesize follows) and how often each chunk offset box type names a moov box.
struct PlainLayout {
    uint32_t mdat_size32 = 0;
    int      stco = 0, co64 = 0;
};
PlainLayout ReadPlainLayout(const std::wstring& path) {
    PlainLayout l;
    const uint64_t size = FileSize(path);
    const std::vector<uint8_t> ftyp = ReadAt(path, 0, 8);
    if (ftyp.empty()) return l;
    const uint64_t mdat_at = Be32(ftyp, 0);
    const std::vector<uint8_t> mdat = ReadAt(path, mdat_at, 16);
    if (mdat.empty()) return l;
    l.mdat_size32 = Be32(mdat, 0);
    uint64_t mdat_size = l.mdat_size32;
    if (mdat_size == 1) {
        mdat_size = (uint64_t{Be32(mdat, 8)} << 32) | Be32(mdat, 12);
    }
    const uint64_t moov_at = mdat_at + mdat_size;
    if (moov_at >= size || size - moov_at > (64u << 20)) return l;
    const std::vector<uint8_t> moov = ReadAt(path, moov_at, static_cast<size_t>(size - moov_at));
    for (size_t p = 0; p + 4 <= moov.size(); ++p) {
        if (std::memcmp(&moov[p], "stco", 4) == 0) ++l.stco;
        if (std::memcmp(&moov[p], "co64", 4) == 0) ++l.co64;
    }
    return l;
}

// Video sample `i` past 4 GB in about 2100 frames: the fixture's AU, then a filler NAL (type 12,
// which decoders skip) of about 2 MiB whose length changes with `i`, so a sample read from the
// wrong offset or with the wrong size shows in its packet hash.
constexpr size_t kBigFrames   = 2160;  // 36 s at 60 fps, a fragment past the 4 GB mark
constexpr size_t kCrashFrame  = 2100;  // a keyframe: the fragment before it ends past 4 GB
void BigAu(const H264Fixture& fx, size_t i, std::vector<uint8_t>& out) {
    const auto& au = fx.aus[i % fx.aus.size()].annexb;
    out.assign(au.begin(), au.end());
    out.insert(out.end(), {0, 0, 0, 1, 0x0C});
    out.resize(out.size() + (size_t{2} << 20) + (i % 97) * 13, 0xFF);
    out.push_back(0x80);  // rbsp_trailing_bits
}

}  // namespace

// Writes about 9 GB (a recording, then a save, each deleted after): only with --large.
TEST_CASE("mux: past 4 GB a recording and a save use co64 and a 64-bit mdat, and every sample reads back" *
          doctest::test_suite("large")) {
    const H264Fixture& fx = StandardFixture();
    REQUIRE(fx.aus.size() % fx.gop == 0);
    REQUIRE(kCrashFrame % fx.gop == 0);
    REQUIRE(kBigFrames % fx.gop == 0);
    ULARGE_INTEGER free_bytes{};
    REQUIRE(GetDiskFreeSpaceExW(TestOutputPath(L"").c_str(), &free_bytes, nullptr, nullptr));
    REQUIRE_MESSAGE(free_bytes.QuadPart > (uint64_t{6} << 30), "the test output folder's disk needs 6 GB free");

    MuxMarkers markers;
    markers.chapters = {{0, "Start"}, {35500, "Late"}};  // the chapter samples land past 4 GB too
    std::vector<uint8_t> bytes;
    uint64_t video_bytes = 0;

    // A recording, probed as a crash would leave it once a fragment ends past 4 GB, then finished.
    std::vector<std::string> video_packets, audio_packets;
    {
        const std::wstring path = TestOutputPath(L"mux_large_recording.mp4");
        DeleteOnExit remove{path};
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {FakeAacTrack("Game")}));
        for (size_t i = 0; i < kBigFrames; ++i) {
            BigAu(fx, i, bytes);
            REQUIRE(w.AddVideo({fx.aus[i % fx.aus.size()].idr, i}, bytes));
            if (i == kCrashFrame) {
                REQUIRE(FileSize(path) > kFourGb);
                const std::vector<std::string> v = PacketHashes(path, L"v:0");
                const std::vector<std::string> a = PacketHashes(path, L"a:0");
                CHECK(v.size() == kCrashFrame);
                CHECK(a.size() == kCrashFrame / 2);
                video_packets = v;
                audio_packets = a;
            }
            if (i % 2 == 0) REQUIRE(w.AddAudio(0, FakeAacFrame(i)));
        }
        REQUIRE(w.Finish(1, markers));

        REQUIRE(FileSize(path) > kFourGb);
        CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
        const PlainLayout layout = ReadPlainLayout(path);
        CHECK(layout.mdat_size32 == 1);
        CHECK(layout.co64 == 3);  // video, audio, chapters: each has a chunk past 4 GB
        CHECK(layout.stco == 0);

        const std::vector<std::string> v = PacketHashes(path, L"v:0");
        const std::vector<std::string> a = PacketHashes(path, L"a:0");
        REQUIRE(v.size() == kBigFrames);
        REQUIRE(a.size() == kBigFrames / 2);
        // The fragments' own offsets (moof + trun) and the final moov's 64-bit chunk offsets agree.
        CHECK(std::equal(video_packets.begin(), video_packets.end(), v.begin()));
        CHECK(std::equal(audio_packets.begin(), audio_packets.end(), a.begin()));
        video_packets = v;
        audio_packets = a;

        const VideoFrames frames = ProbeVideoFrames(path);
        CHECK(frames.count == kBigFrames);
        CHECK(frames.first_is_key);
        CHECK(frames.last_pts_s == doctest::Approx(double(kBigFrames - 1) / fx.fps));
        const ProbeInfo info = Probe(path);
        REQUIRE_MESSAGE(info.ok, info.error);
        REQUIRE(info.chapters.size() == 2);
        CHECK(info.chapters[1].start_s == doctest::Approx(35.5));

        ClipInfo clip;
        REQUIRE(ReadClipInfo(path, clip));
        for (size_t i = 0; i < kBigFrames; ++i) {
            BigAu(fx, i, bytes);
            std::vector<uint8_t> avcc;
            ConvertAnnexBSample(bytes, avcc);
            video_bytes += avcc.size();
        }
        CHECK(clip.video_bytes == video_bytes);
        CHECK(clip.duration_s == doctest::Approx(double(kBigFrames) / fx.fps));
        CHECK(clip.keyframes_s.size() == kBigFrames / fx.gop);
    }

    // A save of the same footage: one video chunk at the start (stco), audio and chapters after
    // more than 4 GB of it (co64), the mdat's size patched to the 64-bit form.
    {
        const std::wstring path = TestOutputPath(L"mux_large_save.mp4");
        DeleteOnExit remove{path};
        MuxAudioTrack track = FakeAacTrack("Game");
        for (size_t i = 0; i < kBigFrames; i += 2) track.frames.push_back(FakeAacFrame(i));
        const std::vector<MuxAudioTrack> audio = {track};
        size_t next = 0;
        const bool ok = MuxWriteMp4Stream(
            path, fx.sps_pps, fx.width, fx.height, fx.fps,
            [&](MuxVideoSample& sample, std::vector<uint8_t>& out) {
                if (next == kBigFrames) return MuxPull::kEnd;
                sample = {fx.aus[next % fx.aus.size()].idr, next};
                BigAu(fx, next++, out);
                return MuxPull::kSample;
            },
            [&](MuxTail& tail) {
                tail.audio   = &audio;
                tail.markers = markers;
                return true;
            },
            VideoCodec::H264);
        REQUIRE(ok);

        REQUIRE(FileSize(path) > kFourGb);
        CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
        const PlainLayout layout = ReadPlainLayout(path);
        CHECK(layout.mdat_size32 == 1);
        CHECK(layout.co64 == 2);
        CHECK(layout.stco == 1);
        // Laid out differently from the recording, the same samples.
        CHECK(PacketHashes(path, L"v:0") == video_packets);
        CHECK(PacketHashes(path, L"a:0") == audio_packets);
        const ProbeInfo info = Probe(path);
        REQUIRE_MESSAGE(info.ok, info.error);
        REQUIRE(info.chapters.size() == 2);
        CHECK(info.chapters[1].start_s == doctest::Approx(35.5));
        ClipInfo clip;
        REQUIRE(ReadClipInfo(path, clip));
        CHECK(clip.video_bytes == video_bytes);
        CHECK(clip.duration_s == doctest::Approx(double(kBigFrames) / fx.fps));
    }

    // The same recording abandoned at the keyframe after its last frame (that fragment is the last
    // written, its start past 4 GB in every track), made plain by Recover.
    {
        const std::wstring path = TestOutputPath(L"mux_large_recovered.mp4");
        DeleteOnExit remove{path};
        {
            Mp4FragmentedWriter w;
            REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {},
                           {FakeAacTrack("Game")}));
            for (size_t i = 0; i <= kBigFrames; ++i) {
                BigAu(fx, i, bytes);
                REQUIRE(w.AddVideo({fx.aus[i % fx.aus.size()].idr, i}, bytes));
                if (i % 2 == 0 && i < kBigFrames) REQUIRE(w.AddAudio(0, FakeAacFrame(i)));
            }
        }
        const Mp4FragmentedWriter::Recovered r = Mp4FragmentedWriter::Recover(path, markers);
        REQUIRE_MESSAGE(r.result == Mp4FragmentedWriter::Recovery::kRecovered, r.detail);
        CHECK(r.cut_bytes == 0);
        CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
        const PlainLayout layout = ReadPlainLayout(path);
        CHECK(layout.mdat_size32 == 1);
        CHECK(layout.co64 == 3);  // video, audio, chapters
        CHECK(layout.stco == 0);
        CHECK(PacketHashes(path, L"v:0") == video_packets);
        CHECK(PacketHashes(path, L"a:0") == audio_packets);
        CHECK(Probe(path).chapters.size() == 2);
    }
}

// --- Recover --------------------------------------------------------------------------------------

namespace {

using Recovery = Mp4FragmentedWriter::Recovery;

struct Box {
    std::string type;
    uint64_t    pos = 0, size = 0;
};
std::vector<Box> BoxList(const std::wstring& path) {
    std::vector<Box> boxes;
    const uint64_t   end = FileSize(path);
    for (uint64_t pos = 0; pos + 8 <= end;) {
        const std::vector<uint8_t> h = ReadAt(path, pos, 8);
        uint64_t size = Be32(h, 0);
        if (size == 1) {
            const std::vector<uint8_t> l = ReadAt(path, pos + 8, 8);
            size = (uint64_t{Be32(l, 0)} << 32) | Be32(l, 4);
        }
        boxes.push_back({std::string(reinterpret_cast<const char*>(h.data() + 4), 4), pos, size});
        if (size < 8) break;
        pos += size;
    }
    return boxes;
}

bool CutAt(const std::wstring& path, uint64_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(size);
    const bool ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

// Writes `bytes` at `pos`, past the end included (the gap reads as zeros).
bool WriteAt(const std::wstring& path, uint64_t pos, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(pos);
    DWORD      wrote = 0;
    const bool ok    = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) &&
                    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) &&
                    wrote == bytes.size();
    CloseHandle(h);
    return ok;
}

std::wstring CopyOf(const std::wstring& src, const wchar_t* name) {
    const std::wstring dst = TestOutputPath(name);
    REQUIRE(CopyFileW(src.c_str(), dst.c_str(), FALSE));
    return dst;
}

// A recording of the fixture's first `frames` frames with one audio track, left as a crash
// leaves it: a fragment ends before each IDR (every 60 frames), the frames after the last unwritten.
std::wstring Abandoned(const wchar_t* name, size_t frames) {
    const H264Fixture& fx   = StandardFixture();
    const std::wstring path = TestOutputPath(name);
    Mp4FragmentedWriter w;
    REQUIRE(w.Open(path, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {FakeAacTrack("Game")}));
    Feed(w, fx, frames, true);
    return path;
}

bool IsPrefix(const std::vector<std::string>& part, const std::vector<std::string>& all) {
    return part.size() <= all.size() && std::equal(part.begin(), part.end(), all.begin());
}

}  // namespace

TEST_CASE("mux: Recover makes an abandoned recording plain, every whole fragment and the markers in it") {
    const H264Fixture& fx   = StandardFixture();
    const std::wstring path = Abandoned(L"mux_recover.mp4", 150);
    const std::vector<std::string> video = PacketHashes(path, L"v:0");
    const std::vector<std::string> audio = PacketHashes(path, L"a:0");
    REQUIRE(video.size() == 120);
    REQUIRE(audio.size() == 60);
    const uint64_t fragmented_size = FileSize(path);

    MuxMarkers markers;
    markers.chapters = {{1000, "Kill"}};
    markers.json     = R"({"v":1})";
    const Mp4FragmentedWriter::Recovered r = Mp4FragmentedWriter::Recover(path, markers);
    REQUIRE_MESSAGE(r.result == Recovery::kRecovered, r.detail);
    CHECK(r.video_ms == 2000);
    CHECK(r.cut_bytes == 0);
    CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
    CHECK(PacketHashes(path, L"v:0") == video);
    CHECK(PacketHashes(path, L"a:0") == audio);
    CHECK(DecodeErrors(path, L"0:v").empty());
    const VideoFrames frames = ProbeVideoFrames(path);
    CHECK(frames.count == 120);
    CHECK(frames.first_is_key);
    ClipInfo clip;
    REQUIRE(ReadClipInfo(path, clip));
    CHECK(clip.duration_s == doctest::Approx(120.0 / fx.fps));
    CHECK(clip.audio_track_names == std::vector<std::string>{"Game"});
    const ProbeInfo info = Probe(path);
    REQUIRE_MESSAGE(info.ok, info.error);
    REQUIRE(info.chapters.size() == 2);
    CHECK(info.chapters[0].title == kStartChapter);
    CHECK(info.chapters[1].title == "Kill");
    CHECK(info.chapters[1].start_s == doctest::Approx(1.0));
    REQUIRE(info.format_tags.count("markers") == 1);
    CHECK(info.format_tags.at("markers") == markers.json);
    CHECK(FileSize(path) > fragmented_size);  // only added to: the moov and chapter samples

    // A second run finds it done and leaves it alone.
    const uint64_t plain_size = FileSize(path);
    CHECK(Mp4FragmentedWriter::Recover(path, {}).result == Recovery::kAlreadyPlain);
    CHECK(FileSize(path) == plain_size);

    // Finish's file is plain already too.
    const std::wstring finished = TestOutputPath(L"mux_recover_finished.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(finished, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
        Feed(w, fx, 90, false);
        REQUIRE(w.Finish(1, {}));
    }
    CHECK(Mp4FragmentedWriter::Recover(finished, {}).result == Recovery::kAlreadyPlain);
}

TEST_CASE("mux: Recover cuts a torn last fragment, wherever it tore") {
    // Three fragments of 60 frames; the frames after 180 never went out.
    const std::wstring source = Abandoned(L"mux_recover_src.mp4", 190);
    const std::vector<std::string> video = PacketHashes(source, L"v:0");
    REQUIRE(video.size() == 180);
    const std::vector<Box> boxes = BoxList(source);
    REQUIRE(boxes.size() == 9);
    const Box& moof = boxes[7];
    const Box& mdat = boxes[8];
    REQUIRE(moof.type == "moof");
    REQUIRE(mdat.type == "mdat");
    const uint64_t whole = mdat.pos + mdat.size;
    REQUIRE(whole == FileSize(source));

    const auto check = [&](const std::wstring& path, size_t frames, uint64_t cut) {
        const Mp4FragmentedWriter::Recovered r = Mp4FragmentedWriter::Recover(path, {});
        REQUIRE_MESSAGE(r.result == Recovery::kRecovered, r.detail);
        CHECK(r.cut_bytes == cut);
        CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
        const std::vector<std::string> got = PacketHashes(path, L"v:0");
        CHECK(got.size() == frames);
        CHECK(IsPrefix(got, video));
        CHECK(DecodeErrors(path, L"0:v").empty());
    };

    // Cut inside the last fragment: in its moof's header, its moof, its mdat's header, its data.
    for (const uint64_t at : {moof.pos + 4, moof.pos + 40, moof.pos + moof.size, mdat.pos + 12,
                              mdat.pos + mdat.size / 2, whole - 1}) {
        CAPTURE(at - moof.pos);
        const std::wstring path = CopyOf(source, L"mux_recover_cut.mp4");
        REQUIRE(CutAt(path, at));
        check(path, 120, at - moof.pos);
    }

    // Whole boxes over data that never reached the disk: a power cut can leave zeros there.
    {
        const std::wstring path = CopyOf(source, L"mux_recover_zeros.mp4");
        REQUIRE(WriteAt(path, mdat.pos + 8, std::vector<uint8_t>(static_cast<size_t>(mdat.size - 8), 0)));
        check(path, 120, whole - moof.pos);
    }
    // ...or after the last box: a size-0 box ends the walk.
    {
        const std::wstring path = CopyOf(source, L"mux_recover_tail.mp4");
        REQUIRE(WriteAt(path, whole, std::vector<uint8_t>(4096, 0)));
        check(path, 180, 4096);
    }
    // Garbage that isn't a moof.
    {
        const std::wstring path = CopyOf(source, L"mux_recover_junk.mp4");
        REQUIRE(WriteAt(path, whole, {0, 0, 0, 16, 'm', 'o', 'o', 'f', 1, 2, 3, 4, 5, 6, 7, 8}));
        check(path, 180, 16);
    }
    // Nothing torn: every fragment stays.
    check(CopyOf(source, L"mux_recover_whole.mp4"), 180, 0);
}

TEST_CASE("mux: Recover finishes a Finish the power cut stopped halfway") {
    const std::wstring source = Abandoned(L"mux_recover_half_src.mp4", 150);
    const std::vector<std::string> video = PacketHashes(source, L"v:0");
    const std::vector<std::string> audio = PacketHashes(source, L"a:0");
    const std::vector<Box> boxes = BoxList(source);
    REQUIRE(boxes.size() >= 3);
    REQUIRE(boxes[1].type == "free");
    const uint64_t placeholder = boxes[1].pos;
    const std::vector<uint8_t> free_box = ReadAt(source, placeholder, 16);
    MuxMarkers markers;
    markers.chapters = {{500, "Kill"}};

    // The mdat header reached the disk, the moov it points at didn't (an older Finish synced them
    // in the wrong order).
    {
        const std::wstring path = CopyOf(source, L"mux_recover_header.mp4");
        const uint64_t     size = FileSize(path) + 900 - placeholder;
        REQUIRE(WriteAt(path, placeholder, {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size),
                                            'm', 'd', 'a', 't'}));
        const Mp4FragmentedWriter::Recovered r = Mp4FragmentedWriter::Recover(path, markers);
        REQUIRE_MESSAGE(r.result == Recovery::kRecovered, r.detail);
        CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
        CHECK(PacketHashes(path, L"v:0") == video);
        CHECK(PacketHashes(path, L"a:0") == audio);
        CHECK(Probe(path).chapters.size() == 2);
    }
    // The chapter samples and the moov reached the disk, the mdat header didn't.
    {
        const std::wstring path = CopyOf(source, L"mux_recover_moov.mp4");
        REQUIRE(Mp4FragmentedWriter::Recover(path, markers).result == Recovery::kRecovered);
        const uint64_t plain_size = FileSize(path);
        REQUIRE(WriteAt(path, placeholder, free_box));
        const Mp4FragmentedWriter::Recovered r = Mp4FragmentedWriter::Recover(path, markers);
        REQUIRE_MESSAGE(r.result == Recovery::kRecovered, r.detail);
        CHECK(r.cut_bytes == plain_size - FileSize(source));
        CHECK(FileSize(path) == plain_size);
        CHECK(TopLevelBoxes(path) == std::vector<std::string>{"ftyp", "mdat", "moov"});
        CHECK(PacketHashes(path, L"v:0") == video);
        CHECK(PacketHashes(path, L"a:0") == audio);
        CHECK(Probe(path).chapters.size() == 2);
    }
}

TEST_CASE("mux: Recover leaves alone what isn't an abandoned recording, and deletes one with no video") {
    const H264Fixture& fx = StandardFixture();
    CHECK(Mp4FragmentedWriter::Recover(TestOutputPath(L"mux_recover_none.mp4"), {}).result == Recovery::kMissing);

    // A save: plain, never fragmented.
    std::vector<MuxAu> aus;
    for (size_t i = 0; i < 30; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
    const std::wstring save = TestOutputPath(L"mux_recover_save.mp4");
    REQUIRE(MuxWriteMp4(save, fx.sps_pps, fx.width, fx.height, fx.fps, aus));
    const uint64_t save_size = FileSize(save);
    CHECK(Mp4FragmentedWriter::Recover(save, {}).result == Recovery::kNotOurs);
    CHECK(FileSize(save) == save_size);

    const std::wstring text = TestOutputPath(L"mux_recover_text.mp4");
    {
        FILE* f = nullptr;
        REQUIRE(_wfopen_s(&f, text.c_str(), L"wb") == 0);
        fputs("not an mp4 at all", f);
        fclose(f);
    }
    CHECK(Mp4FragmentedWriter::Recover(text, {}).result == Recovery::kNotOurs);
    CHECK(FileSize(text) == 17);

    // Open in another program: untouched, for the next start.
    const std::wstring busy = Abandoned(L"mux_recover_busy.mp4", 70);
    const uint64_t     busy_size = FileSize(busy);
    {
        HANDLE h = CreateFileW(busy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        CHECK(Mp4FragmentedWriter::Recover(busy, {}).result == Recovery::kInUse);
        CloseHandle(h);
    }
    CHECK(FileSize(busy) == busy_size);
    CHECK(Mp4FragmentedWriter::Recover(busy, {}).result == Recovery::kRecovered);

    // Audio went out on its own (video stalled), never a video fragment: deleted, as Finish would.
    const std::wstring audio_only = TestOutputPath(L"mux_recover_audio.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(audio_only, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {},
                       {FakeAacTrack("Game")}));
        for (size_t i = 1; i <= 100; ++i) REQUIRE(w.AddAudio(0, FakeAacFrame(i)));
    }
    REQUIRE(TopLevelBoxes(audio_only).size() == 5);  // ftyp, free, moov, moof, mdat
    CHECK(Mp4FragmentedWriter::Recover(audio_only, {}).result == Recovery::kNoVideo);
    CHECK_FALSE(FileExists(audio_only));

    // Opened, nothing written after the header.
    const std::wstring header_only = TestOutputPath(L"mux_recover_header_only.mp4");
    {
        Mp4FragmentedWriter w;
        REQUIRE(w.Open(header_only, fx.sps_pps, fx.width, fx.height, fx.fps, VideoCodec::H264, {}, {}));
    }
    CHECK(Mp4FragmentedWriter::Recover(header_only, {}).result == Recovery::kNoVideo);
    CHECK_FALSE(FileExists(header_only));
}

}  // namespace ombra::test
