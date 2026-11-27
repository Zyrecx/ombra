#include "ombra-ui/frame_clock.h"

#include "ombra/log.h"

#include <dcomp.h>

#include <atomic>
#include <thread>

namespace ombra_ui {

struct FrameClock::State {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);    // set once: the owner is gone
    HANDLE demand = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // set while ticks are wanted
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> pending{false};  // a tick is queued and not yet handled
    UINT msg = 0;
    WaitFn wait = nullptr;

    ~State() {
        if (stop) CloseHandle(stop);
        if (demand) CloseHandle(demand);
    }
};

FrameClock::FrameClock(UINT msg, WaitFn wait) : state_(std::make_shared<State>()) {
    state_->msg = msg;
    state_->wait = wait ? wait : &DCompositionWaitForCompositorClock;
}

FrameClock::~FrameClock() {
    if (state_->demand) ResetEvent(state_->demand);
    if (state_->stop) SetEvent(state_->stop);
}

void FrameClock::Start(HWND hwnd) {
    if (!state_->stop || !state_->demand) return;
    if (running_ && hwnd == hwnd_) return;
    hwnd_ = hwnd;
    state_->hwnd = hwnd;
    state_->pending = false;  // a tick queued before a Stop may have gone to a window since closed
    running_ = true;
    SetEvent(state_->demand);
    if (!started_) {
        started_ = true;
        std::thread([s = state_] { Run(s); }).detach();
    }
}

void FrameClock::Stop() {
    if (!running_) return;
    running_ = false;
    ResetEvent(state_->demand);
}

void FrameClock::TickHandled() {
    state_->pending = false;
}

void FrameClock::Run(std::shared_ptr<State> s) {
    ombra::Log::SetThreadName("frame-clock");
    const HANDLE parked[2] = {s->stop, s->demand};
    for (;;) {
        if (WaitForMultipleObjects(2, parked, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
        // The timeout keeps frames coming if the clock idles (nothing else on screen changing).
        const DWORD r = s->wait(1, &s->stop, 20);
        // The owner may have gone while the wait ignored the stop handle (the display was off).
        if (r == WAIT_OBJECT_0 || WaitForSingleObject(s->stop, 0) == WAIT_OBJECT_0) return;
        if (r == WAIT_FAILED) Sleep(15);
        if (WaitForSingleObject(s->demand, 0) != WAIT_OBJECT_0) continue;  // stopped meanwhile
        // A post that fails (a full queue) must not leave every later tick suppressed.
        if (!s->pending.exchange(true) && !PostMessageW(s->hwnd.load(), s->msg, 0, 0)) s->pending = false;
    }
}

}  // namespace ombra_ui
