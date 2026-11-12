// Adapted from OBS Studio's plugins/obs-outputs/mp4-mux.c (tag 30.2.3), Copyright (C) 2024 Dennis
// Sädtler <dennis@obsproject.com>, GPL-2.0-or-later. The box-writing algorithm (ftyp/moov/mvhd/trak/mdia/
// minf/stbl/stsd/avc1/avcC/stts/stsc/stsz/stco/stss) is ported near-verbatim; the byte-writer
// helpers (write_box/write_box_size/write_fullbox and the s_wbNN big-endian primitives) mirror
// OBS's serializer.h/array-serializer.c, whose original author is Lain Bailey <lain@obsproject.com>
// under a permissive ISC-style notice.
//
// What's deliberately NOT ported: OBS's fragmented moof/traf/trun writer and its "soft remux"
// finalize step. That machinery exists because OBS writes an MP4 continuously while a recording is
// live, so a mid-recording crash must leave a recoverable file. Ombra's save path is different:
// the whole clip already exists, encoded, in the Ring (a save pins it and the mux reads it out one
// sample at a time), so there is no partial-write-during-capture window for the hybrid trick to
// protect. What OBS's soft-remux produces at finalization time — a plain, non-fragmented moov with
// full sample tables — is exactly what this file writes directly in one pass. Also not ported:
// chapter tracks and the mdta/QuickTime metadata boxes. Audio tracks and the audio edit list are
// Ombra's own.
#include "ombra/mux.h"
#include "ombra/brand.h"

#include "ombra/av1.h"
#include "ombra/die.h"
#include "ombra/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ombra {

namespace {

// Mirrors OBS's array_output_write (array-serializer.c): writing at a position inside the
// existing buffer overwrites in place (extending if the write runs past the end); writing at or
// past the end appends. That's what lets write_box_size() go back and patch a box's size field
// after its contents are known, without needing a real seekable file handle.
class ByteWriter {
public:
    size_t Pos() const { return pos_; }
    void   Seek(size_t p) { pos_ = p; }

    void WriteBytes(const void* data, size_t n) {
        if (pos_ < buf_.size()) {
            const size_t new_size = pos_ + n;
            if (new_size > buf_.size()) buf_.resize(new_size);
            std::memcpy(buf_.data() + pos_, data, n);
        } else {
            buf_.insert(buf_.end(), static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + n);
        }
        pos_ += n;
    }

    void W8(uint8_t v) { WriteBytes(&v, 1); }
    void WB16(uint16_t v) { uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)}; WriteBytes(b, 2); }
    void WB24(uint32_t v) { uint8_t b[3] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; WriteBytes(b, 3); }
    void WB32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        WriteBytes(b, 4);
    }
    void WB64(uint64_t v) { WB32(uint32_t(v >> 32)); WB32(uint32_t(v)); }

    const std::vector<uint8_t>& Data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
    size_t                pos_ = 0;
};

// 4.2 Box header with size and char[4] name. Assumes size <= UINT32_MAX (largesize's 64-bit form
// isn't needed at clip lengths).
void WriteBox(ByteWriter& w, uint32_t size, const char name[4]) {
    w.WB32(size);
    w.WriteBytes(name, 4);
}

// Patches the box's 4-byte size field at `start` with (current pos - start); returns that size.
size_t WriteBoxSize(ByteWriter& w, size_t start) {
    const size_t end  = w.Pos();
    const size_t size = end - start;
    w.Seek(start);
    w.WB32(static_cast<uint32_t>(size));
    w.Seek(end);
    return size;
}

// 4.2 FullBox extended header: u8 version, u24 flags.
void WriteFullBox(ByteWriter& w, uint32_t size, const char name[4], uint8_t version, uint32_t flags) {
    WriteBox(w, size, name);
    w.W8(version);
    w.WB24(flags);
}

// ISO/IEC 14496-12 §8.2.2.1 unity transformation matrix.
constexpr int32_t kUnityMatrix[9] = {
    0x00010000, 0, 0,
    0, 0x00010000, 0,
    0, 0, 0x40000000,
};

// One Annex-B NAL's span within the source buffer (start code excluded, NAL header byte included).
struct NalSpan {
    size_t offset;
    size_t length;
};

// Scans Annex-B (3- or 4-byte start codes) for NAL boundaries. Safe against both lengths mixed in
// the same buffer — Encoder::SequenceParams() and NVENC's per-frame output both use 4-byte codes,
// but this doesn't assume that.
// First start code at or after `from`: returns where it begins and sets `len` (3, or 4 for
// 00 00 00 01), or returns `size` if there is none. memchr finds the 0x01 byte: a byte-at-a-time
// scan was most of the mux's CPU time on a 200 MB save.
size_t FindStartCode(const uint8_t* d, size_t size, size_t from, size_t& len) {
    size_t q = from + 2;
    while (q < size) {
        const void* hit = std::memchr(d + q, 1, size - q);
        if (!hit) break;
        q = static_cast<size_t>(static_cast<const uint8_t*>(hit) - d);
        if (d[q - 1] == 0 && d[q - 2] == 0) {
            if (q >= from + 3 && d[q - 3] == 0) {
                len = 4;
                return q - 3;
            }
            len = 3;
            return q - 2;
        }
        ++q;
    }
    len = 0;
    return size;
}

std::vector<NalSpan> SplitAnnexB(const uint8_t* data, size_t size) {
    std::vector<NalSpan> nals;
    size_t len = 0;
    size_t sc  = FindStartCode(data, size, 0, len);
    if (sc == size) return nals;
    size_t nal_start = sc + len;
    for (;;) {
        sc = FindStartCode(data, size, nal_start, len);
        if (sc == size) break;
        nals.push_back({nal_start, sc - nal_start});
        nal_start = sc + len;
    }
    if (nal_start < size) nals.push_back({nal_start, size - nal_start});
    return nals;
}

