// NVENC through the vendored API 11.1 header (third_party/ffnvcodec-11.1): drivers 471.41 to 569,
// H.264 only.
#include "ombra/nvenc.h"

#pragma warning(push, 0)
#include "ffnvcodec-11.1/nvEncodeAPI.h"
#pragma warning(pop)

static_assert(((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION) == ombra::kNvencApi11_1,
              "nvenc11.cpp must see the 11.1 header");

#define OMBRA_NVENC_NS nvenc11
#include "nvenc_impl.inl"
