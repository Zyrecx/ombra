// Reading and keyframe-trimming saved clips (clip_file.cpp), headless, no GPU. Sources are built the
// way a save builds them (MuxWriteMp4 over the fixtures, AAC from AudioTrackHub on a synthetic
// timeline, so the first audio frame carries an edit-list lead-in); results are judged by ffprobe
// and ffmpeg, which share no code with the reader or the muxer.

#include <doctest/doctest.h>

#include "support/amf_caps.h"
#include "support/audio_analysis.h"
#include "support/av1_fixture.h"
#include "support/h264_fixture.h"
#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/audio_tracks.h"
#include "ombra/clip_file.h"
#include "ombra/clip_ops.h"
#include "ombra/clip_reencode.h"
#include "ombra/encode_target.h"
#include "ombra/mux.h"
#include "ombra/util.h"

#include <windows.h>

#include <cmath>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <numbers>
#include <vector>

namespace ombra::test {

namespace {

constexpr uint32_t kRate = 48000;
constexpr double   kToneAt = 2.5;  // video time the Desktop track's tone starts at

// 4 s of fixture video (60 fps, keyframes at 0/1/2/3 s) with "Desktop" (a 1 kHz tone from 2.5 s)
// and "Mic" (silence, ending `mic_until` s into the video if set, as a track whose source went idle
// does). Video starts 0.3 s plus 7 samples into the audio timeline, off the AAC grid.
std::wstring MakeSource(const std::wstring& name, VideoCodec codec, double mic_until = 0.0) {
    const int64_t freq = QpcFrequency();
    const int64_t origin = freq * 1000;
    auto frame_qpc = [&](uint64_t f) { return origin + static_cast<int64_t>(f) * freq / kRate; };
    const uint64_t video_start = static_cast<uint64_t>(0.3 * kRate) + 7;

    AudioTrackHub hub(origin, freq, 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"Desktop", "desktop", 0.0}, {"Mic", "mic", 0.0}}, false);
    const uint64_t tone_from = video_start + static_cast<uint64_t>(kToneAt * kRate);
    const uint64_t end = video_start + 4 * kRate + kRate / 2;
    std::vector<int16_t> packet(480 * 2), silence(480 * 2, 0);
    for (uint64_t f = 0; f < end; f += 480) {
        for (size_t i = 0; i < 480; ++i) {
            const uint64_t k = f + i;
            const double   s = k < tone_from ? 0.0 : 0.6 * std::sin(2.0 * std::numbers::pi * 1000.0 * double(k) / kRate);
            packet[i * 2] = packet[i * 2 + 1] = static_cast<int16_t>(std::lround(s * 32767.0));
        }
        hub.Append(0, packet.data(), packet.size(), frame_qpc(f));
        hub.Append(1, silence.data(), silence.size(), frame_qpc(f));
    }
    hub.EncodeThrough(frame_qpc(end));
    std::vector<MuxAudioTrack> audio = hub.Collect(frame_qpc(video_start), frame_qpc(video_start + 4 * kRate));
    REQUIRE(audio.size() == 2);
    REQUIRE(audio[0].skip_samples != 0);
    if (mic_until > 0) audio[1].frames.resize(static_cast<size_t>(mic_until * kRate / 1024));

    const std::wstring path = TestOutputPath(L"clip_file\\" + name);
    CreateDirectoryW(TestOutputPath(L"clip_file").c_str(), nullptr);
    std::vector<MuxAu> aus;
    if (codec == VideoCodec::H264) {
        const H264Fixture& fx = StandardFixture();
        for (size_t i = 0; i < fx.aus.size(); ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
        REQUIRE(MuxWriteMp4(path, fx.sps_pps, fx.width, fx.height, fx.fps, aus, audio, codec));
    } else {
        const Av1Fixture& fx = StandardAv1Fixture();
        for (size_t i = 0; i < fx.aus.size(); ++i) aus.push_back({fx.aus[i].obus, fx.aus[i].key, i});
        REQUIRE(MuxWriteMp4(path, fx.seq_header, fx.width, fx.height, fx.fps, aus, audio, codec));
    }
    return path;
}

void CheckTrimmed(const std::wstring& file, double expect_duration, double tone_at) {
    const std::string errors = DecodeErrors(file);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);

