#include "ombra/cli.h"

#include "ombra/brand.h"
#include "ombra/events.h"
#include "ombra/json.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ombra {
namespace {

// Exit codes (cli.h): a public contract once released.
constexpr int kExitOk          = 0;
constexpr int kExitError       = 1;
constexpr int kExitUsage       = 2;
constexpr int kExitNotRunning  = 3;
constexpr int kExitNotAllowed  = 4;
constexpr int kExitReplayOff   = 5;
constexpr int kExitNothingNew  = 6;

// A cold engine holds its mutex while it probes GPUs, before it opens the pipe: a command right
// after a login or `ombra start` waits that out rather than failing.
constexpr DWORD  kConnectTimeoutMs = 30000;
constexpr DWORD  kRequestTimeoutMs = 15000;
constexpr double kClipMaxAfterS    = 120;    // the engine's cap on "after" (protocol.h)
constexpr double kClipWaitMarginS  = 120;    // past "after": the clip's tail, then writing the file
// record start waits for the first footage (the engine gives up after 10 s, kRecordStartSeconds);
// record stop for the last of it, then each file's index.
constexpr DWORD  kRecordStartTimeoutMs = 30000;
constexpr DWORD  kRecordStopTimeoutMs  = 300000;

constexpr const char* kDefaultApp = "Command line";

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A console gets UTF-16 (any character shows). Redirected output (`$p = ombra clip`, a .bat's
// `for /f`) is in the console's output code page, which is what PowerShell and cmd decode it
// with; with no console at all, UTF-8. --json output is ASCII, so it reads the same everywhere.
// A character that code page lacks (a CJK folder name under code page 437) can't reach the caller
// intact in any encoding it would decode; stderr then says so and names the ways out.
void Emit(DWORD which, const std::wstring& text) {
    const HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    UINT cp = GetConsoleOutputCP();
    if (cp == 0) cp = CP_UTF8;
    const int n = WideCharToMultiByte(cp, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(n), '\0');
    BOOL lost = FALSE;
    // UTF-8 (and UTF-7) take no lost-character flag, and lose nothing.
    BOOL* lost_flag = (cp == CP_UTF8 || cp == CP_UTF7) ? nullptr : &lost;
    WideCharToMultiByte(cp, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, lost_flag);
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    static bool warned = false;
    if (lost && which == STD_OUTPUT_HANDLE && !warned) {
        warned = true;
        Emit(STD_ERROR_HANDLE, L"" OMBRA_CLI_NAME ": this output has characters code page " + std::to_wstring(cp) +
                                   L" can't show (printed as ?): use --json, or run chcp 65001 first\n");
    }
}

void Out(const std::wstring& line) { Emit(STD_OUTPUT_HANDLE, line + L"\n"); }
void Err(const std::wstring& line) { Emit(STD_ERROR_HANDLE, L"" OMBRA_CLI_NAME ": " + line + L"\n"); }

int UsageError(const std::wstring& message) {
    Err(message);
    Err(L"run \"" OMBRA_CLI_NAME " help\" for the commands");
    return kExitUsage;
}

int ExitCodeFor(const std::string& error) {
    if (error == "not_allowed") return kExitNotAllowed;
    if (error == "replay_off") return kExitReplayOff;
    if (error == "nothing_new") return kExitNothingNew;
    if (error == "not_running") return kExitNotRunning;
    return kExitError;
}

std::string StrField(const JsonValue& v, const char* key) {
    const JsonValue* f = v.Find(key);
    return f && f->type == JsonValue::Type::String ? f->str : std::string{};
}

// ---- the pipe ------------------------------------------------------------------------------------

enum class Connect { kOk, kNotRunning, kNoAnswer };

// Whether an engine of this instance runs: its single-instance mutex (pipe.cpp) exists from the
// start of wmain, before the pipe opens, so a starting engine isn't mistaken for a missing one.
bool EngineRunning() {
    const HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, Instance().instance_mutex.c_str());
    if (m) {
        CloseHandle(m);
        return true;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;  // exists, not ours to open
}

class Pipe {
public:
    Pipe() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Pipe() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        if (event_) CloseHandle(event_);
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    Connect Open(DWORD timeout_ms) {
        if (!EngineRunning()) return Connect::kNotRunning;
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        const std::wstring& name = Instance().pipe_name;
        while (true) {
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return Connect::kOk;
            const DWORD err = GetLastError();
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return Connect::kNoAnswer;
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(name.c_str(), static_cast<DWORD>((std::min<ULONGLONG>)(deadline - now, 1000)));
            } else if (err == ERROR_FILE_NOT_FOUND) {
                if (!EngineRunning()) return Connect::kNotRunning;  // it exited meanwhile
                Sleep(50);
            } else {
                return Connect::kNoAnswer;
            }
        }
    }

    // One request line out, one response line back; nullopt when the engine closed the pipe or
    // didn't answer in time.
    std::optional<JsonValue> Request(const JsonValue& req, DWORD timeout_ms) {
        const std::string line = WriteJsonCompact(req) + "\n";
        DWORD done = 0;
        if (!Io(true, const_cast<char*>(line.data()), static_cast<DWORD>(line.size()), &done, timeout_ms) ||
            done != line.size())
            return std::nullopt;
        return Read(timeout_ms);
    }

    // The next line the engine sends (INFINITE: however long that takes); nullopt as Request's.
    std::optional<JsonValue> Read(DWORD timeout_ms) {
        const ULONGLONG deadline = timeout_ms == INFINITE ? 0 : GetTickCount64() + timeout_ms;
        DWORD done = 0;
        while (true) {
            if (const size_t nl = buffer_.find('\n'); nl != std::string::npos) {
                const std::string text = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                try {
                    return ParseJson(text);
                } catch (const std::string&) {
                    return std::nullopt;
                }
            }
            if (buffer_.size() > kMaxApiMessageBytes) return std::nullopt;
            DWORD wait_ms = INFINITE;
            if (deadline) {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) return std::nullopt;
                wait_ms = static_cast<DWORD>(deadline - now);
            }
            char chunk[4096];
            if (!Io(false, chunk, sizeof(chunk), &done, wait_ms) || done == 0) return std::nullopt;
            buffer_.append(chunk, done);
        }
    }

private:
    bool Io(bool write, char* data, DWORD size, DWORD* done, DWORD timeout_ms) {
        OVERLAPPED ov{};
        ov.hEvent = event_;
        ResetEvent(event_);
        const BOOL started = write ? WriteFile(pipe_, data, size, nullptr, &ov)
                                   : ReadFile(pipe_, data, size, nullptr, &ov);
        if (!started && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event_, timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &ov);
            GetOverlappedResult(pipe_, &ov, done, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe_, &ov, done, FALSE) != FALSE;
    }

