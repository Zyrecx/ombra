// Ported from FFmpeg n7.1 libavformat/av1.c (ff_isom_write_av1c, parse_sequence_header,
// parse_color_config, av1_filter_obus) and libavcodec/av1_parse.h (parse_obu_header,
// get_obu_bit_length), Copyright (c) 2018 James Almer <jamrial@gmail.com>, LGPL-2.1-or-later.
// The parse is the same field for field; FFmpeg's GetBitContext/AVIOContext are replaced by the
// small BitReader below and std::vector. OBS's plugins/obs-outputs/rtmp-av1.c is a GPL adaptation
// of the same file and was read for how OBS uses it, not copied.
#include "ombra/av1.h"

#include <cstring>

namespace ombra {

namespace {

constexpr int kObuSequenceHeader      = 1;
constexpr int kObuTemporalDelimiter   = 2;
constexpr int kObuTileGroup           = 4;
constexpr int kObuFrame               = 6;
constexpr int kObuRedundantFrameHeader = 7;
constexpr int kObuTileList            = 8;
constexpr int kObuPadding             = 15;

constexpr int kProfileMain         = 0;
constexpr int kProfileHigh         = 1;
constexpr int kProfileProfessional = 2;

// MSB-first bit reader over a fixed bit length. Reading past the end returns zeros and leaves
// Left() negative, which the callers check once at the end, as FFmpeg's checked reader does.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t bits) : data_(data), bits_(bits) {}
    uint32_t Bit() {
        uint32_t v = 0;
        if (pos_ < bits_) v = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
        ++pos_;
        return v;
    }
    uint32_t Bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | Bit();
        return v;
    }
    void   Skip(size_t n) { pos_ += n; }
    size_t Count() const { return pos_; }
    long long Left() const { return static_cast<long long>(bits_) - static_cast<long long>(pos_); }

private:
    const uint8_t* data_;
    size_t         bits_;
    size_t         pos_ = 0;
};

struct ObuHeader {
    int      type = 0;
    size_t   payload_offset = 0;  // header (+ leb128 size) length
    size_t   payload_size = 0;
    size_t   total = 0;           // header + payload
};

// av1_parse.h parse_obu_header.
bool ParseObuHeader(const uint8_t* buf, size_t size, ObuHeader& h) {
    if (size < 1) return false;
    BitReader r(buf, (size < 10 ? size : 10) * 8);  // MAX_OBU_HEADER_SIZE: 2 + 8 bytes of leb128
    if (r.Bit() != 0) return false;                 // obu_forbidden_bit
    h.type                   = static_cast<int>(r.Bits(4));
    const bool extension     = r.Bit() != 0;
    const bool has_size      = r.Bit() != 0;
    r.Skip(1);                                      // obu_reserved_1bit
    if (extension) r.Skip(8);                       // temporal_id, spatial_id, reserved
    uint64_t payload = 0;
    if (has_size) {
        for (int i = 0; i < 8; ++i) {               // leb128
            const uint32_t byte = r.Bits(8);
            payload |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
            if (!(byte & 0x80)) break;
        }
    } else {
        payload = size - 1 - (extension ? 1 : 0);
    }
    if (r.Left() < 0) return false;
    h.payload_offset = r.Count() / 8;
    if (payload > size || h.payload_offset + payload > size) return false;
    h.payload_size = static_cast<size_t>(payload);
    h.total        = h.payload_offset + h.payload_size;
    return true;
}

// av1_parse.h get_obu_bit_length: the payload's length in bits without its trailing bits.
size_t ObuBitLength(const uint8_t* buf, size_t size, int type) {
    if (type == kObuTileGroup || type == kObuTileList || type == kObuFrame) return size * 8;
    while (size > 0 && buf[size - 1] == 0) --size;
    if (size == 0) return 0;
    const uint8_t v = buf[size - 1];
    int ctz = 0;
    while (!((v >> ctz) & 1)) ++ctz;
    return size * 8 - (ctz + 1);  // drop trailing_one_bit and the zeros after it
}

struct SequenceParams {
    int profile = 0, level = 0, tier = 0, bitdepth = 8, monochrome = 0;
    int chroma_subsampling_x = 0, chroma_subsampling_y = 0, chroma_sample_position = 0;
    uint32_t max_frame_width = 0, max_frame_height = 0;  // not in FFmpeg's parse: Av1CodedSize
};

