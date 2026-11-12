#include "ombra/audio_tracks.h"

#include "ombra/aac_encoder.h"
#include "ombra/log.h"
#include "ombra/ring.h"

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ombra {

namespace {

constexpr int64_t kFrameSamples = AacEncoder::kSamplesPerFrame;
constexpr int64_t kStageFrames  = AudioTrackHub::kStageFrames;
constexpr int64_t kIdleFrames   = static_cast<int64_t>(AudioTrackHub::kIdleSeconds * AudioTrackHub::kSampleRate);
// A capture that resumes more than this after its last packet is re-placed by QPC; anything
// closer is packet jitter and is concatenated, so 1-sample QPC rounding never opens a gap.
constexpr int64_t kGapToleranceFrames = AudioTrackHub::kSampleRate * 30 / 1000;
constexpr int64_t kMixChunkFrames     = 8 * kFrameSamples;

int64_t FloorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int64_t FloorToFrame(int64_t f) { return FloorDiv(f, kFrameSamples) * kFrameSamples; }

}  // namespace

struct AudioTrackHub::Source {
    std::string          id;
    bool                 live = true;     // false once removed: buf is freed, Append drops its packets
    std::vector<int16_t> buf = std::vector<int16_t>(static_cast<size_t>(kStageFrames) * 2, 0);
    int64_t              write_pos = -1;  // next timeline frame to write; -1 until the first packet
    int64_t              oldest    = 0;   // oldest timeline frame still held
    int64_t              first     = 0;   // first frame ever written: older than this never existed
    // Lowest encode cursor among the tracks mixing this source (after the last pass): a packet
    // written below it is never encoded. INT64_MIN while no track holds the source.
    int64_t              encoded_through = INT64_MIN;
    int64_t              last_audible_qpc = 0;  // QPC of the last packet with an audible sample
    int                  audible_threshold = AudioTrackSource::kAudibleLoopback;

    int16_t* At(int64_t frame) { return buf.data() + static_cast<size_t>(frame % kStageFrames) * 2; }
};

struct AudioTrackHub::Track {
    std::string                 key;      // "g:<group_id>" or "full": identity across SetSources
    std::string                 name;
    std::vector<size_t>         members;
    std::vector<double>         mults;    // gain, divided by member count for a mix
    std::unique_ptr<AacEncoder> encoder;  // null while paused
    int64_t                     cursor     = 0;  // timeline frame encoded up to
    int64_t                     next_frame = 0;  // grid index of the next frame out of the encoder
    Ring                        ring;
};

AudioTrackHub::AudioTrackHub(int64_t origin_qpc, int64_t qpc_freq, uint32_t bitrate_kbps, Sizing sizing)
    : origin_qpc_(origin_qpc), qpc_freq_(qpc_freq), bitrate_kbps_(bitrate_kbps), sizing_(sizing) {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    ProbeEncoderLocked();
}

// One frame of digital silence (fills spans where a track's encoder was paused) and the stream
// parameters every track shares at this bitrate.
void AudioTrackHub::ProbeEncoderLocked() {
    silent_frame_.clear();
    AacEncoder probe;
    if (probe.Open(bitrate_kbps_)) {
        std::vector<int16_t>           zeros(static_cast<size_t>(kFrameSamples) * 2 * 2, 0);
        std::vector<AacEncoder::Frame> frames;
        if (probe.Encode(zeros.data(), zeros.size() / 2, frames) && probe.Drain(frames) && !frames.empty())
            silent_frame_ = frames.front().data;
        asc_            = probe.AudioSpecificConfig();
        negotiated_bps_ = probe.NegotiatedBitrateBps();
    }
    if (silent_frame_.empty()) Log::Error("audio: AAC encoder unavailable, clips will have no audio");
}

AudioTrackHub::~AudioTrackHub() {
    Stop();
    if (wake_) CloseHandle(wake_);
}

int64_t AudioTrackHub::FrameAt(int64_t qpc) const {
    const int64_t d = qpc - origin_qpc_;
    const int64_t n = d * kSampleRate;  // QPC deltas here are hours at most: no overflow
    return n >= 0 ? (n + qpc_freq_ / 2) / qpc_freq_ : -((-n + qpc_freq_ / 2) / qpc_freq_);
}

int64_t AudioTrackHub::QpcAt(int64_t frame) const {
    return origin_qpc_ + FloorDiv(frame * qpc_freq_, kSampleRate);
}

std::vector<size_t> AudioTrackHub::SetSources(const std::vector<AudioTrackSource>& sources,
                                              std::optional<bool>                  full_mix) {
    // encode_mutex_ first, held throughout: EncodeThrough reads slots through the tracks' members.
    std::lock_guard<std::mutex> lock(encode_mutex_);
    std::vector<size_t>         slots;
    {
        std::lock_guard<std::mutex> lock_sources(sources_mutex_);
        std::vector<bool> kept(sources_.size(), false);
        for (size_t i = 0; i < sources.size(); ++i) {
            const std::string id = sources[i].id.empty() ? "#" + std::to_string(i) : sources[i].id;
            size_t slot = sources_.size();
            for (size_t k = 0; k < sources_.size(); ++k) {
                if (sources_[k]->live && !kept[k] && sources_[k]->id == id) {
                    slot = k;
                    break;
                }
            }
            if (slot == sources_.size()) {
                sources_.push_back(std::make_unique<Source>());
                sources_.back()->id = id;
                kept.push_back(false);
            }
            kept[slot] = true;
            sources_[slot]->audible_threshold = sources[i].audible_threshold;
            slots.push_back(slot);
        }
        for (size_t k = 0; k < sources_.size(); ++k) {
            if (kept[k] || !sources_[k]->live) continue;
            sources_[k]->live = false;
            std::vector<int16_t>().swap(sources_[k]->buf);
        }
    }
    specs_      = sources;
    spec_slots_ = slots;
    if (full_mix) full_mix_ = *full_mix;
    RebuildTracksLocked();
    return slots;
}

void AudioTrackHub::SetFullMix(bool full_mix) {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    if (full_mix == full_mix_) return;
    full_mix_ = full_mix;
    RebuildTracksLocked();
}

void AudioTrackHub::SetBitrate(uint32_t bitrate_kbps) {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    if (bitrate_kbps == bitrate_kbps_) return;
    bitrate_kbps_                       = bitrate_kbps;
    const std::vector<uint8_t> old_asc = asc_;
    ProbeEncoderLocked();
    if (asc_ == old_asc) {
        // Same stream format (AAC-LC from 128 kbps up): buffered audio stays valid, so only the
        // encoders restart, at the new rate, where they left off.
        std::vector<AacEncoder::Frame> frames;
        for (auto& t : tracks_) {
            if (!t->encoder) continue;
            if (t->encoder->Drain(frames)) {
                for (auto& f : frames) {
                    t->ring.Append(f.data.data(), f.data.size(), QpcAt(t->next_frame * kFrameSamples),
                                   static_cast<uint64_t>(t->next_frame), /*is_keyframe=*/true);
                    ++t->next_frame;
                }
            }
            frames.clear();
            t->encoder.reset();
        }
        return;
    }
    // HE-AAC <-> AAC-LC: one track can't hold both, so every track starts over.
    tracks_.clear();
    RebuildTracksLocked();
}

void AudioTrackHub::RebuildTracksLocked() {
    const std::vector<AudioTrackSource>& sources = specs_;

    // Group by group_id in first-seen order; a Full Mix of everything goes last when there is more
    // than one group (with one group it would duplicate that track byte for byte). Members are slots.
    std::vector<std::vector<size_t>> groups;  // indices into `sources`
    for (size_t i = 0; i < sources.size(); ++i) {
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const std::vector<size_t>& g) { return sources[g[0]].group_id == sources[i].group_id; });
        if (it == groups.end()) groups.push_back({i});
        else it->push_back(i);
    }
    struct Spec {
        std::string         key, name;
        std::vector<size_t> members;  // indices into `sources`
    };
    std::vector<Spec> specs;
    for (const auto& g : groups) {
        Spec s{"g:" + sources[g[0]].group_id, "", g};
        for (size_t idx : g) s.name += s.name.empty() ? sources[idx].name : " + " + sources[idx].name;
        specs.push_back(std::move(s));
    }
    if (full_mix_ && groups.size() > 1) {
        Spec s{"full", "Full Mix", {}};
        for (size_t i = 0; i < sources.size(); ++i) s.members.push_back(i);
        specs.push_back(std::move(s));
    }

    std::vector<std::unique_ptr<Track>> next;
    for (const Spec& spec : specs) {
        std::unique_ptr<Track> track;
        for (auto& old : tracks_) {
            if (old && old->key == spec.key) {
                track = std::move(old);  // the same track: keep its audio and encoder
                break;
            }
        }
        std::vector<size_t> members;
        for (size_t i : spec.members) members.push_back(spec_slots_[i]);
        if (!track) {
            track = std::make_unique<Track>();
            track->key = spec.key;
            track->ring.Init(sizing_.ring_capacity, sizing_.ring_overshoot);
            track->ring.SetMaxAge(sizing_.max_age_qpc);
            // Start from what the members still have staged, or from now (the last encode pass)
            // for members that have delivered nothing yet: starting at the timeline's origin would
            // count everything since as overrun once the first packet arrives.
            int64_t newest = -1;
            {
                std::lock_guard<std::mutex> lock(sources_mutex_);
                for (size_t m : members) newest = (std::max)(newest, sources_[m]->write_pos);
            }
            track->cursor = newest >= 0 ? FloorToFrame((std::max)(int64_t{0}, newest - kStageFrames / 2))
                                        : FloorToFrame((std::max)(int64_t{0}, last_target_));
        }
        track->members = std::move(members);
        track->name    = spec.name;
        track->mults.clear();
        for (size_t i : spec.members) {
            const double gain = sources[i].gain_db != 0.0 ? std::pow(10.0, sources[i].gain_db / 20.0) : 1.0;
            track->mults.push_back(spec.members.size() > 1 ? gain / static_cast<double>(spec.members.size()) : gain);
        }
        next.push_back(std::move(track));
    }
    tracks_ = std::move(next);
}