    const ProbeInfo probe = Probe(file);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 2);
    CHECK(audio[0].title == "Desktop");
    CHECK(audio[1].title == "Mic");

    const VideoFrames frames = ProbeVideoFrames(file);
    CHECK(frames.first_is_key);
    CHECK(frames.first_pts_s == doctest::Approx(0.0));
    CHECK(frames.count == static_cast<size_t>(std::lround(expect_duration * 60)));

    // In sync: the tone starts where it did relative to the video, to within two samples.
    const Pcm  pcm = DecodeAudio(file, 0);
    const auto onset = FirstFrameAbove(pcm, 0.05);
    REQUIRE(onset.has_value());
    CHECK(std::abs(double(*onset) - tone_at * kRate) <= 2.0);
}

}  // namespace

TEST_CASE("clip file: ReadClipInfo reports codec, size, rate, keyframes and track names") {
    for (VideoCodec codec : {VideoCodec::H264, VideoCodec::AV1}) {
        CAPTURE(VideoCodecName(codec));
        const std::wstring src = MakeSource(codec == VideoCodec::AV1 ? L"info_av1.mp4" : L"info_h264.mp4", codec);
        ClipInfo info;
        std::string error;
        REQUIRE_MESSAGE(ReadClipInfo(src, info, &error), error);
        CHECK(info.codec == codec);
        CHECK(info.width == 320);
        CHECK(info.height == 180);
        CHECK(info.fps == doctest::Approx(60.0));
        CHECK(info.duration_s == doctest::Approx(4.0));
        REQUIRE(info.keyframes_s.size() == 4);
        for (size_t i = 0; i < 4; ++i) CHECK(info.keyframes_s[i] == doctest::Approx(double(i)));
        CHECK(info.audio_track_names == std::vector<std::string>{"Desktop", "Mic"});
    }
}

TEST_CASE("clip file: a trim starts on the keyframe before the request and keeps audio in sync") {
    for (VideoCodec codec : {VideoCodec::H264, VideoCodec::AV1}) {
        CAPTURE(VideoCodecName(codec));
        const bool av1 = codec == VideoCodec::AV1;
        const std::wstring src = MakeSource(av1 ? L"src_av1.mp4" : L"src_h264.mp4", codec);

        SUBCASE("mid-GOP start snaps back to 2.0 s") {
            const std::wstring dst = TestOutputPath(av1 ? L"clip_file\\mid_av1.mp4" : L"clip_file\\mid_h264.mp4");
            const TrimResult r = TrimClip(src, dst, 2.2, 3.5);
            REQUIRE_MESSAGE(r.ok, r.error);
            CHECK(r.start_s == doctest::Approx(2.0));
            CHECK(r.end_s == doctest::Approx(3.5));
            CheckTrimmed(dst, 1.5, kToneAt - 2.0);
        }
        SUBCASE("a start on a keyframe stays there; an end past the clip clamps") {
            const std::wstring dst = TestOutputPath(av1 ? L"clip_file\\key_av1.mp4" : L"clip_file\\key_h264.mp4");
            const TrimResult r = TrimClip(src, dst, 1.0, 99.0);
            REQUIRE_MESSAGE(r.ok, r.error);
            CHECK(r.start_s == doctest::Approx(1.0));
            CHECK(r.end_s == doctest::Approx(4.0));
            CheckTrimmed(dst, 3.0, kToneAt - 1.0);
        }
    }
}

// Clips' "End here" ends the range at the end of the frame on screen: the frame starting exactly at
// the end time is left out, the one before it kept.
TEST_CASE("clip file: a trim ending on a frame boundary keeps the frames before it and not the one at it") {
    const std::wstring src = MakeSource(L"end_boundary.mp4", VideoCodec::H264);
    const std::wstring dst = TestOutputPath(L"clip_file\\end_boundary_out.mp4");
    const TrimResult r = TrimClip(src, dst, 1.0, (120.0 + 1.0) / 60.0);  // through frame 120
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(ProbeVideoFrames(dst).count == 61);  // frames 60..120
}

TEST_CASE("clip file: a first frame held for two ticks keeps the clip's frame rate through a trim") {
    // What an overloaded encoder saves: the pacer skipped a tick right after the first frame.
    const H264Fixture& fx = StandardFixture();
    std::vector<MuxAu> aus;
    for (size_t i = 0; i < fx.aus.size(); ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i == 0 ? 0 : i + 1});
    CreateDirectoryW(TestOutputPath(L"clip_file").c_str(), nullptr);
    const std::wstring src = TestOutputPath(L"clip_file\\held_first.mp4");
    REQUIRE(MuxWriteMp4(src, fx.sps_pps, fx.width, fx.height, fx.fps, aus, {}, VideoCodec::H264));

    ClipInfo info;
    REQUIRE(ReadClipInfo(src, info));
    CHECK(info.fps == doctest::Approx(60.0));

    const std::wstring dst = TestOutputPath(L"clip_file\\held_first_trim.mp4");
    const TrimResult r = TrimClip(src, dst, 0.0, 99.0);
    REQUIRE_MESSAGE(r.ok, r.error);
    ClipInfo out;
    REQUIRE(ReadClipInfo(dst, out));
    CHECK(out.fps == doctest::Approx(60.0));
    CHECK(out.duration_s == doctest::Approx(info.duration_s));
    CHECK(ProbeVideoFrames(dst).count == fx.aus.size());
    const std::string errors = DecodeErrors(dst);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);
}

