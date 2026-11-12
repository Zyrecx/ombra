#include "ombra/crash.h"

#include "ombra/flight_recorder.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/version.h"

#include <windows.h>
#include <dbghelp.h>
#include <objbase.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <vector>

namespace ombra::crash {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Everything the crash path touches, set up by Install(). Statics, not heap: the heap may be what
// broke.
const char*          g_exe = nullptr;
wchar_t              g_dir[MAX_PATH] = {};
wchar_t              g_setup_marker[MAX_PATH] = {};
MiniDumpWriteDumpFn  g_write_dump = nullptr;
HANDLE               g_go = nullptr;           // set by the faulting thread
HANDLE               g_dump_thread = nullptr;
DWORD                g_dump_tid = 0;
volatile LONG        g_claimed = 0;            // the first fault wins; later ones wait

// What the faulting thread hands over.
struct Fault {
    const char*         kind = "";
    char                reason[1024] = {};     // empty for SEH: the dump thread formats it from `ep`
    DWORD               thread_id = 0;
    const char*         thread_name = nullptr;
    EXCEPTION_POINTERS* ep = nullptr;
    unsigned            exit_code = kExitDie;
};
Fault g_fault;

void CopyBounded(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    if (src)
        for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

bool WriteWholeFile(const wchar_t* path, const char* text) {
    const HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD      written = 0;
    const BOOL ok = WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

void DescribeException(const EXCEPTION_RECORD* rec, char* out, size_t cap) {
    const DWORD code = rec->ExceptionCode;
    const char* name = code == EXCEPTION_ACCESS_VIOLATION       ? "access violation"
                       : code == EXCEPTION_STACK_OVERFLOW       ? "stack overflow"
                       : code == EXCEPTION_IN_PAGE_ERROR        ? "in-page error"
                       : code == EXCEPTION_ILLEGAL_INSTRUCTION  ? "illegal instruction"
                       : code == EXCEPTION_INT_DIVIDE_BY_ZERO   ? "integer divide by zero"
                       : code == EXCEPTION_PRIV_INSTRUCTION     ? "privileged instruction"
                       : code == EXCEPTION_BREAKPOINT           ? "breakpoint"
                       : code == 0xE06D7363                     ? "C++ exception"
                                                                : "exception";
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && rec->NumberParameters >= 2) {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        _snprintf_s(out, cap, _TRUNCATE, "%s (0x%08lX) at %p, %s %p", name, code, rec->ExceptionAddress,
                    op == 0 ? "reading" : op == 1 ? "writing" : "executing",
                    reinterpret_cast<void*>(rec->ExceptionInformation[1]));
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%s (0x%08lX) at %p", name, code, rec->ExceptionAddress);
    }
}

// Whether a report of this crash may ever be sent (crash_upload.h). Files, not a flag, so the overlay
// reads what the engine decided, and a setup that finishes after the last config change still counts.
bool SharingNow() {
    static wchar_t marker[MAX_PATH + 16];
    swprintf_s(marker, L"%s\\sharing_on", g_dir);
    return g_setup_marker[0] && GetFileAttributesW(g_setup_marker) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

// The `share:` line: yes, no (the user turned sharing off), or ask (setup isn't finished, so nobody
// has chosen yet; UnansweredCrash).
const char* ShareLine() {
    if (SharingNow()) return "yes";
    return g_setup_marker[0] && GetFileAttributesW(g_setup_marker) == INVALID_FILE_ATTRIBUTES ? "ask" : "no";
}

DWORD WINAPI DumpThread(void*) {
    WaitForSingleObject(g_go, INFINITE);
    const Fault& f = g_fault;

    static char reason[1024];
    if (f.reason[0]) CopyBounded(reason, sizeof(reason), f.reason);
    else if (f.ep && f.ep->ExceptionRecord) DescribeException(f.ep->ExceptionRecord, reason, sizeof(reason));

    static char thread[64];
    if (f.thread_name) _snprintf_s(thread, sizeof(thread), _TRUNCATE, "%s", f.thread_name);
    else _snprintf_s(thread, sizeof(thread), _TRUNCATE, "%lu", f.thread_id);

    static char line[1400];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "FATAL %s: %s", f.kind, reason);
    Log::Emergency(thread, line);
    // The encode side's last 2 s, per pipeline (the engine only; the UI has no recorders).
    static char summary[4096];
    FlightRecorder::ForEachSummary(summary, sizeof(summary), 2.0, [](const char* l) { Log::Emergency("crash", l); });

    SYSTEMTIME st{};
    GetLocalTime(&st);
    static wchar_t base[MAX_PATH + 64];
    static wchar_t name[96];
    swprintf_s(name, L"%hs-%04u%02u%02u-%02u%02u%02u-%lu", g_exe, st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, GetCurrentProcessId());
    swprintf_s(base, L"%s\\%s", g_dir, name);

    static wchar_t path[MAX_PATH + 80];
    static char    text[2048];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s %s (%s)\nkind: %s\nthread: %s\nshare: %s\nreason: %s\n", g_exe,
                AppVersion(), BuildCommit(), f.kind, thread, ShareLine(), reason);
    swprintf_s(path, L"%s.txt", base);
    WriteWholeFile(path, text);

    // Before the dump: MiniDumpWriteDump suspends threads that may hold loader or heap locks and is
    // the step most likely to hang. The next start still learns of the crash, and checks whether
    // the .dmp exists (TakePreviousCrash).
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%ls.dmp\n%s: %s\n", name, f.kind, reason);
    swprintf_s(path, L"%s\\%hs.pending", g_dir, g_exe);
    WriteWholeFile(path, text);

    swprintf_s(path, L"%s.dmp", base);
    bool         dumped = false;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = f.thread_id;
        mei.ExceptionPointers = f.ep;
        mei.ClientPointers    = FALSE;
        // Stacks, thread and module info, and the bytes stack values point at: enough to read the
        // crash, a few hundred KB. Never data segments or full memory (the ring is hundreds of MB).
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                     MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpIgnoreInaccessibleMemory);
        // Each flight recorder whole (name, slots, head) as a user stream, type kFlightRecorderStream.
        static FlightRecorder::RawView     views[8];
        static MINIDUMP_USER_STREAM        streams[8];
        static MINIDUMP_USER_STREAM_INFORMATION user{};
        const size_t nviews = FlightRecorder::RawViews(views, 8);
        for (size_t i = 0; i < nviews; ++i) {
            streams[i].Type       = kFlightRecorderStream;
            streams[i].BufferSize = static_cast<ULONG>(views[i].bytes);
            streams[i].Buffer     = const_cast<void*>(views[i].data);
        }
        user.UserStreamCount = static_cast<ULONG>(nviews);
        user.UserStreamArray = streams;
        dumped = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &mei,
                              nviews ? &user : nullptr, nullptr) != FALSE;
        if (!dumped) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "crash dump failed (error %lu)", GetLastError());
            Log::Emergency("crash", line);
        }
        CloseHandle(file);
        if (!dumped) DeleteFileW(path);
    }
    if (dumped) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "crash dump %ls.dmp", name);
        Log::Emergency("crash", line);
    }

    TerminateProcess(GetCurrentProcess(), f.exit_code);
    return 0;
}

