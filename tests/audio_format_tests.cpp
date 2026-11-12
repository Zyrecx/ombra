// FormatConverter (audio_format.h): every mix format a driver can hand us reads correctly or fails
// that one source. No audio device involved.
#include "ombra/audio_format.h"

#include <doctest/doctest.h>

#include <vector>

using ombra::FormatConverter;

namespace {

WAVEFORMATEX Plain(WORD tag, WORD channels, WORD bits, DWORD rate = 48000) {
    WAVEFORMATEX f{};
    f.wFormatTag = tag;
    f.nChannels = channels;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = bits;
    f.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    return f;
}

WAVEFORMATEXTENSIBLE Extensible(DWORD subformat, WORD channels, WORD bits) {
    WAVEFORMATEXTENSIBLE e{};
    e.Format = Plain(WAVE_FORMAT_EXTENSIBLE, channels, bits);
    e.Format.cbSize = 22;
    e.SubFormat.Data1 = subformat;
    return e;
}

template <typename T>
std::vector<int16_t> Convert(const WAVEFORMATEX& f, const std::vector<T>& samples, uint32_t frames) {
    FormatConverter c;
    REQUIRE(c.Configure(&f));
    std::vector<int16_t> out;
    c.Convert(reinterpret_cast<const uint8_t*>(samples.data()), frames, out);
    return out;
}

}  // namespace

TEST_CASE("audio format: plain IEEE float reads, it used to kill the engine") {
    const auto out = Convert(Plain(WAVE_FORMAT_IEEE_FLOAT, 2, 32), std::vector<float>{0.5f, -0.5f}, 1);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 16384);
    CHECK(out[1] == -16384);
}

TEST_CASE("audio format: extensible float and 32-bit integer read") {
    const auto f = Extensible(3, 2, 32);
    const auto a = Convert(f.Format, std::vector<float>{0.25f, -1.0f}, 1);
    CHECK(a[0] == 8192);
    CHECK(a[1] == -32767);

    const auto i = Extensible(1, 2, 32);
    const auto b = Convert(i.Format, std::vector<int32_t>{1 << 30, -(1 << 30)}, 1);
    CHECK(b[0] == 16384);
    CHECK(b[1] == -16384);
}

TEST_CASE("audio format: packed 24-bit keeps the sign and the frame stride") {
    // Two stereo frames: 0.5, -0.5, then 0.25, -0.25.
    const std::vector<uint8_t> bytes = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0,
                                        0x00, 0x00, 0x20, 0x00, 0x00, 0xE0};
    const auto out = Convert(Plain(WAVE_FORMAT_PCM, 2, 24), bytes, 2);
    REQUIRE(out.size() == 4);
    CHECK(out[0] == 16384);
    CHECK(out[1] == -16384);
    CHECK(out[2] == 8192);
    CHECK(out[3] == -8192);
}

TEST_CASE("audio format: 64-bit float reads") {
    const auto f = Extensible(3, 2, 64);
    const auto out = Convert(f.Format, std::vector<double>{0.5, -0.5}, 1);
    CHECK(out[0] == 16384);
    CHECK(out[1] == -16384);
}

TEST_CASE("audio format: mono duplicates and surround averages") {
    const auto mono = Convert(Plain(WAVE_FORMAT_PCM, 1, 16), std::vector<int16_t>{16384}, 1);
    CHECK(mono[0] == mono[1]);
    const auto wide = Convert(Plain(WAVE_FORMAT_IEEE_FLOAT, 6, 32),
                              std::vector<float>{0.6f, 0.6f, 0, 0, 0, 0}, 1);
    CHECK(wide[0] == wide[1]);
    CHECK(wide[0] == 6553);
}

TEST_CASE("audio format: an unknown format fails the source, without dying") {
    FormatConverter c;
    auto eight_bit = Plain(WAVE_FORMAT_PCM, 2, 8);
    CHECK_FALSE(c.Configure(&eight_bit));
    auto float16 = Plain(WAVE_FORMAT_IEEE_FLOAT, 2, 16);
    CHECK_FALSE(c.Configure(&float16));
    auto adpcm = Plain(WAVE_FORMAT_ADPCM, 2, 16);
    CHECK_FALSE(c.Configure(&adpcm));
    auto no_channels = Plain(WAVE_FORMAT_PCM, 0, 16);
    CHECK_FALSE(c.Configure(&no_channels));
    auto unknown_sub = Extensible(0x7777, 2, 16);
    CHECK_FALSE(c.Configure(&unknown_sub.Format));
}
