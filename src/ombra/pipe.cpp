#include "ombra/pipe.h"
#include "ombra/brand.h"

#include "ombra/log.h"
#include "ombra/protocol.h"
#include "ombra/version.h"

#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace ombra {

namespace {

constexpr DWORD kBufferSize = 8192;
// A request is a small JSON line. A client that never sends a newline is dropped here rather than
// growing the engine's memory until an allocation fails on a worker thread.
constexpr size_t kMaxMessageBytes = 16 * 1024 * 1024;

// Only the creating user and Local System get access — PIPE_REJECT_REMOTE_CLIENTS already blocks
// other machines, but a per-user DACL is what keeps another *local* account on the same machine
// from connecting: this pipe can trigger a save and rewrite output.root, so it isn't
// a general local service open to every account.
PSECURITY_DESCRIPTOR MakeOwnerOnlySecurityDescriptor() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &sd, nullptr)) {
        Log::Error("pipe: ConvertStringSecurityDescriptorToSecurityDescriptorW failed (%lu)",
                   GetLastError());
        return nullptr;
    }
    return sd;
}

// Pipe instances are overlapped so Stop() can wake a blocked connect/read/write with an event.
// The synchronous alternative, CancelSynchronousIo from Stop(), itself blocked forever when it hit
// a thread just entering ConnectNamedPipe (ombra-tests hung ~1 run in 3).
struct PipeIo {
    OVERLAPPED ov{};
    PipeIo() { ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~PipeIo() { if (ov.hEvent) CloseHandle(ov.hEvent); }
    PipeIo(const PipeIo&) = delete;
    PipeIo& operator=(const PipeIo&) = delete;
    OVERLAPPED* Next() {
        const HANDLE e = ov.hEvent;
        ov = {};
        ov.hEvent = e;
        return &ov;
    }
};

// Completes the overlapped call that returned `started`, or abandons it once `stop` is set or
// `timeout_ms` passes. Returns false on failure, stop or timeout.
bool FinishIo(HANDLE pipe, PipeIo& io, BOOL started, HANDLE stop, DWORD& n, DWORD timeout_ms = INFINITE) {
    n = 0;
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    const HANDLE waits[2] = {io.ov.hEvent, stop};
    if (WaitForMultipleObjects(2, waits, FALSE, timeout_ms) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &io.ov);
        GetOverlappedResult(pipe, &io.ov, &n, TRUE);  // the kernel owns `ov` until the cancel lands
        return false;
    }
    return GetOverlappedResult(pipe, &io.ov, &n, FALSE) != FALSE;
}

// Reads one newline-delimited message, using `pending` to carry bytes already read off the pipe
// that belong to the *next* message. A byte-mode pipe makes no promise that one WriteFile on the
// client end is one ReadFile here — two small messages written in a single call commonly arrive
// together — so `pending` must live with the connection (ServeClient's loop), not this call, or
// everything after the first '\n' in a chunk is silently lost. Returns false on disconnect, error,
// or stop.
// `deadline` (GetTickCount64, 0 for none) bounds the whole line.
bool ReadLine(HANDLE pipe, PipeIo& io, HANDLE stop, std::string& pending, std::string& out,
              size_t max_bytes, ULONGLONG deadline) {
    size_t searched = 0;  // only new bytes: rescanning all of `pending` per read is quadratic
    for (;;) {
        const size_t nl  = pending.find('\n', searched);
        const size_t len = nl == std::string::npos ? pending.size() : nl;
        if (len > max_bytes) {
            Log::Warn("pipe: dropping a client whose message passed %zu bytes", max_bytes);
            return false;
        }
        if (nl != std::string::npos) {
            out = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            return true;
        }
        searched = pending.size();
        char  buf[256];
        DWORD n = 0;
        DWORD timeout = INFINITE;
        if (deadline) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            timeout = static_cast<DWORD>(deadline - now);
        }
        const BOOL ok = ReadFile(pipe, buf, sizeof(buf), nullptr, io.Next());
        if (!FinishIo(pipe, io, ok, stop, n, timeout) || n == 0) return false;
        pending.append(buf, n);
    }
}

