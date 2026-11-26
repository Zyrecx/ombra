// Named pipe transport for protocol.h.
//
// One accept thread, one worker thread per connected client, newline-delimited compact JSON
// framing (json.h's WriteJsonCompact on the way out; a line is read up to '\n' and handed to
// ParseJson on the way in). The transport owns the hello (protocol.h): it picks the surface
// (internal for ombra-ui, the public API for apps) or closes the connection with an error first,
// so a Handler never sees an unversioned, wrong-version or ungated client.
//
// THREADING: Handler is invoked on the connected client's own worker thread, never the accept
// thread and never any thread main.cpp owns (the pump, the PipelineSupervisor watchdog). This
// mirrors why BufferController's watchdog is the only thing allowed to touch
// Capture/Encoder/Pacer (see controller.h) — a Handler that needs to reach PipelineSupervisor
// must go through PipelineSupervisor's own thread-safe accessors (CurrentParams(), SetArmed(),
// etc.), the same ones DoSave already calls from a third thread. This class adds no new
// cross-thread contract by itself.
//
// SECURITY: the pipe is created with PIPE_REJECT_REMOTE_CLIENTS and a DACL granting access only
// to the creating user — this pipe can trigger a save and rewrite output.root, so it is not a
// general local service open to every account on the machine.
#pragma once

#include "ombra/events.h"
#include "ombra/json.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

// The pipe name and the single-instance mutex name are paths.h's Instance().pipe_name and
// .instance_mutex: the installed names by default, per-folder names under OMBRA_HOME. ombra-ui
// reads them from there too (it probes the mutex with OpenMutexW before connecting or spawning).

// Acquires the single-instance mutex and returns it held, or nullptr if another instance already
// owns it (a Log::Error already named the reason). This used to happen only inside
// PipeServer::Start(), which wmain calls after the whole pipeline (GPU/capture/audio,
// several seconds) is already up — a second `ombra.exe` launched in that window would build a
// full second pipeline (second NVENC session, second WGC session, a 300MB+ ring) before dying on
// this same check. Callers should call this immediately after argument/config parsing, before any
// GPU/capture/audio setup, and pass the result to PipeServer::Start() below.
HANDLE AcquireEngineInstanceLock();

// Releases a lock returned by AcquireEngineInstanceLock, e.g. on a fatal exit before a PipeServer
// is ever constructed to take ownership of it.
void ReleaseEngineInstanceLock(HANDLE lock);

// Who is on the other end of an API connection: the hello's "app", and what the kernel says.
struct PipeCaller {
    std::string  app;
    DWORD        pid = 0;
    std::wstring exe;  // full image path, empty when it couldn't be read
    // Ombra's own command line (IsCommandLineClient): the one app allowed to act until consent
    // exists. Decided by the image, never by the "app" name, which any caller can claim.
    bool command_line = false;
};

// True when `exe` is the file `name` beside this process's own image (compared by file identity).
bool IsSiblingImage(const std::wstring& exe, const wchar_t* name);
// True when `exe` is the ombra-ui.exe beside this process's own image: the internal gate.
inline bool IsSiblingOverlay(const std::wstring& exe) { return IsSiblingImage(exe, L"ombra-ui.exe"); }
// Ombra's command line: ombra.com or ombra.exe beside the engine, or this process (tests).
bool IsCommandLineClient(DWORD pid, const std::wstring& exe);

class PipeServer {
public:
    // Called once per complete, post-hello message. Returning a JsonValue whose "type"/"ok"/"id"
    // fields are unset is fine — the transport fills in "type" and "id" from the request and
    // defaults "ok" to true if the handler didn't set it explicitly false.
    using Handler    = std::function<JsonValue(const JsonValue& request)>;
    using ApiHandler = std::function<JsonValue(const JsonValue& request, const PipeCaller& caller)>;
    // Decides whether a client may use the internal surface. The default allows this process
    // (in-process tests) and IsSiblingOverlay.
    using InternalGate = std::function<bool(DWORD pid, const std::wstring& exe)>;

    // Without `api`, an API hello is refused with not_allowed. With `events` (outliving this), the
    // `subscribe` is the transport's on both surfaces: it pushes the hub's events down that connection.
    explicit PipeServer(Handler internal, ApiHandler api = nullptr, InternalGate gate = nullptr,
                        EventHub* events = nullptr);
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Starts the accept-loop thread. `external_lock`, if non-null, must be a handle previously
    // returned by AcquireEngineInstanceLock — PipeServer adopts it (and releases/closes it in
    // Stop()) instead of acquiring its own, since a second CreateMutexW in the same process
    // wouldn't take ownership and would misreport "already running". If null (anything that
    // didn't hoist the lock), Start() acquires it itself, same as
    // before this step.
    //
    // Returns false if the pipe name/mutex is already taken (another PipeServer — in this process
    // or another ombra.exe instance — already owns it, or any other process created the pipe name
    // first) or DACL/pipe setup fails; either way a
    // Log::Error already named the reason.
    bool Start(HANDLE external_lock = nullptr);

    // Before Start: who counts as Ombra's own command line (default IsCommandLineClient). Tests
    // run in this process, which the default counts.
    void SetCommandLineGate(InternalGate gate) { command_line_gate_ = std::move(gate); }

    // Signals the accept loop and every client worker to exit, then joins them all. Safe to call
    // from any thread except a client worker thread itself (a Handler must never call Stop()).
    void Stop();

private:
    void AcceptLoop(HANDLE first);  // `first`: the instance Start() created
    void ServeClient(HANDLE pipe);
    // Answers the connection's first message into `resp`; the surface it opened, or nullopt
    // (refused: the connection closes after `resp`).
    std::optional<PipeSurface> Hello(const JsonValue& req, HANDLE pipe, PipeCaller& caller, JsonValue& resp);
    // Answers `subscribe`, making the connection's subscription on the first. `privileged`: it
    // hears the paths only Ombra's own may (Subscription).
    JsonValue Subscribe(const JsonValue& req, bool privileged, std::unique_ptr<Subscription>& subscription);

    Handler                  handler_;
    ApiHandler               api_handler_;
    InternalGate             internal_gate_;
    InternalGate             command_line_gate_ = IsCommandLineClient;
    EventHub*                events_ = nullptr;
    std::atomic<bool>        running_{false};
    std::atomic<int>         api_clients_{0};  // live API connections, capped at kMaxApiClients
    std::atomic<int>         command_line_clients_{0};  // Ombra's own command line's, kMaxCommandLineClients
    HANDLE                   instance_mutex_ = nullptr;  // single-instance guard, see pipe.cpp
    HANDLE                   stop_event_ = nullptr;      // manual-reset; every pipe wait includes it
    std::thread              accept_thread_;
    std::mutex                client_threads_mutex_;
    std::vector<std::thread>  client_threads_;
};

}  // namespace ombra
