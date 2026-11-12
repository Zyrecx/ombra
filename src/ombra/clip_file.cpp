#include "ombra/clip_file.h"

#include "ombra/mux.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <optional>

namespace ombra {
namespace {

// --- file and byte access --------------------------------------------------------------------

class File {
public:
    explicit File(const std::wstring& path)
        : h_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~File() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool Open() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t Size() const {
        LARGE_INTEGER s{};
        return GetFileSizeEx(h_, &s) ? static_cast<uint64_t>(s.QuadPart) : 0;
    }
    bool ReadAt(uint64_t offset, void* dst, size_t size) const {
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (size > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD want = static_cast<DWORD>((std::min)(size, size_t{1} << 30));
            DWORD got = 0;
            if (!ReadFile(h_, out, want, &got, &ov) || got == 0) return false;
            out += got; offset += got; size -= got;
        }
        return true;
    }

private:
    HANDLE h_;
};

// A bounds-checked view of one box's payload. Every read past the end returns 0 and marks the
// view bad, so a truncated or hostile file fails the parse instead of reading out of bounds.
class Bytes {
public:
    Bytes(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool Ok() const { return ok_; }
    size_t Left() const { return pos_ <= n_ ? n_ - pos_ : 0; }
    const uint8_t* Here() const { return p_ + pos_; }
    void Skip(size_t k) { if (k > Left()) { ok_ = false; pos_ = n_; } else pos_ += k; }
    uint8_t U8() { if (Left() < 1) { ok_ = false; return 0; } return p_[pos_++]; }
    uint16_t U16() { const uint16_t a = U8(); return static_cast<uint16_t>((a << 8) | U8()); }
    uint32_t U24() { uint32_t v = 0; for (int i = 0; i < 3; ++i) v = (v << 8) | U8(); return v; }
    uint32_t U32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | U8(); return v; }
    uint64_t U64() { const uint64_t hi = U32(); return (hi << 32) | U32(); }
    Bytes Sub(size_t k) {
        if (k > Left()) { ok_ = false; return Bytes(p_, 0); }
        Bytes b(p_ + pos_, k);
        pos_ += k;
        return b;
    }

private:
    const uint8_t* p_;
    size_t         n_;
    size_t         pos_ = 0;
    bool           ok_ = true;
};

// Calls fn(type, payload) for each child box in `b`. False on a malformed box header.
template <typename Fn>
bool ForEachBox(Bytes b, Fn&& fn) {
    while (b.Left() >= 8) {
        uint64_t size = b.U32();
        char type[5] = {};
        for (int i = 0; i < 4; ++i) type[i] = static_cast<char>(b.U8());
        size_t hdr = 8;
        if (size == 1) { size = b.U64(); hdr = 16; }
        else if (size == 0) size = b.Left() + hdr;
        if (!b.Ok() || size < hdr || size - hdr > b.Left()) return false;
        Bytes payload = b.Sub(static_cast<size_t>(size - hdr));
        if (!fn(std::string(type, 4), payload)) return false;
    }
    return true;
}

// MPEG-4 expandable descriptor length (ISO/IEC 14496-1 §8.3.3).
uint32_t DescLen(Bytes& b) {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t c = b.U8();
        len = (len << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return len;
}

// --- moov model -------------------------------------------------------------------------------

struct Sample {
    uint64_t offset = 0;
    uint32_t size = 0;
    uint64_t dts = 0;  // media timescale
    bool     sync = true;
};

struct Track {
    std::string          handler;        // "vide" / "soun"
    std::string          name;           // hdlr name
    std::string          language = "und";
    uint32_t             timescale = 0;
    int64_t              media_time = 0;  // first non-empty edit's media start
    double               delay_s = 0.0;   // empty edits ahead of it
    std::string          entry;           // sample entry type
    uint32_t             width = 0, height = 0;
    uint32_t             clap_w = 0, clap_h = 0;  // clean aperture, 0 without one
    std::vector<uint8_t> config;          // avcC / av1C payload, or the ASC for audio
    uint32_t             avg_bitrate = 0;
    std::vector<Sample>  samples;
    uint64_t             duration = 0;    // media timescale, sum of stts

    double TimeOf(uint64_t dts) const {
        return timescale ? (static_cast<double>(dts) - static_cast<double>(media_time)) / timescale + delay_s : 0.0;
    }
};

// The image inside the coded size: the clean aperture, else all of it. Not guessed from the size: an
// unpadded 1680x1050 codes 1050 lines, as padding would.
MuxPicture PictureOf(const Track& t) {
    if (t.clap_w && t.clap_h) return {t.clap_w, t.clap_h};
    return {t.width, t.height};
}

struct Movie {
    uint32_t           timescale = 1000;
    std::vector<Track> tracks;
    const Track*       video = nullptr;
    uint64_t           file_size = 0;
};

bool ParseSampleEntry(Track& t, Bytes b) {
    std::optional<Bytes> entry;
    const bool ok = ForEachBox(b, [&](const std::string& type, Bytes payload) {
        if (!entry) { t.entry = type; entry = payload; }
        return true;
    });
    if (!ok || !entry) return false;
    Bytes e = *entry;
    if (t.entry == "avc1" || t.entry == "av01") {
        e.Skip(24);
        t.width = e.U16();
        t.height = e.U16();
        e.Skip(50);
        if (!e.Ok()) return false;
        return ForEachBox(e, [&](const std::string& type, Bytes payload) {
            if (type == "avcC" || type == "av1C") t.config.assign(payload.Here(), payload.Here() + payload.Left());
            if (type == "clap") {
                // Only the size: every writer of these clips puts the aperture at the top left.
                const uint32_t wn = payload.U32(), wd = payload.U32(), hn = payload.U32(), hd = payload.U32();
                if (payload.Ok() && wd && hd && wn / wd <= t.width && hn / hd <= t.height) {
                    t.clap_w = wn / wd;
                    t.clap_h = hn / hd;
                }
            }
            return true;
        });
    }
    if (t.entry == "mp4a") {
        e.Skip(8);
        const uint16_t version = e.U16();
        e.Skip(18);
        if (version == 1) e.Skip(16);
        else if (version == 2) e.Skip(36);
        if (!e.Ok()) return false;
        return ForEachBox(e, [&](const std::string& type, Bytes payload) {
            if (type != "esds") return true;
            payload.Skip(4);  // version/flags
            if (payload.U8() != 3) return false;  // ES_Descriptor
            DescLen(payload);
            payload.Skip(2);
            const uint8_t flags = payload.U8();
            if (flags & 0x80) payload.Skip(2);
            if (flags & 0x40) payload.Skip(payload.U8());
            if (flags & 0x20) payload.Skip(2);
            if (payload.U8() != 4) return false;  // DecoderConfigDescriptor
            DescLen(payload);
            payload.Skip(1 + 1 + 3 + 4);
            t.avg_bitrate = payload.U32();
            if (payload.U8() != 5) return false;  // DecoderSpecificInfo: the ASC
            const uint32_t len = DescLen(payload);
            Bytes asc = payload.Sub(len);
            if (!payload.Ok()) return false;
            t.config.assign(asc.Here(), asc.Here() + asc.Left());
            return true;
        });
    }
    return true;  // some other track type: kept, ignored
}

bool ParseStbl(Track& t, Bytes stbl, uint64_t file_size) {
    std::vector<std::pair<uint32_t, uint32_t>> stts;  // count, delta
    std::vector<uint32_t> sizes, sync;
    uint32_t constant_size = 0, sample_count = 0;
    bool have_stss = false;
    std::vector<uint64_t> chunk_offsets;
    struct Stsc { uint32_t first_chunk, per_chunk; };
    std::vector<Stsc> stsc;

    const bool ok = ForEachBox(stbl, [&](const std::string& type, Bytes b) {
        if (type == "stsd") {
            b.Skip(8);
            return b.Ok() && ParseSampleEntry(t, b);
        }
        if (type == "stts" || type == "stss" || type == "stsz" || type == "stsc" || type == "stco" || type == "co64") {
            b.Skip(4);  // version/flags
            if (type == "stsz") {
                constant_size = b.U32();
                sample_count = b.U32();
                if (constant_size == 0) {
                    if (sample_count > b.Left() / 4) return false;
                    sizes.resize(sample_count);
                    for (auto& s : sizes) s = b.U32();
                }
                // A constant size has no table to bound the count: the samples must fit the file,
                // or a tiny corrupt clip asks for 100 GB of Sample below.
                if (constant_size != 0 && sample_count > file_size / constant_size) return false;
                return b.Ok();
            }
            const uint32_t n = b.U32();
            const size_t per = type == "stts" ? 8 : type == "stsc" ? 12 : type == "co64" ? 8 : 4;
            if (n > b.Left() / per) return false;
            for (uint32_t i = 0; i < n; ++i) {
                if (type == "stts") { const uint32_t c = b.U32(); stts.push_back({c, b.U32()}); }
                else if (type == "stss") sync.push_back(b.U32());
                else if (type == "stsc") { const uint32_t f = b.U32(); const uint32_t p = b.U32(); b.U32(); stsc.push_back({f, p}); }
                else if (type == "stco") chunk_offsets.push_back(b.U32());
                else chunk_offsets.push_back(b.U64());
            }
            if (type == "stss") have_stss = true;
            return b.Ok();
        }
        return true;
    });
    if (!ok) return false;

    t.samples.resize(sample_count);
    for (uint32_t i = 0; i < sample_count; ++i) t.samples[i].size = constant_size ? constant_size : sizes[i];

    uint64_t dts = 0;
    size_t idx = 0;
    for (const auto& [count, delta] : stts) {
        for (uint32_t k = 0; k < count && idx < t.samples.size(); ++k, ++idx) {
            t.samples[idx].dts = dts;
            dts += delta;
        }
    }
    if (idx != t.samples.size()) return false;
    t.duration = dts;

    if (have_stss) {
        for (auto& s : t.samples) s.sync = false;
        for (uint32_t n : sync) {
            if (n == 0 || n > t.samples.size()) return false;
            t.samples[n - 1].sync = true;
        }
    }

    // stsc runs: chunks [first_chunk, next run's first_chunk) each hold per_chunk samples.
    size_t s = 0;
    for (size_t r = 0; r < stsc.size(); ++r) {
        const uint64_t first = stsc[r].first_chunk;
        const uint64_t last = r + 1 < stsc.size() ? stsc[r + 1].first_chunk : chunk_offsets.size() + 1;
        if (first == 0 || last < first || last - 1 > chunk_offsets.size()) return false;
        for (uint64_t c = first; c < last; ++c) {
            uint64_t off = chunk_offsets[c - 1];
            for (uint32_t k = 0; k < stsc[r].per_chunk; ++k) {
                if (s >= t.samples.size()) return false;
                t.samples[s].offset = off;
                off += t.samples[s].size;
                ++s;
            }
        }
    }
    return s == t.samples.size();
}

bool ParseTrak(Movie& m, Bytes trak) {
    Track t;
    bool stbl_ok = true;
    std::function<bool(const std::string&, Bytes)> walk = [&](const std::string& type, Bytes b) -> bool {
        if (type == "mdia" || type == "minf" || type == "edts") return ForEachBox(b, walk);
        if (type == "mdhd") {
            const uint8_t version = b.U8();
            b.Skip(3);
            if (version == 1) { b.Skip(16); t.timescale = b.U32(); b.Skip(8); }
            else { b.Skip(8); t.timescale = b.U32(); b.Skip(4); }
            const uint16_t lang = b.U16();
            if (lang != 0 && lang != 0x7FFF) {
                t.language = {static_cast<char>(((lang >> 10) & 31) + 0x60), static_cast<char>(((lang >> 5) & 31) + 0x60),
                              static_cast<char>((lang & 31) + 0x60)};
            }
            return b.Ok();
        }
        if (type == "hdlr") {
            b.Skip(8);
            for (int i = 0; i < 4; ++i) t.handler.push_back(static_cast<char>(b.U8()));
            b.Skip(12);
            if (!b.Ok()) return false;
            t.name.assign(reinterpret_cast<const char*>(b.Here()), b.Left());
            t.name.erase(std::find(t.name.begin(), t.name.end(), '\0'), t.name.end());
            return true;
        }
        if (type == "elst") {
            const uint8_t version = b.U8();
            b.Skip(3);
            const uint32_t n = b.U32();
            for (uint32_t i = 0; i < n && b.Ok(); ++i) {
                const uint64_t seg = version == 1 ? b.U64() : b.U32();
                const int64_t media = version == 1 ? static_cast<int64_t>(b.U64()) : static_cast<int32_t>(b.U32());
                b.Skip(4);
                if (media == -1) { t.delay_s += static_cast<double>(seg) / m.timescale; continue; }
                t.media_time = media;
                break;
            }
            return b.Ok();
        }
        if (type == "stbl") { stbl_ok = ParseStbl(t, b, m.file_size); return stbl_ok; }
        return true;
    };
    if (!ForEachBox(trak, walk) || !stbl_ok) return false;
    m.tracks.push_back(std::move(t));
    return true;
}

bool ReadMovie(const File& f, Movie& m, std::string& error) {
    const uint64_t file_size = f.Size();
    m.file_size = file_size;
    uint64_t pos = 0;
    std::vector<uint8_t> moov;
    while (pos + 8 <= file_size) {
        uint8_t h[16];
        if (!f.ReadAt(pos, h, 8)) break;
        uint64_t size = (uint64_t{h[0]} << 24) | (uint64_t{h[1]} << 16) | (uint64_t{h[2]} << 8) | h[3];
        uint64_t hdr = 8;
        if (size == 1) {
            if (!f.ReadAt(pos + 8, h + 8, 8)) break;
            size = 0;
            for (int i = 8; i < 16; ++i) size = (size << 8) | h[i];
            hdr = 16;
        } else if (size == 0) {
            size = file_size - pos;
        }
        if (size < hdr || pos + size > file_size) break;
        if (std::memcmp(h + 4, "moov", 4) == 0) {
            if (size - hdr > (uint64_t{256} << 20)) { error = "moov box is implausibly large"; return false; }
            moov.resize(static_cast<size_t>(size - hdr));
            if (!f.ReadAt(pos + hdr, moov.data(), moov.size())) { error = "could not read moov"; return false; }
            break;
        }
        pos += size;
    }
    if (moov.empty()) { error = "no moov box (unfinished or not an MP4)"; return false; }

    // mvhd first: elst durations are in the movie timescale.
    ForEachBox(Bytes(moov.data(), moov.size()), [&](const std::string& type, Bytes b) {
        if (type == "mvhd") {
            const uint8_t version = b.U8();
            b.Skip(3 + (version == 1 ? 16 : 8));
            const uint32_t ts = b.U32();
            if (b.Ok() && ts) m.timescale = ts;
        }
        return true;
    });
    const bool ok = ForEachBox(Bytes(moov.data(), moov.size()), [&](const std::string& type, Bytes b) {
        return type != "trak" || ParseTrak(m, b);
    });
    if (!ok) { error = "malformed moov"; return false; }

    for (const Track& t : m.tracks) {
        if (t.handler == "vide" && (t.entry == "avc1" || t.entry == "av01")) { m.video = &t; break; }
    }
    if (!m.video || m.video->samples.empty() || !m.video->timescale) { error = "no H.264 or AV1 video track"; return false; }
    if (!m.video->samples.front().sync) { error = "video doesn't start on a keyframe"; return false; }
    return true;
}

// One frame's duration in track units: the greatest common divisor of the samples' durations, not
// the first one's, since a save whose encoder fell behind holds a frame for several ticks and that
// can be the first frame.
uint64_t FrameDuration(const Track& v) {
    if (v.samples.size() < 2) return v.duration;
    uint64_t d = 0;
    for (size_t i = 1; i < v.samples.size() && d != 1; ++i) d = std::gcd(d, v.samples[i].dts - v.samples[i - 1].dts);
    return d;
}

// Frames per second as MuxWriteMp4 needs it (timescale == fps, one tick per frame).
uint32_t VideoFps(const Track& v) {
    const uint64_t delta = FrameDuration(v);
    return delta ? static_cast<uint32_t>(std::lround(static_cast<double>(v.timescale) / static_cast<double>(delta))) : 0;
}

// The codec header MuxWriteMp4 takes: Annex-B SPS+PPS from avcC, the sequence header OBU from av1C.
bool CodecHeader(const Track& v, std::vector<uint8_t>& out, uint32_t& nal_len_size) {
    out.clear();
    Bytes b(v.config.data(), v.config.size());
    if (v.entry == "av01") {
        b.Skip(4);
        if (!b.Ok() || b.Left() == 0) return false;
        out.assign(b.Here(), b.Here() + b.Left());
        return true;
    }
    b.Skip(4);
    nal_len_size = (b.U8() & 3) + 1u;
    for (int set = 0; set < 2; ++set) {
        const uint32_t count = set == 0 ? (b.U8() & 31u) : b.U8();
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t len = b.U16();
            Bytes nal = b.Sub(len);
            if (!b.Ok()) return false;
            static const uint8_t kStart[4] = {0, 0, 0, 1};
            out.insert(out.end(), kStart, kStart + 4);
            out.insert(out.end(), nal.Here(), nal.Here() + nal.Left());
        }
    }
    return b.Ok() && !out.empty();
}

// Every kept audio track of `m` cut to [start_s, end_s): its first frame's lead-in becomes
// skip_samples (an edit list), as a save does. False with `error` set on a read failure or a
// track this writer can't cut.
bool CutAudio(const File& f, const Movie& m, double start_s, double end_s, const std::vector<bool>* keep_audio,
              std::vector<MuxAudioTrack>& audio, std::string& error) {
    audio.clear();
    size_t audio_index = 0;
    for (const Track& t : m.tracks) {
        if (t.handler != "soun") continue;
        const size_t index = audio_index++;
        if (keep_audio && index < keep_audio->size() && !(*keep_audio)[index]) continue;
        if (t.entry != "mp4a" || t.samples.empty() || !t.timescale) continue;
        if (t.samples.size() > 1 && t.samples[1].dts - t.samples[0].dts != 1024) {
            error = "audio track \"" + t.name + "\" doesn't use 1024-sample AAC frames";
            return false;
        }
        // Positions in the track's media timeline: the sample the output's first video frame lines
        // up with, and where the last one ends. The first kept frame's lead-in becomes skip_samples.
        const double sr = t.timescale;
        const int64_t pos0 = std::llround((start_s - t.delay_s) * sr) + t.media_time;
        const int64_t pos1 = std::llround((end_s - t.delay_s) * sr) + t.media_time;
        // A track can end early (the hub stops one whose source went idle): nothing to keep if
        // the range starts after its last frame.
        if (pos0 >= static_cast<int64_t>(t.samples.back().dts + 1024)) continue;
        size_t a0 = 0;
        while (a0 + 1 < t.samples.size() && static_cast<int64_t>(t.samples[a0 + 1].dts) <= pos0) ++a0;
        size_t a1 = a0;
        while (a1 < t.samples.size() && static_cast<int64_t>(t.samples[a1].dts) < pos1) ++a1;
        if (a1 == a0) continue;  // the track has nothing in this range

        MuxAudioTrack track;
        track.name = t.name;
        track.language = t.language;
        track.sample_rate = t.timescale;
        track.bitrate_bps = t.avg_bitrate;
        track.asc = t.config;
        track.skip_samples = static_cast<uint32_t>((std::max<int64_t>)(0, pos0 - static_cast<int64_t>(t.samples[a0].dts)));
        track.frames.reserve(a1 - a0);
        for (size_t i = a0; i < a1; ++i) {
            MuxAudioFrame frame;
            frame.data.resize(t.samples[i].size);
            if (!f.ReadAt(t.samples[i].offset, frame.data.data(), frame.data.size())) {
                error = "could not read an audio sample";
                return false;
            }
            track.frames.push_back(std::move(frame));
        }
        audio.push_back(std::move(track));
    }
    return true;
}

}  // namespace

bool ReadClipAudio(const std::wstring& src, double start_s, double end_s, const std::vector<bool>* keep_audio,
                   std::vector<MuxAudioTrack>& out, std::string& error) {
    File f(src);
    Movie m;
    if (!f.Open()) { error = "could not open the file"; return false; }
    if (!ReadMovie(f, m, error)) return false;
    return CutAudio(f, m, start_s, end_s, keep_audio, out, error);
}

bool ReadClipInfo(const std::wstring& path, ClipInfo& out, std::string* error) {
    std::string err;
    File f(path);
    Movie m;
    if (!f.Open()) err = "could not open the file";
    else ReadMovie(f, m, err);
    if (!err.empty()) {
        if (error) *error = err;
        return false;
    }
    const Track& v = *m.video;
    out = {};
    out.codec = v.entry == "av01" ? VideoCodec::AV1 : VideoCodec::H264;
    out.width = v.width;
    out.height = v.height;
    const MuxPicture picture = PictureOf(v);
    out.picture_width = picture.width;
    out.picture_height = picture.height;
    const uint32_t fps = VideoFps(v);
    out.fps = fps;
    out.duration_s = static_cast<double>(v.duration) / v.timescale;
    out.video_offset_s = v.TimeOf(0);
    for (const Sample& s : v.samples) {
        if (s.sync) out.keyframes_s.push_back(v.TimeOf(s.dts));
        out.video_bytes += s.size;
    }
    for (const Track& t : m.tracks) {
        if (t.handler == "soun") out.audio_track_names.push_back(t.name);
    }
    return true;
}

TrimResult TrimClip(const std::wstring& src, const std::wstring& dst, double start_s, double end_s,
                    const std::vector<bool>* keep_audio) {
    TrimResult r;
    File f(src);
    Movie m;
    if (!f.Open()) { r.error = "could not open the file"; return r; }
    if (!ReadMovie(f, m, r.error)) return r;
    const Track& v = *m.video;
    // MuxWriteMp4 writes timescale == fps, one tick per frame: the source must sit on a grid of a
    // whole number of frames per second, which every save does.
    const uint64_t frame_dur = FrameDuration(v);
    if (frame_dur == 0 || v.timescale % frame_dur != 0) {
        r.error = "video frame rate isn't a whole number of frames per second";
        return r;
    }
    const uint32_t fps = static_cast<uint32_t>(v.timescale / frame_dur);
    std::vector<uint8_t> codec_header;
    uint32_t nal_len_size = 4;
    if (!CodecHeader(v, codec_header, nal_len_size)) { r.error = "unreadable codec configuration"; return r; }

    // Half a frame of slack: a requested time is a frame's start, give or take float rounding.
    const double eps = 0.5 / fps;
    size_t v0 = 0;
    for (size_t i = 0; i < v.samples.size(); ++i) {
        if (v.TimeOf(v.samples[i].dts) > start_s + eps) break;
        if (v.samples[i].sync) v0 = i;
    }
    size_t v1 = v.samples.size();
    for (size_t i = v0 + 1; i < v.samples.size(); ++i) {
        if (v.TimeOf(v.samples[i].dts) >= end_s - eps) { v1 = i; break; }
    }
    const uint64_t dts0 = v.samples[v0].dts;
    const uint64_t dts1 = v1 < v.samples.size() ? v.samples[v1].dts : v.duration;
    r.start_s = v.TimeOf(dts0);
    r.end_s = v.TimeOf(dts1);

    // Samples are read from the source as the mux writes them, so a trim holds one frame, not the
    // kept range.
    std::vector<MuxVideoSample> samples;
    samples.reserve(v1 - v0);
    for (size_t i = v0; i < v1; ++i) samples.push_back({v.samples[i].sync, (v.samples[i].dts - dts0) / frame_dur});
    std::vector<uint8_t> buf;
    auto read = [&](size_t k, std::vector<uint8_t>& annexb) {
        const Sample& s = v.samples[v0 + k];
        buf.resize(s.size);
        if (!f.ReadAt(s.offset, buf.data(), buf.size())) { r.error = "could not read a video sample"; return false; }
        if (v.entry == "av01") {
            annexb.swap(buf);
            return true;
        }
        // Length-prefixed NALs back to Annex-B, which is what MuxWriteMp4 takes.
        annexb.clear();
        Bytes b(buf.data(), buf.size());
        while (b.Left() > 0) {
            uint32_t len = 0;
            for (uint32_t n = 0; n < nal_len_size; ++n) len = (len << 8) | b.U8();
            Bytes nal = b.Sub(len);
            if (!b.Ok()) { r.error = "malformed H.264 sample"; return false; }
            static const uint8_t kStart[4] = {0, 0, 0, 1};
            annexb.insert(annexb.end(), kStart, kStart + 4);
            annexb.insert(annexb.end(), nal.Here(), nal.Here() + nal.Left());
        }
        return true;
    };

    std::vector<MuxAudioTrack> audio;
    if (!CutAudio(f, m, r.start_s, r.end_s, keep_audio, audio, r.error)) return r;

    // Written beside the target and renamed into place: a write cut short (the overlay process
    // exiting, a full disk) never leaves an unplayable .mp4 the Clips grid would list.
    const VideoCodec codec = v.entry == "av01" ? VideoCodec::AV1 : VideoCodec::H264;
    const std::wstring part = dst + L".part";
    const uint32_t last_ticks = static_cast<uint32_t>((dts1 - v.samples[v1 - 1].dts) / frame_dur);
    if (!MuxWriteMp4(part, codec_header, v.width, v.height, fps, samples, read, audio, codec, last_ticks, PictureOf(v))) {
        DeleteFileW(part.c_str());
        if (r.error.empty()) r.error = "writing the trimmed clip failed (see the log)";
        return r;
    }
    if (!MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(part.c_str());
        r.error = "could not rename the trimmed clip into place";
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace ombra
