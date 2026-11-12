// AV1 bitstream helpers for the MP4 muxer: the av1C record (AV1-ISOBMFF §2.3) and the per-sample
// OBU filter (§2.4). The parsing is ported from FFmpeg; see av1.cpp's header for source and licence.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ombra {

// AV1CodecConfigurationRecord for `seq_header`, the encoder's sequence header OBU (low-overhead
// format, as nvEncGetSequenceParams / AMF's AV1 extra data return it): the 4-byte record with
// profile/level/tier/bit depth/subsampling parsed out of the header, followed by the header OBU
// itself as configOBUs. Empty when `seq_header` holds no parseable sequence header.
std::vector<uint8_t> BuildAv1C(const uint8_t* seq_header, size_t size);

// The size `seq_header` codes frames at (max_frame_width/height). An encoder may pad past the size it
// was opened at (RDNA3 codes 1080 lines as 1082), and the mux must write what the bitstream says.
bool Av1CodedSize(const uint8_t* seq_header, size_t size, uint32_t& width, uint32_t& height);

// Appends one temporal unit's OBUs to `out` minus the ones AV1-ISOBMFF keeps out of samples
// (temporal delimiters, padding, redundant frame headers, tile lists). Returns false on a malformed
// OBU; `out` then holds whatever came before it.
bool FilterAv1Sample(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

}  // namespace ombra
