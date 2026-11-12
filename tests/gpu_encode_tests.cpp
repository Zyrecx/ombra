// Real NVENC without capture: solid colours drawn into a D3D11 texture must decode back to those
// colours, and the stream must say how to decode them (BT.709, limited range). This is the check
// colortest left to a person looking at a file.
//
// "gpu" suite: runs only with `ombra-tests --gpu`.

#include <doctest/doctest.h>

#include "support/amf_caps.h"
#include "support/media_probe.h"
#include "support/process.h"
#include "support/test_env.h"

#include "ombra/av1.h"
#include "ombra/capture.h"
#include "ombra/config.h"
#include "ombra/encode_target.h"
#include "ombra/encoder.h"
#include "ombra/x264_encoder.h"

#include <optional>
#include "ombra/util.h"

#include <d3d11_4.h>
#include <inspectable.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

namespace {

// The NVENC adapter, however this machine's topology enumerates.
EncodeAdapter NvidiaAdapter() {
    auto picked = PickEncodeAdapter(EnumerateAdapters(nullptr), "nvidia", {});
    REQUIRE_MESSAGE(picked.has_value(), "no NVIDIA adapter");
    REQUIRE(picked->backend == EncoderBackend::Nvenc);
    return *picked;
}

// The AMD adapter, or nullopt when this machine has none. AMF runs in-process here: the engine's
// out-of-process probe guards a resident recorder, not a test run someone is watching.
std::optional<EncodeAdapter> AmdAdapter() {
    auto picked = PickEncodeAdapter(EnumerateAdapters(nullptr), "amd", AllAmdH264Caps());
    if (!picked || picked->backend != EncoderBackend::Amf) return std::nullopt;
    return picked;
}

// The CPU encoder on WARP, the software rasterizer: the NV12 shaders, the readback and x264 run with
// no GPU at all, so these tests are in the default suite.
EncodeAdapter WarpCpuAdapter() {
    for (const auto& a : EnumerateAdapters(nullptr))
        if (a.software) return EncodeAdapter{EncoderBackend::X264, a.luid, a.description};
    FAIL("no software (WARP) adapter");
    return {};
}

struct Colour {
    const char* name;
    uint8_t     r, g, b;
};

// Mean of one BGRA channel (0 = B, 1 = G, 2 = R) over a centred 64x64 block.
double CentreMean(const VideoBgra& video, size_t frame, int channel) {
    constexpr uint32_t kBlock = 64;
    const uint32_t     x0     = (video.width - kBlock) / 2;
    const uint32_t     y0     = (video.height - kBlock) / 2;
    const uint8_t*     px     = video.Frame(frame);
    double             sum    = 0;
    for (uint32_t y = y0; y < y0 + kBlock; ++y)
        for (uint32_t x = x0; x < x0 + kBlock; ++x) sum += px[(size_t(y) * video.width + x) * 4 + channel];
    return sum / (kBlock * kBlock);
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    if (!EnsureParentDir(path)) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}


// Packet indices ffprobe flags as key frames, in stream order.
std::vector<size_t> KeyPackets(const std::wstring& file) {
    const ProcessResult r = RunProcess(Quote(FfprobePath()) +
                                       L" -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 " +
                                       Quote(file));
    REQUIRE(r.exit_code == 0);
    std::vector<size_t> keys;
    std::istringstream lines(r.out);
    std::string line;
    for (size_t i = 0; std::getline(lines, line); ++i)
        if (!line.empty() && line[0] == 'K') keys.push_back(i);
    return keys;
}

// Solid colours drawn into a BGRA texture on `adapter`, encoded by its backend, decoded by ffmpeg.
// The raw stream is written as-is (Annex-B .h264 or low-overhead .obu), so nothing of the muxer is
// under test: the encoder's colour, size, tags and key-frame flags against what ffprobe parses.
// `coded` is the size the stream is expected to decode at, when the encoder pads (AMF AV1 off its
// 64x16 grid); the encoder's sequence header must say the same.
void RoundTripColours(const EncodeAdapter& adapter, const std::wstring& out_name,
                      VideoCodec codec = VideoCodec::H264, uint32_t kWidth = 320, uint32_t kHeight = 180,
                      VideoSize coded = {}) {
    if (coded.width == 0) coded = {kWidth, kHeight};
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(adapter.luid, device, context, error), error);
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);  // as Capture::Open does

    constexpr uint32_t kFps = 30, kFramesPerColour = 20;
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = kWidth;
    td.Height           = kHeight;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;  // what Capture's stage slots use
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, texture.put())));
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(device->CreateRenderTargetView(texture.get(), nullptr, rtv.put())));

    auto           encoder_ptr = CreateVideoEncoder(adapter.backend);
    IVideoEncoder& encoder     = *encoder_ptr;
    EncoderOptions opts;
    opts.codec = codec;
    REQUIRE_MESSAGE(encoder.TryOpen(device.get(), kWidth, kHeight, kFps, 4000, 1, opts), encoder.FaultReason());
    REQUIRE(encoder.Codec() == codec);
    REQUIRE_MESSAGE(encoder.RegisterSlot(texture.get(), kWidth, kHeight), encoder.FaultReason());
    MESSAGE("sequence params " << encoder.SequenceParams().size() << " bytes");
    if (codec == VideoCodec::AV1) {
        uint32_t w = 0, h = 0;
        REQUIRE(Av1CodedSize(encoder.SequenceParams().data(), encoder.SequenceParams().size(), w, h));
        CHECK(w == coded.width);
        CHECK(h == coded.height);
    }

    const Colour colours[] = {
        {"red", 255, 0, 0},      {"green", 0, 255, 0},     {"blue", 0, 0, 255},
        {"grey", 128, 128, 128}, {"white", 255, 255, 255}, {"near-black", 16, 16, 16},
        {"orange", 255, 128, 0},
    };

    std::vector<uint8_t> stream;
    std::vector<size_t>  keys;  // indices the encoder flagged is_keyframe
    uint64_t             timestamp = 0;
    for (const Colour& c : colours) {
        const float rgba[4] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f};
        context->ClearRenderTargetView(rtv.get(), rgba);
        context->Flush();
        for (uint32_t i = 0; i < kFramesPerColour; ++i) {
            const bool first = timestamp == 0;
            const auto au    = encoder.EncodeSlot(0, kWidth, kHeight, timestamp++);
            REQUIRE_MESSAGE(au.ok, encoder.FaultReason());
            if (first) CHECK_MESSAGE(au.is_keyframe, "the first AU must be an IDR");
            if (au.is_keyframe) keys.push_back(static_cast<size_t>(timestamp - 1));
            stream.insert(stream.end(), au.data, au.data + au.size);
            encoder.UnlockLast();
        }
    }
    encoder.Close();

    const std::wstring file = TestOutputPath(L"gpu\\" + out_name);
    REQUIRE(WriteFileBytes(file, stream));

    const ProbeInfo probe = Probe(file);
    REQUIRE_MESSAGE(probe.ok, probe.error);
    const auto video = probe.Of("video");
    REQUIRE(video.size() == 1);
    CHECK(video[0].codec_name == (codec == VideoCodec::AV1 ? "av1" : "h264"));
    CHECK(video[0].width == coded.width);
    CHECK(video[0].height == coded.height);
    CHECK(video[0].color_range == "tv");
    CHECK(video[0].color_primaries == "bt709");
    CHECK(video[0].color_transfer == "bt709");
    CHECK(video[0].color_space == "bt709");
    CHECK(DecodeErrors(file).empty());
    // A one-second GOP: key frames every kFps frames, and the encoder's flag on exactly those.
    std::vector<size_t> expected_keys;
    for (size_t i = 0; i < std::size(colours) * kFramesPerColour; i += kFps) expected_keys.push_back(i);
    CHECK(keys == expected_keys);
    CHECK(KeyPackets(file) == expected_keys);

    const VideoBgra decoded = DecodeVideoBgra(file);
    REQUIRE(decoded.FrameCount() == std::size(colours) * kFramesPerColour);
    constexpr double kTolerance = 5.0;  // 8-bit YUV rounding on a near-lossless flat frame
    for (size_t i = 0; i < std::size(colours); ++i) {
        const Colour& c     = colours[i];
        const size_t  frame = (i + 1) * kFramesPerColour - 1;  // rate control has settled by now
        const double  r     = CentreMean(decoded, frame, 2);
        const double  g     = CentreMean(decoded, frame, 1);
        const double  b     = CentreMean(decoded, frame, 0);
        CAPTURE(c.name);
        MESSAGE(std::string(c.name) << ": sent " << int(c.r) << "," << int(c.g) << "," << int(c.b) << "  decoded "
                       << r << "," << g << "," << b);
        CHECK(std::abs(r - c.r) <= kTolerance);
        CHECK(std::abs(g - c.g) <= kTolerance);
        CHECK(std::abs(b - c.b) <= kTolerance);
    }
    if (coded.height > kHeight) {
        // What the padding decodes as, for the Codec card's note. Last frame, last colour.
        const size_t   last = decoded.FrameCount() - 1;
        const uint8_t* px   = decoded.Frame(last) + size_t(coded.height - 1) * decoded.width * 4 + size_t(kWidth / 2) * 4;
        MESSAGE("padding row " << coded.height - 1 << " decodes as " << int(px[2]) << "," << int(px[1]) << ","
                               << int(px[0]) << " (the picture is " << std::string(colours[std::size(colours) - 1].name) << ")");
    }
}

}  // namespace