// Runs on the faulting thread: record, wake the dump thread, wait. Keeps its frame small.
[[noreturn]] void Trigger(const char* kind, const char* reason, EXCEPTION_POINTERS* ep, unsigned exit_code) {
    if (GetCurrentThreadId() == g_dump_tid) TerminateProcess(GetCurrentProcess(), exit_code);  // the dump path itself broke
    if (InterlockedCompareExchange(&g_claimed, 1, 0) != 0) {
        // Another thread is already crashing the process; it terminates it.
        WaitForSingleObject(g_dump_thread, 60000);
        TerminateProcess(GetCurrentProcess(), exit_code);
    }
    g_fault.kind        = kind;
    g_fault.thread_id   = GetCurrentThreadId();
    g_fault.thread_name = Log::CurrentThreadName();
    if (!ep) {
        // die() and the abort family have no exception: make one, so the dump has an exception
        // stream on this thread (debuggers' .ecxr, !analyze and Sentry find the crash from it).
        // The context is this frame's; the stack below it is the caller's.
        static CONTEXT            ctx;
        static EXCEPTION_RECORD   rec;
        static EXCEPTION_POINTERS ptrs;
        RtlCaptureContext(&ctx);
        rec.ExceptionCode    = exit_code == kExitDie ? kDieExceptionCode : kAbortExceptionCode;
        rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
        ptrs.ExceptionRecord = &rec;
        ptrs.ContextRecord   = &ctx;
        ep                   = &ptrs;
    }
    g_fault.ep          = ep;
    g_fault.exit_code   = exit_code;
    CopyBounded(g_fault.reason, sizeof(g_fault.reason), reason);
    SetEvent(g_go);
    WaitForSingleObject(g_dump_thread, 60000);
    TerminateProcess(GetCurrentProcess(), exit_code);
    for (;;) Sleep(INFINITE);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
    Trigger("unhandled exception", nullptr, ep, ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : kExitAbort);
}

