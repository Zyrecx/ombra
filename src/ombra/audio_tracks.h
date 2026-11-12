// Continuous audio encoding — the OBS/ShadowPlay model. Every recorded track is mixed and
// AAC-encoded as it is captured, and the encoded frames sit in a ring like the video's, so a save
// copies finished frames instead of encoding minutes of PCM (2.5 s for a 120 s, 2-track save).
// It also holds ~8x less memory than raw PCM: 24 KB/s per track at 192 kbps against 192 KB/s per
// source.
//
// Sources and tracks:
//   - A source is one capture (desktop loopback, an app, the mic). Its PCM goes into a short
//     staging buffer indexed by absolute sample position on the hub's timeline: frame
//     round((qpc - origin) * 48000 / freq). Packets are concatenated while they're contiguous and
//     re-placed by QPC across a gap over 30 ms (a capture that stopped and restarted), the same
//     rule the old save-time placement used.
//   - A track is what lands in the file: one source, a group of sources sharing a group_id (summed,
//     divided by member count so simultaneous peaks can't clip), or the optional Full Mix. Gain is
//     applied while mixing, so a gain or group change applies from the moment it's made on.
//   - Frames sit on one grid: AAC frame k covers timeline frames [k*1024, (k+1)*1024) and is
//     stamped with that span's QPC. The MF encoder has no priming delay to correct
//     (AacEncoder::kMeasuredPrimingDelaySamples).
//
// Encoding runs behind real time: EncodeThrough(target) advances each track only as far as its
// active members have delivered, so a late packet is waited for, not overwritten with silence. A
// member that has delivered nothing for kIdleSeconds stops holding the track back; a track whose
// members are all idle drains and closes its encoder until one comes back (a per-app target that
// isn't running costs nothing). A save fills that span with a canned silent frame.
//
// Threads: capture threads call Append; a background thread (Start) calls EncodeThrough every
// 20 ms; saves call Collect. Tests drive EncodeThrough/Collect directly on a synthetic timeline.
#pragma once

#include "ombra/mux.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

struct AudioTrackSource {
    std::string name;      // AudioTrackName: what a single-source track is called
    std::string group_id;  // sources sharing one are mixed into one track
    double      gain_db = 0.0;
    // |sample| above this counts as audible for LastAudibleQpc (status only, never the mix).
    // Loopback is exact zeros when idle, so -60 dBFS is enough; a mic always has a noise floor
    // (this laptop's peaks at -47 dBFS in every 21 ms frame of a saved clip), so input sources
    // use -38 dBFS.
    static constexpr int kAudibleLoopback = 32;
    static constexpr int kAudibleInput    = 410;
    int                  audible_threshold = kAudibleLoopback;
    // Identity across SetSources calls (audio.sources[].id). Empty: the source's position in the list.
    std::string          id;
};

class AudioTrackHub {
public:
    struct Sizing {
        size_t  ring_capacity  = 0;  // per track, encoded bytes
        size_t  ring_overshoot = 0;
        int64_t max_age_qpc    = 0;  // AudioRingMaxAgeQpc(buffer.seconds); 0 = bytes only
    };

    static constexpr uint32_t kSampleRate   = 48000;
    static constexpr double   kIdleSeconds  = 0.5;
    // Background encoding trails real time by this. It must exceed a loopback client's buffer
    // (200 ms, audio.cpp) plus the 20 ms encode tick: a loopback stream resuming after a silence
    // delivers that whole buffer at once, ending at "now" (LoopbackStampRepair), and at 150 ms its
    // first ~50 ms landed behind the cursor and were dropped (audio_late_ms, 2026-10-15..17).
    static constexpr double   kLagSeconds   = 0.3;
    static constexpr double   kStageSeconds = 4.0;   // per-source staging, must exceed idle + lag
    static constexpr uint32_t kMaxBitrateKbps = 320;   // the MF AAC encoder's top rate

    static constexpr int64_t  kStageFrames  = static_cast<int64_t>(kStageSeconds * kSampleRate);

    // Per-track ring sizing for `buffer_seconds` at `bitrate_kbps` (1.3x for container jitter, 2 s
    // of pin headroom), shared with ComputeRingSizing so startup, preview and live apply agree.
    // Inline because config.cpp (linked into ombra-ui too) calls it; the hub itself is engine-only.
    // Sized for the MFT's top rate whatever was asked: it picks the nearest rate it offers, which
    // can be above the request, and a ring sized below its real rate evicts by bytes before age —
    // a save's audio then starts late with no error. 320 kbps for 120 s is 6.2 MB a track.
    static Sizing SizeFor(double buffer_seconds, uint32_t bitrate_kbps, int64_t max_age_qpc) {
        const double bytes_per_sec = (std::max)(bitrate_kbps, kMaxBitrateKbps) * 1000.0 / 8.0;
        Sizing s;
        // + 12 s: AudioRingMaxAgeQpc's reach past the buffer (slack + the longest keyframe interval).
        s.ring_capacity  = static_cast<size_t>((buffer_seconds * 1.3 + 12.0) * bytes_per_sec) + 64 * 1024;
        s.ring_overshoot = static_cast<size_t>(2.0 * bytes_per_sec) + 64 * 1024;
        s.max_age_qpc    = max_age_qpc;
        return s;
    }
    // Staging bytes per source (stereo int16).
    static size_t StageBytes() { return static_cast<size_t>(kStageFrames) * 2 * sizeof(int16_t); }

    // `origin_qpc` anchors the timeline; packets before it are dropped.
    AudioTrackHub(int64_t origin_qpc, int64_t qpc_freq, uint32_t bitrate_kbps, Sizing sizing);
    ~AudioTrackHub();
    AudioTrackHub(const AudioTrackHub&) = delete;
    AudioTrackHub& operator=(const AudioTrackHub&) = delete;

