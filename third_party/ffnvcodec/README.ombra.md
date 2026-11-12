# ffnvcodec — vendored NVENC header

`nvEncodeAPI.h` is taken verbatim from [FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers),
tag **n13.0.19.1** (NVENCAPI 13.0, i.e. Video Codec SDK 13.0).

**Why vendored rather than requiring the NVIDIA Video Codec SDK:** the SDK download is
login-gated, and the header is all we need. We load `nvEncodeAPI64.dll` at runtime with
`LoadLibrary` + `NvEncodeAPICreateInstance` — no import library, no static link, no SDK install.
This is what OBS does.

**Licence:** the header carries NVIDIA's own MIT-style permissive notice at the top
(copy, modify, merge, publish, distribute, sublicense, sell — attribution only). That is
GPLv2-compatible, so vendoring it does not conflict with Ombra's licence.

**To update:** re-copy `include/ffnvcodec/nvEncodeAPI.h` from a newer tag and bump the tag above.

**Why 13.0 and not 13.1:** driver 596.36 reports `NvEncodeAPIGetMaxSupportedVersion` = 13.0, and
NVENC refuses an `apiVersion` newer than the driver. Bump this only alongside a driver that
advertises the newer API.
