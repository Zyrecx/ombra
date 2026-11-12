// Instance isolation (paths.h) and the pipe transport (pipe.h), no GPU and no engine.
//
// The harness runs under its own OMBRA_HOME (test_env.cpp), so the pipe tests behave the same
// whether or not the real Ombra is running on this machine. Several of them exist to prove that.

#include <doctest/doctest.h>

#include "support/test_env.h"

#include "ombra/json.h"
#include "ombra/paths.h"
#include "ombra/pipe.h"
#include "ombra/protocol.h"
#include "ombra-ui/pipe_client.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace ombra::test {

namespace {

constexpr wchar_t kInstalledPipe[]  = LR"(\\.\pipe\OmbraEngine)";
constexpr wchar_t kInstalledMutex[] = L"Local\\OmbraEnginePipeInstance";

std::wstring Expand(const wchar_t* s) {
    wchar_t buf[MAX_PATH];
    const DWORD n = ExpandEnvironmentStringsW(s, buf, MAX_PATH);
    return (n > 0 && n <= MAX_PATH) ? std::wstring(buf) : std::wstring();
}

// A raw client on the instance's pipe: CreateFileW/ReadFile/WriteFile, the same calls ombra-ui's
// PipeClient makes, so the tests exercise the real named pipe rather than a mock of PipeServer.
class RawPipeClient {
public:
    explicit RawPipeClient(DWORD timeout_ms = 2000) {
        const std::wstring& name     = Instance().pipe_name;
        const ULONGLONG     deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                             nullptr);
            if (h_ != INVALID_HANDLE_VALUE) return;
            const DWORD err = GetLastError();
            if ((err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) || GetTickCount64() >= deadline)
                return;
            if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 100);
            else Sleep(10);
        }
    }
    ~RawPipeClient() { Close(); }
    RawPipeClient(const RawPipeClient&) = delete;
    RawPipeClient& operator=(const RawPipeClient&) = delete;

    bool Connected() const { return h_ != INVALID_HANDLE_VALUE; }

    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

    bool WriteRaw(const std::string& data) {
        DWORD written = 0;
        return WriteFile(h_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
               written == data.size();
    }

    bool Send(const JsonValue& message) { return WriteRaw(WriteJsonCompact(message) + "\n"); }

    // One response line, or a Null value on timeout, disconnect or unparseable JSON. `pending_`
    // carries bytes already read off the pipe that belong to a later response — a byte-mode pipe
    // makes no promise that one server WriteFile is one ReadFile here, so two responses (or a test
    // deliberately coalescing two requests) can arrive in the same chunk; without this, a second
    // Receive() call would read fresh bytes and never see the first response's leftovers.
    JsonValue Receive(DWORD timeout_ms = 2000) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            const size_t nl = pending_.find('\n');
            if (nl != std::string::npos) {
                const std::string line = pending_.substr(0, nl);
                pending_.erase(0, nl + 1);
                try {
                    return ParseJson(line);
                } catch (const std::string&) {
                    return {};
                }
            }
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return {};
            if (avail == 0) {
                if (GetTickCount64() >= deadline) return {};
                Sleep(5);
                continue;
            }
            char  buf[256];
            DWORD n = 0;
            if (!ReadFile(h_, buf, sizeof(buf), &n, nullptr) || n == 0) return {};
            pending_.append(buf, n);
        }
    }

    // True once the server has closed its end.
    bool ServerClosed(DWORD timeout_ms = 2000) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        do {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return true;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        return false;
    }

private:
    HANDLE      h_ = INVALID_HANDLE_VALUE;
    std::string pending_;
};

JsonValue Request(const char* type, double id) {
    JsonValue r = JsonValue::MakeObject();
    r.Set("type", JsonValue::MakeStr(type));
    r.Set("id", JsonValue::MakeNum(id));
    return r;
}

JsonValue Hello(int version = kProtocolVersion, double id = 1) {
    JsonValue r = Request("hello", id);
    r.Set("v", JsonValue::MakeNum(version));
    return r;
}

bool OkIs(const JsonValue& resp, bool want) {
    const JsonValue* ok = resp.Find("ok");
    return ok && ok->type == JsonValue::Type::Bool && ok->b == want;
}

