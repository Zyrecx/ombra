// Frame-exact trims: [start_s, end_s) of a saved clip decoded on the GPU (Media Foundation's
// source reader, DXVA) and encoded again with the engine's encoders, on one adapter. A trim that
// starts on a keyframe doesn't need this: TrimClip copies the stream.
//
// Chosen over smart-cut (re-encoding only the lead-in GOP): NVENC re-encodes a 30 s 1080p30
// clip in ~1.5 s at VMAF ~99 against the source,
// and one path also serves crop and other export formats later.
//
// Runs in `ombra.exe export` (engine.cpp), never inside the resident engine or the overlay.
#pragma once

#include "ombra/clip_file.h"
#include "ombra/encoder.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ombra {

struct ReencodeOptions {
    std::string    adapter = "auto";  // encode.adapter
    // encode.preset and friends. `codec` is ignored: the output keeps the source's codec when the
    // chosen encoder has it (x264 and a GPU without AV1 give H.264).
    EncoderOptions encoder;
    uint32_t       keyframe_sec = 2;  // encode.keyframe_sec, so the output trims again on its own keyframes
    std::string    av1_unaligned = "pad";
    // False makes the decoder return system-memory frames, as one without DXVA for the clip does
    // (tests).
    bool           gpu_decode = true;
};

// Called after every encoded frame. Returning false cancels: the partial output is deleted and
// the result says "cancelled".
using ReencodeProgress = std::function<bool(uint32_t done, uint32_t total)>;

// As TrimClip, but the output starts exactly at the frame at or before `start_s`. The AMF probe
// must have run in this process (SetAmfAdapterCaps) for AMD to be eligible. Never dies.
TrimResult ReencodeClip(const std::wstring& src, const std::wstring& dst, double start_s, double end_s,
                        const std::vector<bool>* keep_audio, const ReencodeOptions& options,
                        const ReencodeProgress& progress = {});

}  // namespace ombra
