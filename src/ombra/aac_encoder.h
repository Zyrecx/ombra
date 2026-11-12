// AAC encoding via Windows Media Foundation's built-in encoder MFT (mfAACEnc.dll, an OS component:
// no codec pack, no bundled library).
//
// Streaming: AudioTrackHub (audio_tracks.h) keeps one encoder per recorded track open while it
// records and feeds it PCM as it's captured, so a save only copies finished frames. Nothing here
// dies: a failure returns false and logs, and the caller drops that track's encoder.
//
// Media Foundation is started once per process (never shut down), so encoders on different
// threads don't race MFStartup/MFShutdown.
//
// The negotiated output is checked to be raw raw_data_block frames (MF_MT_AAC_PAYLOAD_TYPE 0), not
// ADTS: MP4's mdat wants raw access units.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ombra {

class AacEncoder {
public:
    AacEncoder() = default;
    ~AacEncoder() { Close(); }
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    // 48 kHz stereo s16 in, AAC-LC out at the supported bitrate closest to `bitrate_kbps` (the MFT
    // only offers a fixed list). False on any MF failure, already logged.
    bool Open(uint32_t bitrate_kbps);

    // One encoded AAC access unit: a raw (non-ADTS) raw_data_block.
    struct Frame {
        std::vector<uint8_t> data;
    };

    // Feeds `frames` interleaved stereo sample frames and appends every AAC frame the MFT has
    // ready. Output is in input order, one frame per kSamplesPerFrame input frames; the MFT may
    // hold the newest back until more input or Drain().
    bool Encode(const int16_t* interleaved, size_t frames, std::vector<Frame>& out);

    // Flushes the frames the MFT is holding (the last one zero-padded to a full frame). The
    // encoder is finished afterwards; Close() it.
    bool Drain(std::vector<Frame>& out);

    // The negotiated output type's MF_MT_USER_DATA blob — HEAACWAVEINFO-shaped, AudioSpecificConfig
    // in its tail (the MP4 esds box needs it). Valid after Open().
    const std::vector<uint8_t>& UserDataBlob() const { return user_data_blob_; }
    // The AudioSpecificConfig alone.
    std::vector<uint8_t> AudioSpecificConfig() const;

    // What the MFT accepted, which may differ from what was asked for (see Open).
    uint32_t NegotiatedBitrateBps() const { return negotiated_bitrate_bps_; }

    static constexpr uint32_t kSamplesPerFrame = 1024;  // AAC-LC, fixed

    // Measured, not the textbook 2112: encode an onset, decode the raw ADTS with ffmpeg (no
    // initial_padding there), diff the onset positions. 1 sample (2026-08-26, reproduced
    // 2026-09-26 with an impulse: 2). Decoded frame k covers input frames [k*1024, (k+1)*1024), so
    // AudioTrackHub stamps frame k with the capture time of input frame k*1024 and no correction.
    static constexpr int64_t kMeasuredPrimingDelaySamples = 1;

    void Close();

private:
    bool DrainAvailable(std::vector<Frame>& out);

    void*                transform_ = nullptr;  // IMFTransform*, opaque to keep MF headers out
    std::vector<uint8_t> user_data_blob_;
    bool                 provides_output_samples_ = false;
    uint32_t             min_output_buffer_size_  = 0;
    uint32_t             negotiated_bitrate_bps_  = 0;
    int64_t              input_frames_            = 0;  // for IMFSample timestamps
};

}  // namespace ombra