TEST_CASE("clip file: a trim ending at a gap keeps the gap's length on its last frame") {
    // Ticks 0..59, then a stall: the next frame is at tick 120, on a keyframe at 2.0 s.
    const H264Fixture& fx = StandardFixture();
    std::vector<MuxAu> aus;
    for (size_t i = 0; i < 120; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i < 60 ? i : i + 60});
    CreateDirectoryW(TestOutputPath(L"clip_file").c_str(), nullptr);
    const std::wstring src = TestOutputPath(L"clip_file\\gap.mp4");
    REQUIRE(MuxWriteMp4(src, fx.sps_pps, fx.width, fx.height, fx.fps, aus, {}, VideoCodec::H264));

    const std::wstring dst = TestOutputPath(L"clip_file\\gap_trim.mp4");
    const TrimResult r = TrimClip(src, dst, 0.0, 2.0);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.end_s == doctest::Approx(2.0));
    ClipInfo out;
    REQUIRE(ReadClipInfo(dst, out));
    CHECK(out.duration_s == doctest::Approx(2.0));
    CHECK(ProbeVideoFrames(dst).count == 60);
}

TEST_CASE("clip file: a corrupt constant-size sample count fails instead of asking for 100 GB") {
    const std::wstring src = MakeSource(L"bad_stsz.mp4", VideoCodec::H264);
    std::ifstream in(src, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const size_t at = bytes.find("stsz");
    REQUIRE(at != std::string::npos);
    // After the type: version/flags, then sample_size = 1 and sample_count = 0xFFFFFFFF, big-endian.
    const char patch[8] = {0, 0, 0, 1, '\xFF', '\xFF', '\xFF', '\xFF'};
    bytes.replace(at + 8, sizeof(patch), patch, sizeof(patch));
    const std::wstring bad = TestOutputPath(L"clip_file\\bad_stsz_patched.mp4");
    std::ofstream(bad, std::ios::binary) << bytes;

    ClipInfo info;
    CHECK_FALSE(ReadClipInfo(bad, info));
}

TEST_CASE("clip file: a track that ended before the trim's start is left out") {
    const std::wstring src = MakeSource(L"short_mic.mp4", VideoCodec::H264, 1.0);
    const std::wstring dst = TestOutputPath(L"clip_file\\short_mic_trim.mp4");
    const TrimResult r = TrimClip(src, dst, 2.2, 3.5);
    REQUIRE_MESSAGE(r.ok, r.error);
    const ProbeInfo probe = Probe(dst);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 1);
    CHECK(audio[0].title == "Desktop");
}

TEST_CASE("clip file: a track switched off is left out, the others keep their names and sync") {
    const std::wstring src = MakeSource(L"keep.mp4", VideoCodec::H264);
    const std::wstring dst = TestOutputPath(L"clip_file\\keep_desktop.mp4");
    const std::vector<bool> keep = {true, false};
    const TrimResult r = TrimClip(src, dst, 0.0, 4.0, &keep);
    REQUIRE_MESSAGE(r.ok, r.error);
    const ProbeInfo probe = Probe(dst);
    REQUIRE(probe.ok);
    const auto audio = probe.Of("audio");
    REQUIRE(audio.size() == 1);
    CHECK(audio[0].title == "Desktop");
    const Pcm  pcm = DecodeAudio(dst, 0);
    const auto onset = FirstFrameAbove(pcm, 0.05);
    REQUIRE(onset.has_value());
    CHECK(std::abs(double(*onset) - kToneAt * kRate) <= 2.0);
}

