// The overlay's frame clock: one worker waits on DWM's compositor clock and posts one tick per
// refresh to a window (coalesced: never more than one queued), so clip frames land on vsync instead
// of the 15.6 ms system timer, which skipped 3-6% of a 60 fps clip's frames.
//
// DCompositionWaitForCompositorClock doesn't return while the display is off (Modern Standby):
// neither its timeout nor its stop handle wakes it. So the UI thread never waits for the worker.
// Stop only parks it, and destruction detaches it. Its state is shared, not borrowed from the owner,
// so a worker still stuck in the wait when the owner goes away touches nothing freed.
#pragma once

#include <windows.h>

#include <memory>

namespace ombra_ui {

class FrameClock {
public:
    // DCompositionWaitForCompositorClock's shape: WAIT_OBJECT_0 + i for handles[i], else a tick,
    // a timeout or WAIT_FAILED. Tests pass one that never returns.
    using WaitFn = DWORD(WINAPI*)(UINT count, const HANDLE* handles, DWORD timeout_ms);

    explicit FrameClock(UINT msg, WaitFn wait = nullptr);  // nullptr: the compositor clock
    ~FrameClock();
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    // Ticks start going to `hwnd` (the first call starts the worker). Repeating it is free.
    void Start(HWND hwnd);
    // No more ticks; returns at once even if the worker is inside the wait.
    void Stop();
    // The window took the queued tick, so the next one may be posted.
    void TickHandled();

private:
    struct State;
    static void Run(std::shared_ptr<State> s);  // the worker
    std::shared_ptr<State> state_;
    bool started_ = false;  // the worker exists
    bool running_ = false;  // Start without a Stop since
    HWND hwnd_ = nullptr;
};

}  // namespace ombra_ui