uint8_t NalType(const uint8_t* data, const NalSpan& n) { return data[n.offset] & 0x1F; }

// ISO/IEC 14496-12 §8.4.2: mdhd's language field packs a 3-letter ISO-639-2 code as 3x5 bits
// (bit 15 reserved as 0), each letter offset from 0x60. "und" -> 21956, which is the literal this
// file used to hardcode before per-track language became a thing. Falls back to "und" for
// anything that isn't exactly 3 lowercase ASCII letters.
uint16_t PackIso639Lang(const std::string& code) {
    auto valid = code.size() == 3 && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' &&
                 code[1] <= 'z' && code[2] >= 'a' && code[2] <= 'z';
    const std::string& c = valid ? code : std::string("und");
    return static_cast<uint16_t>(((c[0] - 0x60) << 10) | ((c[1] - 0x60) << 5) | (c[2] - 0x60));
}

}  // namespace

// ISO/IEC 14496-15 §5.2.4.1 AVCDecoderConfigurationRecord: one SPS + one PPS, 4-byte NAL lengths
// (matches the length prefixes ConvertAnnexBSample below writes into every sample). Exported in
// mux.h for mux_tests.cpp.
std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps_pps_annexb) {
    auto nals = SplitAnnexB(sps_pps_annexb.data(), sps_pps_annexb.size());
    const NalSpan* sps = nullptr;
    const NalSpan* pps = nullptr;
    for (const auto& n : nals) {
        const uint8_t t = NalType(sps_pps_annexb.data(), n);
        if (t == 7 && !sps) sps = &n;
        else if (t == 8 && !pps) pps = &n;
    }
    // An empty result fails the save (MuxWriteMp4), never the process: a trim reads this from a
    // file on disk, and a die() here would also take down a resident engine mid-save.
    if (!sps || !pps) {
        Log::Error("mux: codec header did not contain both an SPS and a PPS NAL");
        return {};
    }
    // profile_idc/constraint_flags/level_idc live at sps_data[1..3]; a malformed or truncated SPS
    // NAL (e.g. consecutive Annex-B start codes producing a zero-length span) would read past its
    // own declared length here.
    if (sps->length < 4) {
        Log::Error("mux: SPS NAL is too short to contain a profile/level");
        return {};
    }

    const uint8_t* sps_data = sps_pps_annexb.data() + sps->offset;

    std::vector<uint8_t> out;
    out.push_back(1);                // configurationVersion
    out.push_back(sps_data[1]);      // AVCProfileIndication
    out.push_back(sps_data[2]);      // profile_compatibility
    out.push_back(sps_data[3]);      // AVCLevelIndication
    out.push_back(0xFF);             // reserved(6)=111111, lengthSizeMinusOne(2)=3 (4-byte lengths)
    out.push_back(0xE1);             // reserved(3)=111, numOfSequenceParameterSets(5)=1
    out.push_back(uint8_t(sps->length >> 8));
    out.push_back(uint8_t(sps->length));
    out.insert(out.end(), sps_pps_annexb.begin() + sps->offset, sps_pps_annexb.begin() + sps->offset + sps->length);
    out.push_back(1);                // numOfPictureParameterSets
    out.push_back(uint8_t(pps->length >> 8));
    out.push_back(uint8_t(pps->length));
    out.insert(out.end(), sps_pps_annexb.begin() + pps->offset, sps_pps_annexb.begin() + pps->offset + pps->length);
    return out;
}

// Converts one AU's Annex-B NALs into length-prefixed form for the mdat, at mux time
// (ring.h's contract stays a raw-Annex-B byte store). Drops SPS/PPS NALs:
// NVENC's repeatSPSPPS=1 puts them in-band on every IDR (encoder.h), but avcC above already
// carries them once; duplicating them into every keyframe sample is non-standard and some strict
// parsers reject it. Everything else (slice data, SEI, AUD if present) passes through unchanged.
void ConvertAnnexBSample(const std::vector<uint8_t>& annexb, std::vector<uint8_t>& out) {
    auto nals = SplitAnnexB(annexb.data(), annexb.size());
    for (const auto& n : nals) {
        const uint8_t t = NalType(annexb.data(), n);
        if (t == 7 || t == 8) continue;  // SPS / PPS
        uint8_t len_be[4] = {
            uint8_t(n.length >> 24), uint8_t(n.length >> 16), uint8_t(n.length >> 8), uint8_t(n.length),
        };
        out.insert(out.end(), len_be, len_be + 4);
        out.insert(out.end(), annexb.begin() + n.offset, annexb.begin() + n.offset + n.length);
    }
}

