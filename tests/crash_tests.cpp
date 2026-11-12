// The fatal path (crash.h): every way the process can die ends in one FATAL log line, one readable
// minidump and the expected exit code, without a dialog. Each case runs `ombra-tests --crash-child
// <kind>` in its own OMBRA_HOME, since a crash ends the process that has it.

#include <doctest/doctest.h>

#include "support/process.h"
#include "support/test_env.h"

#include "ombra/crash.h"
#include "ombra/crash_upload.h"
#include "ombra/flight_recorder.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/util.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

// ---- the child side, called from tests/main.cpp before doctest ---------------------------------

namespace {

#pragma optimize("", off)
int Recurse(int depth) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(depth);
    return Recurse(depth + 1) + pad[0];
}
#pragma optimize("", on)

struct PureBase {
    PureBase() { CallPure(); }
    void         CallPure() { Pure(); }
    virtual void Pure() = 0;
    virtual ~PureBase() = default;
};
struct PureDerived : PureBase {
    void Pure() override {}
};

__declspec(noinline) void AccessViolation() {
    volatile int* p = nullptr;
    *p              = 1;
}

}  // namespace

int RunCrashChild(const char* kind) {
    Log::SetThreadName("main");
    Log::Init();
    if (!crash::Install("ombra")) return 90;  // under ASan or a debugger: the parent skips
    const std::string k = kind;
    if (k == "die") crash::Fatal("test die reason");
    if (k == "die-sharing" || k == "die-sharing-no-setup" || k == "die-setup") {
        crash::SetSharingEnabled(k != "die-setup");
        if (k != "die-sharing-no-setup") {
            CreateDirectoryW(Instance().config_dir.c_str(), nullptr);
            std::ofstream(SetupCompletedPath(), std::ios::binary);
        }
        crash::Fatal("test die reason");
    }
    if (k == "flight") {
        static FlightRecorder rec("testmon");
        for (int i = 0; i < 10; ++i) {
            FlightRecord r;
            r.qpc       = i * 1000;
            r.encode_us = 7000;
            rec.Push(r);
        }
        crash::Fatal("with a recorder");
    }
    if (k == "access-violation") AccessViolation();
    if (k == "abort") std::abort();
    if (k == "purecall") {
        PureDerived d;
    }
    if (k == "invalid-parameter") {
        char buf[2];
        strcpy_s(buf, sizeof(buf), "too long for the buffer");
    }
    if (k == "stack-overflow") return Recurse(0);
    if (k == "thread-throw") {
        std::thread t([] {
            Log::SetThreadName("thrower");
            throw std::runtime_error("worker exploded");
        });
        t.join();
    }
    if (k == "two-threads") {
        HANDLE           go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::vector<std::thread> threads;
        for (int i = 0; i < 2; ++i)
            threads.emplace_back([go] {
                WaitForSingleObject(go, INFINITE);
                AccessViolation();
            });
        SetEvent(go);
        for (auto& t : threads) t.join();
    }
    return 91;  // the crash didn't happen
}

// ---- the parent side ---------------------------------------------------------------------------

