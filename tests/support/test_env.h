// Keeps a test run away from the real install: its own folder, config, pipe and instance mutex.
#pragma once

#include <string>

namespace ombra::test {

// Creates %TEMP%\ombra-tests\<timestamp>-<pid>\ and sets OMBRA_HOME to it (paths.h), so config,
// logs, telemetry and the pipe/mutex names all belong to this run, and so does any engine or UI
// the tests spawn. Call once, first thing. Outputs are left in place for inspection.
void InitTestEnvironment();

const std::wstring& TestRoot();
std::wstring TestOutputPath(const std::wstring& relative);

// Production code still printf()s progress on some paths (the save path). Tests redirect the
// process's stdout to NUL around those calls so the report stays readable; set OMBRA_TEST_VERBOSE
// to see it.
class StdoutSilencer {
public:
    StdoutSilencer();
    ~StdoutSilencer();
    StdoutSilencer(const StdoutSilencer&) = delete;
    StdoutSilencer& operator=(const StdoutSilencer&) = delete;

private:
    int saved_fd_ = -1;
};

}  // namespace ombra::test
