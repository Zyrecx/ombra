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
#include <vector>

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

namespace {

JsonValue ApiHello(double api = kApiVersionMax, const char* app = "Test App") {
    JsonValue r = Request("hello", 1);
    r.Set("api", JsonValue::MakeNum(api));
    if (app) r.Set("app", JsonValue::MakeStr(app));
    return r;
}

// Internal requests echo like EchoServer; API requests answer with the caller's app name.
class TwoSurfaceServer {
public:
    explicit TwoSurfaceServer(PipeServer::InternalGate gate = nullptr)
        : server_(
              [this](const JsonValue&) {
                  internal.fetch_add(1);
                  return JsonValue::MakeObject();
              },
              [this](const JsonValue&, const PipeCaller& caller) {
                  api.fetch_add(1);
                  JsonValue resp = JsonValue::MakeObject();
                  resp.Set("app", JsonValue::MakeStr(caller.app));
                  resp.Set("self", JsonValue::MakeBool(caller.pid == GetCurrentProcessId()));
                  resp.Set("command_line", JsonValue::MakeBool(caller.command_line));
                  return resp;
              },
              std::move(gate)) {}

    std::atomic<int> internal{0};
    std::atomic<int> api{0};
    PipeServer       server_;
};

}  // namespace

TEST_CASE("pipe api: hello, then requests reach the API handler with type and id echoed") {
    TwoSurfaceServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    REQUIRE(client.Send(ApiHello()));
    const JsonValue hello = client.Receive();
    CHECK(OkIs(hello, true));
    CHECK(StrOf(hello, "type") == "hello");
    CHECK(NumOf(hello, "api") == kApiVersionMax);
    CHECK(!StrOf(hello, "engine").empty());

    JsonValue req = JsonValue::MakeObject();
    req.Set("type", JsonValue::MakeStr("status"));
    req.Set("id", JsonValue::MakeStr("a7"));
    REQUIRE(client.Send(req));
    const JsonValue resp = client.Receive();
    CHECK(OkIs(resp, true));
    CHECK(StrOf(resp, "type") == "status");
    CHECK(StrOf(resp, "id") == "a7");
    CHECK(StrOf(resp, "app") == "Test App");
    const JsonValue* self = resp.Find("self");
    CHECK((self && self->type == JsonValue::Type::Bool && self->b));
    const JsonValue* command_line = resp.Find("command_line");  // this process counts as Ombra's own
    CHECK((command_line && command_line->type == JsonValue::Type::Bool && command_line->b));
    CHECK(s.internal.load() == 0);
}

TEST_CASE("pipe api: a request without an id is refused and the connection stays open") {
    TwoSurfaceServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(ApiHello()));
    REQUIRE(OkIs(client.Receive(), true));

    REQUIRE(client.WriteRaw("{\"type\":\"status\"}\n"));
    const JsonValue refused = client.Receive();
    CHECK(OkIs(refused, false));
    CHECK(StrOf(refused, "error") == "bad_request");
    CHECK(StrOf(refused, "type") == "status");
    CHECK(s.api.load() == 0);

    REQUIRE(client.Send(Request("status", 2)));
    CHECK(OkIs(client.Receive(), true));
    CHECK(s.api.load() == 1);
}

TEST_CASE("pipe api: refused hellos say why and close the connection") {
    TwoSurfaceServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());

    std::string code = "bad_request";
    SUBCASE("unsupported version") {
        REQUIRE(client.Send(ApiHello(kApiVersionMax + 1)));
        code = "unsupported_version";
    }
    SUBCASE("fractional version") { REQUIRE(client.Send(ApiHello(kApiVersionMin + 0.5))); code = "unsupported_version"; }
    SUBCASE("no app") { REQUIRE(client.Send(ApiHello(kApiVersionMax, nullptr))); }
    SUBCASE("empty app") { REQUIRE(client.Send(ApiHello(kApiVersionMax, ""))); }
    SUBCASE("app with a newline") { REQUIRE(client.Send(ApiHello(kApiVersionMax, "a\nb"))); }
    SUBCASE("neither api nor v") { REQUIRE(client.Send(Request("hello", 1))); }

    const JsonValue resp = client.Receive();
    CHECK(OkIs(resp, false));
    CHECK(StrOf(resp, "type") == "hello");
    CHECK(StrOf(resp, "error") == code);
    CHECK(!StrOf(resp, "message").empty());
    if (code == "unsupported_version") {
        CHECK(NumOf(resp, "min") == kApiVersionMin);
        CHECK(NumOf(resp, "max") == kApiVersionMax);
    }
    CHECK(client.ServerClosed());
    CHECK(s.api.load() == 0);
}

