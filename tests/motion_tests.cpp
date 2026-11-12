// Motion (ui/motion.h): fades on a hand-driven clock, and who keeps the frame clock on.
#include <doctest/doctest.h>

#include "ombra-ui/ui/motion.h"
#include "ombra-ui/ui/theme.h"

using namespace ombra_ui;

namespace {

double g_now = 1000.0;
double TestClock() { return g_now; }

// Motion on, on the test clock, whatever Windows' "Animation effects" says; restored after.
struct MotionOn {
    MotionOn() {
        g_now = 1000.0;
        motion::SetClockForTest(TestClock);
        motion::SetForcedOff(false);
        if (!motion::Enabled()) skip = true;
        motion::TakeRunning();
    }
    ~MotionOn() {
        motion::SetClockForTest(nullptr);
        motion::SetForcedOff(false);
    }
    bool skip = false;
};

}  // namespace

TEST_CASE("motion: a fade runs for its duration, then rests and asks for no frames") {
    MotionOn m;
    if (m.skip) { MESSAGE("Animation effects are off in Windows: nothing to time"); return; }
    Fade f;
    CHECK(f.Track(false, kFadeMs) == 0.0f);  // first read: the state as it is, no fade in
    CHECK_FALSE(motion::TakeRunning());
    CHECK(f.Track(true, kFadeMs) == 0.0f);
    CHECK(motion::TakeRunning());
    g_now += kFadeMs / 2.0;
    const float mid = f.Track(true, kFadeMs);
    CHECK(mid > 0.5f);  // eased out: past half way at half time
    CHECK(mid < 1.0f);
    CHECK(motion::TakeRunning());
    g_now += kFadeMs;
    CHECK(f.Track(true, kFadeMs) == 1.0f);
    CHECK_FALSE(motion::TakeRunning());  // at rest: the host stops its clock
}

TEST_CASE("motion: a reversal starts from where the fade is and takes the time left") {
    MotionOn m;
    if (m.skip) return;
    Fade f;
    f.Track(false, kFadeMs);
    f.Track(true, kFadeMs);
    g_now += kFadeMs / 2.0;
    const float at = f.Track(true, kFadeMs);
    CHECK(f.Track(false, kFadeMs) == doctest::Approx(at));  // no jump
    g_now += kFadeMs * at + 1.0;  // the distance back, not a whole fade
    CHECK(f.Track(false, kFadeMs) == 0.0f);
}

TEST_CASE("motion: off, every fade is its end state at once") {
    MotionOn m;
    motion::SetForcedOff(true);
    Fade f;
    f.Track(false, kFadeMs);
    CHECK(f.Track(true, kFadeMs) == 1.0f);
    CHECK(f.Track(false, kFadeMs) == 0.0f);
    ItemFade items;
    items.Track(2, kFadeMs);
    CHECK(items.Amount(2) == 1.0f);
    CHECK(items.Amount(1) == 0.0f);
    CHECK_FALSE(motion::TakeRunning());
}

TEST_CASE("motion: hover moving between items fades one out while the next comes in") {
    MotionOn m;
    if (m.skip) return;
    ItemFade items;
    items.Track(-1, kFadeMs);
    items.Track(0, kFadeMs);
    g_now += kFadeMs * 2.0;
    CHECK(items.Amount(0) == 1.0f);
    items.Track(1, kFadeMs);
    CHECK(items.Amount(0) == 1.0f);
    CHECK(items.Amount(1) == 0.0f);
    g_now += kFadeMs / 2.0;
    CHECK(items.Amount(0) < 1.0f);
    CHECK(items.Amount(1) > 0.0f);
    g_now += kFadeMs;
    CHECK(items.Amount(0) == 0.0f);
    CHECK(items.Amount(1) == 1.0f);
}

TEST_CASE("motion: Clips playback and a fade share the frame clock without stopping each other") {
    FrameDemand d;
    d.clips = true;                     // a clip is playing
    d.motion = true;                    // and a hover fade starts
    d.clips = false;                    // Clips stops playback mid-fade
    CHECK(d.Running());                 // the fade still gets its frames
    CHECK(d.PaintOnTick(false));
    d = {};
    d.clips = true;
    d.motion = true;
    d.motion = false;                   // the fade ends while the clip plays
    CHECK(d.Running());
    CHECK(d.PaintOnTick(true));         // a new video frame still paints
    CHECK_FALSE(d.PaintOnTick(false));  // and a tick without one paints nothing
    d.clips = false;
    CHECK_FALSE(d.Running());           // both done: no clock, no redraws at rest
}