namespace {

// --- moov box tree, video-only, single track, non-fragmented -----------------------------------

struct TrackTiming {
    uint32_t              timescale = 0;  // == fps: one CFR tick is exactly one timescale unit
    uint64_t              duration  = 0;  // total ticks
    std::vector<uint32_t> sample_sizes;
    std::vector<uint32_t> sync_samples;   // 1-indexed sample numbers that are keyframes
    struct DeltaRun { uint32_t count, delta; };
    std::vector<DeltaRun> deltas;         // run-length compressed, per 8.6.1.2
    uint64_t               mdat_data_offset = 0;  // absolute file offset of first sample byte
    uint64_t               total_sample_bytes = 0;
};

// 12.1.3 Visual Sample Entry (shared header all codec-specific sample entries start with).
void WriteVisualSampleEntry(ByteWriter& w, uint32_t width, uint32_t height) {
    for (int i = 0; i < 6; ++i) w.W8(0);  // reserved
    w.WB16(1);                            // data_reference_index

    w.WB16(0); w.WB16(0);                 // pre_defined, reserved
    w.WB32(0); w.WB32(0); w.WB32(0);      // pre_defined[3]
    w.WB16(uint16_t(width));
    w.WB16(uint16_t(height));
    w.WB32(0x00480000);                   // horizresolution 72dpi
    w.WB32(0x00480000);                   // vertresolution 72dpi
    w.WB32(0);                            // reserved
    w.WB16(1);                            // frame_count

    uint8_t compressor_name[32] = {0};    // pascal string, we leave it empty (length byte 0)
    w.WriteBytes(compressor_name, sizeof(compressor_name));

    w.WB16(0x0018);  // depth
    w.WB16(0xFFFF);  // pre_defined = -1
}

// 12.1.5 Colour information. Hardcoded to bt709 limited range: matches the VUI Encoder::Open()
// bakes into the SPS itself — this box is a second, MP4-level copy of the
// same signal for players that read colr instead of parsing the SPS's VUI.
void WriteColr(ByteWriter& w) {
    WriteBox(w, 19, "colr");
    w.WriteBytes("nclx", 4);
    w.WB16(1);  // colour_primaries = BT.709
    w.WB16(1);  // transfer_characteristics = BT.709
    w.WB16(1);  // matrix_coefficients = BT.709
    w.W8(0);    // full_range_flag = 0 (tv/limited) + 7 reserved bits
}

void WritePasp(ByteWriter& w) {
    WriteBox(w, 16, "pasp");
    w.WB32(1);  // hSpacing
    w.WB32(1);  // vSpacing
}

// The codec-specific half of the video sample entry: ISO/IEC 14496-15 §5.4.2.1 AVCSampleEntry
// ('avc1' + 'avcC') or AV1-ISOBMFF §2.2 AV1SampleEntry ('av01' + 'av1C'). Both carry colr and pasp.
struct VideoEntry {
    const char*          fourcc;
    const char*          config_box;
    std::vector<uint8_t> config;
    MuxPicture           picture;  // resolved: never 0, at most the coded size
};

// 12.1.4 Clean aperture: the top-left `pic_w` x `pic_h` of the coded picture. The offsets are from
// the coded picture's centre, in halves so an odd difference stays exact.
void WriteClap(ByteWriter& w, uint32_t width, uint32_t height, MuxPicture pic) {
    WriteBox(w, 40, "clap");
    w.WB32(pic.width);  w.WB32(1);
    w.WB32(pic.height); w.WB32(1);
    w.WB32(static_cast<uint32_t>(static_cast<int32_t>(pic.width) - static_cast<int32_t>(width)));   w.WB32(2);
    w.WB32(static_cast<uint32_t>(static_cast<int32_t>(pic.height) - static_cast<int32_t>(height))); w.WB32(2);
}

void WriteVideoSampleEntry(ByteWriter& w, uint32_t width, uint32_t height, const VideoEntry& entry) {
    const size_t start = w.Pos();
    WriteBox(w, 0, entry.fourcc);
    WriteVisualSampleEntry(w, width, height);
    WriteBox(w, static_cast<uint32_t>(entry.config.size() + 8), entry.config_box);
    w.WriteBytes(entry.config.data(), entry.config.size());
    WriteColr(w);
    if (entry.picture.width != width || entry.picture.height != height) WriteClap(w, width, height, entry.picture);
    WritePasp(w);
    WriteBoxSize(w, start);
}

void WriteStsd(ByteWriter& w, uint32_t width, uint32_t height, const VideoEntry& entry) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "stsd", 0, 0);
    w.WB32(1);  // entry_count
    WriteVideoSampleEntry(w, width, height, entry);
    WriteBoxSize(w, start);
}

// 8.6.1.2 Decoding Time to Sample Box.
void WriteStts(ByteWriter& w, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "stts", 0, 0);
    w.WB32(static_cast<uint32_t>(t.deltas.size()));
    for (const auto& d : t.deltas) { w.WB32(d.count); w.WB32(d.delta); }
    WriteBoxSize(w, start);
}

// 8.6.2 Sync Sample Box (keyframes).
void WriteStss(ByteWriter& w, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "stss", 0, 0);
    w.WB32(static_cast<uint32_t>(t.sync_samples.size()));
    for (uint32_t s : t.sync_samples) w.WB32(s);
    WriteBoxSize(w, start);
}

// 8.7.4 Sample To Chunk Box — one chunk holds every sample (no interleaving to do with a single
// video-only track), so this is always a single-entry table.
void WriteStsc(ByteWriter& w, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "stsc", 0, 0);
    w.WB32(1);  // entry_count
    w.WB32(1);  // first_chunk
    w.WB32(static_cast<uint32_t>(t.sample_sizes.size()));  // samples_per_chunk
    w.WB32(1);  // sample_description_index
    WriteBoxSize(w, start);
}

// 8.7.3 Sample Size Box.
void WriteStsz(ByteWriter& w, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "stsz", 0, 0);
    w.WB32(0);  // sample_size = 0 -> explicit per-sample table follows
    w.WB32(static_cast<uint32_t>(t.sample_sizes.size()));
    for (uint32_t sz : t.sample_sizes) w.WB32(sz);
    WriteBoxSize(w, start);
}

// 8.7.5 Chunk Offset Box; co64 once the chunk starts past 4 GB.
void WriteStco(ByteWriter& w, const TrackTiming& t) {
    const size_t start = w.Pos();
    const bool   wide  = t.mdat_data_offset > UINT32_MAX;
    WriteFullBox(w, 0, wide ? "co64" : "stco", 0, 0);
    w.WB32(1);  // entry_count — one chunk
    if (wide) w.WB64(t.mdat_data_offset);
    else w.WB32(static_cast<uint32_t>(t.mdat_data_offset));
    WriteBoxSize(w, start);
}

