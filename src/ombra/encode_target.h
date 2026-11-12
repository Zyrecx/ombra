// Which GPU records: the adapter whose hardware encoder a pipeline uses. Capture's D3D11 device lives
// on the same adapter (WGC copies into textures the encoder reads directly), so this one choice
// places both.
//
// `encode.adapter` is persisted as a vendor ("auto" | "nvidia" | "amd"), never a LUID: LUIDs change
// on reboot. It is re-resolved on every pipeline build, so an adapter that vanished (MUX switch,
// driver reset) falls back to auto on the next rebuild instead of refusing to start.
#pragma once

#include "ombra/amf_probe.h"
#include "ombra/gputopo.h"

#include <windows.h>
#include <d3d11.h>

#include <winrt/base.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ombra {

// X264 is the CPU encoder (x264_encoder.h): its "adapter" is only where capture runs.
enum class EncoderBackend { Nvenc, Amf, X264 };

inline constexpr UINT kVendorNvidia = 0x10DE;
inline constexpr UINT kVendorAmd    = 0x1002;

const char* BackendName(EncoderBackend b);  // "NVENC" / "AMF" / "x264"

struct EncodeAdapter {
    EncoderBackend backend = EncoderBackend::Nvenc;
    LUID           luid{};
    std::wstring   description;
    // Sizes its AV1 encoder codes exactly (PlanEncode). 1x1 for NVENC, the probe's caps for AMF.
    uint32_t       av1_align_w = 1, av1_align_h = 1;
};

inline bool SameLuid(const LUID& a, const LUID& b) {
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

// Pure: picks from an enumerated adapter list. `amf` is the out-of-process probe's per-adapter
// caps (amf_probe.h): an AMD adapter alone is not enough (AMF init can hang or crash on a bad
// driver), and on a Ryzen iGPU + Radeon dGPU machine each has its own answer. Only an adapter the
// probe found H.264-capable encodes through AMF; of several, the AV1-capable one, then the one with
// the most dedicated memory (the discrete GPU). "auto" takes NVIDIA when `game_gpu` (nullable: the
// adapter the recorded game renders on) is an NVIDIA adapter in the list, as that GPU is awake
// anyway; else the adapter that owns the captured monitor (AdapterFacts::owns_target_monitor) when
// it encodes (NVIDIA, or AMD with the probe passed), else NVIDIA, then AMD, then the CPU. An
// explicit vendor that isn't available falls back to auto and says so in `note`. nullopt only when
// there is no adapter at all to capture on (not even WARP).
// The CPU target captures on the adapter that owns the captured monitor (AdapterFacts::
// owns_target_monitor: no cross-adapter copy), else one that drives any display, else any hardware
// adapter, else a software one.
std::optional<EncodeAdapter> PickEncodeAdapter(const std::vector<AdapterFacts>& adapters,
                                               std::string_view preference,
                                               const std::vector<AmfAdapterCaps>& amf,
                                               std::string* note = nullptr, const LUID* game_gpu = nullptr);

// Process-wide result of the AMF probe, set once at startup before any pipeline is built. Default
// empty: no AMF until something has checked it's safe.
void SetAmfAdapterCaps(const std::vector<AmfAdapterCaps>& caps);
std::vector<AmfAdapterCaps> AmfAdapterCapsSnapshot();
bool AmfUsable();  // some adapter's AMF H.264 encoder passed the probe

// Enumerates adapters and picks. Logs a fallback note once per distinct note. `capture_monitor`
// (nullable) is the monitor being recorded; with none, auto has no owner to prefer. `exclude`
// (nullable) leaves one adapter out: the next choice after its encoder wouldn't open. `game_gpu`
// as for PickEncodeAdapter.
std::optional<EncodeAdapter> ResolveEncodeAdapter(std::string_view preference, HMONITOR capture_monitor = nullptr,
                                                  const LUID* exclude = nullptr, const LUID* game_gpu = nullptr);

// The CPU target alone.
std::optional<EncodeAdapter> PickCpuEncodeAdapter(const std::vector<AdapterFacts>& adapters);

// For capturebench: resolve or die() with a reason. The
// resident engine never uses this; its pipelines resolve per build and retry.
EncodeAdapter RequireEncodeAdapter(std::string_view preference);

// A hardware D3D11 device with BGRA support (plus `extra_flags`, e.g. VIDEO_SUPPORT for decoding)
// on the adapter with this LUID. Never dies.
bool CreateD3D11DeviceOnAdapter(const LUID& luid, winrt::com_ptr<ID3D11Device>& device,
                                winrt::com_ptr<ID3D11DeviceContext>& context, std::string& error,
                                UINT extra_flags = 0);

}  // namespace ombra
