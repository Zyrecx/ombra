#include "support/av1_fixture.h"

#include <doctest/doctest.h>

#include <fstream>
#include <iterator>

namespace ombra::test {

namespace {

// Written from the AV1 spec (§5.3.1 obu_header, §4.10.5 leb128) independently of
// src/ombra/av1.cpp, so the fixture doesn't share the code under test.
struct Obu {
    int    type = 0;
    size_t header = 0;  // bytes before the payload
    size_t total = 0;
};

bool ReadObu(const uint8_t* p, size_t left, Obu& obu) {
    if (left < 1) return false;
    obu.type             = (p[0] >> 3) & 0x0F;
    const bool extension = (p[0] >> 2) & 1;
    const bool has_size  = (p[0] >> 1) & 1;
    size_t pos = 1 + (extension ? 1 : 0);
    if (!has_size || pos > left) return false;  // ffmpeg's -f obu always writes sizes
    uint64_t size = 0;
    for (int i = 0;; ++i) {
        if (pos >= left || i == 8) return false;
        const uint8_t b = p[pos++];
        size |= uint64_t(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) break;
    }
    if (size > left - pos) return false;
    obu.header = pos;
    obu.total  = pos + static_cast<size_t>(size);
    return true;
}

}  // namespace

bool LoadAv1Fixture(const std::wstring& path, uint32_t width, uint32_t height, uint32_t fps,
                    Av1Fixture& out, std::string& error) {
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

    Av1Fixture::Au current;
    bool frame_seen = false;
    for (size_t pos = 0; pos < data.size();) {
        Obu obu;
        if (!ReadObu(data.data() + pos, data.size() - pos, obu)) {
            error = "malformed OBU at byte " + std::to_string(pos);
            return false;
        }
        const uint8_t* p = data.data() + pos;
        if (obu.type == 2 && !current.obus.empty()) {  // OBU_TEMPORAL_DELIMITER starts the next unit
            out.aus.push_back(std::move(current));
            current    = {};
            frame_seen = false;
        }
        if (obu.type == 1 && out.seq_header.empty()) out.seq_header.assign(p, p + obu.total);
        // OBU_FRAME_HEADER / OBU_FRAME: show_existing_frame (1 bit), then frame_type (2 bits).
        // Holds because the fixture's sequence header has reduced_still_picture_header = 0.
        if ((obu.type == 3 || obu.type == 6) && !frame_seen && obu.total > obu.header) {
            const uint8_t first = p[obu.header];
            current.key = !(first & 0x80) && ((first >> 5) & 0x3) == 0;
            frame_seen  = true;
        }
        current.obus.insert(current.obus.end(), p, p + obu.total);
        pos += obu.total;
    }
    if (!current.obus.empty()) out.aus.push_back(std::move(current));

    if (out.seq_header.empty()) {
        error = "no sequence header";
        return false;
    }
    if (out.aus.empty() || !out.aus.front().key) {
        error = "first temporal unit is not a key frame";
        return false;
    }
    return true;
}

const Av1Fixture& StandardAv1Fixture() {
    static const Av1Fixture fixture = [] {
        Av1Fixture f;
        std::string error;
        const bool ok = LoadAv1Fixture(std::wstring(OMBRA_TEST_FIXTURE_DIR) + L"/testsrc2_320x180_60fps_4s.obu",
                                       320, 180, 60, f, error);
        REQUIRE_MESSAGE(ok, error);
        REQUIRE(f.aus.size() == 240);
        return f;
    }();
    return fixture;
}

}  // namespace ombra::test