    HANDLE      pipe_ = INVALID_HANDLE_VALUE;
    HANDLE      event_;
    std::string buffer_;
};

// ---- arguments -----------------------------------------------------------------------------------

struct Args {
    std::vector<std::wstring>             positional;
    std::map<std::wstring, std::wstring>  values;  // "--before" -> "30"
    std::set<std::wstring>                flags;   // "--json"
    bool Has(const wchar_t* flag) const { return flags.count(flag) != 0; }
    const std::wstring* Value(const wchar_t* option) const {
        const auto it = values.find(option);
        return it == values.end() ? nullptr : &it->second;
    }
};

bool InList(const std::wstring& name, std::initializer_list<const wchar_t*> list) {
    for (const wchar_t* item : list)
        if (name == item) return true;
    return false;
}

// Options take "--name value" or "--name=value"; a value may start with '-' ("--at -3"). "--" ends
// the options, so a label can start with '-'. Every command takes --json and --as.
std::optional<Args> ParseArgs(int argc, wchar_t** argv, std::initializer_list<const wchar_t*> value_options,
                              std::initializer_list<const wchar_t*> flag_options, int* exit_code) {
    Args args;
    bool options_done = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (!options_done && arg == L"--") {
            options_done = true;
            continue;
        }
        if (options_done || arg.size() < 2 || arg[0] != L'-') {
            args.positional.push_back(arg);
            continue;
        }
        const size_t eq = arg.find(L'=');
        const std::wstring name = arg.substr(0, eq);
        if (InList(name, value_options) || name == L"--as") {
            if (eq != std::wstring::npos) {
                args.values[name] = arg.substr(eq + 1);
            } else if (i + 1 < argc) {
                args.values[name] = argv[++i];
            } else {
                *exit_code = UsageError(name + L" needs a value");
                return std::nullopt;
            }
        } else if ((InList(name, flag_options) || name == L"--json") && eq == std::wstring::npos) {
            args.flags.insert(name);
        } else {
            *exit_code = UsageError(L"unknown option " + arg + L" for " + argv[1]);
            return std::nullopt;
        }
    }
    return args;
}

