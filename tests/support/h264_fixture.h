// A real, decodable H.264 elementary stream (tests/fixtures/*.h264, made with x264) split into
// access units, so the save path can be fed through Ring without a GPU.
//
// The fixture carries x264's SPS, not NVENC's: anything asserted about SPS contents (VUI colour
// info, profile, level) through this fixture tests the muxer's pass-through only, never the encoder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ombra::test {

struct H264Fixture {
    struct Au {
        std::vector<uint8_t> annexb;  // AUD removed; in-band SPS/PPS kept on IDRs, as NVENC emits
        bool                 idr = false;
    };
    std::vector<uint8_t> sps_pps;  // first SPS then first PPS, 4-byte start codes
    uint32_t             width  = 0;
    uint32_t             height = 0;
    uint32_t             fps    = 0;
    uint32_t             gop    = 0;  // frames between IDRs, checked to be constant
    std::vector<Au>      aus;
};

// Splits on AUD NALs (the fixture is encoded with aud=1). `error` explains a false return.
bool LoadH264Fixture(const std::wstring& path, uint32_t width, uint32_t height, uint32_t fps,
                     H264Fixture& out, std::string& error);

// The standard fixture: testsrc2 320x180, 60 fps, 4 s, IDR every 60 frames, no B-frames.
const H264Fixture& StandardFixture();

}  // namespace ombra::test