int64_t AudioTrackHub::Append(size_t source, const int16_t* samples, size_t sample_count, int64_t qpc) {
    const int64_t frames = static_cast<int64_t>(sample_count / 2);
    if (frames == 0) return 0;
    int64_t pos = FrameAt(qpc);
    int peak = 0;  // scanned before the lock
    for (size_t k = 0; k < sample_count; ++k) peak = (std::max)(peak, std::abs(static_cast<int>(samples[k])));

    std::lock_guard<std::mutex> lock(sources_mutex_);
    if (source >= sources_.size() || !sources_[source]->live) return 0;
    Source& s = *sources_[source];
    if (peak > s.audible_threshold) s.last_audible_qpc = qpc;

    int64_t skip = 0;
    int64_t late = 0;
    if (s.write_pos < 0) {
        if (pos + frames <= 0) return 0;  // entirely before the timeline's origin
        if (pos < 0) {
            skip = -pos;
            pos  = 0;
        }
        s.write_pos = pos;
        s.oldest    = pos;
        s.first     = pos;
    } else if (pos > s.write_pos + kGapToleranceFrames) {
        // The capture stopped and restarted: silence up to where this packet belongs.
        for (int64_t f = (std::max)(s.write_pos, pos - kStageFrames); f < pos; ++f) {
            int16_t* d = s.At(f);
            d[0] = d[1] = 0;
        }
        s.write_pos = pos;
    }
    if (s.encoded_through > s.write_pos) {
        late = (std::min)(s.write_pos + frames - skip, s.encoded_through) - s.write_pos;
        late_frames_.fetch_add(static_cast<uint64_t>(late), std::memory_order_relaxed);
    }
    for (int64_t i = skip; i < frames; ++i) {
        int16_t* d = s.At(s.write_pos);
        d[0] = samples[i * 2];
        d[1] = samples[i * 2 + 1];
        ++s.write_pos;
    }
    s.oldest = (std::max)(s.oldest, s.write_pos - kStageFrames);
    return late;
}

