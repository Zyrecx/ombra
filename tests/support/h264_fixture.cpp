#include "support/h264_fixture.h"

#include <doctest/doctest.h>

#include <fstream>
#include <iterator>

namespace ombra::test {

namespace {

struct Nal {
    size_t begin = 0;  // first byte after the start code
    size_t end   = 0;
};

std::vector<Nal> SplitNals(const std::vector<uint8_t>& d) {
    std::vector<Nal> nals;
    auto find_start = [&](size_t from, size_t& code_len) -> size_t {
        for (size_t p = from; p + 3 <= d.size(); ++p) {
            if (d[p] == 0 && d[p + 1] == 0) {
                if (d[p + 2] == 1) { code_len = 3; return p; }
                if (p + 4 <= d.size() && d[p + 2] == 0 && d[p + 3] == 1) { code_len = 4; return p; }
            }
        }
        code_len = 0;
        return d.size();
    };
    size_t code_len = 0;
    size_t start = find_start(0, code_len);
    while (start < d.size()) {
        const size_t payload = start + code_len;
        size_t next_len = 0;
        const size_t next = find_start(payload, next_len);
        nals.push_back({payload, next});
        start = next;
        code_len = next_len;
    }
    return nals;
}

void AppendNal(std::vector<uint8_t>& out, const std::vector<uint8_t>& d, const Nal& n) {
    static constexpr uint8_t kStart[4] = {0, 0, 0, 1};
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), d.begin() + n.begin, d.begin() + n.end);
}

}  // namespace

bool LoadH264Fixture(const std::wstring& path, uint32_t width, uint32_t height, uint32_t fps,
                     H264Fixture& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open fixture";
        return false;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    out = {};
    out.width  = width;
    out.height = height;
    out.fps    = fps;

    std::vector<uint8_t> sps, pps;
    H264Fixture::Au current;
    bool have_current = false;
    for (const Nal& n : SplitNals(data)) {
        if (n.begin >= n.end) continue;
        const uint8_t type = data[n.begin] & 0x1F;
        if (type == 9) {  // AUD starts a new access unit
            if (have_current && !current.annexb.empty()) out.aus.push_back(std::move(current));
            current = {};
            have_current = true;
            continue;
        }
        if (type == 7 && sps.empty()) AppendNal(sps, data, n);
        if (type == 8 && pps.empty()) AppendNal(pps, data, n);
        if (type == 5) current.idr = true;
        AppendNal(current.annexb, data, n);
        have_current = true;
    }
    if (have_current && !current.annexb.empty()) out.aus.push_back(std::move(current));

    if (sps.empty() || pps.empty()) {
        error = "fixture has no SPS/PPS";
        return false;
    }
    out.sps_pps = sps;
    out.sps_pps.insert(out.sps_pps.end(), pps.begin(), pps.end());

    if (out.aus.empty() || !out.aus.front().idr) {
        error = "fixture does not start with an IDR";
        return false;
    }
    uint32_t last_idr = 0;
    for (uint32_t i = 1; i < out.aus.size(); ++i) {
        if (!out.aus[i].idr) continue;
        const uint32_t gap = i - last_idr;
        if (out.gop == 0) out.gop = gap;
        if (gap != out.gop) {
            error = "fixture GOP length is not constant";
            return false;
        }
        last_idr = i;
    }
    if (out.gop == 0 || out.aus.size() % out.gop != 0) {
        error = "fixture length is not a whole number of GOPs";
        return false;
    }
    return true;
}

const H264Fixture& StandardFixture() {
    static const H264Fixture fixture = [] {
        H264Fixture f;
        std::string error;
        const bool ok = LoadH264Fixture(std::wstring(OMBRA_TEST_FIXTURE_DIR) + L"/testsrc2_320x180_60fps_4s.h264",
                                        320, 180, 60, f, error);
        if (!ok) FAIL("standard H.264 fixture: " << error);
        return f;
    }();
    return fixture;
}

}  // namespace ombra::test
