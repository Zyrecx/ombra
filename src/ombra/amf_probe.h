// Is AMD's AMF usable on this machine? Asked in a child process (`ombra.exe amfprobe`), because AMF
// init can hang or crash on a bad driver and the engine must survive that. Same approach as OBS's
// obs-amf-test.exe (plugins/obs-ffmpeg/obs-amf-test, tag 30.2.3), which the engine's own binary
// stands in for here.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ombra {

struct AmfAdapterCaps {
    LUID luid{};
    bool h264 = false, hevc = false, av1 = false;
    // AV1 codes exactly only at multiples of this; other sizes come out padded (PlanEncode).
    uint32_t av1_align_w = 1, av1_align_h = 1;
};

struct AmfProbeResult {
    bool                        ran = false;      // false: skipped (no AMD adapter or no runtime)
    std::string                 runtime_version;  // "1.4.36"; empty when the child didn't report it
    std::vector<AmfAdapterCaps> adapters;
    std::string                 error;            // why AMF is unusable, when it is
    DWORD                       elapsed_ms = 0;

    bool H264Usable() const {
        for (const auto& a : adapters)
            if (a.h264) return true;
        return false;
    }
};

// Parent side: skips cheaply when there's no AMD adapter or no amfrt64.dll, otherwise runs the
// child with a hard timeout. Never dies. Logs one summary line.
AmfProbeResult ProbeAmfOutOfProcess();

// Child side (`ombra.exe amfprobe`): writes "key=value" lines to stdout and exits. Terminates
// itself after 2.5 s if AMF init hangs. Must run before Log::Init and the instance lock. Log lines
// from shared code (LoadAmfRuntime) still reach the pipe, since an uninitialised Log writes to
// stdout only; the parser skips anything that isn't key=value.
int RunAmfProbeChild();

// Pure: parses the child's output. Exposed for headless tests.
AmfProbeResult ParseAmfProbeOutput(const std::string& text);
// The child's report, trusted only if it ran to a clean exit: otherwise `error` says why and no
// adapter counts.
AmfProbeResult FinishAmfProbe(const std::string& output, bool timed_out, unsigned long exit_code);

}  // namespace ombra