// Seconds as typed: a finite number, the whole text.
std::optional<double> ParseSeconds(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

// ---- running a command ---------------------------------------------------------------------------

// The engine's answer without the request id: --json prints the pipe's own shape.
JsonValue WithoutId(const JsonValue& resp) {
    JsonValue out = JsonValue::MakeObject();
    for (const auto& [key, value] : resp.obj)
        if (key != "id") out.obj.emplace_back(key, value);
    return out;
}

void PrintJson(const JsonValue& v) { Out(Wide(WriteJsonCompactAscii(v))); }

// A failure the CLI found itself (no engine, no answer), in the same shape as the engine's.
int Fail(bool json, const char* type, const char* error, const std::wstring& message) {
    if (json) {
        JsonValue v = JsonValue::MakeObject();
        v.Set("type", JsonValue::MakeStr(type));
        v.Set("ok", JsonValue::MakeBool(false));
        v.Set("error", JsonValue::MakeStr(error));
        v.Set("message", JsonValue::MakeStr(Utf8(message)));
        PrintJson(v);
    } else {
        Err(message);
    }
    return ExitCodeFor(error);
}

// The engine's error: the message on stderr (or the answer, with --json), the exit code its code maps to.
int EngineFailure(bool json, const JsonValue& resp) {
    if (json) PrintJson(WithoutId(resp));
    else Err(Wide(StrField(resp, "message").empty() ? StrField(resp, "error") : StrField(resp, "message")));
    return ExitCodeFor(StrField(resp, "error"));
}

bool IsOk(const JsonValue& resp) {
    const JsonValue* ok = resp.Find("ok");
    return ok && ok->type == JsonValue::Type::Bool && ok->b;
}

// Connects, says hello as `app`, sends `req` (its "type" set) and returns the answer, or the exit
// code of a failure already reported.
struct Answer {
    std::optional<JsonValue> resp;
    int                      exit_code = kExitOk;
};

// Connects and says hello as `app`; kExitOk, or the exit code of a failure already reported.
int OpenSession(const Args& args, const std::string& type, Pipe& pipe) {
    const bool json = args.Has(L"--json");
    switch (pipe.Open(kConnectTimeoutMs)) {
        case Connect::kOk: break;
        case Connect::kNotRunning:
            return Fail(json, type.c_str(), "not_running",
                        L"" OMBRA_PRODUCT_NAME " isn't running (start it with \"" OMBRA_CLI_NAME " start\")");
        case Connect::kNoAnswer:
            return Fail(json, type.c_str(), "no_answer", L"" OMBRA_PRODUCT_NAME " didn't answer");
    }
    JsonValue hello = JsonValue::MakeObject();
    hello.Set("type", JsonValue::MakeStr("hello"));
    hello.Set("id", JsonValue::MakeNum(1));
    hello.Set("api", JsonValue::MakeNum(kApiVersionMax));
    const std::wstring* as = args.Value(L"--as");
    hello.Set("app", JsonValue::MakeStr(as ? Utf8(*as) : std::string(kDefaultApp)));
    const std::optional<JsonValue> greeted = pipe.Request(hello, kRequestTimeoutMs);
    if (!greeted) return Fail(json, type.c_str(), "no_answer", L"" OMBRA_PRODUCT_NAME " didn't answer");
    if (!IsOk(*greeted)) return EngineFailure(json, *greeted);
    return kExitOk;
}

Answer Ask(const Args& args, JsonValue req, DWORD timeout_ms) {
    const bool json = args.Has(L"--json");
    const std::string type = StrField(req, "type");
    Answer answer;
    Pipe pipe;
    answer.exit_code = OpenSession(args, type, pipe);
    if (answer.exit_code != kExitOk) return answer;
    req.Set("id", JsonValue::MakeNum(2));
    answer.resp = pipe.Request(req, timeout_ms);
    if (!answer.resp)
        answer.exit_code = Fail(json, type.c_str(), "no_answer", L"" OMBRA_PRODUCT_NAME " didn't answer");
    return answer;
}

JsonValue Request(const char* type) {
    JsonValue req = JsonValue::MakeObject();
    req.Set("type", JsonValue::MakeStr(type));
    return req;
}

// Ctrl+C while a clip is being waited for: the clip is the engine's, it still saves.
BOOL WINAPI ClipWaitCtrlHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
        Err(L"stopped waiting; the clip still saves to " OMBRA_PRODUCT_NAME "'s clips folder");
    return FALSE;  // the default handler ends the process
}