namespace {

std::string ReadAll(const std::wstring& path) {
    std::ifstream      f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::wstring> FilesIn(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW          fd{};
    const HANDLE              h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(dir + L"\\" + fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

struct DumpFacts {
    uint64_t address   = 0;  // the exception address
    bool     user_stream = false;  // a flight recorder stream
    bool     readable  = false;
    bool     exception = false;
    DWORD    code      = 0;
    ULONG    threads   = 0;
    uint64_t bytes     = 0;
};

DumpFacts ReadDump(const std::wstring& path) {
    DumpFacts    f;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return f;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    f.bytes          = static_cast<uint64_t>(size.QuadPart);
    const HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void*        view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (view) {
        void*  stream = nullptr;
        ULONG  stream_size = 0;
        if (MiniDumpReadDumpStream(view, ThreadListStream, nullptr, &stream, &stream_size) && stream) {
            f.readable = true;
            f.threads  = static_cast<const MINIDUMP_THREAD_LIST*>(stream)->NumberOfThreads;
        }
        if (MiniDumpReadDumpStream(view, ExceptionStream, nullptr, &stream, &stream_size) && stream) {
            f.exception = true;
            f.code      = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream)->ExceptionRecord.ExceptionCode;
            f.address   = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream)->ExceptionRecord.ExceptionAddress;
        }
        if (MiniDumpReadDumpStream(view, crash::kFlightRecorderStream, nullptr, &stream, &stream_size) && stream)
            f.user_stream = stream_size == sizeof(FlightRecorder) &&
                            std::memcmp(stream, "testmon", 8) == 0;  // name_ is the first member
        UnmapViewOfFile(view);
    }
    if (map) CloseHandle(map);
    CloseHandle(file);
    return f;
}

// The load address the dump recorded for `module_path` (matched by file name), 0 if absent.
uint64_t DumpModuleBase(const std::wstring& dump, const std::wstring& module_path) {
    const std::wstring want = module_path.substr(module_path.find_last_of(L"\\/") + 1);
    uint64_t     base = 0;
    const HANDLE file = CreateFileW(dump.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const HANDLE map  = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const auto*  view = static_cast<const uint8_t*>(map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr);
    void*        stream = nullptr;
    ULONG        size   = 0;
    if (view && MiniDumpReadDumpStream(const_cast<uint8_t*>(view), ModuleListStream, nullptr, &stream, &size) && stream) {
        const auto* list = static_cast<const MINIDUMP_MODULE_LIST*>(stream);
        for (ULONG i = 0; i < list->NumberOfModules && !base; ++i) {
            const MINIDUMP_MODULE& m    = list->Modules[i];
            const auto*            name = reinterpret_cast<const MINIDUMP_STRING*>(view + m.ModuleNameRva);
            const std::wstring     path(name->Buffer, name->Length / sizeof(wchar_t));
            if (_wcsicmp(path.substr(path.find_last_of(L"\\/") + 1).c_str(), want.c_str()) == 0) base = m.BaseOfImage;
        }
    }
    if (view) UnmapViewOfFile(view);
    if (map) CloseHandle(map);
    CloseHandle(file);
    return base;
}

struct ChildRun {
    bool        skipped = false;
    DWORD       exit_code = 0;
    std::string log;
    std::vector<std::wstring> dumps;
    std::vector<std::wstring> texts;
    std::string pending;
};

ChildRun RunChild(const char* kind) {
    static std::atomic<int> n{0};
    const std::wstring home = TestOutputPath(L"crash-" + WideFromUtf8(kind) + L"-" + std::to_wstring(n++));
    EnsureParentDir(home + L"\\x");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    wchar_t saved[MAX_PATH] = {};
    GetEnvironmentVariableW(L"OMBRA_HOME", saved, MAX_PATH);
    SetEnvironmentVariableW(L"OMBRA_HOME", home.c_str());
    const ProcessResult r = RunProcess(Quote(exe) + L" --crash-child " + WideFromUtf8(kind), 60000);
    SetEnvironmentVariableW(L"OMBRA_HOME", saved);

    ChildRun run;
    REQUIRE(r.launched);
    REQUIRE_FALSE(r.timed_out);
    run.exit_code = r.exit_code;
    run.skipped   = r.exit_code == 90;
    const InstancePaths paths = ResolveInstancePaths(home);
    for (const auto& f : FilesIn(paths.data_dir + L"\\logs", L"*.log")) run.log += ReadAll(f);
    const std::wstring crashes = paths.data_dir + L"\\crashes";
    run.dumps   = FilesIn(crashes, L"*.dmp");
    run.texts   = FilesIn(crashes, L"*.txt");
    run.pending = ReadAll(crashes + L"\\ombra.pending");
    return run;
}

void CheckCrash(const ChildRun& run, const char* fatal_text, DWORD exit_code, bool seh, const char* thread = "main") {
    CHECK(run.exit_code == exit_code);
    INFO(run.log);
    CHECK(run.log.find(fatal_text) != std::string::npos);
    CHECK(run.log.find(std::string("[E ")) != std::string::npos);
    if (*thread) CHECK(run.log.find(std::string(" ") + thread + "] FATAL") != std::string::npos);
    CHECK(run.log.find("crash dump ombra-") != std::string::npos);
    REQUIRE(run.dumps.size() == 1);
    CHECK(run.texts.size() == 1);
    const DumpFacts d = ReadDump(run.dumps[0]);
    CHECK(d.readable);
    CHECK(d.threads >= 2);  // the faulting thread and the dump thread at least
    CHECK(d.bytes < 16ull * 1024 * 1024);
    // Every kind has an exception stream on the faulting thread; die and the abort family carry
    // crash.h's own codes.
    CHECK(d.exception);
    CHECK(d.code == (seh ? exit_code : exit_code == crash::kExitDie ? crash::kDieExceptionCode
                                                                     : crash::kAbortExceptionCode));
    CHECK(run.pending.find(".dmp\n") != std::string::npos);
}

}  // namespace

TEST_CASE("crash: the report says whether sharing was on when it happened") {
    const auto text_of = [](const ChildRun& run) { return run.texts.empty() ? std::string() : ReadAll(run.texts[0]); };
    const ChildRun off = RunChild("die");
    if (off.skipped) return;
    // Before setup finished: never sent on its own, asked about instead (crash.h UnansweredCrash).
    CHECK(text_of(off).find("\nshare: ask\n") != std::string::npos);
    CHECK(text_of(RunChild("die-sharing-no-setup")).find("\nshare: ask\n") != std::string::npos);
    CHECK(text_of(RunChild("die-setup")).find("\nshare: no\n") != std::string::npos);
    const std::string on = text_of(RunChild("die-sharing"));
    CHECK(on.find("\nshare: yes\n") != std::string::npos);
    const auto parsed = crash_upload::ParseCrashText(on);  // the uploader reads what the crash path writes
    REQUIRE(parsed.has_value());
    CHECK(parsed->share);
    CHECK(parsed->kind == "die");
    CHECK(parsed->reason == "test die reason");
}

TEST_CASE("crash: die() logs, dumps and exits 1") {
    const ChildRun run = RunChild("die");
    if (run.skipped) return;
    CheckCrash(run, "FATAL die: test die reason", crash::kExitDie, false);
    CHECK(run.pending.find("die: test die reason") != std::string::npos);
}

TEST_CASE("crash: the flight recorders go into the log and the dump") {
    const ChildRun run = RunChild("flight");
    if (run.skipped) return;
    CheckCrash(run, "FATAL die: with a recorder", crash::kExitDie, false);
    CHECK(run.log.find("flight recorder testmon, last") != std::string::npos);
    REQUIRE(run.dumps.size() == 1);
    CHECK(ReadDump(run.dumps[0]).user_stream);
}

TEST_CASE("crash: an access violation is dumped with its exception") {
    const ChildRun run = RunChild("access-violation");
    if (run.skipped) return;
    CheckCrash(run, "FATAL unhandled exception: access violation (0xC0000005)", EXCEPTION_ACCESS_VIOLATION, true);
    CHECK(run.log.find("writing 0000000000000000") != std::string::npos);

    // The Release build's PDB names the faulting function: the exe's base from the dump's module
    // list, then dbghelp against ombra-tests.pdb next to the exe.
    REQUIRE(run.dumps.size() == 1);
    const DumpFacts d = ReadDump(run.dumps[0]);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const uint64_t base = DumpModuleBase(run.dumps[0], exe);
    REQUIRE(base != 0);
    const HANDLE       sym = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5EED));
    const std::wstring dir(exe, std::wstring(exe).find_last_of(L'\\'));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    REQUIRE(SymInitializeW(sym, dir.c_str(), FALSE));
    REQUIRE(SymLoadModuleExW(sym, nullptr, exe, nullptr, base, 0, nullptr, 0) != 0);
    alignas(SYMBOL_INFOW) unsigned char buf[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
    auto* info         = reinterpret_cast<SYMBOL_INFOW*>(buf);
    info->SizeOfStruct = sizeof(SYMBOL_INFOW);
    info->MaxNameLen   = 255;
    DWORD64 disp       = 0;
    const BOOL found = SymFromAddrW(sym, d.address, &disp, info);
    INFO("SymFromAddrW error " << GetLastError() << " address " << d.address << " base " << base);
    CHECK(found);
    CHECK(std::wstring(info->Name).find(L"AccessViolation") != std::wstring::npos);
    SymCleanup(sym);
}

TEST_CASE("crash: a stack overflow is dumped from the dump thread") {
    const ChildRun run = RunChild("stack-overflow");
    if (run.skipped) return;
    CheckCrash(run, "FATAL unhandled exception: stack overflow", EXCEPTION_STACK_OVERFLOW, true);
}

TEST_CASE("crash: abort, purecall and invalid parameter end on the fatal path without a dialog") {
    // A dialog would block the child until RunChild's 60 s timeout: REQUIRE_FALSE(timed_out).
    const ChildRun a = RunChild("abort");
    if (a.skipped) return;
    CheckCrash(a, "FATAL abort: abort() called", crash::kExitAbort, false);
    CheckCrash(RunChild("purecall"), "FATAL purecall", crash::kExitAbort, false);
    CheckCrash(RunChild("invalid-parameter"), "FATAL invalid parameter", crash::kExitAbort, false);
}

TEST_CASE("crash: an exception escaping a std::thread names it and that thread") {
    const ChildRun run = RunChild("thread-throw");
    if (run.skipped) return;
    CheckCrash(run, "FATAL terminate: uncaught exception: worker exploded", crash::kExitAbort, false, "thrower");
}

TEST_CASE("crash: two threads crashing at once leave one report") {
    const ChildRun run = RunChild("two-threads");
    if (run.skipped) return;
    CheckCrash(run, "FATAL unhandled exception: access violation", EXCEPTION_ACCESS_VIOLATION, true, "");
    size_t fatal = 0;
    for (size_t at = run.log.find("FATAL"); at != std::string::npos; at = run.log.find("FATAL", at + 1)) ++fatal;
    CHECK(fatal == 1);
}

TEST_CASE("crash: TakePreviousCrash reports once; PruneDumps keeps the newest") {
    const std::wstring dir = crash::CrashesDir();
    REQUIRE_FALSE(dir.empty());
    EnsureParentDir(dir + L"\\x");
    {
        std::ofstream(dir + L"\\ombra.pending", std::ios::binary) << "ombra-20261106-120000-42.dmp\ndie: boom\n";
        std::ofstream(dir + L"\\ombra-20261106-120000-42.dmp") << "x";
    }
    CHECK_FALSE(crash::TakePreviousCrash("ombra-ui").has_value());
    const auto prev = crash::TakePreviousCrash("ombra");
    REQUIRE(prev.has_value());
    CHECK(prev->dump_name == L"ombra-20261106-120000-42.dmp");
    CHECK(prev->reason == "die: boom");
    CHECK_FALSE(crash::TakePreviousCrash("ombra").has_value());
    DeleteFileW((dir + L"\\ombra-20261106-120000-42.dmp").c_str());
    // A dump that never got written (it hung or failed) is reported without a name.
    std::ofstream(dir + L"\\ombra.pending", std::ios::binary) << "ombra-20261106-130000-43.dmp\nabort: x\n";
    const auto no_dump = crash::TakePreviousCrash("ombra");
    REQUIRE(no_dump.has_value());
    CHECK(no_dump->dump_name.empty());

    for (int i = 0; i < 7; ++i) {
        const std::wstring base = dir + L"\\ombra-2026110" + std::to_wstring(i) + L"-000000-1";
        std::ofstream(base + L".dmp") << "x";
        std::ofstream(base + L".txt") << "x";
        const HANDLE h = CreateFileW((base + L".dmp").c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME ft{};
        ft.dwHighDateTime = 0x01D00000u + static_cast<DWORD>(i);  // older to newer
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
    }
    crash::PruneDumps(5);
    const auto left = FilesIn(dir, L"*.dmp");
    CHECK(left.size() == 5);
    CHECK(FilesIn(dir, L"*.txt").size() == 5);
    for (const auto& f : left) {
        CHECK(f.find(L"ombra-20261100-") == std::wstring::npos);
        CHECK(f.find(L"ombra-20261101-") == std::wstring::npos);
    }
}

}  // namespace ombra::test
