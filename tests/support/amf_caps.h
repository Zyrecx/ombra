// The AMF probe's per-adapter caps, read in-process for the gpu suite. The engine reads them in a
// child process (amf_probe.h) because AMF init can hang on a bad driver; a test run on the dev
// machine accepts that risk, as gpu_encode_tests' AmdAdapter() already does.
#pragma once

#include "ombra/encode_target.h"

#include <cstdint>
#include <vector>

namespace ombra::test {

// The AV1 size alignment of `adapter`'s AMF encoder. False when AMF or its AV1 encoder is missing.
bool AmfAv1AlignmentInProcess(const EncodeAdapter& adapter, uint32_t& align_w, uint32_t& align_h);

// SetAmfAdapterCaps for every AMD adapter, AV1 alignment included, as the engine's startup probe
// would. Undo with SetAmfAdapterCaps({}).
void SeedAmfCapsInProcess();

// H.264-capable caps for every hardware AMD adapter, without touching AMF: what a passed probe
// reports, for tests that only need AMF picked.
std::vector<AmfAdapterCaps> AllAmdH264Caps();

}  // namespace ombra::test
