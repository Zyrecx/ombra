// Runs a child process to completion with stdout/stderr captured, for driving ffmpeg/ffprobe.
#pragma once

#include <windows.h>

#include <string>

namespace ombra::test {

struct ProcessResult {
    bool        launched  = false;
    bool        timed_out = false;
    DWORD       exit_code = ~0ul;
    std::string out;
    std::string err;
};

// `command_line` is the full command line, program first (quote it yourself if it has spaces).
// A timed-out child is terminated.
ProcessResult RunProcess(const std::wstring& command_line, DWORD timeout_ms = 120000);

std::wstring Quote(const std::wstring& arg);

}  // namespace ombra::test