void WriteStbl(ByteWriter& w, uint32_t width, uint32_t height, const VideoEntry& entry,
               const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "stbl");
    WriteStsd(w, width, height, entry);
    WriteStts(w, t);
    WriteStss(w, t);
    WriteStsc(w, t);
    WriteStsz(w, t);
    WriteStco(w, t);
    WriteBoxSize(w, start);
}

void WriteUrl(ByteWriter& w) { WriteFullBox(w, 12, "url ", 0, 1); }  // flag 1: data is in this file

void WriteDref(ByteWriter& w) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "dref", 0, 0);
    w.WB32(1);  // entry_count
    WriteUrl(w);
    WriteBoxSize(w, start);
}

void WriteDinf(ByteWriter& w) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "dinf");
    WriteDref(w);
    WriteBoxSize(w, start);
}

void WriteVmhd(ByteWriter& w) {
    WriteFullBox(w, 20, "vmhd", 0, 1);
    w.WB16(0); w.WB16(0); w.WB16(0); w.WB16(0);  // graphicsmode, opcolor r/g/b
}

void WriteMinf(ByteWriter& w, uint32_t width, uint32_t height, const VideoEntry& entry,
               const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "minf");
    WriteVmhd(w);
    WriteDinf(w);
    WriteStbl(w, width, height, entry, t);
    WriteBoxSize(w, start);
}

void WriteHdlr(ByteWriter& w) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "hdlr", 0, 0);
    w.WB32(0);
    w.WriteBytes("vide", 4);
    w.WB32(0); w.WB32(0); w.WB32(0);
    static const char kName[] = OMBRA_PRODUCT_NAME " Video Handler";
    w.WriteBytes(kName, sizeof(kName));  // includes NUL terminator
    WriteBoxSize(w, start);
}

// 8.4.2 Media Header Box. `lang_packed` defaults to "und" (21956) — see PackIso639Lang.
void WriteMdhd(ByteWriter& w, uint64_t creation_time, const TrackTiming& t, uint16_t lang_packed = 21956) {
    WriteFullBox(w, 32, "mdhd", 0, 0);
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(t.timescale);
    w.WB32(static_cast<uint32_t>(t.duration));
    w.WB16(lang_packed);
    w.WB16(0);       // pre_defined
}

void WriteMdia(ByteWriter& w, uint64_t creation_time, uint32_t width, uint32_t height,
               const VideoEntry& entry, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "mdia");
    WriteMdhd(w, creation_time, t);
    WriteHdlr(w);
    WriteMinf(w, width, height, entry, t);
    WriteBoxSize(w, start);
}

// 8.3.2 Track Header Box.
void WriteTkhd(ByteWriter& w, uint64_t creation_time, uint32_t width, uint32_t height,
               uint64_t movie_duration_ms) {
    WriteFullBox(w, 92, "tkhd", 0, /*flags=*/0x1 | 0x2);  // enabled | in movie
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(1);  // track_id
    w.WB32(0);  // reserved
    w.WB32(static_cast<uint32_t>(movie_duration_ms));
    w.WB32(0); w.WB32(0);  // reserved[2]
    w.WB16(0);              // layer
    w.WB16(0);              // alternate_group
    w.WB16(0);              // volume (video track)
    w.WB16(0);              // reserved
    for (int32_t v : kUnityMatrix) w.WB32(static_cast<uint32_t>(v));
    w.WB32(width << 16);
    w.WB32(height << 16);
}

void WriteTrak(ByteWriter& w, uint64_t creation_time, uint32_t width, uint32_t height,
               const VideoEntry& entry, const TrackTiming& t, uint64_t movie_duration_ms) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "trak");
    // The display size: the clean aperture where there is one.
    WriteTkhd(w, creation_time, entry.picture.width, entry.picture.height, movie_duration_ms);
    WriteMdia(w, creation_time, width, height, entry, t);
    WriteBoxSize(w, start);
}

// --- audio (mp4a/esds) box tree -----------------------------------------------------------------

// MPEG-4 "expandable class length" (ISO/IEC 14496-1 §8.3.3): high bit = continuation, low 7 bits
// = value. Every length esds writes here is well under 128 (ASC is 2 bytes; the largest is the
// outer ES_Descriptor at ~25 bytes), so a single terminal byte always suffices — dies rather than
// silently truncating if that ever isn't true (e.g. an ASC with SBR/PS extension data, which this
// project's AAC-LC-only encoder never produces, but a future encoder change might).
void WriteDescLen(ByteWriter& w, uint32_t len) {
    if (len >= 0x80)
        die("mux: esds descriptor length needs multi-byte encoding — not implemented (AAC-LC's "
            "ASC and this file's fixed descriptor fields were assumed to always fit in 7 bits)");
    w.W8(static_cast<uint8_t>(len));
}

