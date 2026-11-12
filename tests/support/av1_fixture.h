// A real, decodable AV1 stream (tests/fixtures/*.obu: low-overhead OBUs as ffmpeg's `-f obu` writes
// them, made with SVT-AV1) split into temporal units, so AV1 saves can be fed through Ring without
// a GPU. Made with:
//   ffmpeg -f lavfi -i testsrc2=size=320x180:rate=60:duration=4 -pix_fmt yuv420p -c:v libsvtav1
//     -preset 8 -g 60 -svtav1-params "pred-struct=1:keyint=60:color-primaries=1:
//     transfer-characteristics=1:matrix-coefficients=1:color-range=0" -f obu <out>
//
// The sequence header is SVT-AV1's, not NVENC's or AMF's: what it shows about av1C is the parse of
// a real header, not what a given hardware encoder writes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ombra::test {

struct Av1Fixture {
    struct Au {
        std::vector<uint8_t> obus;  // one temporal unit, temporal delimiter included, as encoders emit
        bool                 key = false;
    };
    std::vector<uint8_t> seq_header;  // the first sequence header OBU, as an encoder reports it
    uint32_t             width  = 0;
    uint32_t             height = 0;
    uint32_t             fps    = 0;
    std::vector<Au>      aus;
};

// Splits at temporal delimiters; `key` is frame_type == KEY_FRAME in the unit's first frame header.
bool LoadAv1Fixture(const std::wstring& path, uint32_t width, uint32_t height, uint32_t fps,
                    Av1Fixture& out, std::string& error);

// testsrc2 320x180, 60 fps, 4 s, a key frame every 60 frames, low-delay (no hidden frames).
const Av1Fixture& StandardAv1Fixture();

}  // namespace ombra::test
