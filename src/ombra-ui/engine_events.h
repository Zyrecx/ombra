// The overlay's live view of the engine: a pipe connection of its own that subscribes to the
// engine's events (protocol.h "subscribe") and waits, instead of asking get_status on a timer.
//
// One worker thread, parked in an overlapped read beside a stop event: nothing runs between
// events. It posts each event to the overlay window as `msg` with an EngineEvent in wParam; the
// overlay then asks get_status once (events say what changed, status says what is). A dropped
// connection (the engine exiting or restarting itself, or this side falling 64 events behind)
// posts kLost and reconnects once a second; each (re)connect subscribes first, then posts
// kConnected, so nothing raised in between goes unseen.
#pragma once

#include <windows.h>

#include <string>
#include <thread>

namespace ombra_ui {

enum class EngineEvent : WPARAM {
    kConnected,  // subscribed: anything known before now may be stale
    kLost,       // the connection dropped: the engine is gone or restarting
    kReplay,     // the Instant Replay switch changed
    kBuffering,  // footage started or stopped being kept
    kRecording,  // a recording started or stopped
    kGame,       // a game started or ended
};

class EngineEvents {
public:
    EngineEvents() = default;
    ~EngineEvents() { Stop(); }
    EngineEvents(const EngineEvents&) = delete;
    EngineEvents& operator=(const EngineEvents&) = delete;

    // Starts the worker (a no-op while one runs). `hwnd` must outlive the next Stop().
    void Start(HWND hwnd, UINT msg);
    // Signals the worker and joins it; returns within one I/O cancel, whatever the engine does.
    void Stop();
    bool Running() const { return worker_.joinable(); }

    // What an event line's name posts; false for names the overlay doesn't act on.
    static bool Classify(const std::string& name, EngineEvent& out);

private:
    void Run(HWND hwnd, UINT msg);

    std::thread worker_;
    HANDLE      stop_ = nullptr;
};

}  // namespace ombra_ui
