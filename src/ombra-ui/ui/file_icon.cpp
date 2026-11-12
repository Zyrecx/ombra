#include "ombra-ui/ui/file_icon.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

namespace ombra_ui {

namespace {

using Microsoft::WRL::ComPtr;

bool HasExtension(const std::wstring& path, std::initializer_list<const wchar_t*> exts) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    for (const wchar_t* e : exts)
        if (_wcsicmp(path.c_str() + dot, e) == 0) return true;
    return false;
}

// Converted to premultiplied BGRA, and scaled down to fit `px` if it is bigger (a small icon is
// kept at its own size: the widget scales it at draw time).
bool CopyPbgra(IWICImagingFactory* wic, IWICBitmapSource* src, UINT px, FileIcon& out) {
    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || sw == 0 || sh == 0) return false;
    ComPtr<IWICBitmapScaler> scaler;
    if (sw > px || sh > px) {
        const double k = static_cast<double>(px) / (sw > sh ? sw : sh);
        const UINT   dw = (std::max)(1u, static_cast<UINT>(sw * k + 0.5)), dh = (std::max)(1u, static_cast<UINT>(sh * k + 0.5));
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(src, dw, dh, WICBitmapInterpolationModeFant))) {
            return false;
        }
        src = scaler.Get();
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        return false;
    }
    UINT w = 0, h = 0;
    if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
    out.w = w;
    out.h = h;
    out.bgra = std::move(pixels);
    return true;
}

}  // namespace

FileIcon LoadFileIcon(const std::wstring& path, UINT px) {
    FileIcon out;
    out.path = path;
    if (path.empty() || px == 0) return out;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return out;

    if (HasExtension(path, {L".exe", L".dll", L".ico"})) {
        // S_FALSE: the file has no icon resource. The caller draws its own placeholder then, not
        // Windows' generic exe icon.
        HICON icon = nullptr;
        if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, px) == S_OK && icon) {
            ComPtr<IWICBitmap> bmp;
            if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bmp))) CopyPbgra(wic.Get(), bmp.Get(), px, out);
            DestroyIcon(icon);
        }
        return out;
    }

    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame))) {
        CopyPbgra(wic.Get(), frame.Get(), px, out);
    }
    return out;
}

}  // namespace ombra_ui