TEST_CASE("clip file: a truncated or foreign file fails cleanly") {
    const std::wstring src = MakeSource(L"whole.mp4", VideoCodec::H264);
    std::ifstream in(src, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Cut through the moov at the end: the reader must say no, not read past the buffer.
    for (size_t keep : {bytes.size() - 1, bytes.size() - 500, size_t{64}}) {
        CAPTURE(keep);
        const std::wstring cut = TestOutputPath(L"clip_file\\cut.mp4");
        { std::ofstream out(cut, std::ios::binary | std::ios::trunc); out.write(bytes.data(), static_cast<std::streamsize>(keep)); }
        ClipInfo info;
        CHECK_FALSE(ReadClipInfo(cut, info));
        CHECK_FALSE(TrimClip(cut, TestOutputPath(L"clip_file\\cut_out.mp4"), 0.0, 1.0).ok);
    }
    ClipInfo info;
    std::string error;
    CHECK_FALSE(ReadClipInfo(TestOutputPath(L"clip_file\\missing.mp4"), info, &error));
    CHECK(!error.empty());
}

// How far apart two decoded frames are: mean absolute difference over the area both have (an AMF
// AV1 encode pads the picture past the source's size).
double FrameDistance(const VideoBgra& a, size_t ia, const VideoBgra& b, size_t ib) {
    const uint32_t w = (std::min)(a.width, b.width), h = (std::min)(a.height, b.height);
    double sum = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w * 4; ++x)
            sum += std::abs(int(a.Frame(ia)[(size_t(y) * a.width) * 4 + x]) - int(b.Frame(ib)[(size_t(y) * b.width) * 4 + x]));
    return sum / (double(w) * h * 4);
}

// The frame-exact trim: decoded on the GPU, encoded again on the same one. Also decoded into system
// memory (a decoder without DXVA for the clip) and uploaded, on NVIDIA only to keep the run short.
TEST_CASE("gpu: a re-encoded trim starts on the exact frame and keeps audio in sync" * doctest::test_suite("gpu")) {
    SeedAmfCapsInProcess();
    for (const char* gpu : {"nvidia", "amd"}) {
        if (std::string(gpu) == "amd" && !AmfUsable()) continue;
        for (VideoCodec codec : {VideoCodec::H264, VideoCodec::AV1})
        for (bool gpu_decode : {true, false}) {
            if (!gpu_decode && gpu[0] != 'n') continue;
            const std::string gpu_name = gpu;
            CAPTURE(gpu_name);
            CAPTURE(VideoCodecName(codec));
            CAPTURE(gpu_decode);
            const bool av1 = codec == VideoCodec::AV1;
            const std::wstring src = MakeSource(av1 ? L"re_src_av1.mp4" : L"re_src_h264.mp4", codec);
            const std::wstring dst = TestOutputPath(std::wstring(L"clip_file\\re_") + (av1 ? L"av1_" : L"h264_") +
                                                    (gpu[0] == 'n' ? L"nv.mp4" : L"amd.mp4"));
            ReencodeOptions opts;
            opts.adapter = gpu;
            opts.keyframe_sec = 1;
            opts.gpu_decode = gpu_decode;
            uint32_t calls = 0, last_done = 0, last_total = 0;
            const TrimResult r = ReencodeClip(src, dst, 2.2, 3.5, nullptr, opts, [&](uint32_t done, uint32_t total) {
                ++calls;
                last_done = done;
                last_total = total;
                return true;
            });
            REQUIRE_MESSAGE(r.ok, r.error);
            CHECK(r.start_s == doctest::Approx(2.2));  // frame 132, mid-GOP: no snap to 2.0
            CHECK(r.end_s == doctest::Approx(3.5));
            CHECK(calls == 78);
            CHECK(last_done == last_total);
            CheckTrimmed(dst, 1.3, kToneAt - 2.2);

            ClipInfo info;
            REQUIRE(ReadClipInfo(dst, info));
            CHECK(info.codec == codec);  // the source's codec
            CHECK(info.keyframes_s.size() == 2);  // keyframe_sec 1 over 1.3 s
            CHECK(info.picture_width == 320);    // an AMF AV1 encode pads, the picture doesn't grow
            CHECK(info.picture_height == 180);

            // The first frame is source frame 132, not a neighbour: of frames 130-134 (testsrc2 moves
            // every frame) it is the closest one. Encoding noise at this size is close to one frame's
            // motion (distances 3.2-4.5 vs 4.2-6.1 for the neighbours, 2026-11-06), so only the order
            // is checked.
            const VideoBgra in = DecodeVideoBgra(src);
            const VideoBgra out = DecodeVideoBgra(dst, 1);
            REQUIRE(in.FrameCount() == 240);
            REQUIRE(out.FrameCount() == 1);
            const double exact = FrameDistance(out, 0, in, 132);
            for (size_t other : {130, 131, 133, 134}) {
                CAPTURE(other);
                CHECK(exact < FrameDistance(out, 0, in, other));
            }
        }
    }
    SetAmfAdapterCaps({});
}

