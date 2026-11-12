// GPU topology probe + encoder capability enumeration.
//
// Everything here is diagnostic: it must never die() or exit() the process, because the point of
// this module is to report what a machine has,
// including "no NVIDIA adapter" or "NVENC session failed to open" as data, not as a fatal error.
// Every probe function returns a status/result struct instead.
//
// Three call sites share this implementation: `ombra.exe gputest` (RunGpuTest), the
// engine's own startup log block (LogGpuTopologyStartup, called once per run and again on
// foreground change once game-mode arming exists), and eventually ombra-ui.
#pragma once

#include "ombra/amf_probe.h"
#include "ombra/config.h"

#include <windows.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ombra {

struct AdapterFacts {
    LUID                     luid{};
    std::wstring             description;
    UINT                     vendor_id     = 0;
    double                   dedicated_mb  = 0;
    bool                     software      = false;
    std::wstring             driver_version;    // user-mode driver, "32.0.15.6614"; empty if unknown
    std::vector<std::wstring> output_devices;   // e.g. L"\\\\.\\DISPLAY1", this adapter's outputs
    bool                     owns_target_monitor = false;
};

// Enumerates every DXGI adapter (including software/WARP, reported but not useful) and, for each,
// every output it owns. `target_monitor` (typically the monitor Capture would attach to) is
// compared against each output's HMONITOR so callers get "who owns the display" for free — this is
// the thing to verify first before any classification is built
// on top of it. Never fails outward: an adapter or output that can't be queried is skipped and
// logged, not fatal. `driver_versions` also asks each adapter for its user-mode driver version (for
// the startup log); the encode adapter's periodic recheck leaves it off and only reads DXGI's lists.
std::vector<AdapterFacts> EnumerateAdapters(HMONITOR target_monitor, bool driver_versions = false);

// Opens a throwaway D3D11 device + NVENC session on `adapter`, queries
// nvEncGetEncodeGUIDs/nvEncGetEncodeCaps for H.264/HEVC/AV1, and tears the session down before
// returning — callers (esp. LogGpuTopologyStartup, which runs before the engine's real
// Encoder::Open()) must not leave a session open, or the real session can fail on a session-count
// limit. Never dies: a non-NVIDIA adapter, missing driver, or failed session open is reported in
// `fail_reason`, not exit()'d.
struct NvencProbe {
    bool        session_ok = false;
    bool        h264 = false, hevc = false, av1 = false;
    std::string driver_version;   // "13.0" style, from NvEncodeAPIGetMaxSupportedVersion
    std::string fail_reason;
};
NvencProbe ProbeNvenc(IDXGIAdapter1* adapter);

// Media Foundation MFTEnumEx(MFT_ENUM_FLAG_HARDWARE) over the video
// encoder category. This is a generic MF probe, not backend-specific — it reliably finds Intel
// QSV's MFT encoders. It is NOT used as a substitute for AMF: AMD's MF transform registration is
// partial and does not reliably surface AV1, so a low codec count here would be ambiguous between
// "AMD not probed" and "AMD probed and lacking". Self-contained CoInitializeEx/MFStartup, safe to
// call standalone (matches the audio.cpp / main.cpp RunAacTest precedent).
struct MftCaps {
    std::vector<std::wstring> h264_encoders;
    std::vector<std::wstring> hevc_encoders;
};
MftCaps ProbeMftHardwareEncoders();

// Per-process adapter via PDH `GPU Engine` counters. Two-collect (PDH's utilization
// counters are meaningless on a single sample — see PDH_INVALID_DATA behavior), ~1s apart, so this
// is a one-shot diagnostic call, never a per-frame one.
struct ProcessAdapterResult {
    bool    found = false;
    LUID    luid{};
    double  util_percent = 0;
    std::wstring process_image;   // best-effort, from the PDH instance name where available
};
ProcessAdapterResult QueryProcessAdapter(DWORD pid);

// The adapter LUID in a `GPU Engine` instance name
// (`pid_<pid>_luid_0x<hi>_0x<lo>_phys_<n>_eng_<n>_engtype_<type>`); false when it has none.
bool ParseEngineInstanceLuid(const std::wstring& instance, LUID* luid);