void AudioTrackHub::EncodeThrough(int64_t target_qpc) { EncodeThrough(target_qpc, false); }

void AudioTrackHub::EncodeThrough(int64_t target_qpc, bool hold_idle_cursors) {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    if (silent_frame_.empty()) return;
    const int64_t target = FrameAt(target_qpc);
    last_target_         = (std::max)(last_target_, target);
    if (suspended_.load(std::memory_order_acquire)) {
        for (auto& t : tracks_) {
            t->encoder.reset();  // on this thread: the MFT is a COM object of its apartment
            t->cursor = (std::max)(t->cursor, FloorToFrame(target));
            t->ring.Release();   // false while a save pins it; retried next pass
        }
    } else {
        for (auto& t : tracks_) EncodeTrackLocked(*t, target, hold_idle_cursors);
    }

    std::lock_guard<std::mutex> lock_sources(sources_mutex_);
    for (auto& s : sources_) s->encoded_through = INT64_MIN;
    for (const auto& t : tracks_) {
        for (size_t m : t->members) {
            int64_t& through = sources_[m]->encoded_through;
            through = through == INT64_MIN ? t->cursor : (std::min)(through, t->cursor);
        }
    }
}

void AudioTrackHub::EncodeTrackLocked(Track& t, int64_t target, bool hold_idle_cursor) {
    // Active members hold the track back until their audio has arrived; idle ones don't.
    int64_t allow  = target;
    bool    active = false;
    {
        std::lock_guard<std::mutex> lock(sources_mutex_);
        for (size_t m : t.members) {
            const int64_t wp = sources_[m]->write_pos;
            if (wp < 0 || wp < target - kIdleFrames) continue;
            active = true;
            allow  = (std::min)(allow, wp);
        }
    }

    auto append = [&](std::vector<AacEncoder::Frame>& frames) {
        for (auto& f : frames) {
            t.ring.Append(f.data.data(), f.data.size(), QpcAt(t.next_frame * kFrameSamples),
                          static_cast<uint64_t>(t.next_frame), /*is_keyframe=*/true);
            ++t.next_frame;
        }
        frames.clear();
    };
    std::vector<AacEncoder::Frame> out;

    if (!active) {
        // Everyone idle: flush what the encoder holds and stop until someone comes back.
        if (t.encoder) {
            if (t.encoder->Drain(out)) append(out);
            t.encoder.reset();
        }
        // A save reaches up to the press, past the lag that gives a resuming source time to deliver.
        // Moving the cursor there would make that source's next packet late, and lose it.
        if (!hold_idle_cursor) t.cursor = (std::max)(t.cursor, FloorToFrame(target));
        return;
    }

    const int64_t end = FloorToFrame(allow);
    if (end <= t.cursor) return;
    if (!t.encoder) {
        // Staging only reaches back kStageFrames; nothing older can be mixed.
        if (const int64_t reach = FloorToFrame(end - kStageFrames + kFrameSamples); reach > t.cursor) {
            overrun_frames_.fetch_add(static_cast<uint64_t>(reach - t.cursor), std::memory_order_relaxed);
            t.cursor = reach;
        }
        if (end <= t.cursor) return;
        auto enc = std::make_unique<AacEncoder>();
        if (!enc->Open(bitrate_kbps_)) {
            failed_frames_.fetch_add(static_cast<uint64_t>(end - t.cursor), std::memory_order_relaxed);
            encoder_errors_.fetch_add(1, std::memory_order_relaxed);
            t.cursor = end;  // logged by Open; try again on the next pass
            return;
        }
        t.encoder    = std::move(enc);
        t.next_frame = t.cursor / kFrameSamples;
    }

    std::vector<int32_t> accum;
    std::vector<int16_t> pcm;
    for (int64_t a = t.cursor; a < end; a += kMixChunkFrames) {
        const int64_t b = (std::min)(end, a + kMixChunkFrames);
        const size_t  n = static_cast<size_t>(b - a);
        accum.assign(n * 2, 0);
        {
            std::lock_guard<std::mutex> lock(sources_mutex_);
            for (size_t i = 0; i < t.members.size(); ++i) {
                Source&       s    = *sources_[t.members[i]];
                const double  mult = t.mults[i];
                const int64_t lo   = (std::max)(a, s.oldest);
                const int64_t hi   = (std::min)(b, s.write_pos);
                if (s.write_pos >= 0) {
                    // Held once (at or after `first`) but overwritten before this mix reached it.
                    if (const int64_t lost = (std::min)(b, s.oldest) - (std::max)(a, s.first); lost > 0)
                        overrun_frames_.fetch_add(static_cast<uint64_t>(lost), std::memory_order_relaxed);
                }
                for (int64_t f = lo; f < hi; ++f) {
                    const int16_t* p = s.At(f);
                    const size_t   o = static_cast<size_t>(f - a) * 2;
                    accum[o]     += static_cast<int32_t>(p[0] * mult);
                    accum[o + 1] += static_cast<int32_t>(p[1] * mult);
                }
            }
        }
        pcm.resize(n * 2);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<int16_t>(std::clamp<int32_t>(accum[i], -32768, 32767));
        if (!t.encoder->Encode(pcm.data(), n, out)) {
            failed_frames_.fetch_add(static_cast<uint64_t>(end - a), std::memory_order_relaxed);
            encoder_errors_.fetch_add(1, std::memory_order_relaxed);
            t.encoder.reset();  // logged; reopened on the next pass
            t.cursor = end;
            return;
        }
        append(out);
    }
    t.cursor = end;
}