// ISO/IEC 14496-14 §5.6 ES Descriptor box, carrying the AudioSpecificConfig AacEncoder::Open()
// already verified is raw (not ADTS-wrapped) and MF-negotiated for AAC-LC.
void WriteEsds(ByteWriter& w, const std::vector<uint8_t>& asc, uint32_t bitrate_bps) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "esds", 0, 0);

    const uint32_t dec_specific_len = static_cast<uint32_t>(asc.size());
    const uint32_t dec_config_len = 1 /*objectTypeIndication*/ + 1 /*streamType+flags byte*/ +
                                     3 /*bufferSizeDB*/ + 4 /*maxBitrate*/ + 4 /*avgBitrate*/ +
                                     2 /*DecSpecificInfo tag+len*/ + dec_specific_len;
    const uint32_t es_descr_len = 2 /*ES_ID*/ + 1 /*flags*/ +
                                   2 /*DecoderConfigDescr tag+len*/ + dec_config_len +
                                   2 /*SLConfigDescr tag+len*/ + 1 /*SLConfig data*/;

    w.W8(0x03);  // ES_DescrTag
    WriteDescLen(w, es_descr_len);
    w.WB16(0);   // ES_ID (unused when embedded in an MP4 sample entry)
    w.W8(0);     // streamDependenceFlag(1)=0, URL_Flag(1)=0, OCRstreamFlag(1)=0, streamPriority(5)=0

    w.W8(0x04);  // DecoderConfigDescrTag
    WriteDescLen(w, dec_config_len);
    w.W8(0x40);  // objectTypeIndication: MPEG-4 Audio (AAC)
    w.W8(0x15);  // streamType(6)=0x05 (audio)<<2 | upStream(1)=0<<1 | reserved(1)=1
    w.WB24(0);   // bufferSizeDB — informative only; 0 is a common, accepted muxer default
    w.WB32(bitrate_bps);  // maxBitrate
    w.WB32(bitrate_bps);  // avgBitrate — this project doesn't do true VBR AAC, so max==avg

    w.W8(0x05);  // DecSpecificInfoTag
    WriteDescLen(w, dec_specific_len);
    w.WriteBytes(asc.data(), asc.size());

    w.W8(0x06);  // SLConfigDescrTag
    WriteDescLen(w, 1);
    w.W8(0x02);  // predefined = 0x02 (reserved for use in MP4 files)

    WriteBoxSize(w, start);
}

// ISO/IEC 14496-14 §5.6.1 AudioSampleEntry ('mp4a'), built on 14496-12 §12.2.3's base layout.
void WriteMp4a(ByteWriter& w, uint32_t sample_rate, const std::vector<uint8_t>& asc, uint32_t bitrate_bps) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "mp4a");
    for (int i = 0; i < 6; ++i) w.W8(0);  // reserved (SampleEntry base)
    w.WB16(1);                             // data_reference_index
    w.WB32(0); w.WB32(0);                  // reserved[2] (AudioSampleEntry)
    w.WB16(2);                             // channelcount — stereo throughout
    w.WB16(16);                            // samplesize — matches PcmRing's s16 storage
    w.WB16(0);                             // pre_defined
    w.WB16(0);                             // reserved
    w.WB32(sample_rate << 16);             // samplerate, 16.16 fixed point
    WriteEsds(w, asc, bitrate_bps);
    WriteBoxSize(w, start);
}

void WriteStsdAudio(ByteWriter& w, uint32_t sample_rate, const std::vector<uint8_t>& asc, uint32_t bitrate_bps) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "stsd", 0, 0);
    w.WB32(1);  // entry_count
    WriteMp4a(w, sample_rate, asc, bitrate_bps);
    WriteBoxSize(w, start);
}

// Audio's stbl omits stss (8.6.2) entirely — its absence means every sample is a random access
// point, which is true for AAC (no analog to H.264's IDR/non-IDR distinction). stts/stsc/stsz/
// stco are the same box shapes as video's, reused as-is against an audio-flavored TrackTiming.
void WriteStblAudio(ByteWriter& w, uint32_t sample_rate, const std::vector<uint8_t>& asc,
                    uint32_t bitrate_bps, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "stbl");
    WriteStsdAudio(w, sample_rate, asc, bitrate_bps);
    WriteStts(w, t);
    WriteStsc(w, t);
    WriteStsz(w, t);
    WriteStco(w, t);
    WriteBoxSize(w, start);
}

// 8.4.5.2 Sound Media Header Box — audio's analog to WriteVmhd.
void WriteSmhd(ByteWriter& w) {
    WriteFullBox(w, 16, "smhd", 0, 0);
    w.WB16(0);  // balance (centered)
    w.WB16(0);  // reserved
}

void WriteMinfAudio(ByteWriter& w, uint32_t sample_rate, const std::vector<uint8_t>& asc,
                    uint32_t bitrate_bps, const TrackTiming& t) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "minf");
    WriteSmhd(w);
    WriteDinf(w);
    WriteStblAudio(w, sample_rate, asc, bitrate_bps, t);
    WriteBoxSize(w, start);
}

// `name` becomes the track's handler_name — what Premiere/Resolve/Vegas actually display for an
// audio track. This is not cosmetic, do not skip.
void WriteHdlrAudio(ByteWriter& w, const std::string& name) {
    const size_t start = w.Pos();
    WriteFullBox(w, 0, "hdlr", 0, 0);
    w.WB32(0);
    w.WriteBytes("soun", 4);
    w.WB32(0); w.WB32(0); w.WB32(0);
    w.WriteBytes(name.c_str(), name.size() + 1);  // pascal-less cstring, NUL included
    WriteBoxSize(w, start);
}

// WriteMdhd (8.4.2) is reused unchanged for audio — its `timescale`/`duration` come from the
// TrackTiming passed in, which the caller sets to the audio sample rate, not fps.
void WriteMdiaAudio(ByteWriter& w, uint64_t creation_time, uint32_t sample_rate,
                    const std::vector<uint8_t>& asc, uint32_t bitrate_bps, const TrackTiming& t,
                    const std::string& name, uint16_t lang_packed) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "mdia");
    WriteMdhd(w, creation_time, t, lang_packed);
    WriteHdlrAudio(w, name);
    WriteMinfAudio(w, sample_rate, asc, bitrate_bps, t);
    WriteBoxSize(w, start);
}

void WriteTkhdAudio(ByteWriter& w, uint64_t creation_time, uint32_t track_id, uint64_t movie_duration_ms) {
    WriteFullBox(w, 92, "tkhd", 0, /*flags=*/0x1 | 0x2);  // enabled | in movie
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(track_id);
    w.WB32(0);  // reserved
    w.WB32(static_cast<uint32_t>(movie_duration_ms));
    w.WB32(0); w.WB32(0);   // reserved[2]
    w.WB16(0);               // layer
    w.WB16(0);               // alternate_group
    w.WB16(0x0100);          // volume 1.0 — audio track, unlike video's 0
    w.WB16(0);                // reserved
    for (int32_t v : kUnityMatrix) w.WB32(static_cast<uint32_t>(v));
    w.WB32(0);  // width = 0 for a non-visual track
    w.WB32(0);  // height = 0 for a non-visual track
}