// A padded source (as AMF writes 1080 lines in 1082) re-encodes at its picture size: the padding
// rows are not carried over as image.
TEST_CASE("gpu: a re-encode of a padded clip keeps only its picture" * doctest::test_suite("gpu")) {
    const Av1Fixture& fx = StandardAv1Fixture();
    std::vector<MuxAu> aus;
    for (size_t i = 0; i < fx.aus.size(); ++i) aus.push_back({fx.aus[i].obus, fx.aus[i].key, i});
    const std::wstring src = TestOutputPath(L"clip_file\\re_padded_src.mp4");
    CreateDirectoryW(TestOutputPath(L"clip_file").c_str(), nullptr);
    REQUIRE(MuxWriteMp4(src, fx.seq_header, fx.width, fx.height, fx.fps, aus, {}, VideoCodec::AV1, MuxPicture{320, 176}));
    const std::wstring dst = TestOutputPath(L"clip_file\\re_padded.mp4");
    ReencodeOptions opts;
    opts.adapter = "nvidia";  // codes any size exactly, so the output's coded size is the picture
    const TrimResult r = ReencodeClip(src, dst, 1.1, 2.5, nullptr, opts);
    REQUIRE_MESSAGE(r.ok, r.error);
    ClipInfo info;
    REQUIRE(ReadClipInfo(dst, info));
    CHECK(info.width == 320);
    CHECK(info.height == 176);
    CHECK(info.picture_height == 176);
    const VideoBgra in = DecodeVideoBgra(src);
    const VideoBgra out = DecodeVideoBgra(dst, 1);
    REQUIRE(out.FrameCount() == 1);
    CHECK(out.height == 176);
    CHECK(FrameDistance(out, 0, in, 66) < FrameDistance(out, 0, in, 64));  // and it is the picture, unscaled
}

TEST_CASE("clip ops: replace keeps the original's name and times; the busy lock is seen by path") {
    const std::wstring src = MakeSource(L"ops_src.mp4", VideoCodec::H264);
    const std::wstring edited = TestOutputPath(L"clip_file\\ops_src (trim).mp4");
    REQUIRE(TrimClip(src, edited, 1.0, 3.0).ok);
    FILETIME old_write{};
    {
        HANDLE h = CreateFileW(src.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        SYSTEMTIME st{2026, 1, 4, 1, 12, 0, 0, 0};
        SystemTimeToFileTime(&st, &old_write);
        SetFileTime(h, nullptr, nullptr, &old_write);
        CloseHandle(h);
    }
    CHECK_FALSE(ClipBusy(src));
    {
        const ClipBusyLock lock(src);
        CHECK(ClipBusy(src));
        // Another spelling of the same path is the same clip.
        std::wstring upper = src;
        for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
        CHECK(ClipBusy(upper));
        CHECK_FALSE(ClipBusy(edited));
    }
    CHECK_FALSE(ClipBusy(src));

    // An isolated instance (this test run) deletes instead of using the Recycle Bin.
    REQUIRE(ReplaceWithEdited(src, edited));
    CHECK(GetFileAttributesW(edited.c_str()) == INVALID_FILE_ATTRIBUTES);
    ClipInfo info;
    REQUIRE(ReadClipInfo(src, info));
    CHECK(info.duration_s == doctest::Approx(2.0));
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    REQUIRE(GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &attr));
    CHECK(CompareFileTime(&attr.ftLastWriteTime, &old_write) == 0);
}

TEST_CASE("gpu: a re-encode that is cancelled leaves no output" * doctest::test_suite("gpu")) {
    const std::wstring src = MakeSource(L"re_cancel_src.mp4", VideoCodec::H264);
    const std::wstring dst = TestOutputPath(L"clip_file\\re_cancel.mp4");
    DeleteFileW(dst.c_str());
    ReencodeOptions opts;
    opts.adapter = "nvidia";
    const TrimResult r = ReencodeClip(src, dst, 0.5, 3.0, nullptr, opts, [](uint32_t done, uint32_t) { return done < 10; });
    CHECK_FALSE(r.ok);
    CHECK(r.error == "cancelled");
    CHECK(GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((dst + L".part").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((dst + L".part.video").c_str()) == INVALID_FILE_ATTRIBUTES);
}

}  // namespace ombra::test
