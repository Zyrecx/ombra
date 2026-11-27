// ombra-tests.exe: behavior tests against the engine's own code (ombra_core), checked from the
// outside with ffprobe/ffmpeg. Any doctest flag works, e.g. `ombra-tests -tc="save:*"`.
//
// Tests in the "gpu" suite open real NVENC sessions and are skipped unless `--gpu` is passed or
// OMBRA_TEST_GPU is set. The "audio-hw" suite opens the real audio
// device (silent, no GPU) and runs only with `--audio` or OMBRA_TEST_AUDIO. The "large" suite
// writes files past 4 GB (about 9 GB in all, deleted after) and runs only with `--large` or
// OMBRA_TEST_LARGE. A variable set to "" or "0" counts as unset.
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

// An empty value or "0" counts as unset: shells differ on whether `VAR= cmd` passes an empty
// variable, and a suite that records the user's audio must not start by accident.
static bool EnvOn(const wchar_t* name) {
    wchar_t value[8] = {};
    DWORD   n        = GetEnvironmentVariableW(name, value, 8);
    if (n == 0) return false;
    return n >= 8 || std::wcscmp(value, L"0") != 0;
}

int main(int argc, char** argv) {
    // crash_tests.cpp runs this binary as the crashing process, in the OMBRA_HOME it set.
    if (argc > 2 && std::strcmp(argv[1], "--crash-child") == 0) return ombra::test::RunCrashChild(argv[2]);
    ombra::test::InitTestEnvironment();

    bool                gpu   = EnvOn(L"OMBRA_TEST_GPU");
    bool                audio = EnvOn(L"OMBRA_TEST_AUDIO");
    bool                large = EnvOn(L"OMBRA_TEST_LARGE");
    std::vector<char*>  args;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") == 0) gpu = true;
        else if (std::strcmp(argv[i], "--audio") == 0) audio = true;
        else if (std::strcmp(argv[i], "--large") == 0) large = true;
        else args.push_back(argv[i]);
        // doctest's failures-only mode: engine log lines go only to the output folder's logs.
        if (std::strcmp(argv[i], "--minimal") == 0 || std::strcmp(argv[i], "-m") == 0) ombra::Log::SetEcho(false);
    }

    doctest::Context context;
    if (!gpu) context.addFilter("test-suite-exclude", "gpu");
    if (!audio) context.addFilter("test-suite-exclude", "audio-hw");
    if (!large) context.addFilter("test-suite-exclude", "large");
    context.applyCommandLine(static_cast<int>(args.size()), args.data());
    return context.run();
}