// FILE_ID_INFO of the file at `path`: the same file under any spelling (short names, subst
// drives, \\?\ prefixes) compares equal.
std::optional<FILE_ID_INFO> FileIdentity(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_ID_INFO id{};
    const bool ok = GetFileInformationByHandleEx(f, FileIdInfo, &id, sizeof(id)) != FALSE;
    CloseHandle(f);
    if (!ok) return std::nullopt;
    return id;
}

// Image paths can reach 32767 characters; MAX_PATH-sized buffers would fail a long install path.
constexpr DWORD kMaxImagePath = 32768;

// The full image path of the process on the other end of `pipe`, empty when it can't be read.
std::wstring ClientImagePath(HANDLE pipe, DWORD& pid) {
    pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring path(kMaxImagePath, L'\0');
    DWORD size = kMaxImagePath;
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
    CloseHandle(process);
    path.resize(ok ? size : 0);
    return path;
}

JsonValue ErrorReply(const std::string& type, const char* code, const std::string& message) {
    JsonValue resp = JsonValue::MakeObject();
    resp.Set("type", JsonValue::MakeStr(type));
    resp.Set("ok", JsonValue::MakeBool(false));
    resp.Set("error", JsonValue::MakeStr(code));
    resp.Set("message", JsonValue::MakeStr(message));
    return resp;
}

std::string StringField(const JsonValue& obj, const char* key) {
    const JsonValue* v = obj.Find(key);
    return (v && v->type == JsonValue::Type::String) ? v->str : std::string{};
}

