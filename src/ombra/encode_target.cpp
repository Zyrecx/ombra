#include "ombra/encode_target.h"

#include "ombra/die.h"
#include "ombra/log.h"

#include <dxgi1_2.h>

#include <cstdio>
#include <mutex>

namespace ombra {

const char* BackendName(EncoderBackend b) {
    switch (b) {
        case EncoderBackend::Amf: return "AMF";
        case EncoderBackend::X264: return "x264";
        default: return "NVENC";
    }
}

namespace {

std::mutex                  g_amf_caps_mutex;
std::vector<AmfAdapterCaps> g_amf_caps;

const AdapterFacts* FindVendor(const std::vector<AdapterFacts>& adapters, UINT vendor) {
    for (const auto& a : adapters)
        if (a.vendor_id == vendor && !a.software) return &a;
    return nullptr;
}

EncodeAdapter MakeTarget(const AdapterFacts& a, EncoderBackend b) {
    return EncodeAdapter{b, a.luid, a.description};
}

const AmfAdapterCaps* CapsFor(const std::vector<AmfAdapterCaps>& amf, const LUID& luid) {
    for (const auto& c : amf)
        if (SameLuid(c.luid, luid)) return &c;
    return nullptr;
}

// The AMD adapter AMF records on, per the probe; nullptr when none passed.
const AdapterFacts* PickAmd(const std::vector<AdapterFacts>& adapters, const std::vector<AmfAdapterCaps>& amf) {
    const AdapterFacts*   best      = nullptr;
    const AmfAdapterCaps* best_caps = nullptr;
    for (const auto& a : adapters) {
        if (a.vendor_id != kVendorAmd || a.software) continue;
        const AmfAdapterCaps* c = CapsFor(amf, a.luid);
        if (!c || !c->h264) continue;
        if (!best || (c->av1 && !best_caps->av1) ||
            (c->av1 == best_caps->av1 && a.dedicated_mb > best->dedicated_mb)) {
            best      = &a;
            best_caps = c;
        }
    }
    return best;
}

}  // namespace

std::optional<EncodeAdapter> PickCpuEncodeAdapter(const std::vector<AdapterFacts>& adapters) {
    const AdapterFacts* pick = nullptr;
    for (const auto& a : adapters)
        if (!pick && !a.software && a.owns_target_monitor) pick = &a;
    for (const auto& a : adapters)
        if (!pick && !a.software && !a.output_devices.empty()) pick = &a;
    for (const auto& a : adapters)
        if (!pick && !a.software) pick = &a;
    if (!pick && !adapters.empty()) pick = &adapters.front();
    if (!pick) return std::nullopt;
    return MakeTarget(*pick, EncoderBackend::X264);
}

std::optional<EncodeAdapter> PickEncodeAdapter(const std::vector<AdapterFacts>& adapters,
                                               std::string_view preference,
                                               const std::vector<AmfAdapterCaps>& amf,
                                               std::string* note, const LUID* game_gpu) {
    const AdapterFacts* nvidia = FindVendor(adapters, kVendorNvidia);
    const AdapterFacts* amd    = PickAmd(adapters, amf);
    auto amf_target = [&](const AdapterFacts& a) {
        EncodeAdapter t = MakeTarget(a, EncoderBackend::Amf);
        const AmfAdapterCaps* c = CapsFor(amf, a.luid);
        t.av1_align_w = c->av1_align_w;
        t.av1_align_h = c->av1_align_h;
        return t;
    };

    if (preference == "cpu") return PickCpuEncodeAdapter(adapters);
    if (preference == "nvidia" && nvidia) return MakeTarget(*nvidia, EncoderBackend::Nvenc);
    if (preference == "amd" && amd) return amf_target(*amd);
    if (note && (preference == "nvidia" || preference == "amd")) {
        *note = std::string("encode.adapter=") + std::string(preference) +
                " is not available (" +
                (preference == "amd" && FindVendor(adapters, kVendorAmd) ? "AMF probe failed"
                                                                          : "no such adapter") +
                "); using auto";
    }
    // Auto. NVENC looks clearly better than AMF on game footage (VMAF, 60 fps, a 780M against an
    // RTX 5060): GTA V H.264 at 15 Mbps 95.8 vs 90.2, and AMF needs 15.4 Mbps where NVENC needs 10
    // for VMAF 90; Dota 2 and CS:GO 1-2.5 points apart. So a game rendering on the NVIDIA GPU, which
    // is awake for it anyway, records there.
    if (game_gpu)
        for (const auto& a : adapters)
            if (!a.software && a.vendor_id == kVendorNvidia && SameLuid(a.luid, *game_gpu))
                return MakeTarget(a, EncoderBackend::Nvenc);
    // Otherwise the GPU that drives the recorded monitor, when it can encode: frames never cross
    // adapters, and a sleeping NVIDIA GPU isn't woken just to record. On the panel (780M owns it,
    // 1080p30 H.264), AMF against NVENC: encode p99 9.5 vs 14.8 ms on the desktop, 10.0 vs 14.3 with
    // a game on the 780M (its 1% low 170 vs 149 fps); 3-6 W less in every case.
    for (const auto& a : adapters) {
        if (a.software || !a.owns_target_monitor) continue;
        if (a.vendor_id == kVendorNvidia) return MakeTarget(a, EncoderBackend::Nvenc);
        const AmfAdapterCaps* c = a.vendor_id == kVendorAmd ? CapsFor(amf, a.luid) : nullptr;
        if (c && c->h264) return amf_target(a);
    }
    if (nvidia) return MakeTarget(*nvidia, EncoderBackend::Nvenc);
    if (amd) return amf_target(*amd);
    return PickCpuEncodeAdapter(adapters);
}

void SetAmfAdapterCaps(const std::vector<AmfAdapterCaps>& caps) {
    std::lock_guard<std::mutex> lock(g_amf_caps_mutex);
    g_amf_caps = caps;
}

std::vector<AmfAdapterCaps> AmfAdapterCapsSnapshot() {
    std::lock_guard<std::mutex> lock(g_amf_caps_mutex);
    return g_amf_caps;
}

bool AmfUsable() {
    std::lock_guard<std::mutex> lock(g_amf_caps_mutex);
    for (const auto& c : g_amf_caps)
        if (c.h264) return true;
    return false;
}

std::optional<EncodeAdapter> ResolveEncodeAdapter(std::string_view preference, HMONITOR capture_monitor,
                                                  const LUID* exclude, const LUID* game_gpu) {
    // DXGI enumeration only: no device opens, so a sleeping GPU stays asleep through the recheck.
    auto adapters = EnumerateAdapters(capture_monitor);
    if (exclude)
        std::erase_if(adapters, [&](const AdapterFacts& a) { return SameLuid(a.luid, *exclude); });
    std::string note;
    auto        picked = PickEncodeAdapter(adapters, preference, AmfAdapterCapsSnapshot(), &note, game_gpu);
    if (!note.empty()) {
        static std::mutex  mutex;
        static std::string last_note;
        std::lock_guard<std::mutex> lock(mutex);
        if (note != last_note) {
            Log::Warn("%s", note.c_str());
            last_note = note;
        }
    }
    return picked;
}

EncodeAdapter RequireEncodeAdapter(std::string_view preference) {
    auto picked = ResolveEncodeAdapter(preference);
    if (!picked) die("no adapter to capture on");
    Log::InfoW(L"encode adapter    %s  (%hs)", picked->description.c_str(), BackendName(picked->backend));
    return *picked;
}

bool CreateD3D11DeviceOnAdapter(const LUID& luid, winrt::com_ptr<ID3D11Device>& device,
                                winrt::com_ptr<ID3D11DeviceContext>& context, std::string& error,
                                UINT extra_flags) {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), factory.put_void()))) {
        error = "CreateDXGIFactory1 failed";
        return false;
    }
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, candidate.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (SameLuid(desc.AdapterLuid, luid)) {
            adapter = candidate;
            break;
        }
    }
    if (!adapter) {
        error = "encode adapter is no longer present";
        return false;
    }
    D3D_FEATURE_LEVEL got_level{};
    const HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT | extra_flags, nullptr, 0,
                                         D3D11_SDK_VERSION, device.put(), &got_level, context.put());
    if (FAILED(hr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "D3D11CreateDevice failed (hr=0x%08lX)",
                      static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }
    return true;
}

}  // namespace ombra