std::string StrOf(const JsonValue& resp, const char* key) {
    const JsonValue* v = resp.Find(key);
    return (v && v->type == JsonValue::Type::String) ? v->str : std::string();
}

double NumOf(const JsonValue& resp, const char* key) {
    const JsonValue* v = resp.Find(key);
    return (v && v->type == JsonValue::Type::Number) ? v->num : -1.0;
}

// Echoes the request type back and counts how many requests reached application code.
class EchoServer {
public:
    EchoServer()
        : server_([this](const JsonValue& req) {
              handled.fetch_add(1);
              JsonValue resp = JsonValue::MakeObject();
              resp.Set("type", JsonValue::MakeStr(StrOf(req, "type")));
              return resp;
          }) {}

    std::atomic<int> handled{0};  // declared first: server_'s destructor joins handler threads
    PipeServer       server_;
};

}  // namespace

TEST_CASE("pipe server: a message that never ends is dropped, and the server keeps serving") {
    PipeServer server([](const JsonValue& req) {
        JsonValue resp = JsonValue::MakeObject();
        resp.Set("type", JsonValue::MakeStr(StrOf(req, "type")));
        return resp;
    });
    REQUIRE(server.Start());
    {
        RawPipeClient flood;
        REQUIRE(flood.Connected());
        const std::string chunk(1024 * 1024, 'x');  // no newline, ever
        bool dropped = false;
        for (int i = 0; i < 64 && !dropped; ++i) dropped = !flood.WriteRaw(chunk);
        CHECK(dropped);
    }
    ombra_ui::PipeClient client;
    REQUIRE(client.Connect(2000));
    CHECK(client.Hello(2000));
}

TEST_CASE("pipe client: lost replies retry values but never repeat actions") {
    const char* type = "save_now";
    bool retry = false;
    JsonValue patch = JsonValue::MakeObject();
    SUBCASE("save") {}
    SUBCASE("screenshot") { type = "screenshot"; }
    SUBCASE("action setting") {
        type = "set_config";
        patch.Set("engine.autotune", JsonValue::MakeStr("run"));
    }
    SUBCASE("query") { type = "get_config"; retry = true; }
    SUBCASE("value replacement") {
        type = "set_config";
        patch.Set("output.filename", JsonValue::MakeStr("retry-test"));
        retry = true;
    }
    std::atomic<int> actions{0};
    PipeServer server([&](const JsonValue& req) {
        if (StrOf(req, "type") == type && actions.fetch_add(1) == 0) Sleep(150);
        JsonValue resp = JsonValue::MakeObject();
        resp.Set("type", JsonValue::MakeStr(StrOf(req, "type")));
        return resp;
    });
    REQUIRE(server.Start());
    ombra_ui::PipeClient client;
    REQUIRE(client.Connect(2000));
    REQUIRE(client.Hello(2000));
    JsonValue req = Request(type, 1);
    if (std::string(type) == "set_config") req.Set("config", patch);
    const auto resp = client.Request(req, 40);
    CHECK(resp.has_value() == retry);
    CHECK(actions.load() == (retry ? 2 : 1));
    // A fresh request recovers after the ambiguous failure; no stale response can satisfy it.
    const auto next = client.Request(Request("get_status", 2), 2000);
    REQUIRE(next);
    CHECK(StrOf(*next, "type") == "get_status");
}

TEST_CASE("pipe server: a client that never reads can't hang Stop, and its reply still arrives") {
    PipeServer server([](const JsonValue&) { return JsonValue::MakeObject(); });
    REQUIRE(server.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.WriteRaw("{\"type\":\"hello\",\"v\":-1}\n"));
    Sleep(100);  // the worker rejects it and closes while this client sits on the connection

    auto stopped = std::async(std::launch::async, [&] { server.Stop(); });
    const bool finished = stopped.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!finished) client.Close();  // lets a hung Stop go, so the failure reports instead of hanging
    CHECK(finished);
    if (finished) {
        const JsonValue reply = client.Receive();
        REQUIRE(reply.type == JsonValue::Type::Object);
        const JsonValue* ok = reply.Find("ok");
        REQUIRE(ok);
        CHECK_FALSE(ok->b);
    }
}

