#include "ombra/gputopo.h"
#include "ombra/brand.h"

#include "ombra/encode_target.h"
#include "ombra/log.h"

#pragma warning(push, 0)
#include "ffnvcodec/nvEncodeAPI.h"
#pragma warning(pop)

#include <d3d11.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <mferror.h>
#include <combaseapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace ombra {

namespace {

// Logs via Log::Info, which per log.h already echoes every write to stdout — so this does not
// also fprintf to `out` (kept as a parameter for callers that might redirect it, currently unused)
// or every gputest line would print twice, doubling the output intended for bug reports.
void Report(FILE* out, const char* fmt, ...) {
    (void)out;
    char    buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log::Info("%s", buf);
}

}  // namespace

std::vector<AdapterFacts> EnumerateAdapters(HMONITOR target_monitor, bool driver_versions) {
    std::vector<AdapterFacts> result;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return result;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            AdapterFacts fa;
            fa.luid         = desc.AdapterLuid;
            fa.description  = desc.Description;
            fa.vendor_id    = desc.VendorId;
            fa.dedicated_mb = static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0);
            fa.software     = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            // The user-mode driver's version, as Device Manager shows it. IDXGIDevice is the one
            // interface CheckInterfaceSupport still answers for (D3D10-era API).
            LARGE_INTEGER umd{};
            if (driver_versions && SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                wchar_t v[48];
                swprintf(v, 48, L"%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                         HIWORD(umd.LowPart), LOWORD(umd.LowPart));
                fa.driver_version = v;
            }

            IDXGIOutput* output = nullptr;
            for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(output->GetDesc(&od))) {
                    fa.output_devices.push_back(od.DeviceName);
                    if (target_monitor && od.Monitor == target_monitor) fa.owns_target_monitor = true;
                }
                output->Release();
                output = nullptr;
            }
            result.push_back(std::move(fa));
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return result;
}

NvencProbe ProbeNvenc(IDXGIAdapter1* adapter) {
    NvencProbe result;

    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL    level{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr) || !device) {
        result.fail_reason = "D3D11CreateDevice failed";
        if (context) context->Release();
        if (device) device->Release();
        return result;
    }

    HMODULE dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll) {
        result.fail_reason = "nvEncodeAPI64.dll not found (no NVIDIA driver)";
        context->Release();
        device->Release();
        return result;
    }

    using GetMaxVer_t  = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using CreateInst_t = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto get_max     = reinterpret_cast<GetMaxVer_t>(GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion"));
    auto create_inst = reinterpret_cast<CreateInst_t>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    if (!get_max || !create_inst) {
        result.fail_reason = "GetProcAddress failed on nvEncodeAPI64.dll";
        FreeLibrary(dll);
        context->Release();
        device->Release();
        return result;
    }

    uint32_t driver_max = 0;
    if (get_max(&driver_max) != NV_ENC_SUCCESS) {
        result.fail_reason = "NvEncodeAPIGetMaxSupportedVersion failed";
        FreeLibrary(dll);
        context->Release();
        device->Release();
        return result;
    }
    char verbuf[16];
    snprintf(verbuf, sizeof(verbuf), "%u.%u", driver_max >> 4, driver_max & 0xF);
    result.driver_version = verbuf;

    NV_ENCODE_API_FUNCTION_LIST fn{};
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create_inst(&fn) != NV_ENC_SUCCESS || !fn.nvEncOpenEncodeSessionEx) {
        result.fail_reason = "NvEncodeAPICreateInstance failed";
        FreeLibrary(dll);
        context->Release();
        device->Release();
        return result;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = device;
    open.apiVersion = NVENCAPI_VERSION;
    void* enc = nullptr;
    NVENCSTATUS s = fn.nvEncOpenEncodeSessionEx(&open, &enc);
    if (s != NV_ENC_SUCCESS || !enc) {
        result.fail_reason = "nvEncOpenEncodeSessionEx failed (not an NVIDIA adapter, or no NVENC on it)";
        FreeLibrary(dll);
        context->Release();
        device->Release();
        return result;
    }
    result.session_ok = true;

    uint32_t guid_count = 0;
    if (fn.nvEncGetEncodeGUIDCount(enc, &guid_count) == NV_ENC_SUCCESS && guid_count > 0) {
        std::vector<GUID> guids(guid_count);
        uint32_t          filled = 0;
        if (fn.nvEncGetEncodeGUIDs(enc, guids.data(), guid_count, &filled) == NV_ENC_SUCCESS) {
            for (uint32_t i = 0; i < filled; ++i) {
                if (guids[i] == NV_ENC_CODEC_H264_GUID) result.h264 = true;
                else if (guids[i] == NV_ENC_CODEC_HEVC_GUID) result.hevc = true;
                else if (guids[i] == NV_ENC_CODEC_AV1_GUID) result.av1 = true;
            }
        }
    }

    // Must not leave a session open — LogGpuTopologyStartup runs before the engine's real
    // Encoder::Open(), and NVENC sessions are limited per-process/per-adapter by the driver.
    if (fn.nvEncDestroyEncoder) fn.nvEncDestroyEncoder(enc);
    FreeLibrary(dll);
    context->Release();
    device->Release();
    return result;
}