// 8.6.5 Edit List: the track plays from `media_time` samples into its media, for
// `segment_duration_ms`. One edit, rate 1.
void WriteEdts(ByteWriter& w, uint64_t segment_duration_ms, uint32_t media_time) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "edts");
    WriteFullBox(w, 28, "elst", 0, 0);
    w.WB32(1);  // entry_count
    w.WB32(static_cast<uint32_t>(segment_duration_ms));
    w.WB32(media_time);
    w.WB16(1);  // media_rate_integer
    w.WB16(0);  // media_rate_fraction
    WriteBoxSize(w, start);
}

void WriteTrakAudio(ByteWriter& w, uint64_t creation_time, uint32_t track_id, uint32_t sample_rate,
                    const std::vector<uint8_t>& asc, uint32_t bitrate_bps, const TrackTiming& t,
                    uint64_t movie_duration_ms, const std::string& name, uint16_t lang_packed,
                    uint32_t skip_samples) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "trak");
    WriteTkhdAudio(w, creation_time, track_id, movie_duration_ms);
    if (skip_samples > 0) {
        const uint64_t played = t.duration > skip_samples ? t.duration - skip_samples : 0;
        WriteEdts(w, t.timescale ? played * 1000 / t.timescale : 0, skip_samples);
    }
    WriteMdiaAudio(w, creation_time, sample_rate, asc, bitrate_bps, t, name, lang_packed);
    WriteBoxSize(w, start);
}

// 8.2.2 Movie Header Box.
void WriteMvhd(ByteWriter& w, uint64_t creation_time, uint64_t movie_duration_ms, uint32_t next_track_id) {
    WriteFullBox(w, 108, "mvhd", 0, 0);
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(static_cast<uint32_t>(creation_time));
    w.WB32(1000);  // movie timescale: milliseconds
    w.WB32(static_cast<uint32_t>(movie_duration_ms));
    w.WB32(0x00010000);  // rate 1.0
    w.WB16(0x0100);       // volume 1.0
    w.WB16(0); w.WB32(0); w.WB32(0);  // reserved
    for (int32_t v : kUnityMatrix) w.WB32(static_cast<uint32_t>(v));
    for (int i = 0; i < 6; ++i) w.WB32(0);  // pre_defined[6]
    w.WB32(next_track_id);
}

void WriteFtyp(ByteWriter& w, VideoCodec codec) {
    const size_t start = w.Pos();
    WriteBox(w, 0, "ftyp");
    w.WriteBytes("isom", 4);  // major_brand
    w.WB32(512);               // minor_version
    w.WriteBytes("isom", 4);
    w.WriteBytes("iso2", 4);
    w.WriteBytes(codec == VideoCodec::AV1 ? "av01" : "avc1", 4);
    w.WriteBytes("mp41", 4);
    WriteBoxSize(w, start);
}

// Empty `audio` is a video-only file (the default when a save has
// no audio ring or the audio window came up empty). Each entry's TrackTiming's timescale is that
// track's own sample rate, not fps — WriteMdhd (reused for every track) doesn't care which.
struct AudioMoovParams {
    uint32_t                    sample_rate = 48000;
    const std::vector<uint8_t>* asc = nullptr;
    uint32_t                    bitrate_bps = 0;
    const TrackTiming*          timing = nullptr;
    std::string                 name;
    uint16_t                    lang_packed = 21956;  // "und"
    uint32_t                    skip_samples = 0;     // MuxAudioTrack::skip_samples
};

void WriteMoov(ByteWriter& w, uint64_t creation_time, uint32_t width, uint32_t height,
               const VideoEntry& entry, const TrackTiming& video_t,
               const std::vector<AudioMoovParams>& audio) {
    auto duration_ms = [](const TrackTiming& t) {
        return t.timescale ? (t.duration * 1000 / t.timescale) : 0;
    };
    uint64_t movie_duration_ms = duration_ms(video_t);
    for (const auto& a : audio) {
        TrackTiming played = *a.timing;
        played.duration    = played.duration > a.skip_samples ? played.duration - a.skip_samples : 0;
        movie_duration_ms  = (std::max)(movie_duration_ms, duration_ms(played));
    }

    const size_t start = w.Pos();
    WriteBox(w, 0, "moov");
    WriteMvhd(w, creation_time, movie_duration_ms, static_cast<uint32_t>(2 + audio.size()));
    WriteTrak(w, creation_time, width, height, entry, video_t, movie_duration_ms);
    for (size_t i = 0; i < audio.size(); ++i) {
        const auto& a = audio[i];
        WriteTrakAudio(w, creation_time, /*track_id=*/static_cast<uint32_t>(2 + i), a.sample_rate,
                       *a.asc, a.bitrate_bps, *a.timing, movie_duration_ms, a.name, a.lang_packed,
                       a.skip_samples);
    }
    WriteBoxSize(w, start);
}

// The mdat goes straight to disk through this in large writes; only ftyp and moov (small) are
// built in memory. Building the whole file in a vector first held the clip twice (the AU copies
// plus the file image) and spent its time growing and copying that vector.
class Mp4FileSink {
public:
    explicit Mp4FileSink(const std::wstring& path) : path_(path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        ok_ = handle_ != INVALID_HANDLE_VALUE;
        if (ok_) buf_.reserve(kBufferBytes);
    }
    ~Mp4FileSink() {
        const bool created = handle_ != INVALID_HANDLE_VALUE;
        if (created) CloseHandle(handle_);
        if (created && !ok_) DeleteFileW(path_.c_str());  // no half-written clip left behind
    }
    Mp4FileSink(const Mp4FileSink&) = delete;
    Mp4FileSink& operator=(const Mp4FileSink&) = delete;

