#include "ombra/encoder.h"

#include "ombra/amf_encoder.h"
#include "ombra/nvenc.h"
#include "ombra/x264_encoder.h"

#include <atomic>

namespace ombra {

namespace {
std::atomic<bool> g_av1_unsupported_for_test{false};
}  // namespace

void SetAv1UnsupportedForTest(bool unsupported) { g_av1_unsupported_for_test.store(unsupported); }
bool Av1UnsupportedForTest() { return g_av1_unsupported_for_test.load(); }

std::unique_ptr<IVideoEncoder> CreateVideoEncoder(EncoderBackend backend) {
    switch (backend) {
        case EncoderBackend::Nvenc: return CreateNvencEncoder();
        case EncoderBackend::Amf: return std::make_unique<AmfEncoder>();
        case EncoderBackend::X264: return std::make_unique<X264Encoder>();
    }
    return CreateNvencEncoder();
}

}  // namespace ombra