TEST_CASE("instance: without OMBRA_HOME, the installed names and folders") {
    const InstancePaths p = ResolveInstancePaths(L"");
    CHECK_FALSE(p.isolated);
    // ombra-ui and ombra.exe builds from before this change use these literal names; an installed
    // engine and UI must keep finding each other.
    CHECK(p.pipe_name == kInstalledPipe);
    CHECK(p.instance_mutex == kInstalledMutex);
    CHECK(p.config_dir == Expand(L"%APPDATA%\\Ombra"));
    CHECK(p.data_dir == Expand(L"%LOCALAPPDATA%\\Ombra"));
}

TEST_CASE("instance: OMBRA_HOME moves every folder and renames the pipe and mutex") {
    const std::wstring  home = TestOutputPath(L"instance-a");
    const InstancePaths p    = ResolveInstancePaths(home);
    CHECK(p.isolated);
    CHECK(p.config_dir == home);
    CHECK(p.data_dir == home);
    CHECK(p.pipe_name != kInstalledPipe);
    CHECK(p.pipe_name.rfind(kInstalledPipe, 0) == 0);
    CHECK(p.instance_mutex != kInstalledMutex);
    CHECK(p.instance_mutex.rfind(kInstalledMutex, 0) == 0);
}

TEST_CASE("instance: one folder is one instance however it's spelled; two folders are two") {
    const std::wstring home = TestOutputPath(L"instance-a");
    std::wstring       shouted(home);
    std::transform(shouted.begin(), shouted.end(), shouted.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });

    const InstancePaths a = ResolveInstancePaths(home);
    CHECK(ResolveInstancePaths(shouted + L"\\").pipe_name == a.pipe_name);
    CHECK(ResolveInstancePaths(home + L"\\sub\\..").pipe_name == a.pipe_name);

    const InstancePaths b = ResolveInstancePaths(TestOutputPath(L"instance-b"));
    CHECK(b.pipe_name != a.pipe_name);
    CHECK(b.instance_mutex != a.instance_mutex);
}

TEST_CASE("instance: the test harness itself runs isolated") {
    // If this fails, every other test in this binary is talking to the real Ombra instance.
    REQUIRE(Instance().isolated);
    CHECK(Instance().config_dir == TestRoot());
    CHECK(Instance().pipe_name != kInstalledPipe);
}

TEST_CASE("pipe: an installed engine holding its instance lock doesn't stop an isolated server") {
    // What a resident engine holds. If the real Ombra is running this opens its existing mutex,
    // which is the same situation; nothing is created on the real pipe name.
    HANDLE installed_lock = CreateMutexW(nullptr, FALSE, kInstalledMutex);
    REQUIRE(installed_lock != nullptr);

    EchoServer echo;
    CHECK(echo.server_.Start());
    RawPipeClient client;
    CHECK(client.Connected());
    client.Close();
    echo.server_.Stop();

    CloseHandle(installed_lock);
}

TEST_CASE("pipe: a second server for the same instance refuses to start") {
    EchoServer first;
    REQUIRE(first.server_.Start());
    EchoServer second;
    CHECK_FALSE(second.server_.Start());
}

TEST_CASE("pipe: a server refuses to start on a pipe name another process created first") {
    HANDLE squatter = CreateNamedPipeW(Instance().pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                       4096, 4096, 0, nullptr);
    REQUIRE(squatter != INVALID_HANDLE_VALUE);
    {
        EchoServer echo;
        CHECK_FALSE(echo.server_.Start());
    }
    CloseHandle(squatter);

    // The instance lock was released on the failure, so a retry once the name is free works.
    EchoServer echo;
    CHECK(echo.server_.Start());
}

// An engine self-restart: the overlay can still hold its handle on the old engine's pipe when the
// new engine starts. That leftover client handle must not count as "someone else owns the name".
TEST_CASE("pipe: a client still connected to a stopped server doesn't block the next one") {
    std::optional<RawPipeClient> lingering;
    {
        EchoServer old_engine;
        REQUIRE(old_engine.server_.Start());
        lingering.emplace();
        REQUIRE(lingering->Connected());
        old_engine.server_.Stop();
    }
    EchoServer new_engine;
    CHECK(new_engine.server_.Start());
    RawPipeClient fresh;
    CHECK(fresh.Connected());
}