    uint64_t Pos() const { return flushed_ + buf_.size(); }

    void Write(const uint8_t* p, size_t n) {
        if (!ok_) return;
        if (buf_.size() + n > kBufferBytes) {
            Flush();
            if (n >= kBufferBytes) {
                WriteRaw(p, n);
                flushed_ += n;
                return;
            }
        }
        buf_.insert(buf_.end(), p, p + n);
    }
    void Write(const std::vector<uint8_t>& v) { Write(v.data(), v.size()); }

    // Overwrites bytes already written (a box size), then appending continues at the end.
    void Patch(uint64_t pos, const uint8_t* p, size_t n) {
        Flush();
        if (!ok_) return;
        ok_ = SeekTo(pos);
        if (ok_) WriteRaw(p, n);
        if (ok_) ok_ = SeekTo(flushed_);
    }

    // The file is deleted on destruction instead of kept.
    void Discard() { ok_ = false; }

    bool Finish() {
        Flush();
        return ok_;
    }

private:
    static constexpr size_t kBufferBytes = 4u << 20;

    void Flush() {
        if (!ok_ || buf_.empty()) return;
        WriteRaw(buf_.data(), buf_.size());
        flushed_ += buf_.size();
        buf_.clear();
    }
    void WriteRaw(const uint8_t* p, size_t n) {
        while (ok_ && n > 0) {
            const DWORD chunk = static_cast<DWORD>((std::min)(n, size_t{1} << 30));
            DWORD       wrote = 0;
            if (!WriteFile(handle_, p, chunk, &wrote, nullptr) || wrote == 0) {
                ok_ = false;
                return;
            }
            p += wrote;
            n -= wrote;
        }
    }
    bool SeekTo(uint64_t pos) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(pos);
        return SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN) != 0;
    }

    std::wstring         path_;
    HANDLE               handle_ = INVALID_HANDLE_VALUE;
    bool                 ok_ = false;
    std::vector<uint8_t> buf_;
    uint64_t             flushed_ = 0;  // bytes already handed to WriteFile
};

}  // namespace

