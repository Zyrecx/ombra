// Which video codec a segment was encoded with. Shared by the encoders (what they opened), the ring
// (per segment) and the muxer (which sample entry to write).
#pragma once

#include <string_view>

namespace ombra {

enum class VideoCodec { H264, AV1 };

// encode.codec's config values.
inline const char* VideoCodecName(VideoCodec c) { return c == VideoCodec::AV1 ? "av1" : "h264"; }
inline VideoCodec  ParseVideoCodec(std::string_view s) { return s == "h264" ? VideoCodec::H264 : VideoCodec::AV1; }

}  // namespace ombra