std::vector<MuxAudioTrack> AudioTrackHub::Collect(int64_t floor_qpc, int64_t t_end_qpc) {
    EncodeThrough(t_end_qpc, /*hold_idle_cursors=*/true);

    std::lock_guard<std::mutex> lock(encode_mutex_);
    std::vector<MuxAudioTrack>  result;
    const int64_t floor_frame = FrameAt(floor_qpc);
    const int64_t end_frame   = FrameAt(t_end_qpc);
    if (end_frame <= floor_frame || silent_frame_.empty()) return result;
    const int64_t first_slot = FloorDiv(floor_frame, kFrameSamples);
    const int64_t end_slot   = FloorDiv(end_frame + kFrameSamples - 1, kFrameSamples);

    std::vector<uint8_t> bytes;
    for (auto& t : tracks_) {
        const auto descriptors = t->ring.Pin(QpcAt(first_slot * kFrameSamples), QpcAt(end_slot * kFrameSamples));
        if (descriptors.empty()) {
            t->ring.Unpin();
            continue;  // no audio for this track in the window
        }
        MuxAudioTrack m;
        m.name         = t->name;
        m.sample_rate  = kSampleRate;
        m.bitrate_bps  = negotiated_bps_;
        m.asc          = asc_;
        m.skip_samples = static_cast<uint32_t>(floor_frame - first_slot * kFrameSamples);
        int64_t slot = first_slot;
        bool    read_ok = true;
        for (const auto& d : descriptors) {
            // A paused encoder left a hole on the grid: silence keeps every later frame in place.
            for (; slot < static_cast<int64_t>(d.index); ++slot) m.frames.push_back({silent_frame_});
            if (!(read_ok = t->ring.ReadInto(d, bytes))) break;
            m.frames.push_back({bytes});
            ++slot;
        }
        t->ring.Unpin();
        if (read_ok) result.push_back(std::move(m));
    }
    return result;
}

