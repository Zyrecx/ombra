// ombra-ui's client for the pipe protocol.h/pipe.h contract.
//
// A small, synchronous, blocking client: ombra-ui has exactly one thing to talk to (the one
// engine instance it either found or spawned) and no concurrent-request need, so
// there is nothing here pipe.h's own worker-per-client model needs mirroring. Uses the same raw
// Win32 CreateFileW/ReadFile/WriteFile calls and retry-on-ERROR_FILE_NOT_FOUND/ERROR_PIPE_BUSY
// logic as the pipe tests' raw client (tests/instance_tests.cpp).
#pragma once

#include "ombra/json.h"

#include <windows.h>

#include <optional>
#include <string>

namespace ombra_ui {

class PipeClient {
public:
    PipeClient() = default;
    virtual ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Retries CreateFileW against \\.\pipe\OmbraEngine for up to timeout_ms, treating
    // ERROR_FILE_NOT_FOUND (no instance created yet) and ERROR_PIPE_BUSY (an instance exists, all
    // busy) as retryable — see pipe.h's own comment on why both matter, not just the busy case.
    bool Connect(DWORD timeout_ms);

    // Sends {"type":"hello","id":<n>,"v":kProtocolVersion} and checks the response's "ok"/"v".
    // Must be the first call after Connect() succeeds — the server enforces this at the transport
    // layer and will reject anything else first.
    //
    // If the server responded with an explicit rejection (ok:false, e.g. a version mismatch —
    // exactly what kProtocolVersion exists to make legible), *reject_reason (if non-null) is set
    // to its "error" text and this returns false. Retrying won't fix that case — a caller should
    // fail fast, not treat it the same as "no response at all" (a still-starting or absent
    // engine, where retrying is the right move). *reject_reason is left untouched on any other
    // failure (timeout, transport error, malformed response) so a caller can tell the two apart.
    bool Hello(DWORD timeout_ms, std::string* reject_reason = nullptr);

    // Sends a request object (type + any other fields the caller already set) with a fresh "id",
    // and returns the parsed response, or std::nullopt on a transport/timeout/parse failure.
    //
    // Ombra's engine restarts itself for a capture.monitors change (EngineSelfRestart, main.cpp)
    // and that used to strand every ombra-ui session holding this
    // client: the write against the now-dead pipe instance fails immediately, Request() returned
    // nullopt, and every caller's failure path (ApplySettingChange/CommitAudioSourcesNoRealloc,
    // screens.cpp) only logs when a response object actually came back — a transport failure logs
    // nothing and repaints as if the click had simply done nothing. Found 2026-09-10 diagnosing
    // "the audio panel doesn't do anything": drag/ungroup/remove all worked perfectly against a
    // freshly-launched ombra-ui, and all silently no-op'd the moment the engine had restarted since
    // that launch — same click, same code, only the pipe status differed. Request() now retries
    // reads and value replacements once through a fresh Connect()+Hello() on a write/read failure.
    // Actions (including Action settings) are never repeated after an ambiguous failure.
    // A request issued shortly
    // after a restart finds the new engine instance instead of silently dying against the old one.
    // Virtual so headless UI tests (tests/ui_tests.cpp) can substitute a fake that serves canned
    // JSON instead of talking to a real engine over a real pipe.
    virtual std::optional<ombra::JsonValue> Request(ombra::JsonValue req, DWORD timeout_ms);

    bool Connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    void Close();

private:
    // Close()s the current handle (if any) and re-establishes the connection: Connect() then
    // Hello(), both against the same timeout_ms — reconnect_timeout_ms on Request()'s retry path is
    // deliberately generous (the new engine instance is still finishing its own startup, not just
    // slow to accept a pipe client) but still bounded, so a genuinely-gone engine fails the retry
    // instead of hanging the UI thread.
    bool Reconnect(DWORD timeout_ms);

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    int    next_id_ = 1;
};

}  // namespace ombra_ui