TEST_CASE("gpu: NVENC round-trips solid colours and tags the stream BT.709 limited" *
          doctest::test_suite("gpu")) {
    RoundTripColours(NvidiaAdapter(), L"colours.h264");
}

TEST_CASE("gpu: AMF round-trips solid colours and tags the stream BT.709 limited" *
          doctest::test_suite("gpu")) {
    const auto amd = AmdAdapter();
    if (!amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }
    RoundTripColours(*amd, L"colours-amf.h264");
}

TEST_CASE("gpu: NVENC AV1 round-trips solid colours, tags BT.709 limited, flags key frames" *
          doctest::test_suite("gpu")) {
    RoundTripColours(NvidiaAdapter(), L"colours.obu", VideoCodec::AV1);
}

TEST_CASE("gpu: AMF AV1 round-trips solid colours, tags BT.709 limited, flags key frames" *
          doctest::test_suite("gpu")) {
    const auto amd = AmdAdapter();
    if (!amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }
    // 320x176: on RDNA3's 64x16 grid, so it codes exactly.
    RoundTripColours(*amd, L"colours-amf.obu", VideoCodec::AV1, 320, 176);
}

// Off that grid it pads whatever AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE says (1080 lines code as
// 1082). encode.amf_av1_unaligned=pad relies on the sequence header naming the padded size, which
// the mux writes. Expected sizes are PredictPaddedSize's, so a driver that pads differently fails here.
TEST_CASE("gpu: AMF AV1 off its alignment grid pads, and the sequence header says to what" *
          doctest::test_suite("gpu")) {
    const auto amd = AmdAdapter();
    if (!amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }
    uint32_t aw = 1, ah = 1;
    REQUIRE(AmfAv1AlignmentInProcess(*amd, aw, ah));
    MESSAGE("AV1 alignment " << aw << "x" << ah);
    RoundTripColours(*amd, L"colours-amf-1080p.obu", VideoCodec::AV1, 1920, 1080,
                     PredictPaddedSize({1920, 1080}, aw, ah));
    RoundTripColours(*amd, L"colours-amf-180.obu", VideoCodec::AV1, 320, 180, PredictPaddedSize({320, 180}, aw, ah));
}

