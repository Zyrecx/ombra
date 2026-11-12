// BuildAvcC (mux.cpp), headless, no GPU: real SPS/PPS bytes from the x264 fixture, not synthetic
// data, so the avcC fields assert against what an actual encoder emits. save_tests.cpp only
// exercises this transitively (ffprobe on a finished file); these check the AVCDecoderConfiguration
// Record's bytes directly.

#include <doctest/doctest.h>

#include "support/h264_fixture.h"

#include "ombra/mux.h"

#include <cstddef>
#include <cstdint>
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

}  // namespace ombra::test