void Uvlc(BitReader& r) {
    int leading_zeros = 0;
    while (r.Left() > 0) {
        if (r.Bit()) break;
        ++leading_zeros;
    }
    if (leading_zeros >= 32) return;
    r.Skip(leading_zeros);
}

// av1.c parse_color_config.
void ParseColorConfig(SequenceParams& seq, BitReader& r) {
    int twelve_bit = 0;
    const int high_bitdepth = static_cast<int>(r.Bit());
    if (seq.profile == kProfileProfessional && high_bitdepth) twelve_bit = static_cast<int>(r.Bit());
    seq.bitdepth = 8 + high_bitdepth * 2 + twelve_bit * 2;

    seq.monochrome = seq.profile == kProfileHigh ? 0 : static_cast<int>(r.Bit());

    uint32_t primaries = 2, transfer = 2, matrix = 2;  // unspecified
    if (r.Bit()) {                                     // color_description_present_flag
        primaries = r.Bits(8);
        transfer  = r.Bits(8);
        matrix    = r.Bits(8);
    }

    if (seq.monochrome) {
        r.Skip(1);  // color_range
        seq.chroma_subsampling_x = 1;
        seq.chroma_subsampling_y = 1;
        seq.chroma_sample_position = 0;
        return;
    }
    if (primaries == 1 && transfer == 13 && matrix == 0) {  // BT.709 / sRGB / identity
        seq.chroma_subsampling_x = 0;
        seq.chroma_subsampling_y = 0;
    } else {
        r.Skip(1);  // color_range
        if (seq.profile == kProfileMain) {
            seq.chroma_subsampling_x = 1;
            seq.chroma_subsampling_y = 1;
        } else if (seq.profile == kProfileHigh) {
            seq.chroma_subsampling_x = 0;
            seq.chroma_subsampling_y = 0;
        } else if (twelve_bit) {
            seq.chroma_subsampling_x = static_cast<int>(r.Bit());
            seq.chroma_subsampling_y = seq.chroma_subsampling_x ? static_cast<int>(r.Bit()) : 0;
        } else {
            seq.chroma_subsampling_x = 1;
            seq.chroma_subsampling_y = 0;
        }
        if (seq.chroma_subsampling_x && seq.chroma_subsampling_y)
            seq.chroma_sample_position = static_cast<int>(r.Bits(2));
    }
    r.Skip(1);  // separate_uv_delta_q
}

// av1.c parse_sequence_header, over the OBU's payload.
bool ParseSequenceHeader(const uint8_t* buf, size_t size, SequenceParams& seq) {
    BitReader r(buf, ObuBitLength(buf, size, kObuSequenceHeader));
    seq = {};
    seq.profile = static_cast<int>(r.Bits(3));
    r.Skip(1);  // still_picture
    const bool reduced_still_picture_header = r.Bit() != 0;

    if (reduced_still_picture_header) {
        seq.level = static_cast<int>(r.Bits(5));
        seq.tier  = 0;
    } else {
        bool decoder_model_info_present = false;
        int  buffer_delay_length_minus_1 = 0;
        if (r.Bit()) {     // timing_info_present_flag
            r.Skip(64);    // num_units_in_display_tick, time_scale
            if (r.Bit()) Uvlc(r);  // equal_picture_interval -> num_ticks_per_picture_minus_1
            decoder_model_info_present = r.Bit() != 0;
            if (decoder_model_info_present) {
                buffer_delay_length_minus_1 = static_cast<int>(r.Bits(5));
                r.Skip(32);  // num_units_in_decoding_tick
                r.Skip(10);  // buffer_removal_time_length_minus_1, frame_presentation_time_length_minus_1
            }
        }
        const bool initial_display_delay_present = r.Bit() != 0;
        const int  operating_points_cnt_minus_1  = static_cast<int>(r.Bits(5));
        for (int i = 0; i <= operating_points_cnt_minus_1; ++i) {
            r.Skip(12);  // operating_point_idc
            const int seq_level_idx = static_cast<int>(r.Bits(5));
            const int seq_tier      = seq_level_idx > 7 ? static_cast<int>(r.Bit()) : 0;
            if (decoder_model_info_present && r.Bit()) {  // decoder_model_present_for_this_op
                r.Skip(buffer_delay_length_minus_1 + 1);  // decoder_buffer_delay
                r.Skip(buffer_delay_length_minus_1 + 1);  // encoder_buffer_delay
                r.Skip(1);                                // low_delay_mode_flag
            }
            if (initial_display_delay_present && r.Bit()) r.Skip(4);  // initial_display_delay_minus_1
            if (i == 0) {
                seq.level = seq_level_idx;
                seq.tier  = seq_tier;
            }
        }
    }

    const int frame_width_bits_minus_1  = static_cast<int>(r.Bits(4));
    const int frame_height_bits_minus_1 = static_cast<int>(r.Bits(4));
    seq.max_frame_width  = r.Bits(frame_width_bits_minus_1 + 1) + 1;
    seq.max_frame_height = r.Bits(frame_height_bits_minus_1 + 1) + 1;

    if (!reduced_still_picture_header && r.Bit()) r.Skip(7);  // frame_id_numbers_present_flag
    r.Skip(3);  // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter

    if (!reduced_still_picture_header) {
        r.Skip(4);  // enable_interintra_compound, masked_compound, warped_motion, dual_filter
        const bool enable_order_hint = r.Bit() != 0;
        if (enable_order_hint) r.Skip(2);  // enable_jnt_comp, enable_ref_frame_mvs
        int seq_force_screen_content_tools = 2;
        if (!r.Bit()) seq_force_screen_content_tools = static_cast<int>(r.Bit());  // seq_choose_screen_content_tools
        if (seq_force_screen_content_tools && !r.Bit()) r.Skip(1);  // seq_choose_integer_mv / force
        if (enable_order_hint) r.Skip(3);  // order_hint_bits_minus_1
    }

    r.Skip(3);  // enable_superres, enable_cdef, enable_restoration
    ParseColorConfig(seq, r);
    r.Skip(1);  // film_grain_params_present
    return r.Left() == 0;
}

