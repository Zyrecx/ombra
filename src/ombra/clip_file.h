// Reading and trimming saved clips: the MP4s MuxWriteMp4 writes (and ffmpeg stream copies of
// them). Not a general MP4 demuxer: one video track (avc1/av01), AAC audio tracks, no B-frames,
// no fragments. Used by ombra-ui's Clips screen, off the UI thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ombra/mux.h"
#include "ombra/video_codec.h"

namespace ombra {

struct ClipInfo {
    VideoCodec          codec = VideoCodec::H264;
    uint32_t            width = 0, height = 0;   // the sample entry's coded size
    // The image inside it (MuxPicture): smaller than the coded size where the encoder padded.
    uint32_t            picture_width = 0, picture_height = 0;
    double              fps = 0.0;
    double              duration_s = 0.0;        // the video track's
    uint64_t            video_bytes = 0;         // every video sample's size, summed
    std::vector<double> keyframes_s;             // presentation time of every sync sample
    std::vector<std::string> audio_track_names;  // hdlr names ("Desktop", "Mic")
    // Presentation time of video media time 0 (the video edit list; 0 in every save). Media
    // Foundation ignores edit lists, so a decoded frame's presentation time is its sample time plus
    // this.
    double              video_offset_s = 0.0;
};

// Parses the moov box only (a few small reads). False with `error` set for anything this reader
// doesn't handle.
bool ReadClipInfo(const std::wstring& path, ClipInfo& out, std::string* error = nullptr);

struct TrimResult {
    bool        ok = false;
    double      start_s = 0.0;  // where the output really starts: a keyframe at or before the request
    double      end_s = 0.0;
    std::string error;
};

// Writes [start_s, end_s) of `src` to `dst` without re-encoding: video from the last keyframe at
// or before start_s up to the first frame at or after end_s, every audio track cut to match (its
// first frame's lead-in trimmed by an edit list, as a save does). Holds the kept samples in memory.
// `keep_audio`, if given, says per audio track (file order, as ClipInfo::audio_track_names) whether
// it goes into the output; tracks past its end are kept.
// The audio TrimClip would write for [start_s, end_s), for a writer that makes its own video
// (ReencodeClip). False with `error` set.
bool ReadClipAudio(const std::wstring& src, double start_s, double end_s, const std::vector<bool>* keep_audio,
                   std::vector<MuxAudioTrack>& out, std::string& error);

TrimResult TrimClip(const std::wstring& src, const std::wstring& dst, double start_s, double end_s,
                    const std::vector<bool>* keep_audio = nullptr);

}  // namespace ombra