// The fallback path, forced: a GPU "without" AV1 opens H.264 and says so.
TEST_CASE("gpu: an encoder asked for AV1 on a GPU without it opens H.264" * doctest::test_suite("gpu")) {
    SetAv1UnsupportedForTest(true);
    const EncodeAdapter adapter = NvidiaAdapter();
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(adapter.luid, device, context, error), error);
    auto encoder = CreateVideoEncoder(adapter.backend);
    EncoderOptions opts;
    opts.codec = VideoCodec::AV1;
    REQUIRE_MESSAGE(encoder->TryOpen(device.get(), 320, 180, 30, 4000, 1, opts), encoder->FaultReason());
    CHECK(encoder->Codec() == VideoCodec::H264);
    CHECK(!encoder->SequenceParams().empty());
    CHECK(encoder->SequenceParams()[0] == 0);  // Annex-B start code, not an OBU
    encoder->Close();
    if (const auto amd = AmdAdapter()) {
        winrt::com_ptr<ID3D11Device>        amd_device;
        winrt::com_ptr<ID3D11DeviceContext> amd_context;
        REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(amd->luid, amd_device, amd_context, error), error);
        auto amf = CreateVideoEncoder(amd->backend);
        REQUIRE_MESSAGE(amf->TryOpen(amd_device.get(), 320, 180, 30, 4000, 1, opts), amf->FaultReason());
        CHECK(amf->Codec() == VideoCodec::H264);
        amf->Close();
    }
    SetAv1UnsupportedForTest(false);
}