// The one sequence header OBU in `buf`, parsed. nullptr when there is none, more than one, or it
// doesn't parse.
const uint8_t* FindSequenceHeader(const uint8_t* buf, size_t size, SequenceParams& seq, size_t& obu_size) {
    const uint8_t* seq_obu = nullptr;
    while (size > 0) {
        ObuHeader h;
        if (!ParseObuHeader(buf, size, h)) return nullptr;
        if (h.type == kObuSequenceHeader) {
            if (seq_obu || h.payload_size == 0) return nullptr;  // exactly one, non-empty
            if (!ParseSequenceHeader(buf + h.payload_offset, h.payload_size, seq)) return nullptr;
            seq_obu  = buf;
            obu_size = h.total;
        }
        buf  += h.total;
        size -= h.total;
    }
    return seq_obu;
}

}  // namespace

bool Av1CodedSize(const uint8_t* buf, size_t size, uint32_t& width, uint32_t& height) {
    SequenceParams seq;
    size_t         obu_size = 0;
    if (!FindSequenceHeader(buf, size, seq, obu_size)) return false;
    width  = seq.max_frame_width;
    height = seq.max_frame_height;
    return true;
}

std::vector<uint8_t> BuildAv1C(const uint8_t* buf, size_t size) {
    SequenceParams seq;
    size_t         seq_obu_size = 0;
    const uint8_t* seq_obu      = FindSequenceHeader(buf, size, seq, seq_obu_size);
    if (!seq_obu) return {};

    std::vector<uint8_t> out(4);
    out[0] = 0x81;  // marker 1, version 1
    out[1] = static_cast<uint8_t>((seq.profile << 5) | (seq.level & 0x1f));
    out[2] = static_cast<uint8_t>((seq.tier << 7) | ((seq.bitdepth > 8) << 6) | ((seq.bitdepth == 12) << 5) |
                                  (seq.monochrome << 4) | (seq.chroma_subsampling_x << 3) |
                                  (seq.chroma_subsampling_y << 2) | (seq.chroma_sample_position & 3));
    out[3] = 0;     // reserved, initial_presentation_delay_present = 0
    out.insert(out.end(), seq_obu, seq_obu + seq_obu_size);
    return out;
}

bool FilterAv1Sample(const uint8_t* buf, size_t size, std::vector<uint8_t>& out) {
    while (size > 0) {
        ObuHeader h;
        if (!ParseObuHeader(buf, size, h)) return false;
        switch (h.type) {
        case kObuTemporalDelimiter:
        case kObuRedundantFrameHeader:
        case kObuTileList:
        case kObuPadding:
            break;
        default:
            out.insert(out.end(), buf, buf + h.total);
            break;
        }
        buf  += h.total;
        size -= h.total;
    }
    return true;
}

}  // namespace ombra
