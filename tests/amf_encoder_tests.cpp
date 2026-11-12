// AmfEncoder's pure pieces. Headless: nothing here loads amfrt64.dll or touches a GPU. The encoder
// itself is exercised by the "gpu" suite (pipeline_gpu_tests.cpp) with encode.adapter=amd.

#include <doctest/doctest.h>

#include "ombra/amf_encoder.h"
#include "ombra/amf_probe.h"

namespace ombra::test {

TEST_CASE("AmfH264Level: the lowest level that fits frame size and macroblock rate") {
    // AMF's default 4.2 is right up to 1080p60 and wrong above it.
    CHECK(AmfH264Level(1920, 1080, 60) == AMF_H264_LEVEL__4_2);
    CHECK(AmfH264Level(1920, 1080, 120) == AMF_H264_LEVEL__5_1);
    CHECK(AmfH264Level(2560, 1440, 60) == AMF_H264_LEVEL__5_1);
    CHECK(AmfH264Level(2560, 1440, 144) == AMF_H264_LEVEL__5_2);
    CHECK(AmfH264Level(3840, 2160, 60) == AMF_H264_LEVEL__5_2);
    CHECK(AmfH264Level(3840, 2160, 120) == AMF_H264_LEVEL__6);
    // Beyond every level: clamp to the top one rather than fail.
    CHECK(AmfH264Level(7680, 4320, 240) == AMF_H264_LEVEL__6_2);
}

TEST_CASE("AmfQualityPresetFor: encode.preset's p1..p7 map onto AMF's three steps") {
    CHECK(AmfQualityPresetFor("p1") == AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
    CHECK(AmfQualityPresetFor("p3") == AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
    CHECK(AmfQualityPresetFor("p4") == AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED);
    CHECK(AmfQualityPresetFor("p5") == AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED);
    CHECK(AmfQualityPresetFor("p7") == AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY);
    CHECK(AmfQualityPresetFor("bogus") == AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED);
}

TEST_CASE("AmfHeaderSpacing: within AMF's 1000-frame cap and landing on every IDR") {
    CHECK(AmfHeaderSpacing(60, 2) == 120);
    CHECK(AmfHeaderSpacing(144, 4) == 576);
    // 144 fps x 10 s and 360 Hz x 4 s both pass the cap: fall back to one second of frames.
    CHECK(AmfHeaderSpacing(144, 10) == 144);
    CHECK(AmfHeaderSpacing(360, 4) == 360);
    for (uint32_t fps : {30u, 60u, 144u, 165u, 240u, 360u}) {
        for (uint32_t sec = 1; sec <= 10; ++sec) {
            const int64_t spacing = AmfHeaderSpacing(fps, sec);
            CAPTURE(fps);
            CAPTURE(sec);
            CHECK(spacing <= 1000);
            CHECK((int64_t(fps) * sec) % spacing == 0);
        }
    }
}

TEST_CASE("IsAnnexB: start-code-prefixed SPS/PPS only") {
    CHECK(IsAnnexB({0, 0, 0, 1, 0x67, 0x64}));
    CHECK(IsAnnexB({0, 0, 1, 0x67}));
    // avcC (starts with configurationVersion 1) and length-prefixed NALs are not.
    CHECK_FALSE(IsAnnexB({1, 0x64, 0x00, 0x2a, 0xff}));
    CHECK_FALSE(IsAnnexB({0, 0, 0, 0x19, 0x67}));
    CHECK_FALSE(IsAnnexB({}));
}

TEST_CASE("ParseAmfProbeOutput: a healthy child's report") {
    const auto r = ParseAmfProbeOutput(
        "version=1.4.36\r\nadapter=0000000000012235 h264=1 hevc=1 av1=0\r\n");
    CHECK(r.ran);
    CHECK(r.runtime_version == "1.4.36");
    REQUIRE(r.adapters.size() == 1);
    CHECK(r.adapters[0].luid.LowPart == 0x12235);
    CHECK(r.adapters[0].luid.HighPart == 0);
    CHECK(r.adapters[0].h264);
    CHECK(r.adapters[0].hevc);
    CHECK_FALSE(r.adapters[0].av1);
    CHECK(r.H264Usable());
    CHECK(r.error.empty());
    CHECK(r.adapters[0].av1_align_w == 1);  // an older child that didn't report it: no constraint
    CHECK(r.adapters[0].av1_align_h == 1);

    const auto rdna3 = ParseAmfProbeOutput("adapter=0000000000010d3d h264=1 hevc=1 av1=1 av1_align=64x16\n");
    REQUIRE(rdna3.adapters.size() == 1);
    CHECK(rdna3.adapters[0].av1);
    CHECK(rdna3.adapters[0].av1_align_w == 64);
    CHECK(rdna3.adapters[0].av1_align_h == 16);
}

TEST_CASE("FinishAmfProbe: an adapter reported before a hang or crash doesn't count") {
    const std::string healthy = "version=1.4.36\nadapter=0000000000010d3d h264=1 hevc=1 av1=1 av1_align=64x16\n";
    CHECK(FinishAmfProbe(healthy, false, 0).H264Usable());
    CHECK(FinishAmfProbe(healthy, false, 0).error.empty());

    auto hung = FinishAmfProbe(healthy + "error=AMF init timed out\n", false, 1);  // the child's own deadline
    CHECK_FALSE(hung.H264Usable());
    CHECK(hung.adapters.empty());
    CHECK(hung.error == "AMF init timed out");

    auto killed = FinishAmfProbe(healthy, true, 1);  // the parent's deadline
    CHECK(killed.adapters.empty());
    CHECK(killed.error == "probe process hung");

    auto crashed = FinishAmfProbe(healthy, false, 0xC0000005);  // in the driver's teardown
    CHECK(crashed.adapters.empty());
    CHECK(crashed.error.find("died") != std::string::npos);

    CHECK(FinishAmfProbe("", false, 0xC0000005).error.find("died") != std::string::npos);
}

TEST_CASE("ParseAmfProbeOutput: a child that hung, died or found nothing is not usable") {
    auto r = ParseAmfProbeOutput("version=1.4.36\nerror=AMF init timed out\n");
    CHECK_FALSE(r.H264Usable());
    CHECK(r.error == "AMF init timed out");

    r = ParseAmfProbeOutput("");  // crashed before writing anything
    CHECK_FALSE(r.H264Usable());
    CHECK_FALSE(r.error.empty());

    r = ParseAmfProbeOutput("version=1.4.36\nadapter=0000000000012235 h264=0 hevc=0 av1=0\n");
    CHECK_FALSE(r.H264Usable());
    CHECK(r.error == "no AMD adapter can encode H.264");
}

}  // namespace ombra::test
