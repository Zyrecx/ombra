#include "support/audio_analysis.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ombra::test {

namespace {

size_t ClampCount(const Pcm& pcm, size_t first_frame, size_t frame_count) {
    const size_t frames = pcm.Frames();
    if (first_frame >= frames) return 0;
    return (std::min)(frame_count, frames - first_frame);
}

}  // namespace

double ToneAmplitude(const Pcm& pcm, double freq_hz, size_t first_frame, size_t frame_count) {
    const size_t n = ClampCount(pcm, first_frame, frame_count);
    if (n == 0) return 0.0;
    const double w = 2.0 * std::numbers::pi * freq_hz / pcm.sample_rate;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double x = pcm.samples[(first_frame + i) * pcm.channels] / 32768.0;
        re += x * std::cos(w * double(i));
        im -= x * std::sin(w * double(i));
    }
    return 2.0 * std::sqrt(re * re + im * im) / double(n);
}

double Rms(const Pcm& pcm, size_t first_frame, size_t frame_count) {
    const size_t n = ClampCount(pcm, first_frame, frame_count);
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double x = pcm.samples[(first_frame + i) * pcm.channels] / 32768.0;
        sum += x * x;
    }
    return std::sqrt(sum / double(n));
}

std::optional<size_t> FirstFrameAbove(const Pcm& pcm, double threshold, size_t from_frame) {
    const int32_t limit = static_cast<int32_t>(threshold * 32768.0);
    for (size_t f = from_frame; f < pcm.Frames(); ++f) {
        if (std::abs(static_cast<int32_t>(pcm.samples[f * pcm.channels])) > limit) return f;
    }
    return std::nullopt;
}

}  // namespace ombra::test
