// capture.resolution: the output-size rule, the bitrate derivation that follows it, and the
// FrameScaler's area filter run on WARP (software D3D11: no GPU, nothing on screen), so the shader
// math is checked without NVENC. Whether NVENC/AMF read the scaled texture correctly is the gpu
// suite's "a scaled pipeline" case.

#include <doctest/doctest.h>

#include "ombra/config.h"
#include "ombra/frame_scaler.h"
#include "ombra/json.h"

#include <windows.h>
#include <d3d11.h>

#include <winrt/base.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ombra::test {

TEST_CASE("capture.resolution: ResolveOutputSize keeps the aspect, sets the short side, never upscales") {
    struct Case { uint32_t sw, sh; const char* res; uint32_t w, h; };
    const Case cases[] = {
        {3440, 1440, "1080p",  2580, 1080},  // ultrawide: no bars, no squash
        {2560, 1600, "1080p",  1728, 1080},  // 16:10
        {2560, 1440, "1080p",  1920, 1080},
        {3840, 2160, "720p",   1280,  720},
        {3840, 2160, "1440p",  2560, 1440},
        {1366,  768, "720p",   1280,  720},  // 1365.33 rounds to the nearest even width
        {1080, 1920, "720p",    720, 1280},  // portrait window: the short side is the width
        {1920, 1080, "1080p",  1920, 1080},  // preset == source: native
        {1920, 1080, "1440p",  1920, 1080},  // never scales up
        {1281,  721, "native", 1280,  720},  // native rounds down to even, as the stage textures do
        {1281,  721, "1440p",  1280,  720},
        {2560, 1440, "bogus",  2560, 1440},
    };
    for (const Case& c : cases) {
        CAPTURE(c.sw); CAPTURE(c.sh); CAPTURE(c.res);
        const VideoSize out = ResolveOutputSize(c.sw, c.sh, c.res);
        CHECK(out.width == c.w);
        CHECK(out.height == c.h);
        CHECK(out.width % 2 == 0);
        CHECK(out.height % 2 == 0);
    }
}

TEST_CASE("encode.amf_av1_unaligned: PlanEncode picks H.264, padded AV1 or a scaled-down AV1 size") {
    const VideoSize p1080{1920, 1080};
    // Aligned, or no alignment constraint: AV1 as asked, whatever the mode.
    for (const char* mode : {"h264", "pad", "scale"}) {
        CAPTURE(mode);
        const EncodePlan aligned = PlanEncode({1920, 1072}, VideoCodec::AV1, 64, 16, mode);
        CHECK(aligned.codec == VideoCodec::AV1);
        CHECK(aligned.size == VideoSize{1920, 1072});
        const EncodePlan nvenc = PlanEncode(p1080, VideoCodec::AV1, 1, 1, mode);
        CHECK(nvenc.codec == VideoCodec::AV1);
        CHECK(nvenc.size == p1080);
        const EncodePlan h264 = PlanEncode(p1080, VideoCodec::H264, 64, 16, mode);
        CHECK(h264.codec == VideoCodec::H264);
        CHECK(h264.size == p1080);
    }
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "h264").codec == VideoCodec::H264);
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "h264").size == p1080);
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "pad").codec == VideoCodec::AV1);
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "pad").size == p1080);  // the encoder pads
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "scale").codec == VideoCodec::AV1);
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "scale").size == VideoSize{1920, 1072});
    CHECK(PlanEncode({1366, 768}, VideoCodec::AV1, 64, 16, "scale").size == VideoSize{1344, 768});
    CHECK(PlanEncode({320, 180}, VideoCodec::AV1, 64, 16, "scale").size == VideoSize{320, 176});
    CHECK(PlanEncode({48, 180}, VideoCodec::AV1, 64, 16, "scale").codec == VideoCodec::H264);  // rounds to 0
    CHECK(PlanEncode(p1080, VideoCodec::AV1, 64, 16, "bogus").codec == VideoCodec::H264);
}

