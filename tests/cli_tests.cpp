// The command line (cli.h) as scripts see it: ombra.com and ombra.exe from this build's folder,
// run as children in their own OMBRA_HOME, where no engine runs. Exit codes are its contract.

#include <doctest/doctest.h>
#include "support/process.h"
#include "support/test_env.h"
#include "ombra/brand.h"
#include "ombra/json.h"
#include "ombra/util.h"

#include <windows.h>

#include <string>

namespace ombra::test {
namespace {

std::wstring BuildDir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring path(exe, n);
    return path.substr(0, path.find_last_of(L'\\') + 1);
}

// `binary` (OMBRA_CLI_EXE or OMBRA_ENGINE_EXE) with `args`, in a home of its own.
ProcessResult RunCli(const wchar_t* binary, const std::wstring& args) {
    const std::wstring home = TestOutputPath(L"cli-home");
    EnsureParentDir(home + L"\\x");
    wchar_t saved[MAX_PATH] = {};
    GetEnvironmentVariableW(L"OMBRA_HOME", saved, MAX_PATH);
    SetEnvironmentVariableW(L"OMBRA_HOME", home.c_str());
    const ProcessResult r = RunProcess(Quote(BuildDir() + binary) + L" " + args, 30000);
    SetEnvironmentVariableW(L"OMBRA_HOME", saved);
    REQUIRE(r.launched);
    REQUIRE_FALSE(r.timed_out);
    return r;
}

bool Built(const wchar_t* binary) {
    return GetFileAttributesW((BuildDir() + binary).c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

TEST_CASE("cli: usage errors exit 2 on stderr, help exits 0 on stdout") {
    if (!Built(L"" OMBRA_CLI_EXE)) return;
    const wchar_t* com = L"" OMBRA_CLI_EXE;
    for (const wchar_t* args : {L"", L"frob", L"clip --nope", L"clip --before", L"clip --before -1",
                                L"clip --after x", L"clip extra", L"marker a b", L"marker x --at 3",
                                L"replay", L"replay maybe", L"status --json=1", L"start --json"}) {
        const std::string shown = NarrowUtf8(args);
        CAPTURE(shown);
        const ProcessResult r = RunCli(com, args);
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK_FALSE(r.err.empty());
    }
    const ProcessResult help = RunCli(com, L"help");
    CHECK(help.exit_code == 0);
    CHECK(help.out.find("Exit codes") != std::string::npos);
    CHECK(help.err.empty());
}

TEST_CASE("cli: no engine exits 3, --json answers in the engine's error shape") {
    if (!Built(L"" OMBRA_CLI_EXE)) return;
    for (const wchar_t* args : {L"status", L"clip", L"marker x --at -2", L"replay off", L"clip --as \"My App\""}) {
        const ProcessResult r = RunCli(L"" OMBRA_CLI_EXE, args);
        CHECK(r.exit_code == 3);
        CHECK(r.out.empty());
    }
    const ProcessResult r = RunCli(L"" OMBRA_CLI_EXE, L"status --json");
    CHECK(r.exit_code == 3);
    JsonValue v;
    REQUIRE_NOTHROW(v = ParseJson(r.out));
    const JsonValue* error = v.Find("error");
    CHECK((error && error->type == JsonValue::Type::String && error->str == "not_running"));
    const JsonValue* ok = v.Find("ok");
    CHECK((ok && ok->type == JsonValue::Type::Bool && !ok->b));
    CHECK_FALSE(v.Find("id"));
}

// ombra.exe answers the same commands (a script calling it by full path), without starting an engine.
TEST_CASE("cli: ombra.exe runs the command line too, never an engine") {
    if (!Built(L"" OMBRA_ENGINE_EXE)) return;
    CHECK(RunCli(L"" OMBRA_ENGINE_EXE, L"status").exit_code == 3);
    CHECK(RunCli(L"" OMBRA_ENGINE_EXE, L"clip --nope").exit_code == 2);
}

}  // namespace ombra::test