TEST_CASE("pipe api: a server without an API handler refuses API hellos") {
    EchoServer echo;
    REQUIRE(echo.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(ApiHello()));
    const JsonValue resp = client.Receive();
    CHECK(OkIs(resp, false));
    CHECK(StrOf(resp, "error") == "not_allowed");
    CHECK(client.ServerClosed());
}

TEST_CASE("pipe api: a line past the API limit closes the connection") {
    TwoSurfaceServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(ApiHello()));
    REQUIRE(OkIs(client.Receive(), true));

    JsonValue big = Request("status", 2);
    big.Set("pad", JsonValue::MakeStr(std::string(kMaxApiMessageBytes, 'x')));
    client.Send(big);  // may fail part-way once the server closes
    CHECK(client.ServerClosed());
    CHECK(s.api.load() == 0);
}

TEST_CASE("pipe: the internal surface is refused to a client the gate doesn't pass") {
    std::atomic<DWORD> seen_pid{0};
    TwoSurfaceServer s([&](DWORD pid, const std::wstring&) {
        seen_pid = pid;
        return false;
    });
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(Hello()));
    const JsonValue resp = client.Receive();
    CHECK(OkIs(resp, false));
    CHECK(StrOf(resp, "error") == "not_allowed");
    CHECK(client.ServerClosed());
    CHECK(seen_pid.load() == GetCurrentProcessId());
    CHECK(s.internal.load() == 0);
}

TEST_CASE("pipe: the internal gate's default passes only ombra-ui.exe beside this image") {
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    REQUIRE(n > 0);
    std::wstring dir(self, n);
    dir.erase(dir.find_last_of(L'\\') + 1);
    CHECK(IsSiblingOverlay(dir + L"ombra-ui.exe"));
    CHECK(IsSiblingOverlay(dir + L"OMBRA-UI.EXE"));
    CHECK_FALSE(IsSiblingOverlay(dir + L"ombra-ui.exe.bak"));
    CHECK_FALSE(IsSiblingOverlay(dir + L"sub\\ombra-ui.exe"));
    CHECK_FALSE(IsSiblingOverlay(L"C:\\Elsewhere\\ombra-ui.exe"));
    CHECK_FALSE(IsSiblingOverlay(L""));
    CHECK_FALSE(IsSiblingOverlay(std::wstring(self, n)));  // a real file, but not the overlay
    // The same file under another spelling still passes: identity, not path text.
    wchar_t short_path[MAX_PATH * 2];
    const DWORD s = GetShortPathNameW((dir + L"ombra-ui.exe").c_str(), short_path, MAX_PATH * 2);
    if (s > 0 && s < MAX_PATH * 2) CHECK(IsSiblingOverlay(std::wstring(short_path, s)));
    CHECK(IsSiblingOverlay(L"\\\\?\\" + dir + L"ombra-ui.exe"));
}

TEST_CASE("pipe api: app connections are capped; a closed one frees its place") {
    TwoSurfaceServer s;
    REQUIRE(s.server_.Start());
    std::vector<std::unique_ptr<RawPipeClient>> apps;
    for (int i = 0; i < kMaxApiClients; ++i) {
        apps.push_back(std::make_unique<RawPipeClient>());
        REQUIRE(apps.back()->Connected());
        REQUIRE(apps.back()->Send(ApiHello()));
        REQUIRE(OkIs(apps.back()->Receive(), true));
    }
    {
        RawPipeClient extra;
        REQUIRE(extra.Connected());
        REQUIRE(extra.Send(ApiHello()));
        const JsonValue resp = extra.Receive();
        CHECK(OkIs(resp, false));
        CHECK(StrOf(resp, "error") == "too_many_connections");
        CHECK(extra.ServerClosed());
    }
    // The internal surface doesn't count against it.
    {
        RawPipeClient overlay;
        REQUIRE(overlay.Connected());
        REQUIRE(overlay.Send(Hello()));
        CHECK(OkIs(overlay.Receive(), true));
    }
    apps.front().reset();
    bool admitted = false;
    for (int attempt = 0; attempt < 50 && !admitted; ++attempt) {  // the server sees the close a moment later
        RawPipeClient again;
        REQUIRE(again.Connected());
        REQUIRE(again.Send(ApiHello()));
        admitted = OkIs(again.Receive(), true);
        if (!admitted) Sleep(20);
    }
    CHECK(admitted);
}