// Settings offers all seven. H.264's p6 and p7 turn lookahead on in their own config, so every
// frame faulted with NV_ENC_ERR_NEED_MORE_INPUT (found 2026-11-03).
TEST_CASE("gpu: every NVENC preset gives one frame out for one frame in" * doctest::test_suite("gpu")) {
    const EncodeAdapter adapter = NvidiaAdapter();
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(adapter.luid, device, context, error), error);
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    constexpr uint32_t kW = 320, kH = 180;
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = kW;
    td.Height           = kH;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, texture.put())));

    for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::AV1}) {
        for (const char* preset : {"p1", "p2", "p3", "p4", "p5", "p6", "p7"}) {
            INFO(VideoCodecName(codec), " ", preset);
            auto encoder = CreateVideoEncoder(adapter.backend);
            EncoderOptions opts;
            opts.codec  = codec;
            opts.preset = preset;
            REQUIRE_MESSAGE(encoder->TryOpen(device.get(), kW, kH, 30, 2000, 1, opts), encoder->FaultReason());
            REQUIRE_MESSAGE(encoder->RegisterSlot(texture.get(), kW, kH), encoder->FaultReason());
            for (uint64_t t = 0; t < 5; ++t) {
                const auto au = encoder->EncodeSlot(0, kW, kH, t);
                REQUIRE_MESSAGE(au.ok, encoder->FaultReason());
                CHECK(au.size > 0);
                encoder->UnlockLast();
            }
            encoder->Close();
        }
    }
}

TEST_CASE("x264 on WARP round-trips solid colours, tags BT.709 limited, flags key frames") {
    RoundTripColours(WarpCpuAdapter(), L"x264_colours.h264");
}

TEST_CASE("x264 asked for AV1 opens H.264, with SPS and PPS only in its sequence params") {
    const EncodeAdapter adapter = WarpCpuAdapter();
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(adapter.luid, device, context, error), error);
    auto encoder = CreateVideoEncoder(adapter.backend);
    EncoderOptions opts;
    opts.codec = VideoCodec::AV1;
    REQUIRE_MESSAGE(encoder->TryOpen(device.get(), 320, 180, 30, 4000, 1, opts), encoder->FaultReason());
    CHECK(encoder->Codec() == VideoCodec::H264);
    // Two start-code-prefixed NALs, SPS (7) then PPS (8): x264's version SEI must not reach avcC.
    const auto& sp = encoder->SequenceParams();
    std::vector<uint8_t> types;
    for (size_t i = 0; i + 4 < sp.size(); ++i)
        if (sp[i] == 0 && sp[i + 1] == 0 && sp[i + 2] == 1) types.push_back(sp[i + 3] & 0x1F);
    CHECK(types == std::vector<uint8_t>{7, 8});
    // An odd size is refused (4:2:0), not silently cropped: ResolveOutputSize hands out even sizes.
    auto odd = CreateVideoEncoder(adapter.backend);
    CHECK_FALSE(odd->TryOpen(device.get(), 321, 180, 30, 4000, 1, {}));
}