// Stop() right after Start() lands while the accept thread is entering ConnectNamedPipe: the old
// CancelSynchronousIo shutdown blocked there for good (3 of ~10 full suite runs).
// Runs on a detached thread so a regression fails here instead of hanging the whole suite.
TEST_CASE("pipe: Stop immediately after Start never hangs") {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    std::thread([done] {
        for (int i = 0; i < 300; ++i) {
            EchoServer echo;
            if (!echo.server_.Start()) break;
            // Sweep the gap so Stop lands anywhere in the accept thread's startup.
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((i % 40) * 25);
            while (std::chrono::steady_clock::now() < until) {}
            echo.server_.Stop();
        }
        done->set_value();
    }).detach();
    REQUIRE(finished.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
}

TEST_CASE("pipe: handshake, then requests reach the handler with their id echoed") {
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    REQUIRE(client.Send(Hello(kProtocolVersion, 1)));
    const JsonValue hello = client.Receive();
    CHECK(OkIs(hello, true));
    CHECK(NumOf(hello, "v") == kProtocolVersion);
    CHECK(NumOf(hello, "id") == 1);
    CHECK(echo.handled.load() == 0);  // the transport answers hello itself

    REQUIRE(client.Send(Request("get_status", 2)));
    const JsonValue resp = client.Receive();
    CHECK(StrOf(resp, "type") == "get_status");
    CHECK(NumOf(resp, "id") == 2);
    CHECK(OkIs(resp, true));  // defaulted by the transport when the handler doesn't set it
    CHECK(echo.handled.load() == 1);
}

TEST_CASE("pipe: a wrong protocol version is rejected and the connection closed") {
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    REQUIRE(client.Send(Hello(kProtocolVersion + 1)));
    CHECK(OkIs(client.Receive(), false));
    CHECK(client.ServerClosed());
    CHECK(echo.handled.load() == 0);
}

TEST_CASE("pipe: a request before the handshake is rejected and never reaches the handler") {
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    REQUIRE(client.Send(Request("save_now", 1)));
    CHECK(OkIs(client.Receive(), false));
    CHECK(client.ServerClosed());
    CHECK(echo.handled.load() == 0);
}

TEST_CASE("pipe: malformed JSON gets an error and the connection closed") {
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    REQUIRE(client.WriteRaw("{\"type\":\n"));
    CHECK(OkIs(client.Receive(), false));
    CHECK(client.ServerClosed());
}

TEST_CASE("pipe: a client dropping mid-message doesn't wedge the server") {
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    {
        RawPipeClient dropped;
        REQUIRE(dropped.Connected());
        REQUIRE(dropped.WriteRaw("{\"type\":\"hello\",\"v\":2"));  // no closing brace, no newline
    }
    RawPipeClient next;
    REQUIRE(next.Connected());
    REQUIRE(next.Send(Hello()));
    CHECK(OkIs(next.Receive(), true));
}

TEST_CASE("pipe: two requests written in a single WriteFile call both get answered") {
    // A byte-mode pipe makes no promise that one WriteFile is one ReadFile on the other end: two
    // small messages written together in one call commonly arrive in the same chunk on the
    // server. ReadLine/ServeClient (pipe.cpp) must carry any bytes past the first '\n' over to the
    // next read rather than discarding them — this failed before pipe.cpp's `pending` buffer was
    // added (ReadLine used to take a fresh 256-byte buffer per call with nowhere to keep leftovers).
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    const std::string coalesced =
        WriteJsonCompact(Hello(kProtocolVersion, 1)) + "\n" + WriteJsonCompact(Request("get_status", 2)) + "\n";
    REQUIRE(client.WriteRaw(coalesced));

    const JsonValue hello = client.Receive();
    CHECK(OkIs(hello, true));
    CHECK(NumOf(hello, "id") == 1);

    const JsonValue second = client.Receive();
    CHECK(StrOf(second, "type") == "get_status");
    CHECK(NumOf(second, "id") == 2);
    CHECK(OkIs(second, true));
    CHECK(echo.handled.load() == 1);
}

TEST_CASE("pipe: Stop() returns promptly with an idle client still connected") {
    // A client parked in the server's blocking ReadFile is the case CancelSynchronousIo exists
    // for. If it regresses, engine exit hangs.
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(Hello()));
    REQUIRE(OkIs(client.Receive(), true));

    auto stopped = std::async(std::launch::async, [&] { echo.server_.Stop(); });
    CHECK(stopped.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(client.ServerClosed());
}

}  // namespace ombra::test
