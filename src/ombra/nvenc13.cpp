// NVENC through the vendored API 13.0 header (third_party/ffnvcodec): drivers 570 and newer.
#include "ombra/nvenc.h"

#pragma warning(push, 0)
#include "ffnvcodec/nvEncodeAPI.h"
#pragma warning(pop)

static_assert(((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION) == ombra::kNvencApi13_0,
              "nvenc13.cpp must see the 13.0 header");

#define OMBRA_NVENC_NS nvenc13
#include "nvenc_impl.inl"
