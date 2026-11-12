// Failure handling shared by all ombra modules: die loudly, say exactly which call failed.
// die() is the engine's fatal path (crash.h): one log line, a minidump, the process ends. Only for
// what the engine can't continue past; an expected stop (another instance owns the lock) is not a
// crash and doesn't come here.
#pragma once

#include "ombra/crash.h"

#include <windows.h>

#include <cstdio>

namespace ombra {

[[noreturn]] inline void die(const char* what) {
    std::printf("\n  FAIL  %s\n", what);
    crash::Fatal(what);
}

[[noreturn]] inline void die_hr(const char* what, HRESULT hr) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s  (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    die(buf);
}

}  // namespace ombra

#define OMBRA_HR(expr)                                                \
    do {                                                              \
        HRESULT hr_ = (expr);                                        \
        if (FAILED(hr_)) ::ombra::die_hr(#expr, hr_);                 \
    } while (0)
