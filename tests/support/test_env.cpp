#include "support/test_env.h"

#include "ombra/util.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>

namespace ombra::test {

namespace {

std::wstring g_root;

}  // namespace

void InitTestEnvironment() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"ombra-tests\\%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    g_root = std::wstring(temp) + name;

    // Before anything calls Instance(): it reads OMBRA_HOME once per process.
    EnsureParentDir(g_root + L"\\x");
    SetEnvironmentVariableW(L"OMBRA_HOME", g_root.c_str());

    std::printf("ombra-tests output: %ls\n", g_root.c_str());
}

const std::wstring& TestRoot() { return g_root; }

std::wstring TestOutputPath(const std::wstring& relative) { return g_root + L"\\" + relative; }

StdoutSilencer::StdoutSilencer() {
    if (GetEnvironmentVariableW(L"OMBRA_TEST_VERBOSE", nullptr, 0) > 0) return;
    std::fflush(stdout);
    int nul = -1;
    if (_sopen_s(&nul, "NUL", _O_WRONLY, _SH_DENYNO, 0) != 0) return;
    saved_fd_ = _dup(_fileno(stdout));
    _dup2(nul, _fileno(stdout));
    _close(nul);
}

StdoutSilencer::~StdoutSilencer() {
    if (saved_fd_ < 0) return;
    std::fflush(stdout);
    _dup2(saved_fd_, _fileno(stdout));
    _close(saved_fd_);
}

}  // namespace ombra::test
