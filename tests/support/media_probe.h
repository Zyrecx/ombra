// What ffprobe/ffmpeg see in a saved file. The checker has to be independent of the muxer under
// test, so nothing here parses MP4 itself.
#pragma once

#include "support/audio_analysis.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ombra::test {

// %OMBRA_FFMPEG_DIR%\ffprobe.exe, else ffprobe.exe on PATH. A missing tool fails the calling test:
// a check that silently didn't run must not read as a pass.
const std::wstring& FfprobePath();
const std::wstring& FfmpegPath();

struct StreamInfo {
    int         index = -1;
    std::string codec_type;  // "video" / "audio"
    std::string codec_name;
    std::string title;       // MP4 hdlr name (ffprobe's handler_name)
    uint32_t    width = 0, height = 0;
    uint32_t    sample_rate = 0, channels = 0;
    // As ffprobe names them from the H.264 VUI: "tv"/"pc", "bt709", ... Empty when unsignalled.
    std::string color_range, color_space, color_transfer, color_primaries;
};

struct ProbeInfo {
    bool                    ok = false;
    std::string             error;
    std::string             format_name;
    double                  duration_s = 0.0;
    std::vector<StreamInfo> streams;

    std::vector<StreamInfo> Of(const std::string& codec_type) const;
};

ProbeInfo Probe(const std::wstring& file);

// Everything ffmpeg reports at -v error while decoding every stream to null. Empty means clean.
std::string DecodeErrors(const std::wstring& file);

struct VideoFrames {
    size_t count        = 0;
    bool   first_is_key = false;
    double first_pts_s  = 0.0;
    double last_pts_s   = 0.0;
};
VideoFrames ProbeVideoFrames(const std::wstring& file);

// Decodes the `ordinal`-th audio stream (0-based, audio streams only) to s16 stereo 48 kHz.
Pcm DecodeAudio(const std::wstring& file, int ordinal);

// Every frame of the first video stream as tightly packed BGRA, converted with an explicit BT.709
// limited-range matrix rather than whatever the stream's tags say: a stream tagged BT.709 but
// encoded with another matrix must still decode to the wrong colours here.
struct VideoBgra {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> bytes;

    size_t FrameCount() const { return (width && height) ? bytes.size() / (size_t(width) * height * 4) : 0; }
    const uint8_t* Frame(size_t index) const { return bytes.data() + index * size_t(width) * height * 4; }
};
// `max_frames` > 0 decodes only the first that many (-frames:v), for large frames.
VideoBgra DecodeVideoBgra(const std::wstring& file, int max_frames = 0);

}  // namespace ombra::test