MftCaps ProbeMftHardwareEncoders() {
    MftCaps result;

    HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool own_co = SUCCEEDED(co_hr);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (own_co) CoUninitialize();
        return result;
    }

    struct { const GUID* subtype; std::vector<std::wstring>* bucket; } wanted[] = {
        {&MFVideoFormat_H264, &result.h264_encoders},
        {&MFVideoFormat_HEVC, &result.hevc_encoders},
    };

    for (auto& w : wanted) {
        MFT_REGISTER_TYPE_INFO out_type{MFMediaType_Video, *w.subtype};
        IMFActivate** activates = nullptr;
        UINT32        count     = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, nullptr,
                                &out_type, &activates, &count);
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count; ++i) {
                wchar_t* name = nullptr;
                UINT32   name_len = 0;
                if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name,
                                                                &name_len))) {
                    w.bucket->push_back(name);
                    CoTaskMemFree(name);
                }
                activates[i]->Release();
            }
        }
        if (activates) CoTaskMemFree(activates);
    }

    MFShutdown();
    if (own_co) CoUninitialize();
    return result;
}

bool ParseEngineInstanceLuid(const std::wstring& instance, LUID* luid) {
    const size_t luid_pos = instance.find(L"luid_0x");
    if (luid_pos == std::wstring::npos) return false;
    const wchar_t* p   = instance.c_str() + luid_pos + 7;  // past "luid_0x"
    wchar_t*       end = nullptr;
    const unsigned long hi = wcstoul(p, &end, 16);
    if (!end || *end != L'_' || end[1] != L'0' || end[2] != L'x') return false;
    const unsigned long lo = wcstoul(end + 3, nullptr, 16);
    *luid = LUID{static_cast<DWORD>(lo), static_cast<LONG>(hi)};
    return true;
}

