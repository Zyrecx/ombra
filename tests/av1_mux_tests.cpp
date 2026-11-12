// AV1 in MP4 (av1.cpp + mux.cpp), headless, no GPU: a real SVT-AV1 stream through MuxWriteMp4,
// checked by ffprobe/ffmpeg (dav1d), which share no code with the muxer.

#include <doctest/doctest.h>

#include "support/av1_fixture.h"
#include "support/media_probe.h"
#include "support/process.h"
#include "support/test_env.h"

#include "ombra/av1.h"
#include "ombra/clip_file.h"
#include "ombra/mux.h"
#include "ombra/util.h"

#include <windows.h>

#include <sstream>
#include <string>
#include <vector>

namespace ombra::test {

namespace {

std::vector<MuxAu> FixtureAus(const Av1Fixture& fx) {
    std::vector<MuxAu> aus;
    for (size_t i = 0; i < fx.aus.size(); ++i) {
        MuxAu au;
        au.annexb      = fx.aus[i].obus;
        au.is_keyframe = fx.aus[i].key;
        au.tick_index  = i;
        aus.push_back(std::move(au));
    }
    return aus;
}

// Packet indices ffprobe flags as key frames, in file order.
std::vector<size_t> KeyPackets(const std::wstring& file) {
    const ProcessResult r = RunProcess(Quote(FfprobePath()) +
                                       L" -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 " +
                                       Quote(file));
    REQUIRE(r.exit_code == 0);
    std::vector<size_t> keys;
    std::istringstream lines(r.out);
    std::string line;
    for (size_t i = 0; std::getline(lines, line); ++i)
        if (!line.empty() && line[0] == 'K') keys.push_back(i);
    return keys;
}

// The rows ffprobe says the container crops off the bottom (clap), empty for none.
std::string CropBottom(const std::wstring& file) {
    const ProcessResult r = RunProcess(Quote(FfprobePath()) +
                                       L" -v error -select_streams v:0 -show_entries stream_side_data=crop_bottom -of csv=p=0 " +
                                       Quote(file));
    REQUIRE(r.exit_code == 0);
    std::string out = r.out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace

TEST_CASE("av1: av1C carries the sequence header's profile, bit depth and subsampling") {
    const Av1Fixture& fx = StandardAv1Fixture();
    const std::vector<uint8_t> av1c = BuildAv1C(fx.seq_header.data(), fx.seq_header.size());
    REQUIRE(av1c.size() == 4 + fx.seq_header.size());
    CHECK(av1c[0] == 0x81);                  // marker + version 1
    CHECK((av1c[1] >> 5) == 0);              // Main profile
    CHECK((av1c[2] & 0x80) == 0);            // tier 0
    CHECK((av1c[2] & 0x60) == 0);            // 8-bit
    CHECK((av1c[2] & 0x10) == 0);            // not monochrome
    CHECK((av1c[2] & 0x0C) == 0x0C);         // 4:2:0
    CHECK(std::vector<uint8_t>(av1c.begin() + 4, av1c.end()) == fx.seq_header);
}

TEST_CASE("av1: the coded size comes from the sequence header") {
    const Av1Fixture& fx = StandardAv1Fixture();
    uint32_t w = 0, h = 0;
    REQUIRE(Av1CodedSize(fx.seq_header.data(), fx.seq_header.size(), w, h));
    CHECK(w == 320);
    CHECK(h == 180);
    const uint8_t temporal_delimiter[] = {0x12, 0x00};
    CHECK_FALSE(Av1CodedSize(temporal_delimiter, sizeof(temporal_delimiter), w, h));
}

TEST_CASE("av1: a buffer with no sequence header gives no av1C") {
    const uint8_t temporal_delimiter[] = {0x12, 0x00};
    CHECK(BuildAv1C(temporal_delimiter, sizeof(temporal_delimiter)).empty());
    const uint8_t truncated[] = {0x0A, 0x0B, 0x00, 0x00};  // sequence header claiming 11 bytes
    CHECK(BuildAv1C(truncated, sizeof(truncated)).empty());
}

TEST_CASE("av1: samples lose their temporal delimiters and keep everything else") {
    const Av1Fixture& fx = StandardAv1Fixture();
    const auto& tu = fx.aus[1].obus;
    REQUIRE(tu.size() > 2);
    REQUIRE(tu[0] == 0x12);  // the unit starts with a temporal delimiter (type 2, has_size)
    std::vector<uint8_t> out;
    REQUIRE(FilterAv1Sample(tu.data(), tu.size(), out));
    CHECK(out == std::vector<uint8_t>(tu.begin() + 2, tu.end()));  // TD is 2 bytes: header + size 0

    const uint8_t padding_then_td[] = {0x7A, 0x01, 0xFF, 0x12, 0x00};
    out.clear();
    REQUIRE(FilterAv1Sample(padding_then_td, sizeof(padding_then_td), out));
    CHECK(out.empty());

    const uint8_t forbidden_bit[] = {0x92, 0x00};
    CHECK_FALSE(FilterAv1Sample(forbidden_bit, sizeof(forbidden_bit), out));
}

TEST_CASE("av1: MuxWriteMp4 writes an av01 track that decodes cleanly with the right key frames") {
    const Av1Fixture& fx = StandardAv1Fixture();
    const std::wstring file = TestOutputPath(L"av1\\fixture.mp4");
    REQUIRE(EnsureParentDir(file));
    REQUIRE(MuxWriteMp4(file, fx.seq_header, fx.width, fx.height, fx.fps, FixtureAus(fx), {}, VideoCodec::AV1));

    const ProbeInfo probe = Probe(file);
    REQUIRE_MESSAGE(probe.ok, probe.error);
    const auto video = probe.Of("video");
    REQUIRE(video.size() == 1);
    CHECK(video[0].codec_name == "av1");
    CHECK(video[0].width == 320);
    CHECK(video[0].height == 180);
    CHECK(video[0].color_primaries == "bt709");
    CHECK(video[0].color_range == "tv");

    const std::string errors = DecodeErrors(file);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);
    const VideoFrames frames = ProbeVideoFrames(file);
    CHECK(frames.count == 240);
    CHECK(frames.first_is_key);
    CHECK(KeyPackets(file) == std::vector<size_t>{0, 60, 120, 180});
}

TEST_CASE("av1: a padded picture gets a clean aperture that ReadClipInfo and a trim keep") {
    const Av1Fixture& fx = StandardAv1Fixture();
    const std::wstring file = TestOutputPath(L"av1\\clap.mp4");
    REQUIRE(EnsureParentDir(file));
    // As AMF pads: the image is the top 320x176 of a 320x180 coded picture.
    REQUIRE(MuxWriteMp4(file, fx.seq_header, fx.width, fx.height, fx.fps, FixtureAus(fx), {}, VideoCodec::AV1,
                        MuxPicture{320, 176}));
    CHECK(DecodeErrors(file).empty());
    ClipInfo info;
    REQUIRE(ReadClipInfo(file, info));
    CHECK(info.width == 320);
    CHECK(info.height == 180);
    CHECK(info.picture_width == 320);
    CHECK(info.picture_height == 176);
    CHECK(CropBottom(file) == "4");  // ffmpeg reads the clap (and decodes 320x176)

    const std::wstring trimmed = TestOutputPath(L"av1\\clap_trim.mp4");
    REQUIRE(TrimClip(file, trimmed, 2.0, 6.0).ok);
    ClipInfo after;
    REQUIRE(ReadClipInfo(trimmed, after));
    CHECK(after.height == 180);
    CHECK(after.picture_height == 176);
    CHECK(CropBottom(trimmed) == "4");

    // Unpadded: no clap, the picture is the coded size.
    const std::wstring plain = TestOutputPath(L"av1\\noclap.mp4");
    REQUIRE(MuxWriteMp4(plain, fx.seq_header, fx.width, fx.height, fx.fps, FixtureAus(fx), {}, VideoCodec::AV1,
                        MuxPicture{320, 180}));
    ClipInfo plain_info;
    REQUIRE(ReadClipInfo(plain, plain_info));
    CHECK(plain_info.picture_width == 320);
    CHECK(plain_info.picture_height == 180);
    CHECK(CropBottom(plain).empty());
}

TEST_CASE("av1: a save window starting at a later key frame decodes from its first frame") {
    const Av1Fixture& fx = StandardAv1Fixture();
    std::vector<MuxAu> aus = FixtureAus(fx);
    aus.erase(aus.begin(), aus.begin() + 120);  // what a wrapped ring hands the muxer
    const std::wstring file = TestOutputPath(L"av1\\tail.mp4");
    REQUIRE(EnsureParentDir(file));
    REQUIRE(MuxWriteMp4(file, fx.seq_header, fx.width, fx.height, fx.fps, aus, {}, VideoCodec::AV1));
    CHECK(DecodeErrors(file).empty());
    CHECK(ProbeVideoFrames(file).count == 120);
    CHECK(KeyPackets(file) == std::vector<size_t>{0, 60});
}

TEST_CASE("av1: an unparseable sequence header fails the save and leaves no file") {
    const Av1Fixture& fx = StandardAv1Fixture();
    const std::wstring file = TestOutputPath(L"av1\\bad_header.mp4");
    REQUIRE(EnsureParentDir(file));
    DeleteFileW(file.c_str());
    const std::vector<uint8_t> not_a_header = {0x12, 0x00};
    CHECK_FALSE(MuxWriteMp4(file, not_a_header, fx.width, fx.height, fx.fps, FixtureAus(fx), {}, VideoCodec::AV1));
    CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
}

}  // namespace ombra::test