int RunClip(int argc, wchar_t** argv) {
    int code = kExitOk;
    const auto args = ParseArgs(argc, argv, {L"--before", L"--after", L"--title"}, {L"--no-wait"}, &code);
    if (!args) return code;
    if (!args->positional.empty()) return UsageError(L"clip takes no arguments, only options");
    JsonValue req = Request("clip");
    for (const wchar_t* option : {L"--before", L"--after"}) {
        const std::wstring* text = args->Value(option);
        if (!text) continue;
        const std::optional<double> seconds = ParseSeconds(*text);
        if (!seconds || *seconds < 0) return UsageError(std::wstring(option) + L" is seconds: a number, 0 or more");
        req.Set(Utf8(option + 2), JsonValue::MakeNum(*seconds));
    }
    if (const std::wstring* title = args->Value(L"--title")) req.Set("title", JsonValue::MakeStr(Utf8(*title)));
    const bool wait = !args->Has(L"--no-wait");
    req.Set("wait", JsonValue::MakeBool(wait));

    const JsonValue* after = req.Find("after");
    const double     wait_s = (std::min)(after ? after->num : 0, kClipMaxAfterS) + kClipWaitMarginS;
    if (wait) SetConsoleCtrlHandler(ClipWaitCtrlHandler, TRUE);
    const Answer answer = Ask(*args, std::move(req), wait ? static_cast<DWORD>(wait_s * 1000) : kRequestTimeoutMs);
    if (wait) SetConsoleCtrlHandler(ClipWaitCtrlHandler, FALSE);
    if (!answer.resp) return answer.exit_code;
    const JsonValue& resp = *answer.resp;
    if (args->Has(L"--json")) {
        PrintJson(WithoutId(resp));
        return IsOk(resp) ? kExitOk : ExitCodeFor(StrField(resp, "error"));
    }
    // Every file written, one per line, even when another monitor failed: a script keeps what saved.
    bool printed = false;
    if (const JsonValue* monitors = resp.Find("monitors"); monitors && monitors->type == JsonValue::Type::Array) {
        for (const JsonValue& monitor : monitors->arr) {
            const std::string path = StrField(monitor, "path");
            if (!path.empty()) {
                Out(Wide(path));
                printed = true;
            }
        }
    }
    if (!IsOk(resp)) return EngineFailure(false, resp);
    if (!printed) {
        // --no-wait: the clip's id, which the engine reports the files under.
        const JsonValue* id = resp.Find("clip");
        if (id && id->type == JsonValue::Type::Number) Out(std::to_wstring(static_cast<long long>(id->num)));
    }
    return kExitOk;
}

int RunMarker(int argc, wchar_t** argv) {
    int code = kExitOk;
    const auto args = ParseArgs(argc, argv, {L"--at", L"--kind"}, {L"--important"}, &code);
    if (!args) return code;
    if (args->positional.size() > 1)
        return UsageError(L"marker takes one label; put a label with spaces in quotes");
    JsonValue req = Request("marker");
    if (!args->positional.empty()) req.Set("label", JsonValue::MakeStr(Utf8(args->positional[0])));
    if (const std::wstring* kind = args->Value(L"--kind")) req.Set("kind", JsonValue::MakeStr(Utf8(*kind)));
    if (const std::wstring* at = args->Value(L"--at")) {
        const std::optional<double> seconds = ParseSeconds(*at);
        if (!seconds || *seconds > 0) return UsageError(L"--at is seconds before now: 0 or less, like --at -3");
        req.Set("at", JsonValue::MakeNum(*seconds));
    }
    if (args->Has(L"--important")) req.Set("important", JsonValue::MakeBool(true));
    const Answer answer = Ask(*args, std::move(req), kRequestTimeoutMs);
    if (!answer.resp) return answer.exit_code;
    if (!IsOk(*answer.resp)) return EngineFailure(args->Has(L"--json"), *answer.resp);
    if (args->Has(L"--json")) PrintJson(WithoutId(*answer.resp));
    return kExitOk;
}

