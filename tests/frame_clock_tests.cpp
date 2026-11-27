// FrameClock (frame_clock.h): ticks reach the window coalesced, and the UI thread never waits for
// a worker stuck in the compositor-clock wait (it doesn't return while the display is off).
#include <doctest/doctest.h>

#include "ombra-ui/frame_clock.h"

#include <chrono>
#include <optional>

using namespace ombra_ui;

namespace {

constexpr UINT kTick = WM_APP + 1;

// A compositor clock the test drives, one tick per Step(). Like the real one in Modern Standby, the
// wait ignores its handles and timeout: only a step ends it.
HANDLE g_entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // the worker is inside the wait
HANDLE g_step = CreateEventW(nullptr, FALSE, FALSE, nullptr);
DWORD WINAPI SteppedWait(UINT, const HANDLE*, DWORD) {
    SetEvent(g_entered);
    WaitForSingleObject(g_step, INFINITE);
    return WAIT_TIMEOUT;
}
bool Entered(DWORD ms = 2000) { return WaitForSingleObject(g_entered, ms) == WAIT_OBJECT_0; }
// One tick, then back inside the wait: whatever that tick posts has been posted.
bool Step() {
    SetEvent(g_step);
    return Entered();
}

struct MessageWindow {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ~MessageWindow() { DestroyWindow(hwnd); }
    int TakeTicks() const {
        int n = 0;
        MSG m;
        while (PeekMessageW(&m, hwnd, kTick, kTick, PM_REMOVE)) ++n;
        return n;
    }
};

template <class F>
double Ms(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("ui: stopping or dropping the frame clock doesn't wait for a stuck compositor wait") {
    MessageWindow w;
    REQUIRE(w.hwnd);
    ResetEvent(g_entered);
    ResetEvent(g_step);
    std::optional<FrameClock> clock;
    clock.emplace(kTick, &SteppedWait);
    clock->Start(w.hwnd);
    REQUIRE(Entered());
    CHECK(Ms([&] { clock->Stop(); }) < 500.0);
    clock->Start(w.hwnd);
    // The overlay's exit, which used to hang here until the display woke.
    CHECK(Ms([&] { clock.reset(); }) < 500.0);
    SetEvent(g_step);  // the display wakes: the worker sees the stop and leaves
    CHECK_FALSE(Entered(100));
    CHECK(w.TakeTicks() == 0);  // nothing posted for an owner that's gone
}

TEST_CASE("ui: the frame clock queues one tick until it's handled, and none once stopped") {
    MessageWindow w;
    REQUIRE(w.hwnd);
    ResetEvent(g_entered);
    ResetEvent(g_step);
    {
        FrameClock clock(kTick, &SteppedWait);
        clock.Start(w.hwnd);
        REQUIRE(Entered());
        REQUIRE(Step());
        CHECK(w.TakeTicks() == 1);
        REQUIRE(Step());
        CHECK(w.TakeTicks() == 0);  // the first isn't handled yet
        clock.TickHandled();
        REQUIRE(Step());
        CHECK(w.TakeTicks() == 1);

        clock.Stop();
        SetEvent(g_step);  // the wait under way when Stop came ends: no tick, and the worker parks
        CHECK_FALSE(Entered(100));
        CHECK(w.TakeTicks() == 0);

        clock.Start(w.hwnd);  // the same worker resumes, the unhandled tick forgotten
        REQUIRE(Entered());
        REQUIRE(Step());
        CHECK(w.TakeTicks() == 1);
    }
    SetEvent(g_step);  // let the worker out
    CHECK_FALSE(Entered(100));
}
