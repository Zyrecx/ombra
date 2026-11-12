// Converts one WASAPI packet, in whatever format GetMixFormat negotiated, to interleaved s16 stereo
// at 48 kHz, appending onto `out`. Handles integer PCM (16, packed 24, 32 bits) and IEEE float (32,
// 64 bits), each with or without WAVEFORMATEXTENSIBLE. Any channel count goes to stereo (averaged
// down, or duplicated from mono). Rates other than 48 kHz resample with linear interpolation.
//
// Configure returns false on a format it can't read. That fails one audio source and never the
// engine: the mix format comes from the user's driver, so no value of it may die().
#pragma once

#include "ombra/audio.h"

#include <windows.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ombra {

class FormatConverter {
public:
    [[nodiscard]] bool Configure(const WAVEFORMATEX* fmt) {
        channels_    = fmt->nChannels;
        sample_rate_ = fmt->nSamplesPerSec;
        bits_        = fmt->wBitsPerSample;

        is_float_ = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        bool known_tag = is_float_ || fmt->wFormatTag == WAVE_FORMAT_PCM;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
            // KSDATAFORMAT_SUBTYPE_* GUIDs all share the same Data2/Data3/Data4
            // (0000-0010-8000-00AA00389B71) and differ only in Data1, so comparing Data1 alone is
            // enough without pulling in ksmedia.h for the full GUID constants.
            is_float_ = ext->SubFormat.Data1 == 0x00000003;  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            known_tag = is_float_ || ext->SubFormat.Data1 == 0x00000001;  // KSDATAFORMAT_SUBTYPE_PCM
        }
        if (!known_tag || channels_ == 0 || sample_rate_ == 0) return false;
        const bool known_depth = is_float_ ? (bits_ == 32 || bits_ == 64)
                                           : (bits_ == 16 || bits_ == 24 || bits_ == 32);
        if (!known_depth) return false;

        // Input frames per output frame: >1 when downsampling (96 kHz -> 48 kHz gives 2.0), <1 when
        // upsampling (44.1 kHz -> 48 kHz gives ~0.91875).
        step_           = static_cast<double>(sample_rate_) / AudioCapture::kSampleRateHz;
        needs_resample_ = (sample_rate_ != AudioCapture::kSampleRateHz);
        frac_           = 0.0;
        prev_frame_.assign(AudioCapture::kChannels, 0);
        return true;
    }

    uint32_t NegotiatedSampleRate() const { return sample_rate_; }
    uint32_t NegotiatedChannels()   const { return channels_; }
    uint32_t NegotiatedBits()       const { return bits_; }
    bool     NegotiatedIsFloat()    const { return is_float_; }
    bool     IsResampling()         const { return needs_resample_; }

    // `frame_count` frames of `channels_` channels at `bits_`-per-sample, `data` may be nullptr
    // when the AUDCLNT_BUFFERFLAGS_SILENT flag is set (buffer contents are undefined in that case
    // per WASAPI docs; the caller passes nullptr rather than risk reading garbage).
    void Convert(const uint8_t* data, uint32_t frame_count, std::vector<int16_t>& out) {
        for (uint32_t i = 0; i < frame_count; ++i) {
            int16_t stereo[2];
            ReadStereoFrame(data, i, stereo);

            if (!needs_resample_) {
                out.push_back(stereo[0]);
                out.push_back(stereo[1]);
            } else {
                // True linear interpolation between prev_frame_ and stereo, at every output
                // instant that falls inside this input interval. frac_ is the position of the
                // *next* output sample within [prev_frame_, stereo), in units where 1.0 == this
                // whole interval; each output sample consumed advances it by step_. When step_ > 1
                // (downsampling) an interval can produce zero outputs, which is correct: dropping
                // some input intervals entirely is what downsampling is.
                while (frac_ < 1.0) {
                    const double w = frac_;  // interpolation weight toward `stereo`
                    out.push_back(static_cast<int16_t>(std::lround(
                        prev_frame_[0] * (1.0 - w) + stereo[0] * w)));
                    out.push_back(static_cast<int16_t>(std::lround(
                        prev_frame_[1] * (1.0 - w) + stereo[1] * w)));
                    frac_ += step_;
                }
                frac_ -= 1.0;
            }
            prev_frame_[0] = stereo[0];
            prev_frame_[1] = stereo[1];
        }
    }

private:
    float ReadSample(const uint8_t* p) const {
        if (is_float_) {
            if (bits_ == 64) {
                double v;
                std::memcpy(&v, p, sizeof(v));
                return static_cast<float>(v);
            }
            float v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        if (bits_ == 16) {
            int16_t v;
            std::memcpy(&v, p, sizeof(v));
            return v / 32768.0f;
        }
        if (bits_ == 24) {
            // Little-endian packed: shift into the top of an int32 so the sign comes along.
            const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) |
                                                   (static_cast<uint32_t>(p[1]) << 16) |
                                                   (static_cast<uint32_t>(p[2]) << 24));
            return v / 2147483648.0f;
        }
        int32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v / 2147483648.0f;
    }

    void ReadStereoFrame(const uint8_t* data, uint32_t frame_index, int16_t out[2]) const {
        if (!data) { out[0] = 0; out[1] = 0; return; }  // AUDCLNT_BUFFERFLAGS_SILENT

        const size_t bytes = bits_ / 8;
        const uint8_t* frame = data + static_cast<size_t>(frame_index) * channels_ * bytes;
        float ch[8] = {};  // realistic upper bound on WASAPI's negotiated channel count
        const uint32_t n = (std::min)(channels_, uint32_t{8});
        for (uint32_t c = 0; c < n; ++c) ch[c] = ReadSample(frame + c * bytes);

        float l, r;
        if (channels_ == 1) {
            l = r = ch[0];
        } else if (channels_ == 2) {
            l = ch[0]; r = ch[1];
        } else {
            // Downmix anything wider than stereo (5.1, 7.1, ...) by averaging every channel into
            // both outputs: not spec-correct surround downmixing, but correct enough for a replay
            // buffer's audio track.
            float sum = 0;
            for (uint32_t c = 0; c < n; ++c) sum += ch[c];
            l = r = sum / n;
        }

        auto to_s16 = [](float v) {
            v = std::clamp(v, -1.0f, 1.0f);
            return static_cast<int16_t>(std::lround(v * 32767.0f));
        };
        out[0] = to_s16(l);
        out[1] = to_s16(r);
    }

    uint32_t channels_    = 0;
    uint32_t sample_rate_ = 0;
    uint32_t bits_        = 0;
    bool     is_float_    = false;

    double step_           = 1.0;
    double frac_           = 0.0;
    bool   needs_resample_ = false;
    std::vector<int16_t> prev_frame_;
};

}  // namespace ombra