void AudioTrackHub::Resize(Sizing sizing) {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    sizing_ = sizing;
    for (auto& t : tracks_) {
        t->ring.Init(sizing.ring_capacity, sizing.ring_overshoot);
        t->ring.SetMaxAge(sizing.max_age_qpc);
    }
}

void AudioTrackHub::Start() {
    if (running_.exchange(true)) return;
    if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] {
        Log::SetThreadName("audio-encode");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // the AAC MFT is a COM object
        const int64_t lag = static_cast<int64_t>(kLagSeconds * static_cast<double>(qpc_freq_));
        while (running_.load()) {
            WaitForSingleObject(wake_, 20);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            EncodeThrough(now.QuadPart - lag);
        }
        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            for (auto& t : tracks_) t->encoder.reset();  // COM objects die on their own apartment
        }
        CoUninitialize();
    });
}

void AudioTrackHub::Stop() {
    if (!running_.exchange(false)) return;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
}

size_t AudioTrackHub::TrackCount() const {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    return tracks_.size();
}

int64_t AudioTrackHub::LastAudibleQpc(size_t source) const {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    return source < sources_.size() && sources_[source]->live ? sources_[source]->last_audible_qpc : 0;
}

size_t AudioTrackHub::SourceCount() const {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    return static_cast<size_t>(std::count_if(sources_.begin(), sources_.end(), [](const auto& s) { return s->live; }));
}

size_t AudioTrackHub::TrackRingCapacity() const {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    return sizing_.ring_capacity;
}

AudioTrackHub::Losses AudioTrackHub::GetLosses() const {
    Losses l;
    l.overrun_frames = overrun_frames_.load(std::memory_order_relaxed);
    l.late_frames    = late_frames_.load(std::memory_order_relaxed);
    l.failed_frames  = failed_frames_.load(std::memory_order_relaxed);
    l.encoder_errors = encoder_errors_.load(std::memory_order_relaxed);
    return l;
}

size_t AudioTrackHub::LiveBytes() const {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    size_t total = 0;
    for (const auto& t : tracks_) total += t->ring.LiveBytes();
    return total;
}

size_t AudioTrackHub::CommittedBytes() const {
    std::lock_guard<std::mutex> lock(encode_mutex_);
    size_t total = 0;
    for (const auto& t : tracks_) total += t->ring.CommittedBytes();
    return total;
}

}  // namespace ombra