// What std::current_exception holds on this thread, if anything: an uncaught exception reaches
// terminate (and from there abort) with it still current.
bool DescribeCurrentException(char* out, size_t cap) {
    const std::exception_ptr e = std::current_exception();
    if (!e) return false;
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        _snprintf_s(out, cap, _TRUNCATE, "uncaught exception: %s", ex.what());
    } catch (...) {
        CopyBounded(out, cap, "uncaught exception of a non-std type");
    }
    return true;
}

// Installed for the thread that called Install() only: set_terminate is per thread in the MSVC
// runtime. Every other thread's terminate is the default, which calls abort() -> OnAbortSignal.
void OnTerminate() {
    static char what[512];
    if (!DescribeCurrentException(what, sizeof(what)))
        CopyBounded(what, sizeof(what), "std::terminate without an exception (a noexcept violation or a direct call)");
    Trigger("terminate", what, nullptr, kExitAbort);
}

void OnAbortSignal(int) {
    static char what[512];
    if (DescribeCurrentException(what, sizeof(what))) Trigger("terminate", what, nullptr, kExitAbort);
    Trigger("abort", "abort() called", nullptr, kExitAbort);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    Trigger("invalid parameter", "a CRT function was called with an invalid parameter", nullptr, kExitAbort);
}

void OnPurecall() { Trigger("purecall", "pure virtual function call", nullptr, kExitAbort); }

}  // namespace

std::wstring CrashesDir() {
    const std::wstring& data = Instance().data_dir;
    return data.empty() ? std::wstring() : data + L"\\crashes";
}

bool Install(const char* exe) {
#if defined(__SANITIZE_ADDRESS__)
    (void)exe;
    return false;
#else
    if (g_exe || IsDebuggerPresent()) return false;
    const std::wstring dir = CrashesDir();
    if (dir.empty() || dir.size() >= MAX_PATH - 64) return false;
    CreateDirectoryW(Instance().data_dir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp) {
        Log::Warn("crash handler: dbghelp.dll did not load (error %lu); crashes won't be dumped", GetLastError());
        return false;
    }
    g_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    g_go         = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_write_dump || !g_go) return false;
    // 256 KB reserved: MiniDumpWriteDump and the formatting above need some; the default 1 MB
    // reservation would do too, this only commits what's touched.
    g_dump_thread = CreateThread(nullptr, 256 * 1024, DumpThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_dump_tid);
    if (!g_dump_thread) return false;
    SetThreadDescription(g_dump_thread, L"crash-dump");
    wcscpy_s(g_dir, dir.c_str());
    if (const std::wstring setup = SetupCompletedPath(); setup.size() < MAX_PATH) wcscpy_s(g_setup_marker, setup.c_str());
    g_exe = exe;

    // No dialogs on the desktop from any path: no CRT abort/assert message box, no "has
    // stopped working" box for whatever gets past these handlers (__fastfail).
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);

    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbortSignal);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPurecall);
    return true;
#endif
}

bool Installed() { return g_exe != nullptr; }

void RecheckFilter() {
    if (!Installed()) return;
    const LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(OnUnhandledException);
    if (prev != OnUnhandledException)
        Log::Warn("crash handler: something replaced the unhandled-exception filter (%p) after start; restored",
                  reinterpret_cast<void*>(prev));
}

void Fatal(const char* reason) {
    if (!Installed()) {
        Log::Error("FATAL die: %s", reason);
        std::exit(static_cast<int>(kExitDie));
    }
    Trigger("die", reason, nullptr, kExitDie);
}

