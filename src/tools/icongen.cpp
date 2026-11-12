// Build-time generator for the executables' icon resource: renders the brand mark (brand_mark.h,
// mono ring) at the standard Windows icon sizes and packs them into one .ico of PNG frames.
// CMake runs it and both .rc files embed the result, so the .ico can't drift from the tray and
// overlay renderer. Usage: ombra-icongen OUT.ico
// `ombra-icongen --svg OUT.svg` writes the vector mark (BrandMarkSvg); docs/img/ombra-mark.svg is its
// output, and a ui test fails if the two drift. `--png SIZE OUT.png` writes one raster of it.
#include "ombra/brand_mark.h"

#include <cwchar>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool EncodePng(int size, std::vector<uint8_t>* out) {
    constexpr uint32_t rgb = ombra::kBrandMarkMonoRgb;
    const std::vector<uint8_t> pixels =
        ombra::RenderBrandMark(size, ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                               (rgb & 255) / 255.0f, ombra::BrandMarkAlpha::Straight);

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(size, size))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;  // straight alpha, as rendered
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(size, size * 4, static_cast<UINT>(pixels.size()),
                                  const_cast<BYTE*>(pixels.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return false;
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    const auto* data = static_cast<const uint8_t*>(GlobalLock(mem));
    if (!data) return false;
    out->assign(data, data + st.cbSize.QuadPart);
    GlobalUnlock(mem);
    return true;
}

void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 255); b.push_back(v >> 8); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { Put16(b, v & 0xFFFF); Put16(b, v >> 16); }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--svg") == 0) {
        const std::string svg = ombra::BrandMarkSvg(ombra::kBrandMarkMonoRgb);
        FILE* out = nullptr;
        if (_wfopen_s(&out, argv[2], L"wb") != 0 || !out) {
            std::fprintf(stderr, "ombra-icongen: can't write %ls\n", argv[2]);
            return 1;
        }
        const bool ok = std::fwrite(svg.data(), 1, svg.size(), out) == svg.size();
        return (std::fclose(out) == 0 && ok) ? 0 : 1;
    }
    // --png SIZE OUT.png: one large raster of the mark (the demo video's end card).
    if (argc == 4 && std::wcscmp(argv[1], L"--png") == 0) {
        const int size = _wtoi(argv[2]);
        std::vector<uint8_t> png;
        if (size < 16 || size > 4096 || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || !EncodePng(size, &png)) return 1;
        FILE* out = nullptr;
        if (_wfopen_s(&out, argv[3], L"wb") != 0 || !out) return 1;
        const bool ok = std::fwrite(png.data(), 1, png.size(), out) == png.size();
        return (std::fclose(out) == 0 && ok) ? 0 : 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: ombra-icongen OUT.ico | --svg OUT.svg | --png SIZE OUT.png\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    constexpr int kSizes[] = {16, 24, 32, 48, 64, 128, 256};
    constexpr size_t kCount = sizeof(kSizes) / sizeof(kSizes[0]);
    std::vector<std::vector<uint8_t>> frames(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        if (!EncodePng(kSizes[i], &frames[i])) {
            std::fprintf(stderr, "ombra-icongen: PNG encode failed at %d px\n", kSizes[i]);
            return 1;
        }
    }

    // ICONDIR, one ICONDIRENTRY per frame, then the PNG frames.
    std::vector<uint8_t> ico;
    Put16(ico, 0);
    Put16(ico, 1);  // type: icon
    Put16(ico, static_cast<uint16_t>(kCount));
    uint32_t offset = static_cast<uint32_t>(6 + 16 * kCount);
    for (size_t i = 0; i < kCount; ++i) {
        const uint8_t wh = kSizes[i] >= 256 ? 0 : static_cast<uint8_t>(kSizes[i]);  // 0 means 256
        ico.push_back(wh);
        ico.push_back(wh);
        ico.push_back(0);  // palette
        ico.push_back(0);  // reserved
        Put16(ico, 1);     // planes
        Put16(ico, 32);    // bits per pixel
        Put32(ico, static_cast<uint32_t>(frames[i].size()));
        Put32(ico, offset);
        offset += static_cast<uint32_t>(frames[i].size());
    }
    for (const auto& f : frames) ico.insert(ico.end(), f.begin(), f.end());

    FILE* out = nullptr;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || !out) {
        std::fprintf(stderr, "ombra-icongen: can't write %ls\n", argv[1]);
        return 1;
    }
    const bool ok = std::fwrite(ico.data(), 1, ico.size(), out) == ico.size();
    return (std::fclose(out) == 0 && ok) ? 0 : 1;
}
