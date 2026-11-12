// Dev tool: encodes raw BGRA frames from stdin with the engine's own encoders (NvencEncoder,
// AmfEncoder, x264 through IVideoEncoder, opened exactly as the pipeline opens them) and writes the
// raw stream (Annex-B H.264 or low-overhead AV1 OBUs) for ffmpeg to score. Used to calibrate the
// quality presets per codec (scripts/encbench.py drives the sweep and VMAF). Not shipped.
//
//   ffmpeg -i in.y4m -vf scale=in_color_matrix=bt709:in_range=tv,format=bgra -f rawvideo - |
//     ombra-encbench nvidia|amd|cpu av1|h264 WIDTH HEIGHT FPS BITRATE_KBPS OUT [PRESET] [RC]
//
// AMD's AV1 pads off its alignment grid (1080 lines come out as 1082), as the engine's "pad" mode
// does; the caller crops. AMF runs in-process here, as the gpu tests do (amf_caps.h).
#include "ombra/encode_target.h"
#include "ombra/encoder.h"
#include "ombra/gputopo.h"

#include <d3d11_4.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ombra;

int main(int argc, char** argv) {
    // An isolated instance, as ombra-tests and --drive are: the encoders log, and without this the
    // lines land in the real log. Set before the first Log call, which
    // reads OMBRA_HOME once (paths.h). A caller's own OMBRA_HOME wins.
    if (GetEnvironmentVariableW(L"OMBRA_HOME", nullptr, 0) == 0) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring home = std::wstring(temp) + L"ombra-encbench";
        CreateDirectoryW(home.c_str(), nullptr);
        SetEnvironmentVariableW(L"OMBRA_HOME", home.c_str());
    }
    if (argc < 8) {
        std::fprintf(stderr, "usage: ombra-encbench nvidia|amd|cpu av1|h264 W H FPS KBPS OUT [PRESET] [RC] [CQ] [MAXKBPS]\n");
        return 2;
    }
    const std::string vendor = argv[1];
    const VideoCodec  codec  = std::string(argv[2]) == "av1" ? VideoCodec::AV1 : VideoCodec::H264;
    const uint32_t    w = std::strtoul(argv[3], nullptr, 10), h = std::strtoul(argv[4], nullptr, 10);
    const uint32_t    fps  = std::strtoul(argv[5], nullptr, 10);
    const uint32_t    kbps = std::strtoul(argv[6], nullptr, 10);
    const char*       out_path = argv[7];

    std::vector<AmfAdapterCaps> amf;
    const auto adapters = EnumerateAdapters(nullptr);
    for (const auto& a : adapters) {
        if (a.vendor_id != kVendorAmd || a.software) continue;
        AmfAdapterCaps c;
        c.luid = a.luid;
        c.h264 = true;
        amf.push_back(c);
    }
    const auto adapter = PickEncodeAdapter(adapters, vendor, amf);
    const EncoderBackend want = vendor == "nvidia" ? EncoderBackend::Nvenc
                              : vendor == "amd"    ? EncoderBackend::Amf
                                                   : EncoderBackend::X264;
    if (!adapter || adapter->backend != want) {
        std::fprintf(stderr, "no %s encoder on this machine\n", vendor.c_str());
        return 1;
    }

    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         error;
    if (!CreateD3D11DeviceOnAdapter(adapter->luid, device, context, error)) {
        std::fprintf(stderr, "device: %s\n", error.c_str());
        return 1;
    }
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);  // as Capture::Open does

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;  // Capture's stage slot format
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&td, nullptr, texture.put()))) return 1;

    auto           encoder = CreateVideoEncoder(adapter->backend);
    EncoderOptions opts;
    opts.codec = codec;
    if (argc > 8) opts.preset = argv[8];
    if (argc > 9) opts.rate_control = argv[9];
    if (argc > 10) opts.cq = static_cast<uint32_t>(std::atoi(argv[10]));
    if (argc > 11) opts.max_bitrate_kbps = static_cast<uint32_t>(std::atoi(argv[11]));
    if (!encoder->TryOpen(device.get(), w, h, fps, kbps, 2, opts) || !encoder->RegisterSlot(texture.get(), w, h)) {
        std::fprintf(stderr, "open: %s\n", encoder->FaultReason().c_str());
        return 1;
    }
    if (encoder->Codec() != codec) {
        std::fprintf(stderr, "encoder opened %s instead\n", VideoCodecName(encoder->Codec()));
        return 1;
    }

    FILE* out = nullptr;
    if (fopen_s(&out, out_path, "wb") != 0 || !out) return 1;
    _setmode(_fileno(stdin), _O_BINARY);
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 4);
    uint64_t             n = 0, bytes = 0;
    double               encode_ms = 0;
    while (std::fread(frame.data(), 1, frame.size(), stdin) == frame.size()) {
        context->UpdateSubresource(texture.get(), 0, nullptr, frame.data(), w * 4, 0);
        context->Flush();
        const auto t0 = std::chrono::steady_clock::now();
        const auto au = encoder->EncodeSlot(0, w, h, n);
        encode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!au.ok) {
            std::fprintf(stderr, "encode: %s\n", encoder->FaultReason().c_str());
            return 1;
        }
        std::fwrite(au.data, 1, au.size, out);
        bytes += au.size;
        encoder->UnlockLast();
        ++n;
    }
    encoder->Close();
    std::fclose(out);
    // One machine-readable line for the driver script.
    std::printf("frames=%llu bytes=%llu kbps=%.0f encode_ms_avg=%.2f\n", n, bytes,
                n ? bytes * 8.0 * fps / n / 1000.0 : 0.0, n ? encode_ms / n : 0.0);
    return n ? 0 : 1;
}
