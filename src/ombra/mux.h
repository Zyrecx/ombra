// MP4 muxer: H.264 or AV1
// video plus an optional AAC audio track, both non-fragmented — see the provenance note at the top
// of mux.cpp for what this adapts from OBS's mp4-mux.c (GPL-2.0-or-later) and why it doesn't port OBS's
// fragmented/soft-remux machinery.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ombra/video_codec.h"

namespace ombra {

// One access unit ready for muxing: Annex-B bytes (start-code-prefixed NALs), as read from
// Ring::ReadInto, plus the clock the muxer's sample-duration table (stts) needs. Durations are
// built from tick_index deltas, never qpc — conflating the two produces
// a save window with the right frame count but the wrong duration; qpc has no business here.
struct MuxAu {
    std::vector<uint8_t> annexb;
    bool                  is_keyframe = false;
    uint64_t              tick_index  = 0;
};

// A video sample's metadata, known before its bytes are read: what stts/stss need.
struct MuxVideoSample {
    bool     is_keyframe = false;
    uint64_t tick_index  = 0;
};

// Fills `annexb` with video sample `i`'s bytes (Annex-B for H.264, a temporal unit for AV1),
// called once per sample in order. False aborts the mux: the partial file is deleted and
// MuxWriteMp4 returns false.
using MuxReadSample = std::function<bool(size_t i, std::vector<uint8_t>& annexb)>;

// The picture inside the coded size, when the encoder padded it (AMF's AV1 codes 1080 lines as
// 1082). The mux writes it as a clean aperture (clap, top-left) and as the track's display size.
// 0 means the coded size: no clap.
struct MuxPicture {
    uint32_t width = 0, height = 0;
};

// One AAC access unit ready for muxing: a raw (non-ADTS) raw_data_block, as AacEncoder emits it
// produces (AacEncoder::Open() verifies MF_MT_AAC_PAYLOAD_TYPE==0, so this is guaranteed, not
// re-checked here). Every AAC sample is implicitly a sync sample — no is_keyframe/stss analog
// needed, unlike MuxAu. Duration is derived from AacEncoder::kSamplesPerFrame (1024) uniformly per
// frame, not per-frame metadata — MF's AAC encoder always emits fixed-size frames.
struct MuxAudioFrame {
    std::vector<uint8_t> data;
};

// One output audio track. `name`/`language` are not cosmetic — they are
// what makes an editor show "Game"/"Mic" instead of "Audio 1"/"Audio 2" (hdlr name + elng).
struct MuxAudioTrack {
    std::string                name = "Audio";
    std::string                language = "und";  // ISO 639-2, "und" = undetermined
    uint32_t                    sample_rate = 48000;
    uint32_t                    bitrate_bps = 0;
    std::vector<uint8_t>        asc;    // AudioSpecificConfig, HEAACWAVEINFO prefix already stripped
    std::vector<MuxAudioFrame>  frames;
    // Samples of the first frame that come before the video's first frame (AudioTrackHub's frames
    // sit on a fixed 1024-sample grid). MP4 trims them with an edit list.
    uint32_t                    skip_samples = 0;
};

// Writes a standard (non-fragmented) ISO/IEC 14496-12 MP4 file — ftyp + mdat + moov (full stts/
// stss/stsc/stsz/stco sample tables, a video sample entry built from `codec_header` per `codec`,
// plus one audio trak/esds per entry in `audio_tracks`) — to `path`. `samples` must be in
// ascending tick_index order and start on a keyframe (Ring::Pin already guarantees this). Video
// bytes are pulled one sample at a time through `read` and written straight out, so the clip is
// never held in memory; audio (a few MB for minutes of AAC) is passed whole.
//
// `audio_tracks` empty means video-only output (the default when a save
// has no audio ring or the audio window came up empty).
//
// `codec_header` is the encoder's SequenceParams(): Annex-B SPS+PPS for H.264 (avcC), the sequence
// header OBU for AV1 (av1C). AV1 AUs are low-overhead OBU temporal units.
//
// Returns false (does not die()) on a file I/O failure or a codec header that can't be parsed, so
// the caller can report it as a save failure rather than crashing the engine.
//
// `width`/`height` are the coded size; `picture` the part of it that is image (MuxPicture).
//
// `last_sample_ticks`: the final sample has no next one to measure against. A trim passes how long
// it really held, or the clip ends early when a skipped tick follows the cut.
bool MuxWriteMp4(const std::wstring& path, const std::vector<uint8_t>& codec_header,
                  uint32_t width, uint32_t height, uint32_t fps,
                  const std::vector<MuxVideoSample>& samples, const MuxReadSample& read,
                  const std::vector<MuxAudioTrack>& audio_tracks, VideoCodec codec,
                  uint32_t last_sample_ticks = 1, MuxPicture picture = {});

// The same over AUs already in memory (tests).
bool MuxWriteMp4(const std::wstring& path, const std::vector<uint8_t>& codec_header,
                  uint32_t width, uint32_t height, uint32_t fps, const std::vector<MuxAu>& aus,
                  const std::vector<MuxAudioTrack>& audio_tracks = {},
                  VideoCodec codec = VideoCodec::H264, MuxPicture picture = {});

// ISO/IEC 14496-15 AVCDecoderConfigurationRecord built from Encoder::SequenceParams()'s Annex-B
// SPS/PPS. Exported for mux_tests.cpp, which checks it against a real x264 fixture.
std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps_pps_annexb);

// Converts one AU's Annex-B NALs to length-prefixed form, dropping in-band SPS/PPS (avcC already
// carries them).
void ConvertAnnexBSample(const std::vector<uint8_t>& annexb, std::vector<uint8_t>& out);

}  // namespace ombra