// An "app" name: 1-64 bytes, no control characters (it lands in logs and toasts).
bool ValidAppName(const std::string& app) {
    if (app.empty() || app.size() > 64) return false;
    return std::none_of(app.begin(), app.end(),
                        [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

bool WriteAll(HANDLE pipe, PipeIo& io, HANDLE stop, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        DWORD n = 0;
        const BOOL ok = WriteFile(pipe, data.data() + written,
                                  static_cast<DWORD>(data.size() - written), nullptr, io.Next());
        if (!FinishIo(pipe, io, ok, stop, n) || n == 0) return false;
        written += n;
    }
    return true;
}

// `first` adds FILE_FLAG_FIRST_PIPE_INSTANCE: without it, a process that created the name before
// the engine would keep receiving the overlay's requests (same user only: owner-only DACL, remote
// clients rejected).
HANDLE CreatePipeInstance(bool first) {
    PSECURITY_DESCRIPTOR sd = MakeOwnerOnlySecurityDescriptor();
    SECURITY_ATTRIBUTES  sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
    HANDLE pipe = CreateNamedPipeW(
        Instance().pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, sd ? &sa : nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (first && err == ERROR_ACCESS_DENIED) {
            Log::Warn("pipe: another process already owns %ls", Instance().pipe_name.c_str());
        } else {
            Log::Error("pipe: CreateNamedPipeW failed (%lu)", err);
        }
    }
    if (sd) LocalFree(sd);
    return pipe;
}

}  // namespace

bool IsSiblingOverlay(const std::wstring& exe) {
    if (exe.empty()) return false;
    std::wstring self(kMaxImagePath, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, self.data(), kMaxImagePath);
    if (n == 0 || n >= kMaxImagePath) return false;
    self.resize(n);
    self.erase(self.find_last_of(L'\\') + 1);
    const auto want = FileIdentity(self + L"ombra-ui.exe");
    const auto got  = FileIdentity(exe);
    return want && got && want->VolumeSerialNumber == got->VolumeSerialNumber &&
           std::memcmp(&want->FileId, &got->FileId, sizeof(FILE_ID_128)) == 0;
}

PipeServer::PipeServer(Handler internal, ApiHandler api, InternalGate gate)
    : handler_(std::move(internal)),
      api_handler_(std::move(api)),
      internal_gate_(gate ? std::move(gate) : InternalGate([](DWORD pid, const std::wstring& exe) {
          return pid == GetCurrentProcessId() || IsSiblingOverlay(exe);
      })),
      stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

PipeServer::~PipeServer() {
    Stop();
    if (stop_event_) CloseHandle(stop_event_);
}

HANDLE AcquireEngineInstanceLock() {
    // A named mutex, not a "try to connect as a client" probe. A connect probe is racy against
    // the accept thread's own startup (CreateNamedPipeW/ConnectNamedPipe haven't necessarily run
    // yet by the time a second Start() checks), and multi-instance named pipes let
    // CreateNamedPipeW itself always succeed regardless of whether anyone is actually serving —
    // so it can't reliably answer "is another engine already running" on its own. A mutex answers
    // it independent of thread timing.
    HANDLE h = CreateMutexW(nullptr, TRUE, Instance().instance_mutex.c_str());
    if (!h || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log::Info("pipe: another " OMBRA_PRODUCT_NAME " engine instance already owns %ls", Instance().pipe_name.c_str());
        if (h) { CloseHandle(h); }
        return nullptr;
    }
    return h;
}

void ReleaseEngineInstanceLock(HANDLE lock) {
    if (!lock) return;
    ReleaseMutex(lock);
    CloseHandle(lock);
}

bool PipeServer::Start(HANDLE external_lock) {
    if (running_.exchange(true)) return true;  // already started
    if (!stop_event_) {
        Log::Error("pipe: CreateEventW failed");
        running_.store(false);
        return false;
    }
    ResetEvent(stop_event_);

    if (external_lock) {
        instance_mutex_ = external_lock;
    } else {
        instance_mutex_ = AcquireEngineInstanceLock();
        if (!instance_mutex_) {
            running_.store(false);
            return false;
        }
    }

    // The first instance is created here rather than on the accept thread so a squatted name fails
    // Start() instead of leaving an engine that nobody can reach.
    HANDLE first = CreatePipeInstance(true);
    if (first == INVALID_HANDLE_VALUE) {
        ReleaseMutex(instance_mutex_);
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
        running_.store(false);
        return false;
    }
    accept_thread_ = std::thread([this, first] { AcceptLoop(first); });
    return true;
}

void PipeServer::Stop() {
    if (!running_.exchange(false)) return;

    // Every blocking pipe call also waits on stop_event_ (FinishIo), so this wakes the accept loop
    // and every client worker wherever they are; an event set before a wait starts is still seen.
    SetEvent(stop_event_);
    // The accept thread first: once it's gone, nothing adds to client_threads_.
    if (accept_thread_.joinable()) accept_thread_.join();
    {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        for (auto& t : client_threads_) {
            if (t.joinable()) t.join();
        }
        client_threads_.clear();
    }

    if (instance_mutex_) {
        ReleaseMutex(instance_mutex_);
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
    }
}

void PipeServer::AcceptLoop(HANDLE first) {
    Log::SetThreadName("pipe-accept");
    while (running_.load()) {
        HANDLE pipe = first ? std::exchange(first, nullptr) : CreatePipeInstance(false);
        if (pipe == INVALID_HANDLE_VALUE) break;

        // ERROR_PIPE_CONNECTED: a client connected between CreateNamedPipeW and this call.
        PipeIo     io;
        const BOOL started   = ConnectNamedPipe(pipe, io.Next());
        DWORD      unused    = 0;
        const bool connected = started || GetLastError() == ERROR_PIPE_CONNECTED ||
                               FinishIo(pipe, io, FALSE, stop_event_, unused);
        if (!running_.load()) {
            // Stop() fired while this instance was being set up. Tear it down rather than serving
            // a client on a server that's shutting down.
            CloseHandle(pipe);
            break;
        }
        if (!connected) {
            Log::Warn("pipe: ConnectNamedPipe failed (%lu) — dropping this instance", GetLastError());
            CloseHandle(pipe);
            continue;
        }

        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        // Reap threads for clients that have already disconnected before adding a new one —
        // otherwise an always-resident engine accumulates one dead thread
        // handle per past connection for the life of the process. WaitForSingleObject with a 0
        // timeout is non-blocking: it only removes threads that have already returned from
        // ServeClient.
        client_threads_.erase(
            std::remove_if(client_threads_.begin(), client_threads_.end(),
                            [](std::thread& t) {
                                if (!t.joinable()) return false;
                                if (WaitForSingleObject(t.native_handle(), 0) != WAIT_OBJECT_0)
                                    return false;
                                t.join();
                                return true;
                            }),
            client_threads_.end());
        client_threads_.emplace_back([this, pipe] {
            Log::SetThreadName("pipe-client");
            ServeClient(pipe);
        });
    }
    if (first) CloseHandle(first);  // Stop() before the loop ran once
}

std::optional<PipeSurface> PipeServer::Hello(const JsonValue& req, HANDLE pipe, PipeCaller& caller,
                                             JsonValue& resp) {
    resp = JsonValue::MakeObject();
    resp.Set("type", JsonValue::MakeStr("hello"));
    auto refuse = [&](const char* code, const std::string& message) {
        resp = ErrorReply("hello", code, message);
        return std::nullopt;
    };
    // A whole number, or -1 when the field is missing or anything else.
    auto version_of = [&](const char* key) {
        const JsonValue* v = req.Find(key);
        if (!v || v->type != JsonValue::Type::Number || !(v->num >= 0 && v->num <= 1e6) ||
            v->num != static_cast<double>(static_cast<int>(v->num)))
            return -1;
        return static_cast<int>(v->num);
    };
    if (StringField(req, "type") != "hello")
        return refuse("bad_request", "the first message must be {\"type\":\"hello\",\"api\":" +
                                         std::to_string(kApiVersionMax) + ",\"app\":\"...\"}");

    caller.exe = ClientImagePath(pipe, caller.pid);

    if (req.Find("api")) {
        const int api = version_of("api");
        if (api < kApiVersionMin || api > kApiVersionMax) {
            refuse("unsupported_version", "this Ombra speaks API versions " +
                                              std::to_string(kApiVersionMin) + " to " +
                                              std::to_string(kApiVersionMax));
            resp.Set("min", JsonValue::MakeNum(kApiVersionMin));
            resp.Set("max", JsonValue::MakeNum(kApiVersionMax));
            return std::nullopt;
        }
        const JsonValue* id = req.Find("id");
        if (!id || (id->type != JsonValue::Type::Number && id->type != JsonValue::Type::String))
            return refuse("bad_request", "every request needs an \"id\" (number or string)");
        caller.app = StringField(req, "app");
        if (!ValidAppName(caller.app))
            return refuse("bad_request", "\"app\" must name the caller: 1-64 characters, no control characters");
        if (!api_handler_) return refuse("not_allowed", "this engine takes no API connections");
        if (api_clients_.fetch_add(1) >= kMaxApiClients) {
            api_clients_.fetch_sub(1);
            Log::Warn("pipe: refused app \"%s\": %d app connections already open", caller.app.c_str(),
                      kMaxApiClients);
            return refuse("too_many_connections", "too many apps are connected to Ombra");
        }
        Log::Info("pipe: app \"%s\" connected (pid %lu, %ls)", caller.app.c_str(), caller.pid,
                  caller.exe.empty() ? L"image unknown" : caller.exe.c_str());
        resp.Set("ok", JsonValue::MakeBool(true));
        resp.Set("api", JsonValue::MakeNum(api));
        resp.Set("engine", JsonValue::MakeStr(AppVersion()));
        return PipeSurface::kApi;
    }

    if (req.Find("v")) {
        if (!internal_gate_(caller.pid, caller.exe)) {
            Log::Warn("pipe: refused the internal protocol to pid %lu (%ls)", caller.pid,
                      caller.exe.empty() ? L"image unknown" : caller.exe.c_str());
            return refuse("not_allowed", "the internal protocol is only for ombra-ui; apps say hello with \"api\"");
        }
        if (version_of("v") != kProtocolVersion)
            return refuse("unsupported_version",
                          "expected internal protocol version " + std::to_string(kProtocolVersion));
        resp.Set("ok", JsonValue::MakeBool(true));
        resp.Set("v", JsonValue::MakeNum(kProtocolVersion));
        return PipeSurface::kInternal;
    }
    return refuse("bad_request", "hello needs \"api\" (the API version the client speaks)");
}

void PipeServer::ServeClient(HANDLE pipe) {
    // The hello is the transport's job (protocol.h), not the Handler's: it picks the surface, and
    // nothing reaches a Handler before it.
    std::optional<PipeSurface> surface;
    PipeCaller  caller;
    std::string pending;  // bytes read off the pipe past the last message's '\n' — see ReadLine
    PipeIo io;
    auto write = [&](const JsonValue& resp) {
        return WriteAll(pipe, io, stop_event_, WriteJsonCompact(resp) + "\n");
    };

    while (running_.load()) {
        const size_t max_bytes =
            surface == PipeSurface::kInternal ? kMaxMessageBytes : kMaxApiMessageBytes;
        std::string line;
        // A client that never says hello would hold this thread and its pipe instance forever.
        const ULONGLONG deadline = surface ? 0 : GetTickCount64() + kHelloTimeoutMs;
        if (!ReadLine(pipe, io, stop_event_, pending, line, max_bytes, deadline)) break;
        if (line.empty()) continue;

        JsonValue request;
        try {
            request = ParseJson(line);
        } catch (const std::string& err) {
            write(ErrorReply("error", "bad_message", "malformed JSON: " + err));
            break;  // framing is now suspect for this connection — close rather than guess
        }
        if (request.type != JsonValue::Type::Object) {
            write(ErrorReply("error", "bad_message", "a message must be a JSON object"));
            break;
        }

        const std::string type     = StringField(request, "type");
        const JsonValue*  id_field = request.Find("id");
        const bool        valid_id = id_field && (id_field->type == JsonValue::Type::Number ||
                                                  id_field->type == JsonValue::Type::String);
        auto reply = [&](JsonValue resp) {
            if (!resp.Find("type")) resp.Set("type", JsonValue::MakeStr(type));
            if (!resp.Find("ok")) resp.Set("ok", JsonValue::MakeBool(true));
            if (id_field) resp.Set("id", *id_field);
            return write(resp);
        };

        if (!surface) {
            JsonValue resp;
            surface = Hello(request, pipe, caller, resp);
            if (!reply(std::move(resp)) || !surface) break;
            continue;
        }

        if (*surface == PipeSurface::kInternal) {
            if (!reply(handler_(request))) break;
            continue;
        }
        if (!valid_id) {
            if (!reply(ErrorReply(type, "bad_request", "every request needs an \"id\" (number or string)")))
                break;
            continue;
        }
        if (type.empty() || type == "hello") {
            if (!reply(ErrorReply(type, "bad_request", type.empty() ? "missing \"type\"" : "already said hello")))
                break;
            continue;
        }
        if (!reply(api_handler_(request, caller))) break;
    }
    if (surface == PipeSurface::kApi) api_clients_.fetch_sub(1);

    // Each client has its own instance, so closing it is the disconnect, and what's written stays
    // readable until the client closes its end. DisconnectNamedPipe would discard that, and the
    // FlushFileBuffers it needs blocks with no way out on a client that never reads, hanging Stop().
    CloseHandle(pipe);
}

}  // namespace ombra
