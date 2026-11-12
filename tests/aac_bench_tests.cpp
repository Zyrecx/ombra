// MF AAC encoder speed, for sizing continuous audio encoding (one encoder per recorded track).
// Skipped by default: run with `ombra-tests -ns -tc="aac bench*"`. CPU only, no GPU.
#include <doctest/doctest.h>

#include "ombra/aac_encoder.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace ombra::test {

TEST_CASE("aac bench: MF AAC realtime factor, 120 s stereo 48 kHz at 192 kbps" * doctest::skip()) {
    constexpr size_t kSeconds = 120;
    std::vector<int16_t> pcm(kSeconds * 48000 * 2);
    uint64_t rng = 3;
    for (size_t i = 0; i < pcm.size(); ++i) {  // noise plus a tone: busier than typical game audio
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        pcm[i] = static_cast<int16_t>(static_cast<int32_t>(rng >> 52) - 2048 + ((i / 2) % 109) * 150);
    }
    AacEncoder enc;
    REQUIRE(enc.Open(192));
    std::vector<AacEncoder::Frame> frames;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(enc.Encode(pcm.data(), pcm.size() / 2, frames));
    REQUIRE(enc.Drain(frames));
    const double ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    enc.Close();
    MESSAGE("encoded ", frames.size(), " frames in ", ms, " ms = ", kSeconds * 1000.0 / ms,
            "x realtime = ", ms / (kSeconds * 10.0), "% of one core per continuously encoded track");
    CHECK(frames.size() > 0);
}


// Every rate a config can hold must open as raw AAC: the MFT offers each rate in raw and ADTS
// form, and nearest-first over all of them once chose ADTS for 256 kbps, which the old save-time
// path turned into a die() on every save. 128 and up share one AudioSpecificConfig (AAC-LC), which is
// what lets AudioTrackHub::SetBitrate keep buffered audio across those.
TEST_CASE("aac: every configurable bitrate opens raw; AAC-LC rates share one ASC") {
    std::vector<uint8_t> lc_asc;
    for (uint32_t kbps : {32u, 64u, 96u, 128u, 160u, 192u, 256u, 320u, 512u}) {
        CAPTURE(kbps);
        AacEncoder e;
        REQUIRE(e.Open(kbps));
        CHECK(e.NegotiatedBitrateBps() <= 320000u);
        if (kbps >= 128) {
            if (lc_asc.empty()) lc_asc = e.AudioSpecificConfig();
            CHECK(e.AudioSpecificConfig() == lc_asc);
        }
    }
}

}  // namespace ombra::test
