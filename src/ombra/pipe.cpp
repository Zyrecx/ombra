#include "ombra/pipe.h"
#include "ombra/brand.h"

#include "ombra/log.h"
#include "ombra/protocol.h"

#include <sddl.h>

#include <algorithm>
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

// Completes the overlapped call that returned `started`, or abandons it once `stop` is set.
// Returns false on failure or stop; GetLastError() then holds the reason.
bool FinishIo(HANDLE pipe, PipeIo& io, BOOL started, HANDLE stop, DWORD& n) {
    n = 0;
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    const HANDLE waits[2] = {io.ov.hEvent, stop};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
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
bool ReadLine(HANDLE pipe, PipeIo& io, HANDLE stop, std::string& pending, std::string& out) {
    size_t searched = 0;  // only new bytes: rescanning all of `pending` per read is quadratic
    for (;;) {
        const size_t nl = pending.find('\n', searched);
        if (nl != std::string::npos) {
            out = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            return true;
        }
        searched = pending.size();
        char  buf[256];
        DWORD n = 0;
        const BOOL ok = ReadFile(pipe, buf, sizeof(buf), nullptr, io.Next());
        if (!FinishIo(pipe, io, ok, stop, n) || n == 0) return false;
        pending.append(buf, n);
        if (pending.size() > kMaxMessageBytes) {
            Log::Warn("pipe: dropping a client whose message passed %zu bytes", kMaxMessageBytes);
            return false;
        }
    }
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

PipeServer::PipeServer(Handler handler)
    : handler_(std::move(handler)), stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

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

void PipeServer::ServeClient(HANDLE pipe) {
    // The version handshake is the transport's job (protocol.h), not the Handler's: the first
    // message must be {"type":"hello","v":kProtocolVersion,...} before anything else is accepted.
    bool handshaked = false;
    std::string pending;  // bytes read off the pipe past the last message's '\n' — see ReadLine
    PipeIo io;
    auto write = [&](const JsonValue& resp) {
        return WriteAll(pipe, io, stop_event_, WriteJsonCompact(resp) + "\n");
    };

    while (running_.load()) {
        std::string line;
        if (!ReadLine(pipe, io, stop_event_, pending, line)) break;
        if (line.empty()) continue;

        JsonValue request;
        try {
            request = ParseJson(line);
        } catch (const std::string& err) {
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("ok", JsonValue::MakeBool(false));
            resp.Set("error", JsonValue::MakeStr("malformed JSON: " + err));
            write(resp);
            break;  // framing is now suspect for this connection — close rather than guess
        }

        const JsonValue* id_field = request.Find("id");
        auto respond_with_id = [&](JsonValue resp) {
            if (id_field) resp.Set("id", *id_field);
            return resp;
        };

        if (!handshaked) {
            const JsonValue* type = request.Find("type");
            const JsonValue* v    = request.Find("v");
            if (!type || type->type != JsonValue::Type::String || type->str != "hello" ||
                !v || v->type != JsonValue::Type::Number ||
                static_cast<int>(v->num) != kProtocolVersion) {
                JsonValue resp = JsonValue::MakeObject();
                resp.Set("type", JsonValue::MakeStr("hello"));
                resp.Set("ok", JsonValue::MakeBool(false));
                resp.Set("error", JsonValue::MakeStr(
                    "expected {\"type\":\"hello\",\"v\":" + std::to_string(kProtocolVersion) +
                    ",...} as the first message"));
                write(respond_with_id(resp));
                break;
            }
            handshaked = true;
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("type", JsonValue::MakeStr("hello"));
            resp.Set("ok", JsonValue::MakeBool(true));
            resp.Set("v", JsonValue::MakeNum(kProtocolVersion));
            if (!write(respond_with_id(resp))) break;
            continue;
        }

        JsonValue resp = handler_(request);
        if (!resp.Find("ok")) resp.Set("ok", JsonValue::MakeBool(true));
        resp = respond_with_id(resp);
        if (!write(resp)) break;
    }

    // Each client has its own instance, so closing it is the disconnect, and what's written stays
    // readable until the client closes its end. DisconnectNamedPipe would discard that, and the
    // FlushFileBuffers it needs blocks with no way out on a client that never reads, hanging Stop().
    CloseHandle(pipe);
}

}  // namespace ombra