// What CPU mode costs per tick, on the real capture GPU: 1080p with a busy, moving picture (a fresh
// noise-and-gradient frame uploaded before every encode, outside the timing), each preset at 60 fps.
// Prints p50/p99 of EncodeSlot, which is what the pacer waits on: NV12 passes, readback and x264.
TEST_CASE("gpu: x264 per-frame latency at 1080p" * doctest::test_suite("gpu")) {
    auto picked = ResolveEncodeAdapter("cpu", MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    REQUIRE(picked);
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(picked->luid, device, context, error), error);
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    MESSAGE("capture adapter: " << NarrowUtf8(picked->description));

    constexpr uint32_t kW = 1920, kH = 1080, kFrames = 300;
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = kW;
    td.Height           = kH;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, texture.put())));

    // A gradient that scrolls, plus blocks of hash noise that move: motion everywhere, some of it
    // compressible, roughly like game footage and unlike a flat test card.
    std::vector<uint32_t> pixels(size_t(kW) * kH);
    auto draw = [&](uint32_t f) {
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                uint32_t h = (x / 8 + f) * 2654435761u ^ (y / 8) * 40503u;
                h ^= h >> 13;
                const uint8_t n = ((x / 64 + y / 64 + f / 4) % 3 == 0) ? uint8_t(h) : 0;
                const uint8_t r = uint8_t((x + f * 4) & 0xFF) ^ n, g = uint8_t((y + f * 2) & 0xFF), b = uint8_t((x + y) / 16) ^ n;
                pixels[size_t(y) * kW + x] = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
            }
        context->UpdateSubresource(texture.get(), 0, nullptr, pixels.data(), kW * 4, 0);
    };

    for (const char* preset : {"p1", "p5"}) {
        auto           encoder = CreateVideoEncoder(EncoderBackend::X264);
        EncoderOptions opts;
        opts.preset = preset;
        REQUIRE_MESSAGE(encoder->TryOpen(device.get(), kW, kH, 60, 20000, 2, opts), encoder->FaultReason());
        REQUIRE(encoder->RegisterSlot(texture.get(), kW, kH));
        // CPU time spent encoding: the whole process minus this thread's own drawing.
        auto cpu_s = [](HANDLE h, bool thread) {
            FILETIME c, e, k, u;
            (thread ? GetThreadTimes(h, &c, &e, &k, &u) : GetProcessTimes(h, &c, &e, &k, &u));
            auto v = [](FILETIME t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7; };
            return v(k) + v(u);
        };
        double              draw_cpu = 0;
        const double        proc0    = cpu_s(GetCurrentProcess(), false);
        std::vector<double> ms;
        for (uint32_t f = 0; f < kFrames; ++f) {
            const double d0 = cpu_s(GetCurrentThread(), true);
            draw(f);
            draw_cpu += cpu_s(GetCurrentThread(), true) - d0;
            context->Flush();
            const int64_t t0 = QpcNow();
            const auto    au = encoder->EncodeSlot(0, kW, kH, f);
            ms.push_back(double(QpcNow() - t0) * 1000.0 / double(QpcFrequency()));
            REQUIRE_MESSAGE(au.ok, encoder->FaultReason());
            encoder->UnlockLast();
        }
        const double encode_cpu = cpu_s(GetCurrentProcess(), false) - proc0 - draw_cpu;
        encoder->Close();
        std::sort(ms.begin(), ms.end());
        const double p50 = ms[ms.size() / 2], p99 = ms[ms.size() * 99 / 100];
        MESSAGE("x264 " << std::string(X264PresetFor(preset)) << " 1080p60: p50 " << p50 << " ms, p99 " << p99
                        << " ms, max " << ms.back() << " ms, " << encode_cpu / (kFrames / 60.0)
                        << " cores at 60 fps");
        CHECK(p50 < 16.6);
    }
}

// Capture's frame copies (WGC pool thread) and NVENC's input mapping (pacer thread) share one
// immediate context, and only Capture's own calls take its mutex, so the device has to be
// multithread-protected. The WinRT wrapper WGC needs does not do it (checked here first, since
// that's the assumption that was wrong); Capture::Open must. Opens a WGC session on the primary
// monitor without starting it: nothing is drawn and no border appears.
TEST_CASE("gpu: Capture's device is multithread-protected for NVENC on another thread" *
          doctest::test_suite("gpu")) {
    {
        winrt::com_ptr<ID3D11Device>        device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        std::string                         error;
        REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(NvidiaAdapter().luid, device, context, error), error);
        const auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> winrt_device;
        REQUIRE(SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), winrt_device.put())));
        MESSAGE("WinRT wrapper alone turns protection on: "
                << device.as<ID3D11Multithread>()->GetMultithreadProtected());
    }

    Capture capture;
    REQUIRE(capture.Open(/*capture_cursor=*/false, CaptureTarget{}, NvidiaAdapter().luid));
    winrt::com_ptr<ID3D11Device> device;
    device.copy_from(capture.Device());
    CHECK(device.as<ID3D11Multithread>()->GetMultithreadProtected() == TRUE);
    capture.Close();
}