bool MuxWriteMp4(const std::wstring& path, const std::vector<uint8_t>& codec_header,
                  uint32_t width, uint32_t height, uint32_t fps,
                  const std::vector<MuxVideoSample>& aus, const MuxReadSample& read,
                  const std::vector<MuxAudioTrack>& audio_tracks, VideoCodec codec,
                  uint32_t last_sample_ticks, MuxPicture picture) {
    if (aus.empty() || !aus.front().is_keyframe)
        die("mux: MuxWriteMp4 called with no AUs, or a range not starting on a keyframe");

    const bool av1 = codec == VideoCodec::AV1;
    VideoEntry entry = av1 ? VideoEntry{"av01", "av1C", BuildAv1C(codec_header.data(), codec_header.size())}
                           : VideoEntry{"avc1", "avcC", BuildAvcC(codec_header)};
    entry.picture.width = picture.width ? (std::min)(picture.width, width) : width;
    entry.picture.height = picture.height ? (std::min)(picture.height, height) : height;
    if (entry.config.empty()) {
        Log::Error("mux: %s codec header did not parse (%zu bytes)", av1 ? "AV1" : "H.264", codec_header.size());
        return false;
    }
    // WriteDescLen's single-byte lengths: esds wraps the ASC in ~23 bytes of fixed descriptors.
    for (const auto& track : audio_tracks) {
        if (track.asc.size() > 100) {
            Log::Error("mux: AudioSpecificConfig of %zu bytes doesn't fit esds' 1-byte lengths", track.asc.size());
            return false;
        }
    }

    Mp4FileSink sink(path);
    {
        ByteWriter ftyp;
        WriteFtyp(ftyp, codec);
        sink.Write(ftyp.Data());
    }

    // mdat, written before moov so sample offsets are known by construction: box header now (size
    // patched after), then every sample's data back to back in one chunk — video first, then audio
    // (if present). An 8-byte free box goes first (ffmpeg's trick): if the mdat passes 4 GB, the
    // free box and the 32-bit header become one 64-bit mdat header in the same 16 bytes.
    const uint64_t free_start = sink.Pos();
    const uint64_t mdat_start = free_start + 8;
    {
        ByteWriter hdr;
        WriteBox(hdr, 8, "free");
        WriteBox(hdr, 0, "mdat");
        sink.Write(hdr.Data());
    }
    const uint64_t mdat_data_start = sink.Pos();

    TrackTiming t;
    t.timescale         = fps;
    t.mdat_data_offset  = mdat_data_start;
    t.sample_sizes.reserve(aus.size());
    t.sync_samples.reserve(aus.size() / 8 + 1);

    // H.264: ConvertAnnexBSample's conversion, written straight to the sink with no per-sample copy.
    // AV1: the temporal unit minus the OBUs MP4 keeps out of samples, through one reused buffer.
    std::vector<uint8_t> annexb, obus;
    for (size_t i = 0; i < aus.size(); ++i) {
        if (!read(i, annexb)) {
            sink.Discard();
            return false;
        }
        uint32_t sample_size = 0;
        if (av1) {
            obus.clear();
            if (!FilterAv1Sample(annexb.data(), annexb.size(), obus)) {
                Log::Error("mux: malformed AV1 OBU in sample %zu", i);
                sink.Discard();
                return false;
            }
            sink.Write(obus);
            sample_size = static_cast<uint32_t>(obus.size());
        } else {
            for (const auto& n : SplitAnnexB(annexb.data(), annexb.size())) {
                const uint8_t type = NalType(annexb.data(), n);
                if (type == 7 || type == 8) continue;  // SPS / PPS: avcC carries them
                const uint8_t len_be[4] = {uint8_t(n.length >> 24), uint8_t(n.length >> 16),
                                           uint8_t(n.length >> 8), uint8_t(n.length)};
                sink.Write(len_be, 4);
                sink.Write(annexb.data() + n.offset, n.length);
                sample_size += static_cast<uint32_t>(4 + n.length);
            }
        }
        t.sample_sizes.push_back(sample_size);
        if (aus[i].is_keyframe) t.sync_samples.push_back(static_cast<uint32_t>(i + 1));  // 1-indexed
    }

    // stts: per-sample duration in timescale units, run-length compressed. timescale == fps, so
    // one CFR tick's worth of duration is exactly one timescale unit — a tick_index delta of 1
    // (the normal case) is a duration of 1, with no float rounding anywhere in this computation.
    uint64_t total_ticks = 0;
    for (size_t i = 0; i + 1 < aus.size(); ++i) {
        const uint64_t delta = aus[i + 1].tick_index - aus[i].tick_index;
        total_ticks += delta;
        if (!t.deltas.empty() && t.deltas.back().delta == delta) {
            t.deltas.back().count++;
        } else {
            t.deltas.push_back({1, static_cast<uint32_t>(delta)});
        }
    }
    // Last sample has no following AU to diff against; the caller says how long it held (one tick
    // unless a trim cut before a gap), so stts entry counts equal the sample count (ISO/IEC 14496-12
    // §8.6.1.2 requires this).
    {
        const uint32_t last_delta = last_sample_ticks ? last_sample_ticks : 1;
        total_ticks += last_delta;
        if (!t.deltas.empty() && t.deltas.back().delta == last_delta) t.deltas.back().count++;
        else t.deltas.push_back({1, last_delta});
    }
    t.duration = total_ticks;

    // N audio tracks, each written back to back
    // into the same mdat the video samples just filled. Every AAC frame is a fixed
    // kSamplesPerFrame-sample duration — unlike video's variable-tick-delta stts, each track's stts
    // is always a single-entry run (one delta, repeated for every sample), since MF's AAC encoder
    // never emits a partial-duration frame. `audio_track_timings` must outlive WriteMoov below —
    // AudioMoovParams::timing points into it, so it's declared before that call and not touched
    // again after.
    constexpr uint32_t kSamplesPerAacFrame = 1024;  // AacEncoder::kSamplesPerFrame
    std::vector<TrackTiming> audio_track_timings(audio_tracks.size());
    for (size_t i = 0; i < audio_tracks.size(); ++i) {
        const auto& track = audio_tracks[i];
        TrackTiming& audio_t = audio_track_timings[i];
        audio_t.timescale        = track.sample_rate;
        audio_t.mdat_data_offset = sink.Pos();
        audio_t.sample_sizes.reserve(track.frames.size());
        for (const auto& frame : track.frames) {
            sink.Write(frame.data);
            audio_t.sample_sizes.push_back(static_cast<uint32_t>(frame.data.size()));
        }
        audio_t.deltas.push_back({static_cast<uint32_t>(track.frames.size()), kSamplesPerAacFrame});
        audio_t.duration = static_cast<uint64_t>(track.frames.size()) * kSamplesPerAacFrame;
    }

    const uint64_t mdat_end = sink.Pos();
    if (mdat_end - mdat_start <= UINT32_MAX) {
        const uint32_t size = static_cast<uint32_t>(mdat_end - mdat_start);
        const uint8_t  be[4] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
        sink.Patch(mdat_start, be, 4);
    } else {
        ByteWriter wide;  // size = 1, then the 64-bit largesize, starting where free was
        wide.WB32(1);
        wide.WriteBytes("mdat", 4);
        wide.WB64(mdat_end - free_start);
        sink.Patch(free_start, wide.Data().data(), wide.Data().size());
    }

    // MP4 timestamps are seconds since 1904-01-01; 2082844800 is the offset to the Unix epoch.
    const uint64_t creation_time = static_cast<uint64_t>(std::time(nullptr)) + 2082844800ULL;

    std::vector<AudioMoovParams> audio_moov_params(audio_tracks.size());
    for (size_t i = 0; i < audio_tracks.size(); ++i) {
        const auto& track = audio_tracks[i];
        AudioMoovParams& p = audio_moov_params[i];
        p.sample_rate = track.sample_rate;
        p.asc         = &track.asc;
        p.bitrate_bps = track.bitrate_bps;  // esds's maxBitrate/avgBitrate — informative metadata
                                              // only, not used to decode
        p.timing      = &audio_track_timings[i];
        p.name        = track.name;
        p.lang_packed = PackIso639Lang(track.language);
        p.skip_samples = track.skip_samples;
    }
    ByteWriter moov;
    WriteMoov(moov, creation_time, width, height, entry, t, audio_moov_params);
    sink.Write(moov.Data());
    return sink.Finish();
}

bool MuxWriteMp4(const std::wstring& path, const std::vector<uint8_t>& codec_header,
                  uint32_t width, uint32_t height, uint32_t fps, const std::vector<MuxAu>& aus,
                  const std::vector<MuxAudioTrack>& audio_tracks, VideoCodec codec, MuxPicture picture) {
    std::vector<MuxVideoSample> samples;
    samples.reserve(aus.size());
    for (const MuxAu& au : aus) samples.push_back({au.is_keyframe, au.tick_index});
    return MuxWriteMp4(path, codec_header, width, height, fps, samples,
                       [&](size_t i, std::vector<uint8_t>& out) { out = aus[i].annexb; return true; },
                       audio_tracks, codec, 1, picture);
}

}  // namespace ombra
