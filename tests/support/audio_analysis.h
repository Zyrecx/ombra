// Content checks on decoded PCM: is a given tone present, how loud, where does sound start.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ombra::test {

// Interleaved s16 PCM plus its layout.
struct Pcm {
    std::vector<int16_t> samples;
    uint32_t             channels    = 2;
    uint32_t             sample_rate = 48000;

    size_t Frames() const { return channels ? samples.size() / channels : 0; }
    double Seconds() const { return sample_rate ? double(Frames()) / sample_rate : 0.0; }
};

// Amplitude (0..1 of full scale) of a `freq_hz` sine in channel 0 over [first_frame, first_frame +
// frame_count), by single-bin DFT (Goertzel). A full-scale sine at freq_hz reads ~1.0; a tone at a
// different, well-separated frequency reads near 0.
double ToneAmplitude(const Pcm& pcm, double freq_hz, size_t first_frame, size_t frame_count);

// RMS of channel 0 over the window, 0..1 of full scale.
double Rms(const Pcm& pcm, size_t first_frame, size_t frame_count);

// First frame whose channel-0 magnitude exceeds `threshold` (0..1 of full scale).
std::optional<size_t> FirstFrameAbove(const Pcm& pcm, double threshold, size_t from_frame = 0);

}  // namespace ombra::test