    // The sources to record, and whether to add a Full Mix track (nullopt: as it is). Called again live with any new
    // list: a source is matched to its old one by id and keeps its slot and staged audio; a removed
    // source's slot is freed and never reused, so a capture thread still delivering to it is dropped,
    // not mixed into whatever came next. A track (a group, or Full Mix) that still exists keeps its
    // encoded audio, and its new members and gains apply from now on, the same as a gain change.
    // Returns each source's slot, in `sources` order: Append's and LastAudibleQpc's `source`.
    std::vector<size_t> SetSources(const std::vector<AudioTrackSource>& sources,
                                   std::optional<bool>                  full_mix = std::nullopt);
    // audio.full_mix_track and audio.bitrate_kbps, live. A bitrate change keeps buffered audio
    // within AAC-LC (128 kbps and up); crossing to HE-AAC (below 128) changes the stream format,
    // which one track can't hold, so every track starts over.
    void SetFullMix(bool full_mix);
    void SetBitrate(uint32_t bitrate_kbps);

    // Capture threads, with the slot SetSources returned. `samples` is interleaved stereo s16;
    // `sample_count` counts int16 values.
    // Returns the frames that landed behind the encode cursor (counted in late_frames, never encoded).
    int64_t Append(size_t source, const int16_t* samples, size_t sample_count, int64_t qpc);

    // Encodes every track up to `target_qpc`, as far as each track's active members allow.
    void EncodeThrough(int64_t target_qpc);

    // Background encoding: EncodeThrough(now - kLagSeconds) every 20 ms.
    void Start();
    void Stop();

    // The tracks for a save window starting at `floor_qpc` (the first video frame) and ending at
    // `t_end_qpc`, in track order. A track with no audio in the window is left out. Catches
    // encoding up to t_end first. Each track's skip_samples trims its first frame to floor_qpc.
    std::vector<MuxAudioTrack> Collect(int64_t floor_qpc, int64_t t_end_qpc);

    // Live buffer.seconds change: every track's ring is re-created (encoded audio is dropped, the
    // same as the video ring).
    void Resize(Sizing sizing);

    // Suspended: every track stops encoding (its encoder closes on the next pass) and its ring is
    // released, dropping the audio it held; staging keeps taking packets, so a resumed track picks
    // up at the current time with no gap to fill. main suspends the hub once every pipeline has
    // released its ring (nothing buffered is still saveable) and resumes it when one arms. Cheap:
    // flips a flag; the work happens on the next EncodeThrough.
    void SetSuspended(bool suspended) { suspended_.store(suspended, std::memory_order_release); }
    bool Suspended() const { return suspended_.load(std::memory_order_acquire); }

    size_t TrackCount() const;
    size_t SourceCount() const;  // live sources (removed slots don't count)
    // QPC of the last packet from `source` with a sample above its audible_threshold; 0 = never. Raw fact for
    // ClassifyAudioSource (audio.h); the hub itself doesn't use it.
    int64_t LastAudibleQpc(size_t source) const;
    size_t LiveBytes() const;  // encoded audio held, all tracks
    size_t CommittedBytes() const;  // ring memory committed now, all tracks (grows as audio arrives)
    size_t TrackRingCapacity() const;  // the current Sizing's per-track budget

    // Audio lost without an error reaching the user, cumulative, in 48 kHz frames summed over
    // tracks (a source in a group and in Full Mix counts twice).
    struct Losses {
        uint64_t overrun_frames = 0;  // aged out of staging before its track mixed it
        uint64_t late_frames    = 0;  // arrived behind a track's encode cursor, never encoded
        uint64_t failed_frames  = 0;  // skipped because an AAC encoder failed to open or encode
        uint64_t encoder_errors = 0;  // those failures
    };
    Losses GetLosses() const;

private:
    struct Source;
    struct Track;

    int64_t FrameAt(int64_t qpc) const;       // timeline frame for a QPC, rounded
    int64_t QpcAt(int64_t frame) const;       // QPC for a timeline frame
    void    EncodeThrough(int64_t target_qpc, bool hold_idle_cursors);
    void    EncodeTrackLocked(Track& t, int64_t target_frame, bool hold_idle_cursor);
    void    RebuildTracksLocked();
    void    ProbeEncoderLocked();

    const int64_t origin_qpc_;
    const int64_t qpc_freq_;
    uint32_t      bitrate_kbps_;

    mutable std::mutex                   sources_mutex_;  // staging buffers (capture threads)
    std::vector<std::unique_ptr<Source>> sources_;  // by slot; a removed source stays as a freed husk

    mutable std::mutex                  encode_mutex_;  // tracks, encoders, rings
    std::vector<std::unique_ptr<Track>> tracks_;
    std::vector<AudioTrackSource>       specs_;
    std::vector<size_t>                 spec_slots_;  // specs_[i]'s slot
    int64_t                             last_target_ = 0;  // timeline frame of the last EncodeThrough
    bool                                full_mix_ = false;
    Sizing                              sizing_;
    std::vector<uint8_t>                silent_frame_;   // one AAC frame of digital silence
    std::vector<uint8_t>                asc_;
    uint32_t                            negotiated_bps_ = 0;

    std::atomic<bool>     suspended_{false};
    std::atomic<uint64_t> overrun_frames_{0};
    std::atomic<uint64_t> late_frames_{0};
    std::atomic<uint64_t> failed_frames_{0};
    std::atomic<uint64_t> encoder_errors_{0};

    std::thread       thread_;
    std::atomic<bool> running_{false};
    HANDLE            wake_ = nullptr;
};

}  // namespace ombra
