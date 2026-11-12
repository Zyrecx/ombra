#include "support/process.h"

#include <thread>
#include <vector>

namespace ombra::test {

namespace {

// Drains one pipe on its own thread so a child filling the other pipe can't deadlock us.
void Drain(HANDLE pipe, std::string& sink) {
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) sink.append(buf, n);
}

}  // namespace

std::wstring Quote(const std::wstring& arg) {
    return L"\"" + arg + L"\"";
}

ProcessResult RunProcess(const std::wstring& command_line, DWORD timeout_ms) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) return result;
    if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return result;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nullptr;
    si.hStdOutput = out_write;
    si.hStdError  = err_write;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
    cmd.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (!ok) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        return result;
    }
    result.launched = true;

    std::thread out_thread(Drain, out_read, std::ref(result.out));
    std::thread err_thread(Drain, err_read, std::ref(result.err));

    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    out_thread.join();
    err_thread.join();

    CloseHandle(out_read);
    CloseHandle(err_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
}

}  // namespace ombra::test