TEST_CASE("encode.amf_av1_unaligned: PredictPaddedSize matches what a 780M coded") {
    // ombra-tests --gpu, AmfEncoder opened at each size, coded size read from the sequence header
    // (2026-10-15).
    struct Case { uint32_t w, h, cw, ch; };
    const Case cases[] = {
        {320, 180, 320, 192},    {320, 176, 320, 176},    {320, 184, 320, 186},    {320, 190, 320, 192},
        {1920, 1080, 1920, 1082}, {1920, 1072, 1920, 1072}, {1920, 1088, 1920, 1088}, {1728, 1080, 1728, 1082},
        {1366, 768, 1408, 768},  {1280, 720, 1280, 720},  {1600, 900, 1600, 912},  {2560, 1440, 2560, 1440},
        {1680, 1050, 1728, 1056}, {1360, 768, 1408, 768},  {1280, 1080, 1280, 1082}, {1920, 1200, 1920, 1200},
        {854, 480, 896, 480},    {1000, 562, 1024, 576},
    };
    for (const Case& c : cases) {
        CAPTURE(c.w); CAPTURE(c.h);
        CHECK(PredictPaddedSize({c.w, c.h}, 64, 16) == VideoSize{c.cw, c.ch});
    }
}

TEST_CASE("capture.resolution: a resolution change under a preset re-derives the bitrate from the encoded size") {
    Config cfg;
    cfg.encode_quality_preset = "high";
    cfg.capture_framerate     = 60;
    cfg.capture_resolution    = "native";
    cfg.encode_bitrate_kbps   = QualityPresetToBitrate("high", 2560, 1440, 60, VideoCodec::AV1);

    JsonValue to_1080 = JsonValue::MakeObject();
    to_1080.Set("capture.resolution", JsonValue::MakeStr("1080p"));
    ConfigPreview down = PreviewConfig(cfg, to_1080, 2560, 1440, 60);
    CHECK(down.bitrate_kbps == QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1));
    CHECK(down.bitrate_kbps < cfg.encode_bitrate_kbps);
    CHECK(down.apply_tier == ApplyTier::Realloc);  // a new bitrate resizes the ring

    // Back to native, from a config already at 1080p: derived from the source size, not the
    // 1080p the pipeline is encoding right now.
    Config scaled = cfg;
    scaled.capture_resolution  = "1080p";
    scaled.encode_bitrate_kbps = QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1);
    JsonValue to_native = JsonValue::MakeObject();
    to_native.Set("capture.resolution", JsonValue::MakeStr("native"));
    CHECK(PreviewConfig(scaled, to_native, 2560, 1440, 60).bitrate_kbps ==
          QualityPresetToBitrate("high", 2560, 1440, 60, VideoCodec::AV1));

    // A preset picked while scaled derives at the scaled size.
    JsonValue to_ultra = JsonValue::MakeObject();
    to_ultra.Set("encode.quality_preset", JsonValue::MakeStr("ultra"));
    CHECK(PreviewConfig(scaled, to_ultra, 2560, 1440, 60).bitrate_kbps ==
          QualityPresetToBitrate("ultra", 1920, 1080, 60, VideoCodec::AV1));

    // Custom keeps the user's bitrate; the change is a plain rebuild.
    Config custom = cfg;
    custom.encode_quality_preset = "custom";
    custom.encode_bitrate_kbps   = 33333;
    ConfigPreview kept = PreviewConfig(custom, to_1080, 2560, 1440, 60);
    CHECK(kept.bitrate_kbps == 33333);
    CHECK(kept.apply_tier == ApplyTier::Rebuild);

    // A source already at or below the preset changes nothing.
    ConfigPreview same = PreviewConfig(cfg, to_1080, 1920, 1080, 60);
    CHECK(same.derived.empty());
}

namespace {

struct Warp {
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    Warp() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             levels, 1, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        REQUIRE(SUCCEEDED(hr));
    }

