// Named pipe transport for protocol.h.
//
// One accept thread, one worker thread per connected client, newline-delimited compact JSON
// framing (json.h's WriteJsonCompact on the way out; a line is read up to '\n' and handed to
// ParseJson on the way in). The transport owns the version handshake: the first message on a
// connection must be {"type":"hello","id":...,"v":kProtocolVersion} or the connection is closed
// with an error response first — application code (the Handler) never sees an unversioned or
// wrong-version client.
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

#include "ombra/json.h"
#include "ombra/paths.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
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

class PipeServer {
public:
    // Called once per complete, post-handshake message. Returning a JsonValue whose "ok"/"id"
    // fields are unset is fine — the transport fills in "id" from the request and defaults "ok"
    // to true if the handler didn't set it explicitly false.
    using Handler = std::function<JsonValue(const JsonValue& request)>;

    explicit PipeServer(Handler handler);
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

    // Signals the accept loop and every client worker to exit, then joins them all. Safe to call
    // from any thread except a client worker thread itself (a Handler must never call Stop()).
    void Stop();

private:
    void AcceptLoop(HANDLE first);  // `first`: the instance Start() created
    void ServeClient(HANDLE pipe);

    Handler                  handler_;
    std::atomic<bool>        running_{false};
    HANDLE                   instance_mutex_ = nullptr;  // single-instance guard, see pipe.cpp
    HANDLE                   stop_event_ = nullptr;      // manual-reset; every pipe wait includes it
    std::thread              accept_thread_;
    std::mutex                client_threads_mutex_;
    std::vector<std::thread>  client_threads_;
};

}  // namespace ombra