ProcessAdapterResult QueryProcessAdapter(DWORD pid) {
    ProcessAdapterResult result;

    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return result;

    // Wildcard-expand so we only add counters for instances that actually belong to `pid`, and
    // remember each matched instance's raw path so the LUID can be parsed back out of it —
    // instance names have no separate "get the LUID" API, the LUID is only in the string
    // (`pid_<pid>_luid_0x<hi>_0x<lo>_phys_<n>_eng_<n>_engtype_<type>`).
    PDH_HCOUNTER             util_counters[64]{};
    std::vector<std::wstring> instance_paths;

    wchar_t match[64];
    swprintf(match, std::size(match), L"pid_%lu_luid_0x", static_cast<unsigned long>(pid));

    // A full-system `GPU Engine` wildcard expansion (every process x every engine) easily exceeds
    // a few KB, so PdhExpandWildCardPathW's PDH_MORE_DATA on a too-small buffer must be handled by
    // resizing and retrying, not silently swallowed as "no instances" (which is what a fixed 8K
    // buffer produced on this dev machine — dwm.exe's own GPU Engine instances went missing).
    std::vector<wchar_t> path_buf(8192);
    DWORD                path_len = static_cast<DWORD>(path_buf.size());
    PDH_STATUS           expand_st;
    for (;;) {
        expand_st = PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                                            path_buf.data(), &path_len, 0);
        if (expand_st == PDH_MORE_DATA && path_buf.size() < (1u << 22)) {
            path_buf.resize(path_len);
            continue;
        }
        break;
    }
    if (expand_st == ERROR_SUCCESS) {
        for (wchar_t* p = path_buf.data(); *p && instance_paths.size() < std::size(util_counters);) {
            std::wstring instance_path(p);
            if (instance_path.find(match) != std::wstring::npos) {
                PDH_HCOUNTER c = nullptr;
                if (PdhAddEnglishCounterW(query, instance_path.c_str(), 0, &c) == ERROR_SUCCESS) {
                    util_counters[instance_paths.size()] = c;
                    instance_paths.push_back(instance_path);
                }
            }
            p += instance_path.size() + 1;
        }
    }

    if (instance_paths.empty()) {
        PdhCloseQuery(query);
        return result;  // process not found among current GPU Engine instances
    }

    // PDH utilization counters need two samples with a gap to produce a meaningful rate — the
    // first collect just primes the counter (returns PDH_INVALID_DATA / zero if read).
    PdhCollectQueryData(query);
    Sleep(1000);
    PdhCollectQueryData(query);

    // A process can hold GPU Engine instances on more than one adapter at once (e.g. rendering on
    // the dGPU while presenting through iGPU instances in a T2 topology) — picking "whichever
    // instance PDH happened to enumerate first" silently reports the wrong adapter in exactly the
    // T3/T4 case is the highest-value message in the feature. Tally per LUID
    // instead (3D utilization separately from total) and pick the LUID with the most 3D activity.
    struct LuidTally {
        LUID   luid{};
        double util_3d    = 0;
        double util_total = 0;
    };
    std::vector<LuidTally> tallies;

    for (size_t i = 0; i < instance_paths.size(); ++i) {
        PDH_FMT_COUNTERVALUE val{};
        double                util = 0;
        if (PdhGetFormattedCounterValue(util_counters[i], PDH_FMT_DOUBLE, nullptr, &val) ==
                ERROR_SUCCESS &&
            val.CStatus == PDH_CSTATUS_VALID_DATA) {
            util = val.doubleValue;
        }

        LUID luid{};
        if (!ParseEngineInstanceLuid(instance_paths[i], &luid)) continue;

        LuidTally* t = nullptr;
        for (auto& existing : tallies)
            if (SameLuid(existing.luid, luid)) {
                t = &existing;
                break;
            }
        if (!t) {
            tallies.push_back(LuidTally{luid, 0, 0});
            t = &tallies.back();
        }
        t->util_total += util;
        if (instance_paths[i].find(L"engtype_3d") != std::wstring::npos) t->util_3d += util;
    }

    if (!tallies.empty()) {
        const LuidTally* best = &tallies[0];
        for (auto& t : tallies)
            if (t.util_3d > best->util_3d) best = &t;
        if (best->util_3d == 0) {
            // No 3D activity anywhere — fall back to whichever LUID has the most total activity
            // across all engine types, rather than reporting the first one enumerated.
            for (auto& t : tallies)
                if (t.util_total > best->util_total) best = &t;
        }
        result.found        = true;
        result.luid         = best->luid;
        result.util_percent = best->util_total;
    }

    for (size_t i = 0; i < instance_paths.size(); ++i) PdhRemoveCounter(util_counters[i]);
    PdhCloseQuery(query);
    return result;
}

bool ReadUserGpuPreference(const std::wstring& exe_path, int& pref_out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"SOFTWARE\\Microsoft\\DirectX\\UserGpuPreferences", 0, KEY_READ,
                       &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t data[256]{};
    DWORD   data_size = sizeof(data);
    DWORD   type       = 0;
    LSTATUS st = RegQueryValueExW(key, exe_path.c_str(), nullptr, &type,
                                   reinterpret_cast<BYTE*>(data), &data_size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ) return false;

    // Value looks like "GpuPreference=2;" — pull the digit after '='.
    std::wstring value(data);
    size_t       eq = value.find(L'=');
    if (eq == std::wstring::npos || eq + 1 >= value.size()) return false;
    pref_out = value[eq + 1] - L'0';
    return pref_out >= 0 && pref_out <= 2;
}

