#include "ombra/nvenc.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace ombra {

namespace {

std::atomic<uint32_t> g_max_api_for_test{0};

// OMBRA_NVENC_MAX_API="major.minor", read once; 0 when unset or malformed.
uint32_t MaxApiFromEnvironment() {
    char        buf[16]{};
    const DWORD n = GetEnvironmentVariableA("OMBRA_NVENC_MAX_API", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return 0;
    unsigned major = 0, minor = 0;
    if (sscanf_s(buf, "%u.%u", &major, &minor) != 2 || major > 0xFFFFFFF || minor > 0xF) return 0;
    return (major << 4) | minor;
}

// The two exports read before a copy is picked, so without either header: NVENCSTATUS is an int-sized
// enum whose NV_ENC_SUCCESS is 0, and NVENCAPI is __stdcall.
using GetMaxVer_t  = int(__stdcall*)(uint32_t*);
constexpr int kNvEncSuccess = 0;

// Loads nvEncodeAPI64.dll and reads its maximum API: 0 when there's no driver or the call fails.
// The caller frees `dll` when it's set.
uint32_t ReportedMaxApi(HMODULE& dll) {
    dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll) return 0;
    auto     get_max = reinterpret_cast<GetMaxVer_t>(GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion"));
    uint32_t max     = 0;
    if (!get_max || get_max(&max) != kNvEncSuccess) return 0;
    return max;
}

}  // namespace

NvencApi NvencApiFor(uint32_t driver_max) {
    if (driver_max >= kNvencApi13_0) return NvencApi::V13_0;
    if (driver_max >= kNvencApi11_1) return NvencApi::V11_1;
    return NvencApi::TooOld;
}

void SetNvencMaxApiForTest(uint32_t max_api) { g_max_api_for_test.store(max_api); }

uint32_t NvencEffectiveMaxApi(uint32_t reported) {
    static const uint32_t from_env = MaxApiFromEnvironment();
    const uint32_t        test     = g_max_api_for_test.load();
    const uint32_t        cap      = test ? test : from_env;
    return cap && cap < reported ? cap : reported;
}

uint32_t NvencDriverMaxApi() {
    HMODULE        dll = nullptr;
    const uint32_t max = ReportedMaxApi(dll);
    if (dll) FreeLibrary(dll);
    return NvencEffectiveMaxApi(max);
}

NvencSessionProbe ProbeNvencSession(ID3D11Device* device) {
    NvencSessionProbe out;
    HMODULE           dll = nullptr;
    const uint32_t    reported = ReportedMaxApi(dll);
    if (!dll) {
        out.fail_reason = "nvEncodeAPI64.dll not found (no NVIDIA driver)";
        return out;
    }
    void* create_inst = reinterpret_cast<void*>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    if (reported == 0 || !create_inst) {
        out.fail_reason = "NvEncodeAPIGetMaxSupportedVersion or NvEncodeAPICreateInstance failed";
        FreeLibrary(dll);
        return out;
    }
    out.driver_max_api = NvencEffectiveMaxApi(reported);
    switch (NvencApiFor(out.driver_max_api)) {
        case NvencApi::V13_0: nvenc13::ProbeSession(create_inst, device, out); break;
        case NvencApi::V11_1: nvenc11::ProbeSession(create_inst, device, out); break;
        case NvencApi::TooOld:
            // The driver refuses every struct version this build has: say so rather than fail a session.
            out.driver_too_old = true;
            out.fail_reason    = IVideoEncoder::kFaultDriverTooOld;
            break;
    }
    FreeLibrary(dll);
    return out;
}

std::unique_ptr<IVideoEncoder> CreateNvencEncoder() {
    const uint32_t max = NvencDriverMaxApi();
    // No driver: the 13.0 copy's Open names what's missing. Below 11.1: the 11.1 copy's Open says
    // the driver is too old.
    if (max == 0 || NvencApiFor(max) == NvencApi::V13_0) return nvenc13::CreateEncoder();
    return nvenc11::CreateEncoder();  // Open logs the driver's API and the one it uses
}

}  // namespace ombra