// Ctrl+C while a recording's stop is being waited for: the engine finishes its files anyway.
BOOL WINAPI RecordStopCtrlHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
        Err(L"stopped waiting; the recording still stops and saves to " OMBRA_PRODUCT_NAME "'s clips folder");
    return FALSE;  // the default handler ends the process
}

int RunRecordStart(const Args& args) {
    if (args.positional.size() != 1) return UsageError(L"record start takes no arguments, only options");
    JsonValue req = Request("record_start");
    if (const std::wstring* before = args.Value(L"--before")) {
        const std::optional<double> seconds = ParseSeconds(*before);
        if (!seconds || *seconds < 0) return UsageError(L"--before is seconds: a number, 0 or more");
        req.Set("before", JsonValue::MakeNum(*seconds));
    }
    if (const std::wstring* title = args.Value(L"--title")) req.Set("title", JsonValue::MakeStr(Utf8(*title)));
    const Answer answer = Ask(args, std::move(req), kRecordStartTimeoutMs);
    if (!answer.resp) return answer.exit_code;
    const JsonValue& resp = *answer.resp;
    if (args.Has(L"--json")) {
        PrintJson(WithoutId(resp));
        return IsOk(resp) ? kExitOk : ExitCodeFor(StrField(resp, "error"));
    }
    const JsonValue* id = resp.Find("recording");
    const long long  id_n = id && id->type == JsonValue::Type::Number ? static_cast<long long>(id->num) : 0;
    if (!IsOk(resp)) {
        const int exit_code = EngineFailure(false, resp);
        if (StrField(resp, "error") == "already_recording" && id_n)
            Err(L"recording " + std::to_wstring(id_n) + L" is running: \"" OMBRA_CLI_NAME " record stop " +
                std::to_wstring(id_n) + L"\" ends it");
        return exit_code;
    }
    Out(std::to_wstring(id_n));
    return kExitOk;
}

int RunRecordStop(const Args& args) {
    if (args.positional.size() > 2) return UsageError(L"record stop takes at most one recording id");
    JsonValue req = Request("record_stop");
    if (args.positional.size() == 2) {
        const std::wstring& text = args.positional[1];
        wchar_t* end = nullptr;
        const unsigned long long id = std::wcstoull(text.c_str(), &end, 10);
        if (text.empty() || end != text.c_str() + text.size() || id == 0 || text[0] == L'-')
            return UsageError(L"a recording id is a whole number, 1 or more, as record start printed it");
        req.Set("recording", JsonValue::MakeNum(static_cast<double>(id)));
    }
    SetConsoleCtrlHandler(RecordStopCtrlHandler, TRUE);
    const Answer answer = Ask(args, std::move(req), kRecordStopTimeoutMs);
    SetConsoleCtrlHandler(RecordStopCtrlHandler, FALSE);
    if (!answer.resp) return answer.exit_code;
    const JsonValue& resp = *answer.resp;
    if (args.Has(L"--json")) {
        PrintJson(WithoutId(resp));
        return IsOk(resp) ? kExitOk : ExitCodeFor(StrField(resp, "error"));
    }
    // Every file written, one per line, even when another monitor failed: a script keeps what saved.
    if (const JsonValue* paths = resp.Find("paths"); paths && paths->type == JsonValue::Type::Array)
        for (const JsonValue& path : paths->arr)
            if (path.type == JsonValue::Type::String) Out(Wide(path.str));
    if (!IsOk(resp)) return EngineFailure(false, resp);
    const std::string end = StrField(resp, "end");
    if (end == "disk_low") Err(L"the recording stopped early: the disk is nearly full");
    if (end == "write_failed") Err(L"the recording stopped early: a write failed (see " OMBRA_PRODUCT_NAME "'s log)");
    return kExitOk;
}