// A measurement, not a pass/fail check: AMF at 1080p30 on the AMD GPU while other devices on it
// copy 256 MB textures back to back. Reports how many frames needed more than one QueryOutput wait
// and how long. Opt-in (OMBRA_TEST_AMF_LOAD=N, N load threads): it saturates the GPU that drives the display.
TEST_CASE("gpu: AMF under a saturated GPU waits for late frames instead of faulting" *
          doctest::test_suite("gpu")) {
    char flag[8]{};
    if (!GetEnvironmentVariableA("OMBRA_TEST_AMF_LOAD", flag, sizeof flag)) {
        MESSAGE("OMBRA_TEST_AMF_LOAD unset: skipped");
        return;
    }
    const auto amd = AmdAdapter();
    if (!amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }

    // Wall-clock cap. At kLoadCapS the load stops (does a stuck encode come back once the GPU is
    // free?); at kHardCapS the process exits, naming the phase it was stuck in.
    constexpr int64_t        kLoadCapS = 60, kHardCapS = 90;
    const int64_t            started = QpcNow();
    auto                     elapsed_ms = [&] { return (QpcNow() - started) * 1000 / QpcFrequency(); };
    std::atomic<const char*> phase{"load start"};
    std::atomic<uint32_t>    at_frame{0};
    std::atomic<bool>        done{false};
    auto                     trace = [&](const char* p) {
        phase = p;
        fprintf(stderr, "[amf load %6lld ms] %s\n", static_cast<long long>(elapsed_ms()), p);
        fflush(stderr);
    };

    std::atomic<bool>        stop{false};
    std::atomic<uint64_t>    copies{0};
    const int                threads = (std::min)(16, (std::max)(1, atoi(flag)));
    // Per load thread: 0 starting, 1 copying, 2 waiting for the GPU, 3 tearing down, 4 exited, 5 failed setup.
    std::atomic<int>         state[16]{};
    std::atomic<uint64_t>    batches[16]{};
    std::thread              cap([&] {
        bool load_stopped = false;
        while (!done.load()) {
            Sleep(250);
            const int64_t s = elapsed_ms() / 1000;
            if (!load_stopped && s >= kLoadCapS) {
                load_stopped = true;
                fprintf(stderr, "[amf load] %llds cap: stopping load (phase %s, frame %u)\n",
                        static_cast<long long>(kLoadCapS), phase.load(), at_frame.load());
                fflush(stderr);
                stop = true;
            }
            if (s >= kHardCapS) {
                fprintf(stderr, "[amf load] %llds hard cap: stuck in %s at frame %u, %llu copies; exiting\n",
                        static_cast<long long>(kHardCapS), phase.load(), at_frame.load(),
                        static_cast<unsigned long long>(copies.load()));
                for (int t = 0; t < threads; ++t)
                    fprintf(stderr, "[amf load]   thread %d: state %d, %llu batches\n", t, state[t].load(),
                            static_cast<unsigned long long>(batches[t].load()));
                fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    });
    std::vector<std::thread> load;
    for (int t = 0; t < threads; ++t) {
        load.emplace_back([&, t, luid = amd->luid] {
            struct Exit {
                std::atomic<int>& s;
                ~Exit() {
                    if (s.load() != 5) s = 4;
                }
            } on_exit{state[t]};
            winrt::com_ptr<ID3D11Device>        dev;
            winrt::com_ptr<ID3D11DeviceContext> ctx;
            std::string                         err;
            if (!CreateD3D11DeviceOnAdapter(luid, dev, ctx, err)) {
                state[t] = 5;
                return;
            }
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 8192;
            d.MipLevels = d.ArraySize = 1;
            d.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage            = D3D11_USAGE_DEFAULT;
            d.BindFlags        = D3D11_BIND_RENDER_TARGET;
            winrt::com_ptr<ID3D11Texture2D> a, b;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, a.put())) || FAILED(dev->CreateTexture2D(&d, nullptr, b.put())))
            {
                state[t] = 5;
                return;
            }
            // Keep at most two batches queued. Unbounded, the runtime accepts minutes of copies up
            // front and teardown waits for all of them.
            D3D11_QUERY_DESC                qd{D3D11_QUERY_EVENT, 0};
            winrt::com_ptr<ID3D11Query>     fence[2];
            if (FAILED(dev->CreateQuery(&qd, fence[0].put())) || FAILED(dev->CreateQuery(&qd, fence[1].put()))) {
                state[t] = 5;
                return;
            }
            for (uint64_t n = 0; !stop.load(); ++n) {
                ID3D11Query* f = fence[n % 2].get();
                state[t] = 2;
                if (n >= 2) {
                    BOOL signalled = FALSE;
                    while (ctx->GetData(f, &signalled, sizeof signalled, 0) == S_FALSE && !stop.load()) Sleep(1);
                }
                state[t] = 1;
                for (int i = 0; i < 8; ++i) {
                    ctx->CopyResource(b.get(), a.get());
                    ctx->CopyResource(a.get(), b.get());
                }
                ctx->End(f);
                ctx->Flush();
                copies += 16;
                ++batches[t];
            }
            state[t] = 3;
        });
    }

    constexpr uint32_t kW = 1920, kH = 1080, kFps = 30, kFrames = 300;
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    REQUIRE_MESSAGE(CreateD3D11DeviceOnAdapter(amd->luid, device, context, error), error);
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = td.ArraySize = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, texture.put())));
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(device->CreateRenderTargetView(texture.get(), nullptr, rtv.put())));

    trace("encoder create");
    auto           encoder = CreateVideoEncoder(EncoderBackend::Amf);
    EncoderOptions opts;
    opts.codec = VideoCodec::H264;
    trace("TryOpen");
    const bool opened = encoder->TryOpen(device.get(), kW, kH, kFps, 12000, 2, opts);
    trace("RegisterSlot");
    const bool registered = opened && encoder->RegisterSlot(texture.get(), kW, kH);

    std::vector<double> ms;
    uint32_t            encoded = 0;
    bool                capped  = false;
    trace("encode loop");
    for (uint32_t i = 0; registered && i < kFrames; ++i) {
        if (stop.load()) {
            capped = true;
            break;
        }
        at_frame = i;
        const float c[4] = {float(i % 30) / 30.f, 0.5f, float(i % 7) / 7.f, 1.f};
        context->ClearRenderTargetView(rtv.get(), c);
        const int64_t t0 = QpcNow();
        const auto    au = encoder->EncodeSlot(0, kW, kH, i);
        ms.push_back(double(QpcNow() - t0) * 1000.0 / double(QpcFrequency()));
        if (ms.back() > 100.0)
            fprintf(stderr, "[amf load %6lld ms] frame %u took %.0f ms\n", static_cast<long long>(elapsed_ms()), i,
                    ms.back());
        if (!au.ok) break;
        ++encoded;
        encoder->UnlockLast();
        if (i % 30 == 0) {
            fprintf(stderr, "[amf load %6lld ms] frame %u, %llu copies, batches/state",
                    static_cast<long long>(elapsed_ms()), i, static_cast<unsigned long long>(copies.load()));
            for (int t = 0; t < threads; ++t)
                fprintf(stderr, " %llu/%d", static_cast<unsigned long long>(batches[t].load()), state[t].load());
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        Sleep(1000 / kFps);
    }
    trace("stopping load");
    stop = true;
    for (auto& t : load) t.join();
    trace("done");
    done = true;
    cap.join();
    REQUIRE_MESSAGE(opened, encoder->FaultReason());
    REQUIRE(registered);
    if (capped) MESSAGE("load cap hit at frame " << encoded);

    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted.empty() ? 0.0 : sorted[size_t(p * double(sorted.size() - 1))]; };
    MESSAGE("AMF under load: " << encoded << "/" << kFrames << " frames, " << copies.load()
                                << " 256 MB copies; encode p50 " << pct(0.5) << " ms, p99 " << pct(0.99)
                                << ", max " << pct(1.0) << "; late " << encoder->LateFrames() << ", longest wait "
                                << encoder->LongestWaitMs() << " ms; fault: "
                                << (encoder->Faulted() ? encoder->FaultReason() : std::string("none")));
    CHECK(encoded == kFrames);
}

}  // namespace ombra::test