// Read-only lookup of HKCU\...\UserGpuPreferences for one exe's full path.
// 0 = let Windows decide, 1 = power saving (iGPU), 2 = high performance (dGPU). Returns false if
// no preference is set for that path — Ombra never writes this key.
bool ReadUserGpuPreference(const std::wstring& exe_path, int& pref_out);

enum class Topology { T1, T2, T3, T4, T5, T6, Unknown };
// More than one adapter drives a display (Topology::T6).
bool MixedOutputOwnership(const std::vector<AdapterFacts>& adapters);

// Where the display and the game sit relative to the *encode* adapter (the GPU that captures and
// encodes, encode_target.h), whatever its vendor. `display_on_encoder`/`game_on_encoder` are
// meaningless when the matching "found"/"present" flag is false; T5 (no usable encoder) wins over
// every other input.
Topology ClassifyTopology(bool encoder_present, bool display_on_encoder, bool game_found,
                           bool game_on_encoder, bool mixed_output_ownership);

const char* TopologyLabel(Topology t);   // "T2", ...
const char* TopologyDescription(Topology t);   // "hybrid, display on integrated GPU", ...

// Full `ombra.exe gputest [--pid <n>]` implementation: runs every probe above, prints a
// report to `out` and mirrors it to Log::Info, and returns a process exit code
// (always 0 — a topology this module can't fully resolve, e.g. no NVIDIA adapter, is data to
// report, not a tool failure).
int RunGpuTest(FILE* out, DWORD pid);

// The subset of a startup topology probe worth handing back to a caller instead of just logging
// it — get_status wants this so ombra-ui's first-run screen and the engine's
// own log agree, rather than ombra-ui recomputing topology itself (a second machine-state probe
// that disagreed with the engine's own gputest run across sessions on
// a test machine — T1 one run, T2 another). `game_found` is always false here (no game pid known
// at startup), so only T1/T2/T5/T6 are reachable; T3/T4 need a real gputest --pid call.
struct StartupGpuSummary {
    Topology     topology = Topology::Unknown;
    std::wstring display_owner_description;  // empty if unknown
    LUID         display_owner_luid{};
    bool         mixed_output_ownership = false;
    // PickEncodeAdapter found something to record with: a hardware encoder, or the CPU on some
    // adapter (false only with no adapter at all).
    bool         encoder_available = false;

    // Every GPU that can record H.264, for the Settings GPU picker. Probed once at startup; the
    // engine reports these, the UI never probes itself.
    struct RecordingGpu {
        const char*  vendor  = "";  // encode.adapter value: "nvidia" | "amd" | "cpu"
        const char*  backend = "";  // "NVENC" | "AMF" | "x264"
        std::wstring description;
        bool         hevc = false, av1 = false;
        uint32_t     av1_align_w = 1, av1_align_h = 1;  // EncodeAdapter's, for the UI's Codec card
        bool         drives_display = false;  // owns the first recorded monitor: auto records here
    };
    std::vector<RecordingGpu> recording_gpus;
};

// Re-classifies against the adapter a pipeline actually encodes on, which can differ from the
// startup guess once encode.adapter changes. No enumeration: cheap enough for get_status.
Topology ClassifyForEncoder(const StartupGpuSummary& summary, const LUID& encoder_luid);

// The summary's recording GPUs as config.h's plain caps, for the preset's recorded codec.
std::vector<RecordingGpuCaps> RecordingGpuCapsOf(const StartupGpuSummary& summary);

// Cut-down version of the same probes for the engine's own startup diagnostic block:
// adapter list, display owner, and NVENC/MFT capability enumeration, logged once via
// Log::Info. No `--pid` target (the engine doesn't know which game it's recording yet at startup).
// Returns the classification so a caller (main.cpp's get_status handler) can cache and report it
// without a second probe.
// `encode_preference` is encode.adapter and `monitor` the first recorded one (null: the primary),
// so the startup topology line describes the GPU that will actually record it.
StartupGpuSummary LogGpuTopologyStartup(const std::string& encode_preference,
                                        const AmfProbeResult& amf_probe, HMONITOR monitor = nullptr);

}  // namespace ombra
