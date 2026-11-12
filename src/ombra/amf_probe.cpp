#include "ombra/amf_probe.h"

#include "ombra/amf_encoder.h"
#include "ombra/encode_target.h"
#include "ombra/log.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ombra {

namespace {

// The child's own deadline, and the parent's (a little longer, so the child's report of a hang
// wins when it can). OBS uses 2.5 s for the same probe.
constexpr DWORD kChildTimeoutMs  = 2500;
constexpr DWORD kParentTimeoutMs = 4000;

uint64_t LuidToU64(const LUID& l) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(l.HighPart)) << 32) | l.LowPart;
}

LUID U64ToLuid(uint64_t v) {
    LUID l{};
    l.LowPart  = static_cast<DWORD>(v & 0xFFFFFFFFu);
    l.HighPart = static_cast<LONG>(v >> 32);
    return l;
}

void WriteOut(const std::string& line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

DWORD WINAPI ChildDeadline(LPVOID) {
    Sleep(kChildTimeoutMs);
    WriteOut("error=AMF init timed out\n");
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

}  // namespace

AmfProbeResult ParseAmfProbeOutput(const std::string& text) {
    AmfProbeResult     r;
    r.ran = true;
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("version=", 0) == 0) {
            r.runtime_version = line.substr(8);
        } else if (line.rfind("error=", 0) == 0) {
            if (r.error.empty()) r.error = line.substr(6);
        } else if (line.rfind("adapter=", 0) == 0) {
            AmfAdapterCaps caps;
            std::istringstream fields(line);
            std::string        field;
            while (fields >> field) {
                const auto eq = field.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = field.substr(0, eq), value = field.substr(eq + 1);
                if (key == "adapter") caps.luid = U64ToLuid(std::strtoull(value.c_str(), nullptr, 16));
                else if (key == "h264") caps.h264 = value == "1";
                else if (key == "hevc") caps.hevc = value == "1";
                else if (key == "av1") caps.av1 = value == "1";
                else if (key == "av1_align") {
                    const auto x = value.find('x');
                    if (x != std::string::npos) {
                        caps.av1_align_w = static_cast<uint32_t>((std::max)(1ul, std::strtoul(value.c_str(), nullptr, 10)));
                        caps.av1_align_h = static_cast<uint32_t>((std::max)(1ul, std::strtoul(value.c_str() + x + 1, nullptr, 10)));
                    }
                }
            }
            r.adapters.push_back(caps);
        }
    }
    if (r.error.empty() && !r.H264Usable())
        r.error = r.adapters.empty() ? "probe reported no AMD adapter" : "no AMD adapter can encode H.264";
    return r;
}

AmfProbeResult FinishAmfProbe(const std::string& output, bool timed_out, unsigned long exit_code) {
    AmfProbeResult result = ParseAmfProbeOutput(output);
    if (timed_out && result.error.empty()) result.error = "probe process hung";
    if ((!timed_out && exit_code != 0 && result.error.empty()) ||
        (output.empty() && result.error == "probe reported no AMD adapter")) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "probe process died (exit 0x%08lX)", exit_code);
        result.error = buf;
    }
    // The child writes an adapter's line before tearing its context down, so a hang or crash after
    // that leaves a report of a driver that just failed the test.
    if (!result.error.empty()) result.adapters.clear();
    return result;
}

int RunAmfProbeChild() {
    if (HANDLE t = CreateThread(nullptr, 0, ChildDeadline, nullptr, 0, nullptr)) CloseHandle(t);

    std::string error;
    const AmfRuntime* rt = LoadAmfRuntime(&error);
    if (!rt) {
        WriteOut("error=" + error + "\n");
        return 0;
    }
    WriteOut("version=" + AmfVersionString(rt->version) + "\n");

    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), factory.put_void()))) {
        WriteOut("error=CreateDXGIFactory1 failed\n");
        return 0;
    }
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId != kVendorAmd || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;

        winrt::com_ptr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, device.put(), nullptr, nullptr)))
            continue;
        amf::AMFContextPtr context;
        if (rt->factory->CreateContext(&context) != AMF_OK) continue;
        if (context->InitDX11(device.get()) != AMF_OK) continue;
        auto has = [&](const wchar_t* component) {
            amf::AMFComponentPtr encoder;
            return rt->factory->CreateComponent(context, component, &encoder) == AMF_OK;
        };
        uint32_t   av1_align_w = 1, av1_align_h = 1;
        const bool av1 = AmfAv1Alignment(*rt, context, av1_align_w, av1_align_h);
        char line[192];
        std::snprintf(line, sizeof(line), "adapter=%016llx h264=%d hevc=%d av1=%d av1_align=%ux%u\n",
                      static_cast<unsigned long long>(LuidToU64(desc.AdapterLuid)),
                      has(AMFVideoEncoderVCE_AVC), has(L"AMFVideoEncoderHW_HEVC"), av1, av1_align_w,
                      av1_align_h);
        WriteOut(line);
        context->Terminate();
    }
    return 0;
}

AmfProbeResult ProbeAmfOutOfProcess() {
    AmfProbeResult skipped;
    bool           any_amd = false;
    for (const auto& a : EnumerateAdapters(nullptr))
        if (a.vendor_id == kVendorAmd && !a.software) any_amd = true;
    if (!any_amd) return skipped;
    // Loaded as data only: checks the driver shipped the runtime without running any of its code.
    HMODULE as_data = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!as_data) {
        Log::Info("amf probe         skipped: AMD adapter present but no amfrt64.dll");
        return skipped;
    }
    FreeLibrary(as_data);

    const DWORD started = GetTickCount();
    AmfProbeResult failed;
    failed.ran = true;

    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        failed.error = "GetModuleFileNameW failed";
        return failed;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        failed.error = "CreatePipe failed";
        return failed;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // Hand the child exactly one handle, its stdout, not whatever else happens to be inheritable.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write_end,
                              sizeof(write_end), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb         = sizeof(si);
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError  = write_end;
    si.StartupInfo.hStdInput  = nullptr;
    si.lpAttributeList        = attrs;

    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" amfprobe";
    PROCESS_INFORMATION pi{};
    // Null environment: the child inherits OMBRA_HOME, like every other child the engine starts.
    const BOOL created = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(write_end);  // the child holds its own copy; EOF arrives when it exits
    if (!created) {
        CloseHandle(read_end);
        failed.error = "could not start the probe process";
        return failed;
    }

    std::string output;
    const DWORD wait = WaitForSingleObject(pi.hProcess, kParentTimeoutMs);
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (;;) {
        char  buf[1024];
        DWORD got = 0;
        if (!ReadFile(read_end, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        output.append(buf, got);
    }
    CloseHandle(read_end);

    AmfProbeResult result = FinishAmfProbe(output, wait == WAIT_TIMEOUT, exit_code);
    result.elapsed_ms = GetTickCount() - started;

    std::string caps;
    for (const auto& a : result.adapters) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  LUID=0x%016llx H264=%d HEVC=%d AV1=%d (align %ux%u)",
                      static_cast<unsigned long long>(LuidToU64(a.luid)), a.h264, a.hevc, a.av1,
                      a.av1_align_w, a.av1_align_h);
        caps += buf;
    }
    Log::Info("amf probe         runtime %s%s  (%lu ms)%s%s",
              result.runtime_version.empty() ? "?" : result.runtime_version.c_str(), caps.c_str(),
              static_cast<unsigned long>(result.elapsed_ms), result.error.empty() ? "" : "  unusable: ",
              result.error.c_str());
    return result;
}

}  // namespace ombra