Topology ClassifyTopology(bool encoder_present, bool display_on_encoder, bool game_found,
                           bool game_on_encoder, bool mixed_output_ownership) {
    if (!encoder_present) return Topology::T5;
    if (mixed_output_ownership) return Topology::T6;
    if (game_found) {
        if (!display_on_encoder && !game_on_encoder) return Topology::T3;
        if (display_on_encoder && !game_on_encoder) return Topology::T4;
    }
    if (display_on_encoder) return Topology::T1;
    return Topology::T2;
}

std::vector<RecordingGpuCaps> RecordingGpuCapsOf(const StartupGpuSummary& summary) {
    std::vector<RecordingGpuCaps> caps;
    for (const auto& g : summary.recording_gpus)
        caps.push_back({g.vendor, g.av1, g.av1_align_w, g.av1_align_h, g.drives_display});
    return caps;
}

Topology ClassifyForEncoder(const StartupGpuSummary& summary, const LUID& encoder_luid) {
    const bool encoder_present = summary.encoder_available &&
                                 (encoder_luid.HighPart != 0 || encoder_luid.LowPart != 0);
    return ClassifyTopology(encoder_present, SameLuid(summary.display_owner_luid, encoder_luid),
                            /*game_found=*/false, /*game_on_encoder=*/false,
                            summary.mixed_output_ownership);
}

const char* TopologyLabel(Topology t) {
    switch (t) {
        case Topology::T1: return "T1";
        case Topology::T2: return "T2";
        case Topology::T3: return "T3";
        case Topology::T4: return "T4";
        case Topology::T5: return "T5";
        case Topology::T6: return "T6";
        default: return "unknown";
    }
}

const char* TopologyDescription(Topology t) {
    switch (t) {
        case Topology::T1: return "optimal — the display is on the recording GPU";
        case Topology::T2: return "hybrid — the display is on another GPU; frames cross adapters";
        case Topology::T3: return "game and display both on a different GPU than the recording one";
        case Topology::T4: return "display on the recording GPU, game on another GPU";
        case Topology::T5: return "no supported hardware encoder (NVIDIA NVENC or AMD AMF) found";
        case Topology::T6: return "mixed — monitors owned by different adapters";
        default: return "unknown";
    }
}

namespace {

const AdapterFacts* FindNvidia(const std::vector<AdapterFacts>& adapters) {
    for (auto& a : adapters)
        if (a.vendor_id == kVendorNvidia && !a.software) return &a;
    return nullptr;
}

const AdapterFacts* FindByLuid(const std::vector<AdapterFacts>& adapters, const LUID& luid) {
    for (auto& a : adapters)
        if (SameLuid(a.luid, luid)) return &a;
    return nullptr;
}

}  // namespace

bool MixedOutputOwnership(const std::vector<AdapterFacts>& adapters) {
    int owners = 0;
    for (const auto& a : adapters)
        if (!a.output_devices.empty()) ++owners;
    return owners > 1;
}