    // A BGRA stage-like texture filled by `pixel(x, y)` (0xAARRGGBB, as BGRA bytes in memory).
    template <typename F>
    winrt::com_ptr<ID3D11Texture2D> Source(uint32_t w, uint32_t h, F pixel) {
        std::vector<uint32_t> data(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) data[static_cast<size_t>(y) * w + x] = pixel(x, y);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // as Capture's slots
        D3D11_SUBRESOURCE_DATA init{data.data(), w * 4, 0};
        winrt::com_ptr<ID3D11Texture2D> tex;
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, &init, tex.put())));
        return tex;
    }

    std::vector<uint32_t> Read(ID3D11Texture2D* tex, uint32_t w, uint32_t h) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, staging.put())));
        context->CopyResource(staging.get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        REQUIRE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)));
        std::vector<uint32_t> out(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            memcpy(&out[static_cast<size_t>(y) * w], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, w * 4);
        context->Unmap(staging.get(), 0);
        return out;
    }
};

uint32_t Channel(uint32_t px, int shift) { return (px >> shift) & 0xFF; }

}  // namespace

TEST_CASE("frame scaler: the area filter on WARP") {
    Warp warp;

    SUBCASE("a solid colour comes out exact, from whichever slot is asked for") {
        auto a = warp.Source(64, 36, [](uint32_t, uint32_t) { return 0xFF336699u; });
        auto b = warp.Source(64, 36, [](uint32_t, uint32_t) { return 0xFFC08040u; });
        auto c = warp.Source(64, 36, [](uint32_t, uint32_t) { return 0xFF000000u; });
        FrameScaler scaler;
        std::string error;
        REQUIRE(scaler.Open(warp.device.get(), {a.get(), b.get(), c.get()}, 64, 36, 42, 24, error));
        for (uint32_t slot : {0u, 1u}) {
            scaler.Scale(slot);
            const auto px = warp.Read(scaler.Output(), 42, 24);
            const uint32_t want = slot == 0 ? 0xFF336699u : 0xFFC08040u;
            int off = 0;
            for (uint32_t p : px) off += p != want;
            CAPTURE(slot);
            CHECK(off == 0);
        }
    }

    SUBCASE("a 1-px checker halved averages to grey: the footprint covers exactly 2x2 texels") {
        auto checker = warp.Source(64, 64, [](uint32_t x, uint32_t y) { return ((x + y) & 1) ? 0xFFFFFFFFu : 0xFF000000u; });
        FrameScaler scaler;
        std::string error;
        REQUIRE(scaler.Open(warp.device.get(), {checker.get()}, 64, 64, 32, 32, error));
        scaler.Scale(0);
        for (uint32_t p : warp.Read(scaler.Output(), 32, 32)) {
            const uint32_t g = Channel(p, 8);
            CHECK(g >= 127);
            CHECK(g <= 128);
            CHECK(Channel(p, 24) == 255);  // alpha stays opaque
        }
    }

    SUBCASE("3440x1440 -> 2580x1080: an edge lands on its scaled column, and a split texel blends") {
        // Black left of x=1720 (exactly 1290 at 0.75x), white right of it; and a 1-px white column at
        // x=100, whose footprint (75.0..75.75) falls inside output pixel 75 -> 0.75 of it.
        auto src = warp.Source(3440, 1440, [](uint32_t x, uint32_t) { return (x >= 1720 || x == 100) ? 0xFFFFFFFFu : 0xFF000000u; });
        const VideoSize out = ResolveOutputSize(3440, 1440, "1080p");
        REQUIRE(out == VideoSize{2580, 1080});
        FrameScaler scaler;
        std::string error;
        REQUIRE(scaler.Open(warp.device.get(), {src.get()}, 3440, 1440, out.width, out.height, error));
        CHECK(scaler.Width() == 2580);
        CHECK(scaler.Height() == 1080);
        scaler.Scale(0);
        const auto px = warp.Read(scaler.Output(), out.width, out.height);
        for (uint32_t y : {0u, 540u, 1079u}) {
            const uint32_t* row = &px[static_cast<size_t>(y) * out.width];
            CAPTURE(y);
            CHECK(Channel(row[1289], 16) == 0);
            CHECK(Channel(row[1290], 16) == 255);
            CHECK(Channel(row[2579], 16) == 255);  // right edge: footprint clamped to the texture
            const uint32_t blended = Channel(row[75], 16);
            CHECK(blended >= 190);  // 0.75 * 255 = 191.25
            CHECK(blended <= 192);
            CHECK(Channel(row[74], 16) == 0);
            CHECK(Channel(row[76], 16) == 0);
        }
    }
}

}  // namespace ombra::test