int RunRecord(int argc, wchar_t** argv) {
    int code = kExitOk;
    const auto args = ParseArgs(argc, argv, {L"--before", L"--title"}, {}, &code);
    if (!args) return code;
    const std::wstring action = args->positional.empty() ? std::wstring() : args->positional[0];
    if (action == L"start") return RunRecordStart(*args);
    if (action != L"stop") return UsageError(L"record takes start or stop");
    if (args->Value(L"--before") || args->Value(L"--title"))
        return UsageError(L"--before and --title are for record start");
    return RunRecordStop(*args);
}

int RunStatus(int argc, wchar_t** argv) {
    int code = kExitOk;
    const auto args = ParseArgs(argc, argv, {}, {}, &code);
    if (!args) return code;
    if (!args->positional.empty()) return UsageError(L"status takes no arguments");
    const Answer answer = Ask(*args, Request("status"), kRequestTimeoutMs);
    if (!answer.resp) return answer.exit_code;
    const JsonValue& resp = *answer.resp;
    if (!IsOk(resp)) return EngineFailure(args->Has(L"--json"), resp);
    if (args->Has(L"--json")) {
        PrintJson(WithoutId(resp));
        return kExitOk;
    }
    const auto flag = [&](const char* key) {
        const JsonValue* v = resp.Find(key);
        return v && v->type == JsonValue::Type::Bool && v->b;
    };
    const JsonValue* seconds = resp.Find("buffer_seconds");
    const std::string game = StrField(resp, "game");
    Out(std::wstring(L"Instant Replay  ") + (flag("replay") ? L"on" : L"off"));
    Out(std::wstring(L"Recording       ") + (flag("buffering") ? L"yes" : L"no"));
    if (!game.empty()) Out(L"Game            " + Wide(game));
    if (seconds && seconds->type == JsonValue::Type::Number)
        Out(L"Buffer          " + std::to_wstring(static_cast<long long>(std::llround(seconds->num))) + L" s");
    return kExitOk;
}

int RunReplay(int argc, wchar_t** argv) {
    int code = kExitOk;
    const auto args = ParseArgs(argc, argv, {}, {}, &code);
    if (!args) return code;
    if (args->positional.size() != 1 || (args->positional[0] != L"on" && args->positional[0] != L"off"))
        return UsageError(L"replay takes on or off");
    JsonValue req = Request("replay");
    req.Set("replay", JsonValue::MakeBool(args->positional[0] == L"on"));
    const Answer answer = Ask(*args, std::move(req), kRequestTimeoutMs);
    if (!answer.resp) return answer.exit_code;
    if (!IsOk(*answer.resp)) return EngineFailure(args->Has(L"--json"), *answer.resp);
    if (args->Has(L"--json")) PrintJson(WithoutId(*answer.resp));
    return kExitOk;
}

// One event as a line of text: the local time, the event, what it says.
std::wstring DescribeEvent(const JsonValue& ev) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[16];
    swprintf(stamp, 16, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
    const std::string name = StrField(ev, "event");
    const auto flag = [&](const char* key) {
        const JsonValue* v = ev.Find(key);
        return v && v->type == JsonValue::Type::Bool && v->b;
    };
    std::wstring text = std::wstring(stamp) + L"  ";
    if (name == "replay") return text + L"Instant Replay " + (flag("replay") ? L"on" : L"off");
    if (name == "buffering") return text + (flag("buffering") ? L"Recording" : L"Not recording");
    if (name == "game_start") return text + L"Game started: " + Wide(StrField(ev, "game"));
    if (name == "game_end") return text + L"Game ended: " + Wide(StrField(ev, "game"));
    if (name == "exiting") return text + L"" OMBRA_PRODUCT_NAME " is exiting";
    if (name == "clip_saved") {
        const std::string app = StrField(ev, "app");
        text += L"Clip saved" + (app.empty() ? std::wstring() : L" for " + Wide(app));
        if (const JsonValue* s = ev.Find("seconds"); s && s->type == JsonValue::Type::Number) {
            wchar_t secs[32];
            swprintf(secs, 32, L" (%.1f s)", s->num);
            text += secs;
        }
        if (const JsonValue* paths = ev.Find("paths"); paths && paths->type == JsonValue::Type::Array)
            for (const JsonValue& p : paths->arr)
                if (p.type == JsonValue::Type::String) text += L"\n          " + Wide(p.str);
        return text;
    }
    if (name == "recording_started" || name == "recording_stopped" || name == "recording_recovered") {
        const std::string app = StrField(ev, "app");
        const JsonValue*  id = ev.Find("recording");
        text += name == "recording_started"   ? L"Recording started"
                : name == "recording_stopped" ? L"Recording stopped"
                                              : L"Recording recovered";
        if (id && id->type == JsonValue::Type::Number)
            text += L" (" + std::to_wstring(static_cast<long long>(id->num)) + L")";
        if (!app.empty()) text += L" for " + Wide(app);
        if (const JsonValue* s = ev.Find("seconds"); s && s->type == JsonValue::Type::Number) {
            wchar_t secs[32];
            swprintf(secs, 32, L", %.1f s", s->num);
            text += secs;
        }
        return text;
    }
    return text + Wide(name);
}

