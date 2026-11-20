# ffnvcodec 11.1 — the NVENC header for older NVIDIA drivers

`nvEncodeAPI.h` is taken verbatim from [FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers),
tag **n11.1.5.3** (NVENCAPI 11.1, Video Codec SDK 11.1.5). That tag's README names the minimum
driver: Windows 471.41, Linux 470.57.02.

**Why a second header:** the main one (`third_party/ffnvcodec`, 13.0) needs driver 570 or newer, and
NVENC refuses a session whose API version is newer than the driver's. NVIDIA keeps older API
versions working on newer drivers, so the same encoder compiled against this header
(`src/ombra/nvenc11.cpp`) records on drivers from 471.41 up. OBS does the same with an 11.1
compatibility path (`obs-nvenc.c`, `NVENC_COMPAT_VER`). API 11.1 has no AV1: that path records H.264.

Both headers use the same include guard and C names, so each is included by exactly one
translation unit, inside its own namespace (`nvenc13.cpp`, `nvenc11.cpp`).

**Licence:** NVIDIA's MIT-style notice at the top of the header, as for the 13.0 one.

**Do not update** this copy: it exists for drivers that stop at API 11.1.
