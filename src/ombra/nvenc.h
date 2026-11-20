// NVENC, loaded at runtime with LoadLibrary: no import library, no SDK install. The encoder is
// compiled twice, against the vendored API 13.0 header (nvenc13.cpp) and the API 11.1 one
// (nvenc11.cpp), from one body (nvenc_impl.inl). A driver older than 570 stops at an older API and
// refuses 13.0 sessions; NVIDIA keeps older APIs working on newer drivers, so such a driver records
// through the 11.1 copy (H.264 only: 11.1 has no AV1). Which copy runs is decided here, once, for
// both the encoder and the startup probe.
//
// No NVIDIA header is included here: the two define the same names under the same include guard.
#pragma once

#include "ombra/encoder.h"

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ombra {

// NVENC API versions as NvEncodeAPIGetMaxSupportedVersion reports them: (major << 4) | minor.
inline constexpr uint32_t kNvencApi13_0 = (13u << 4) | 0u;
inline constexpr uint32_t kNvencApi11_1 = (11u << 4) | 1u;

enum class NvencApi {
    V13_0,   // the vendored 13.0 header: H.264 and AV1
    V11_1,   // the vendored 11.1 header: H.264
    TooOld,  // below 11.1: no copy this build carries will open
};
// The copy that runs on a driver whose maximum API is `driver_max`.
NvencApi NvencApiFor(uint32_t driver_max);

// The driver's maximum API, lowered to the test cap below when one is set; 0 without an NVIDIA
// driver. Loads and frees nvEncodeAPI64.dll.
uint32_t NvencDriverMaxApi();
// What the copies compare against: `reported` lowered to the test cap.
uint32_t NvencEffectiveMaxApi(uint32_t reported);
// Test seam: makes the driver look like it stops at `max_api` (0 lifts the cap), so the 11.1 copy
// and the too-old path run on a current driver. The environment variable OMBRA_NVENC_MAX_API
// ("11.1", "11.0") sets the same cap for a whole engine run.
void SetNvencMaxApiForTest(uint32_t max_api);
// The same cap for a scope, lifted on exit even when a test aborts.
struct NvencApiCapForTest {
    explicit NvencApiCapForTest(uint32_t max_api) { SetNvencMaxApiForTest(max_api); }
    ~NvencApiCapForTest() { SetNvencMaxApiForTest(0); }
    NvencApiCapForTest(const NvencApiCapForTest&)            = delete;
    NvencApiCapForTest& operator=(const NvencApiCapForTest&) = delete;
};

// One throwaway session on `device`, through the copy NvencApiFor picks; never leaves it open.
struct NvencSessionProbe {
    bool        session_ok = false;
    bool        h264 = false, hevc = false, av1 = false;  // what this build can record there
    // The GPU lists AV1 but the driver's API is older than any copy that encodes it.
    bool        av1_needs_newer_driver = false;
    bool        driver_too_old = false;  // below 11.1: no session tried
    uint32_t    driver_max_api = 0;
    std::string fail_reason;
};
NvencSessionProbe ProbeNvencSession(ID3D11Device* device);

// The encoder for the copy NvencApiFor picks. Below 11.1 it is the 11.1 copy, whose Open fails
// with IVideoEncoder::kFaultDriverTooOld.
std::unique_ptr<IVideoEncoder> CreateNvencEncoder();

// The two copies (nvenc13.cpp, nvenc11.cpp).
namespace nvenc13 {
std::unique_ptr<IVideoEncoder> CreateEncoder();
void ProbeSession(void* create_instance, ID3D11Device* device, NvencSessionProbe& out);
}  // namespace nvenc13
namespace nvenc11 {
std::unique_ptr<IVideoEncoder> CreateEncoder();
void ProbeSession(void* create_instance, ID3D11Device* device, NvencSessionProbe& out);
}  // namespace nvenc11

}  // namespace ombra