// Prints the engine's events as they happen until it exits (0) or the connection drops (1).
int RunWatch(int argc, wchar_t** argv) {
    int code = kExitOk;
    const auto args = ParseArgs(argc, argv, {}, {}, &code);
    if (!args) return code;
    const bool json = args->Has(L"--json");
    JsonValue req = Request("subscribe");
    req.Set("id", JsonValue::MakeNum(2));
    // exiting is always heard, so the engine quitting ends the watch with 0; shown only if asked for.
    bool show_exiting = args->positional.empty();
    if (!args->positional.empty()) {
        JsonValue names = JsonValue::MakeArray();
        names.arr.push_back(JsonValue::MakeStr("exiting"));
        for (const std::wstring& name : args->positional) {
            if (std::none_of(std::begin(kEventNames), std::end(kEventNames),
                             [&](const char* known) { return Wide(known) == name; }))
                return UsageError(L"unknown event " + name + L" (watch takes replay, buffering, game_start, "
                                  L"game_end, clip_saved, exiting)");
            if (name == L"exiting") show_exiting = true;
            else names.arr.push_back(JsonValue::MakeStr(Utf8(name)));
        }
        req.Set("events", std::move(names));
    }
    Pipe pipe;
    code = OpenSession(*args, "watch", pipe);
    if (code != kExitOk) return code;
    const std::optional<JsonValue> reply = pipe.Request(req, kRequestTimeoutMs);
    if (!reply) return Fail(json, "watch", "no_answer", L"" OMBRA_PRODUCT_NAME " didn't answer");
    if (!IsOk(*reply)) return EngineFailure(json, *reply);
    while (true) {
        const std::optional<JsonValue> line = pipe.Read(INFINITE);
        if (!line) return Fail(json, "watch", "disconnected", L"lost the connection to " OMBRA_PRODUCT_NAME);
        if (StrField(*line, "type") != "event") continue;
        const bool exiting = StrField(*line, "event") == "exiting";
        if (!exiting || show_exiting) {
            if (json) PrintJson(*line);
            else Out(DescribeEvent(*line));
        }
        if (exiting) return kExitOk;
    }
}

// The engine beside this binary, started without a console or focus; returns once its pipe answers.
// On a new install the engine opens its setup: nothing records before the user agrees to it.
int RunStart(int argc) {
    if (argc > 2) return UsageError(L"start takes no options");
    if (EngineRunning()) {
        // Possibly still starting (a login, another script): ready means its pipe answers.
        Pipe pipe;
        switch (pipe.Open(kConnectTimeoutMs)) {
            case Connect::kOk: return kExitOk;
            case Connect::kNotRunning: break;  // it exited meanwhile: start one
            case Connect::kNoAnswer:
                return Fail(false, "start", "no_answer", L"" OMBRA_PRODUCT_NAME " is running but didn't answer");
        }
    }

    std::wstring self(MAX_PATH, L'\0');
    DWORD n = 0;
    while ((n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()))) == self.size())
        self.resize(self.size() * 2);
    if (n == 0) return Fail(false, "start", "failed", L"can't find " OMBRA_PRODUCT_NAME "'s folder");
    self.resize(n);
    const std::wstring engine = self.substr(0, self.find_last_of(L'\\') + 1) + L"" OMBRA_ENGINE_EXE;
    std::wstring cmdline = L"\"" + engine + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // No inherited handles (a caller capturing this command's output must not wait on the engine),
    // no console and its own process group (the shell's Ctrl+C never reaches it).
    if (!CreateProcessW(engine.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return Fail(false, "start", "failed", L"couldn't start " + engine + L" (error " + std::to_wstring(GetLastError()) + L")");
    CloseHandle(pi.hThread);

    const ULONGLONG deadline = GetTickCount64() + kConnectTimeoutMs;
    int result = kExitError;
    while (GetTickCount64() < deadline) {
        Pipe pipe;
        if (pipe.Open(500) == Connect::kOk) {
            result = kExitOk;
            break;
        }
        // Exited: another engine won the race (fine if one runs now), or it failed to start.
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0 && !EngineRunning()) {
            DWORD exit_code = 0;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            return Fail(false, "start", "failed",
                        L"" OMBRA_PRODUCT_NAME " stopped as it started (exit code " + std::to_wstring(exit_code) + L"); see its log");
        }
    }
    CloseHandle(pi.hProcess);
    if (result != kExitOk) Err(L"" OMBRA_PRODUCT_NAME " started but didn't answer within 30 s");
    return result;
}

}  // namespace