std::optional<PreviousCrash> TakePreviousCrash(const char* exe) {
    const std::wstring dir = CrashesDir();
    if (dir.empty()) return std::nullopt;
    const std::wstring path = dir + L"\\" + std::wstring(exe, exe + std::strlen(exe)) + L".pending";
    std::string        text;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::nullopt;
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    DeleteFileW(path.c_str());
    PreviousCrash pc;
    const size_t  nl = text.find('\n');
    const std::string dump = text.substr(0, nl);
    pc.dump_name = std::wstring(dump.begin(), dump.end());  // ASCII: exe name, digits
    // Written before the dump: a hung or failed dump leaves the name without the file.
    if (!pc.dump_name.empty() && GetFileAttributesW((dir + L"\\" + pc.dump_name).c_str()) == INVALID_FILE_ATTRIBUTES)
        pc.dump_name.clear();
    if (nl != std::string::npos) {
        pc.reason = text.substr(nl + 1);
        while (!pc.reason.empty() && (pc.reason.back() == '\n' || pc.reason.back() == '\r')) pc.reason.pop_back();
    }
    return pc;
}

void SetSharingEnabled(bool enabled, bool usage) {
    const std::wstring dir = CrashesDir();
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\sharing_on";
    if (!enabled) {
        DeleteFileW(path.c_str());
        return;
    }
    CreateDirectoryW(Instance().data_dir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    if (usage) WriteFile(f, "usage", 5, &wrote, nullptr);  // crash_upload::UsageAllowed
    CloseHandle(f);
}

void PruneDumps(int keep) {
    const std::wstring dir = CrashesDir();
    if (dir.empty()) return;
    struct Entry { std::wstring base; ULONGLONG written; };
    std::vector<Entry> dumps;
    WIN32_FIND_DATAW   fd{};
    const HANDLE       h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        dumps.push_back({name.substr(0, name.size() - 4),
                         (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(dumps.begin(), dumps.end(), [](const Entry& a, const Entry& b) { return a.written > b.written; });
    for (size_t i = static_cast<size_t>(std::max(keep, 0)); i < dumps.size(); ++i) {
        DeleteFileW((dir + L"\\" + dumps[i].base + L".dmp").c_str());
        DeleteFileW((dir + L"\\" + dumps[i].base + L".txt").c_str());
        DeleteFileW((dir + L"\\" + dumps[i].base + L".sent").c_str());
        DeleteFileW((dir + L"\\" + dumps[i].base + L".asked").c_str());
    }
}

namespace {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Unanswered `share: ask` dumps, newest first.
std::vector<std::wstring> AskDumps() {
    const std::wstring dir = CrashesDir();
    if (dir.empty()) return {};
    std::vector<std::pair<ULONGLONG, std::wstring>> found;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const ULONGLONG now = (static_cast<ULONGLONG>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG written = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (now > written && (now - written) / 10000000ull > kMaxAskAgeDays * 86400ull) continue;
        const std::wstring name = fd.cFileName;
        const std::wstring base = dir + L"\\" + name.substr(0, name.size() - 4);
        if (Exists(base + L".sent") || Exists(base + L".asked")) continue;
        std::ifstream      f(base + L".txt", std::ios::binary);
        std::string        line;
        bool               ask = false;
        while (std::getline(f, line) && line.rfind("reason: ", 0) != 0)  // the reason is free text, last
            if (line == "share: ask") ask = true;
        if (ask) found.push_back({written, name.substr(0, name.size() - 4)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (auto& e : found) out.push_back(std::move(e.second));
    return out;
}

void WriteSmall(const std::wstring& path, const char* text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

}  // namespace

std::wstring UnansweredCrash() {
    const auto dumps = AskDumps();
    return dumps.empty() ? std::wstring() : dumps.front();
}

std::string NewReportId() {
    GUID g{};
    CoCreateGuid(&g);
    unsigned char b[16];
    std::memcpy(b, &g, sizeof(b));
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", b[i]);
    return hex;
}

void AnswerCrash(const std::wstring& base, bool send) {
    const std::wstring dir = CrashesDir();
    if (dir.empty() || base.empty()) return;
    for (const std::wstring& other : AskDumps())
        if (other != base) WriteSmall(dir + L"\\" + other + L".sent", "declined: an older crash, not asked about\n");
    if (send) WriteSmall(dir + L"\\" + base + L".asked", "send\n");
    else WriteSmall(dir + L"\\" + base + L".sent", "declined: Don't send\n");
}

}  // namespace ombra::crash
