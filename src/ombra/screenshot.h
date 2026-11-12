// Screenshot PNG encoding.
//
// WIC (Windows Imaging Component), not a vendored PNG library — same posture as WASAPI/Media
// Foundation elsewhere in this codebase: an OS API, not a third-party dependency, for a part that
// isn't the project's hard part (the hard parts — NVENC setup, MP4 box math — are hand-rolled;
// PNG encoding is neither novel nor worth reinventing).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ombra {

// Encodes a BGRA8 buffer (DXGI_FORMAT_B8G8R8A8_UNORM byte order — same as WIC's
// GUID_WICPixelFormat32bppBGRA, confirmed against Capture::ReadSlotRegionBGRA's own comment, no
// swizzle needed) as a PNG at `path`. `row_pitch` is the buffer's actual stride in bytes — a
// mapped D3D11 staging texture is not guaranteed tightly packed, and `bgra` must be at least
// `row_pitch * height` bytes.
//
// Alpha is forced to 0xFF in a local copy before encoding: WGC surfaces are not guaranteed
// opaque, and a straight 32bpp encode of non-opaque alpha produces a PNG that looks correct in
// some viewers and transparent-over-black in others.
//
// Must be called with COM already initialized on the caller's thread (CoInitializeEx) — WIC is a
// COM API, same requirement mux.cpp's AacEncoder already has for Media Foundation.
bool WriteBgraPng(const std::wstring& path, uint32_t width, uint32_t height, uint32_t row_pitch,
                   const std::vector<uint8_t>& bgra);

// Test-only (capture_gpu_tests.cpp): decodes `path` back to tightly-packed BGRA8, converting to
// GUID_WICPixelFormat32bppBGRA regardless of the file's stored format, so a test can fill a known
// colour, screenshot it, decode the PNG and compare.
bool ReadPngAsBgra(const std::wstring& path, uint32_t& out_width, uint32_t& out_height,
                    std::vector<uint8_t>& out_bgra);

}  // namespace ombra
