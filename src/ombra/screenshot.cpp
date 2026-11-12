#include "ombra/screenshot.h"

#include "ombra/log.h"

#include <windows.h>
#include <wincodec.h>

#include <winrt/base.h>

namespace ombra {

bool WriteBgraPng(const std::wstring& path, uint32_t width, uint32_t height, uint32_t row_pitch,
                   const std::vector<uint8_t>& bgra) {
    if (width == 0 || height == 0 || row_pitch < width * 4 ||
        bgra.size() < static_cast<size_t>(row_pitch) * height) {
        Log::Warn("screenshot: WriteBgraPng called with inconsistent dimensions");
        return false;
    }

    // Force alpha opaque in a local copy — see the header comment. Only the width*4 real bytes of
    // each row are touched, not row_pitch's padding.
    std::vector<uint8_t> opaque = bgra;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = opaque.data() + static_cast<size_t>(y) * row_pitch;
        for (uint32_t x = 0; x < width; ++x) row[x * 4 + 3] = 0xFF;
    }

    winrt::com_ptr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        Log::Warn("screenshot: CoCreateInstance(WICImagingFactory) failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        return false;
    }

    winrt::com_ptr<IWICStream> stream;
    hr = factory->CreateStream(stream.put());
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        Log::Warn("screenshot: could not open \"%ls\" for write (hr=0x%08lX)", path.c_str(),
                   static_cast<unsigned long>(hr));
        return false;
    }

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        Log::Warn("screenshot: PNG encoder init failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        return false;
    }

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(frame.put(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) {
        Log::Warn("screenshot: PNG frame setup failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        return false;
    }
    // WIC is free to hand back a different format than requested if the encoder doesn't support
    // it exactly; PNG's 32bppBGRA support is universal in practice, but check rather than assume.
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        Log::Warn("screenshot: WIC PNG encoder would not accept 32bppBGRA");
        return false;
    }

    hr = frame->WritePixels(height, row_pitch, static_cast<UINT>(opaque.size()), opaque.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) {
        Log::Warn("screenshot: PNG write/commit failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool ReadPngAsBgra(const std::wstring& path, uint32_t& out_width, uint32_t& out_height,
                    std::vector<uint8_t>& out_bgra) {
    winrt::com_ptr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        Log::Warn("ReadPngAsBgra: CoCreateInstance(WICImagingFactory) failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        return false;
    }

    winrt::com_ptr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, decoder.put());
    if (FAILED(hr)) {
        Log::Warn("ReadPngAsBgra: could not open \"%ls\" for decode (hr=0x%08lX)", path.c_str(),
                   static_cast<unsigned long>(hr));
        return false;
    }

    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) {
        Log::Warn("ReadPngAsBgra: GetFrame failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    winrt::com_ptr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.put());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        Log::Warn("ReadPngAsBgra: format conversion to 32bppBGRA failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        return false;
    }

    UINT w = 0, h = 0;
    hr = converter->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        Log::Warn("ReadPngAsBgra: GetSize failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    const uint32_t stride = w * 4;
    out_bgra.assign(static_cast<size_t>(stride) * h, 0);
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out_bgra.size()),
                                out_bgra.data());
    if (FAILED(hr)) {
        Log::Warn("ReadPngAsBgra: CopyPixels failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    out_width  = w;
    out_height = h;
    return true;
}

}  // namespace ombra