bool IsCliCommand(const wchar_t* arg) {
    if (!arg) return false;
    for (const wchar_t* command : {L"clip", L"marker", L"record", L"status", L"replay", L"watch", L"start", L"help"})
        if (std::wcscmp(arg, command) == 0) return true;
    return false;
}

int PrintCliUsage(bool error) {
    const std::wstring text =
        L"Usage: " OMBRA_CLI_NAME " <command> [options]\n"
        L"\n"
        L"Commands:\n"
        L"  clip [--before S] [--after S] [--title T] [--no-wait]\n"
        L"        Save a clip from S seconds back (default: the whole buffer) to S seconds ahead\n"
        L"        (default 0) and print each file's path. --no-wait prints the clip's id at once.\n"
        L"  marker [LABEL] [--at -S] [--kind K] [--important]\n"
        L"        Mark this moment, or S seconds ago, in every clip that covers it. K is one of\n"
        L"        generic, kill, death, assist, win, loss, objective, highlight, round_start, round_end.\n"
        L"  record start [--before S] [--title T]\n"
        L"        Record until \"record stop\", starting S seconds back (default 0), and print the\n"
        L"        recording's id. One recording at a time.\n"
        L"  record stop [ID]\n"
        L"        Stop the recording (default: the latest) and print each file's path.\n"
        L"  status\n"
        L"        Whether Instant Replay is on and recording, the game, the buffer length.\n"
        L"  replay on|off\n"
        L"        Turn Instant Replay on or off.\n"
        L"  watch [EVENT...]\n"
        L"        Print what happens as it happens until " OMBRA_PRODUCT_NAME " exits: replay, buffering,\n"
        L"        game_start, game_end, clip_saved, recording_started, recording_stopped,\n"
        L"        recording_recovered, exiting (default: all of them).\n"
        L"  start\n"
        L"        Start " OMBRA_PRODUCT_NAME " in the background if it isn't running.\n"
        L"\n"
        L"Every command but start also takes:\n"
        L"  --json      Print " OMBRA_PRODUCT_NAME "'s answer as one line of JSON.\n"
        L"  --as NAME   The name " OMBRA_PRODUCT_NAME "'s notifications show (default \"Command line\").\n"
        L"\n"
        L"Exit codes: 0 ok, 1 error, 2 usage, 3 " OMBRA_PRODUCT_NAME " isn't running, 4 not allowed,\n"
        L"5 Instant Replay is off, 6 nothing new to save.\n";
    Emit(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, text);
    return error ? kExitUsage : kExitOk;
}

int RunCli(int argc, wchar_t** argv) {
    if (argc < 2) return PrintCliUsage(true);
    const std::wstring command = argv[1];
    if (command == L"help" || command == L"--help" || command == L"-h" || command == L"/?") return PrintCliUsage(false);
    if (command == L"clip") return RunClip(argc, argv);
    if (command == L"marker") return RunMarker(argc, argv);
    if (command == L"record") return RunRecord(argc, argv);
    if (command == L"status") return RunStatus(argc, argv);
    if (command == L"replay") return RunReplay(argc, argv);
    if (command == L"watch") return RunWatch(argc, argv);
    if (command == L"start") return RunStart(argc);
    return UsageError(L"unknown command " + command);
}

}  // namespace ombra
