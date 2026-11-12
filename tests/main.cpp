// ombra-tests.exe: behavior tests against the engine's own code (ombra_core), checked from the
// outside with ffprobe/ffmpeg. Any doctest flag works, e.g. `ombra-tests -tc="save:*"`.
//
// Tests in the "gpu" suite open real NVENC sessions and are skipped unless `--gpu` is passed or
// OMBRA_TEST_GPU is set. The "audio-hw" suite opens the real audio
// device (silent, no GPU) and runs only with `--audio` or OMBRA_TEST_AUDIO.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "support/test_env.h"

#include "ombra/log.h"
#include "ombra/mux.h"
#include "ombra/util.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace ombra::test {
int RunCrashChild(const char* kind);  // crash_tests.cpp
}

int main(int argc, char** argv) {
    // crash_tests.cpp runs this binary as the crashing process, in the OMBRA_HOME it set.
    if (argc > 2 && std::strcmp(argv[1], "--crash-child") == 0) return ombra::test::RunCrashChild(argv[2]);
    ombra::test::InitTestEnvironment();

    bool                gpu   = GetEnvironmentVariableW(L"OMBRA_TEST_GPU", nullptr, 0) > 0;
    bool                audio = GetEnvironmentVariableW(L"OMBRA_TEST_AUDIO", nullptr, 0) > 0;
    std::vector<char*>  args;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") == 0) gpu = true;
        else if (std::strcmp(argv[i], "--audio") == 0) audio = true;
        else args.push_back(argv[i]);
        // doctest's failures-only mode: engine log lines go only to the output folder's logs.
        if (std::strcmp(argv[i], "--minimal") == 0 || std::strcmp(argv[i], "-m") == 0) ombra::Log::SetEcho(false);
    }

    doctest::Context context;
    if (!gpu) context.addFilter("test-suite-exclude", "gpu");
    if (!audio) context.addFilter("test-suite-exclude", "audio-hw");
    context.applyCommandLine(static_cast<int>(args.size()), args.data());
    return context.run();
}
