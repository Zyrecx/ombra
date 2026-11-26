// MP4 muxer: H.264 or AV1 video plus AAC audio tracks, written plain in one pass (clips) or
// fragmented as footage arrives and made plain at the end (recordings). See the provenance note at
// the top of mux.cpp for what this adapts from OBS's mp4-mux.c (GPL-2.0-or-later).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

// A named moment in the file, `ms` from its first frame (UTF-8 title).
struct MuxChapter {
    uint64_t    ms = 0;
    std::string title;
};

// What marks the footage: chapters (a QuickTime chapter track plus Nero `chpl`, as ffmpeg writes
// both) and `json`, stored as the freeform metadata tag "markers". A chapter at or past the end of
// the video is dropped; a first chapter later than 0 gets kStartChapter before it.
struct MuxMarkers {
    std::vector<MuxChapter> chapters;  // any order
    std::string             json;      // empty: no tag
};
inline constexpr const char* kStartChapter = "Start";

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
                  uint32_t last_sample_ticks = 1, MuxPicture picture = {}, const MuxMarkers& markers = {});

// What a streaming video source hands MuxWriteMp4Stream next.
enum class MuxPull {
    kSample,  // `sample` and `bytes` hold the next video sample
    kEnd,     // no more samples: write the rest of the file
    kFail,    // abort: the partial file is deleted and the mux returns false
};

// Fills the next video sample's metadata and bytes (Annex-B for H.264, a temporal unit for AV1).
// Samples come in rising tick_index order, the first a keyframe. May block while the source waits
// for footage that hasn't been encoded yet (an open clip).
using MuxPullSample = std::function<MuxPull(MuxVideoSample& sample, std::vector<uint8_t>& bytes)>;

// What a source hands over after its last video sample. `audio` must stay alive until the mux
// returns; null is no audio.
struct MuxTail {
    const std::vector<MuxAudioTrack>* audio = nullptr;
    MuxMarkers                        markers;
};

// Called once, after the last video sample. A source still collecting audio or markers hands over
// all of it here; false aborts like kFail. Audio is held whole until then, which is fine for a clip
// (minutes of AAC is a few MB); an hour-long recording would need interleaved chunks instead.
using MuxTakeTail = std::function<bool(MuxTail& tail)>;

// MuxWriteMp4 over a source that doesn't know its samples up front. Holds only the sample tables
// (4 bytes a frame, plus one per keyframe and per duration change); an empty stream, or one not
// starting on a keyframe, fails. Null `take_tail`: video only.
bool MuxWriteMp4Stream(const std::wstring& path, const std::vector<uint8_t>& codec_header,
                        uint32_t width, uint32_t height, uint32_t fps, const MuxPullSample& next,
                        const MuxTakeTail& take_tail, VideoCodec codec,
                        uint32_t last_sample_ticks = 1, MuxPicture picture = {});

// The same over AUs already in memory (tests).
bool MuxWriteMp4(const std::wstring& path, const std::vector<uint8_t>& codec_header,
                  uint32_t width, uint32_t height, uint32_t fps, const std::vector<MuxAu>& aus,
                  const std::vector<MuxAudioTrack>& audio_tracks = {},
                  VideoCodec codec = VideoCodec::H264, MuxPicture picture = {}, const MuxMarkers& markers = {});

// A recording's MP4, written as footage arrives (OBS's "hybrid MP4"): fragmented while open, a moof
// and its mdat about every second, each on the disk (FlushFileBuffers) before the next starts, so
// a crash of the engine or the system leaves a file players open up to the last whole fragment. Finish makes it a plain MP4 in place, without a copy: the full moov goes at the end
// and a 16-byte placeholder after ftyp becomes one mdat header over everything before it.
//
// Holds the sample tables (about 4 bytes a video frame and 4 an AAC frame, plus one entry per
// fragment per track) and one fragment's samples (about a second of video). Not thread-safe.
class Mp4FragmentedWriter {
public:
    Mp4FragmentedWriter();
    ~Mp4FragmentedWriter();  // without Finish: the file stays as written, fragmented
    Mp4FragmentedWriter(const Mp4FragmentedWriter&) = delete;
    Mp4FragmentedWriter& operator=(const Mp4FragmentedWriter&) = delete;

    // Creates the file and writes its header. `audio` gives each track's format (its `frames` are
    // ignored; frames come through AddAudio), skip_samples included: the track set is fixed here.
    // False when the file can't be created or a codec header doesn't parse.
    bool Open(const std::wstring& path, const std::vector<uint8_t>& codec_header, uint32_t width,
              uint32_t height, uint32_t fps, VideoCodec codec, MuxPicture picture,
              const std::vector<MuxAudioTrack>& audio);

    // The next video sample: rising tick_index, the first a keyframe. Writes the fragment before
    // it once that holds a second, or when this is a keyframe. False, and every later Add false,
    // on a sample that can't follow the last or a failed write; Finish keeps what was written.
    bool AddVideo(const MuxVideoSample& sample, const std::vector<uint8_t>& bytes);
    // The next AAC frame of audio track `track` (Open's order), written with the next fragment,
    // or on its own once two seconds of it wait (video stalled).
    bool AddAudio(size_t track, const MuxAudioFrame& frame);

    // Writes the last fragment, then the full moov with `markers`, and makes the file plain.
    // `last_sample_ticks` as MuxWriteMp4. False leaves the file fragmented (still playable up to
    // its last whole fragment), or deletes it if it holds no video.
    bool Finish(uint32_t last_sample_ticks, const MuxMarkers& markers);
    // Deletes the file, unless Finish already made it plain.
    void Discard();

    uint64_t FileBytes() const;   // written so far, buffered bytes included
    uint64_t VideoTicks() const;  // video written so far, in ticks (1/fps), the pending sample excluded

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ISO/IEC 14496-15 AVCDecoderConfigurationRecord built from Encoder::SequenceParams()'s Annex-B
// SPS/PPS. Exported for mux_tests.cpp, which checks it against a real x264 fixture.
std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps_pps_annexb);

// Converts one AU's Annex-B NALs to length-prefixed form, dropping in-band SPS/PPS (avcC already
// carries them).
void ConvertAnnexBSample(const std::vector<uint8_t>& annexb, std::vector<uint8_t>& out);

}  // namespace ombra