TEST_CASE("pipe: a client that never says hello is dropped") {
    TwoSurfaceServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    const ULONGLONG start = GetTickCount64();
    CHECK(client.ServerClosed(kHelloTimeoutMs + 3000));
    const ULONGLONG waited = GetTickCount64() - start;
    CHECK(waited >= kHelloTimeoutMs - 100);
    CHECK(waited < kHelloTimeoutMs + 3000);
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

namespace {

// An API server with an event hub; status answers {"ok":true}.
class EventServer {
public:
    EventServer()
        : server_([](const JsonValue&) { return JsonValue::MakeObject(); },
                  [](const JsonValue&, const PipeCaller&) { return JsonValue::MakeObject(); }, nullptr, &hub) {}

    EventHub   hub;  // declared first: outlives the server's connections
    PipeServer server_;
};

JsonValue Subscribe(std::initializer_list<const char*> names, bool all = false) {
    JsonValue r = Request("subscribe", 2);
    if (!all) {
        JsonValue list = JsonValue::MakeArray();
        for (const char* n : names) list.arr.push_back(JsonValue::MakeStr(n));
        r.Set("events", std::move(list));
    }
    return r;
}

// Polls until the hub hears someone: the subscribe reply is written after the subscription is set.
bool WaitActive(const EventHub& hub) {
    for (int i = 0; i < 200 && !hub.Active(); ++i) Sleep(5);
    return hub.Active();
}

}  // namespace

TEST_CASE("pipe events: subscribe answers with the names taken, then events arrive unasked") {
    EventServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(ApiHello()));
    REQUIRE(OkIs(client.Receive(), true));

    REQUIRE(client.Send(Subscribe({"replay", "made_up"})));
    const JsonValue reply = client.Receive();
    CHECK(OkIs(reply, true));
    CHECK(StrOf(reply, "type") == "subscribe");
    const JsonValue* taken = reply.Find("events");
    REQUIRE(taken);
    REQUIRE(taken->arr.size() == 1);
    CHECK(taken->arr[0].str == "replay");
    REQUIRE(WaitActive(s.hub));

    // The connection's thread sits in a read: the event must still go out at once.
    Sleep(50);
    s.hub.Publish("buffering", JsonValue::MakeObject());  // not asked for
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("replay", JsonValue::MakeBool(false));
    s.hub.Publish("replay", fields);
    const JsonValue ev = client.Receive();
    CHECK(StrOf(ev, "type") == "event");
    CHECK(StrOf(ev, "event") == "replay");
    const JsonValue* on = ev.Find("replay");
    CHECK((on && on->type == JsonValue::Type::Bool && !on->b));

    // Requests still work on a subscribed connection.
    REQUIRE(client.Send(Request("status", 3)));
    const JsonValue status = client.Receive();
    CHECK(OkIs(status, true));
    CHECK(NumOf(status, "id") == 3);
}

TEST_CASE("pipe events: a bad events list is refused, and omitting it takes every event") {
    EventServer s;
    REQUIRE(s.server_.Start());
    RawPipeClient client;
    REQUIRE(client.Connected());
    REQUIRE(client.Send(ApiHello()));
    REQUIRE(OkIs(client.Receive(), true));

    JsonValue bad = Request("subscribe", 2);
    bad.Set("events", JsonValue::MakeStr("replay"));
    REQUIRE(client.Send(bad));
    const JsonValue refused = client.Receive();
    CHECK(OkIs(refused, false));
    CHECK(StrOf(refused, "error") == "bad_request");
    CHECK(!s.hub.Active());

    REQUIRE(client.Send(Subscribe({}, true)));
    const JsonValue reply = client.Receive();
    CHECK(OkIs(reply, true));
    const JsonValue* taken = reply.Find("events");
    REQUIRE(taken);
    CHECK(taken->arr.size() == std::size(kEventNames));
}

TEST_CASE("pipe events: the exit event is flushed before Stop, and a closed client stops listening") {
    EventServer s;
    REQUIRE(s.server_.Start());
    {
        RawPipeClient client;
        REQUIRE(client.Connected());
        REQUIRE(client.Send(ApiHello()));
        REQUIRE(OkIs(client.Receive(), true));
        REQUIRE(client.Send(Subscribe({"exiting"})));
        REQUIRE(OkIs(client.Receive(), true));
        REQUIRE(WaitActive(s.hub));

        s.hub.Publish("exiting", JsonValue::MakeObject());
        s.hub.Close();
        CHECK(s.hub.WaitDrained(2000));
        CHECK(StrOf(client.Receive(), "event") == "exiting");

        auto stopped = std::async(std::launch::async, [&] { s.server_.Stop(); });
        CHECK(stopped.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(client.ServerClosed());
    }
    CHECK(!s.hub.Active());
}

TEST_CASE("pipe events: a client that disconnects unsubscribes") {
    EventServer s;
    REQUIRE(s.server_.Start());
    {
        RawPipeClient client;
        REQUIRE(client.Connected());
        REQUIRE(client.Send(ApiHello()));
        REQUIRE(OkIs(client.Receive(), true));
        REQUIRE(client.Send(Subscribe({"replay"})));
        REQUIRE(OkIs(client.Receive(), true));
        REQUIRE(WaitActive(s.hub));
    }
    for (int i = 0; i < 200 && s.hub.Active(); ++i) Sleep(5);
    CHECK(!s.hub.Active());
}

}  // namespace ombra::test