int RunGpuTest(FILE* out, DWORD pid) {
    Report(out, OMBRA_PRODUCT_NAME " -- GPU topology");

    HMONITOR target = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto     adapters = EnumerateAdapters(target, /*driver_versions=*/true);

    const AdapterFacts* display_owner = nullptr;
    for (auto& a : adapters)
        if (a.owns_target_monitor) display_owner = &a;
    const bool mixed_ownership = MixedOutputOwnership(adapters);

    for (auto& a : adapters) {
        Report(out, "  adapter  %-40ls  LUID=0x%08lX%08lX  vendor=0x%04X  %.0f MB%s%s",
               a.description.c_str(), static_cast<unsigned long>(a.luid.HighPart),
               static_cast<unsigned long>(a.luid.LowPart), a.vendor_id, a.dedicated_mb,
               a.software ? "  [software]" : "", a.owns_target_monitor ? "  [display owner]" : "");
        for (auto& od : a.output_devices)
            Report(out, "    output %ls", od.c_str());
    }

    const AdapterFacts* nvidia = FindNvidia(adapters);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(target, &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    Report(out, "  Display  %ls  %ux%u @%u  owned by  %ls", mi.szDevice, dm.dmPelsWidth,
           dm.dmPelsHeight, dm.dmDisplayFrequency,
           display_owner ? display_owner->description.c_str() : L"(unknown)");

    NvencProbe nvenc_probe;
    if (nvidia) {
        // Re-open the adapter by LUID via a fresh IDXGIFactory (EnumerateAdapters didn't keep the
        // IDXGIAdapter1* interfaces alive).
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.AdapterLuid.HighPart == nvidia->luid.HighPart &&
                    desc.AdapterLuid.LowPart == nvidia->luid.LowPart) {
                    nvenc_probe = ProbeNvenc(adapter);
                    adapter->Release();
                    break;
                }
                adapter->Release();
                adapter = nullptr;
            }
            factory->Release();
        }
        Report(out, "  Encoder  %ls  [dGPU, NVENC %s]  H264=%s HEVC=%s AV1=%s%s",
               nvidia->description.c_str(),
               nvenc_probe.session_ok ? nvenc_probe.driver_version.c_str() : "n/a",
               nvenc_probe.h264 ? "yes" : "no", nvenc_probe.hevc ? "yes" : "no",
               nvenc_probe.av1 ? "yes" : "no",
               nvenc_probe.session_ok ? "" : ("  (probe failed: " + nvenc_probe.fail_reason + ")").c_str());
    } else {
        Report(out, "  Encoder  none — no NVIDIA adapter found");
    }

    MftCaps mft = ProbeMftHardwareEncoders();
    Report(out, "  MFT hw encoders (QSV and others; NOT an AMF probe)  H264=%zu  HEVC=%zu",
           mft.h264_encoders.size(), mft.hevc_encoders.size());
    for (auto& n : mft.h264_encoders) Report(out, "    h264  %ls", n.c_str());
    for (auto& n : mft.hevc_encoders) Report(out, "    hevc  %ls", n.c_str());
    Report(out, "  AMF      %s", AmfUsable() ? "usable (probe passed; details in the log)"
                                              : "not usable (no AMD adapter, or the probe failed)");

    const auto encode = PickEncodeAdapter(adapters, "auto", AmfAdapterCapsSnapshot());
    const AdapterFacts* encoder = encode ? FindByLuid(adapters, encode->luid) : nullptr;
    if (encoder)
        Report(out, "  Records  %ls  (%s, encode.adapter=auto)", encoder->description.c_str(),
               BackendName(encode->backend));

    bool                 game_found      = pid != 0;
    bool                 game_on_encoder = false;
    ProcessAdapterResult game_adapter;
    if (game_found) {
        game_adapter = QueryProcessAdapter(pid);
        if (game_adapter.found && encoder) game_on_encoder = SameLuid(game_adapter.luid, encoder->luid);
        if (game_adapter.found) {
            Report(out, "  Game     pid %lu  rendering on LUID=0x%08lX%08lX  util=%.1f%%  [%s]",
                   static_cast<unsigned long>(pid),
                   static_cast<unsigned long>(game_adapter.luid.HighPart),
                   static_cast<unsigned long>(game_adapter.luid.LowPart), game_adapter.util_percent,
                   game_on_encoder ? "recording GPU" : "other GPU");
        } else {
            Report(out, "  Game     pid %lu  not found among current GPU Engine instances",
                   static_cast<unsigned long>(pid));
            game_found = false;
        }

        // Read-only, informational — never written back.
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t exe_path[MAX_PATH]{};
            DWORD   len = static_cast<DWORD>(std::size(exe_path));
            if (QueryFullProcessImageNameW(proc, 0, exe_path, &len)) {
                int pref = 0;
                if (ReadUserGpuPreference(exe_path, pref)) {
                    static const char* kPrefName[] = {"let Windows decide", "power saving (iGPU)",
                                                        "high performance (dGPU)"};
                    Report(out, "  GPU pref %ls -- %s", exe_path,
                           kPrefName[pref >= 0 && pref <= 2 ? pref : 0]);
                }
            }
            CloseHandle(proc);
        }
    }

    const bool display_on_encoder = encoder && display_owner == encoder;
    Topology   topo = ClassifyTopology(encoder != nullptr, display_on_encoder,
                                        game_found && game_adapter.found, game_on_encoder,
                                        mixed_ownership);
    Report(out, "  Topology %s -- %s", TopologyLabel(topo), TopologyDescription(topo));

    return 0;
}

StartupGpuSummary LogGpuTopologyStartup(const std::string& encode_preference,
                                        const AmfProbeResult& amf_probe, HMONITOR monitor) {
    HMONITOR target = monitor ? monitor : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto     adapters = EnumerateAdapters(target, /*driver_versions=*/true);
    for (auto& a : adapters) {
        Log::InfoW(L"gputopo adapter  %s  driver %s  LUID=0x%08lX%08lX  vendor=0x%04X  %.0f MB%s%s",
                   a.description.c_str(), a.driver_version.empty() ? L"?" : a.driver_version.c_str(),
                   static_cast<unsigned long>(a.luid.HighPart),
                   static_cast<unsigned long>(a.luid.LowPart), a.vendor_id, a.dedicated_mb,
                   a.software ? L"  [software]" : L"", a.owns_target_monitor ? L"  [display owner]" : L"");
    }
    const AdapterFacts* nvidia = FindNvidia(adapters);
    StartupGpuSummary  summary;
    const auto encode = PickEncodeAdapter(adapters, encode_preference, amf_probe.adapters);
    summary.encoder_available = encode.has_value();
    {
        const AdapterFacts* display_owner = nullptr;
        for (auto& a : adapters)
            if (a.owns_target_monitor) display_owner = &a;
        if (display_owner) {
            summary.display_owner_description = display_owner->description;
            summary.display_owner_luid        = display_owner->luid;
        }
        summary.mixed_output_ownership = MixedOutputOwnership(adapters);
        summary.topology = ClassifyForEncoder(summary, encode ? encode->luid : LUID{});
        Log::Info("gputopo topology %s -- %s", TopologyLabel(summary.topology),
                  TopologyDescription(summary.topology));
    }
    if (nvidia) {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.AdapterLuid.HighPart == nvidia->luid.HighPart &&
                    desc.AdapterLuid.LowPart == nvidia->luid.LowPart) {
                    NvencProbe p = ProbeNvenc(adapter);
                    Log::Info("gputopo nvenc    ok=%d H264=%d HEVC=%d AV1=%d driver=%s (%s)",
                              p.session_ok, p.h264, p.hevc, p.av1, p.driver_version.c_str(),
                              p.fail_reason.c_str());
                    if (p.session_ok && p.h264)
                        summary.recording_gpus.push_back({"nvidia", "NVENC", nvidia->description, p.hevc, p.av1,
                                                          1, 1, nvidia->owns_target_monitor});
                    adapter->Release();
                    break;
                }
                adapter->Release();
                adapter = nullptr;
            }
            factory->Release();
        }
    } else {
        Log::Info("gputopo nvenc    no NVIDIA adapter");
    }
    for (const auto& caps : amf_probe.adapters) {
        if (!caps.h264) continue;
        const AdapterFacts* amd = FindByLuid(adapters, caps.luid);
        if (amd) summary.recording_gpus.push_back({"amd", "AMF", amd->description, caps.hevc, caps.av1,
                                                   caps.av1_align_w, caps.av1_align_h, amd->owns_target_monitor});
    }
    // The CPU encoder records on any machine; always offered, always last.
    summary.recording_gpus.push_back({"cpu", "x264", L"CPU (x264)", false, false});
    MftCaps mft = ProbeMftHardwareEncoders();
    Log::Info("gputopo mft      H264=%zu HEVC=%zu (QSV and other MFT hw encoders)",
              mft.h264_encoders.size(), mft.hevc_encoders.size());
    return summary;
}

}  // namespace ombra
