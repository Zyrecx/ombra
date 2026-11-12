// flight_recorder.h: the summary covers the window before the newest tick, marks and counts the
// flags, and survives wrap-around. Headless.

#include <doctest/doctest.h>

#include "ombra/flight_recorder.h"

#include <windows.h>

#include <string>

namespace ombra::test {

namespace {
int64_t Freq() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}
}  // namespace

TEST_CASE("flight recorder: an empty recorder says so") {
    FlightRecorder r("empty");
    CHECK(r.Summary(2.0) == "flight recorder empty: no ticks recorded");
}

TEST_CASE("flight recorder: the summary covers the last seconds, with counts and marks") {
    FlightRecorder r("DISPLAY1");
    const int64_t  f = Freq();
    // 10 s at 60 fps, wrapping the 1024-slot ring; the last 2 s hold one slow keyframe and a resync.
    for (int i = 0; i < 600; ++i) {
        FlightRecord rec;
        rec.qpc       = 1000000 + static_cast<int64_t>(i) * f / 60;
        rec.encode_us = 4000;
        rec.au_bytes  = 20 * 1024;
        if (i == 0) rec.flags |= FlightRecord::kFirst;
        if (i == 590) {
            rec.encode_us = 41000;
            rec.au_bytes  = 300 * 1024;
            rec.flags |= FlightRecord::kKeyframe | FlightRecord::kLate;
        }
        if (i == 595) {
            rec.flags |= FlightRecord::kResync;
            rec.skipped = 3;
        }
        r.Push(rec);
    }
    for (int i = 600; i < 1500; ++i) {  // more than the capacity: older slots are overwritten
        FlightRecord rec;
        rec.qpc       = 1000000 + static_cast<int64_t>(i) * f / 60;
        rec.encode_us = 5000;
        r.Push(rec);
    }
    const std::string s = r.Summary(2.0);
    INFO(s);
    CHECK(s.find("flight recorder DISPLAY1, last 2.0 s: 121 ticks") != std::string::npos);
    CHECK(s.find("0 late") != std::string::npos);  // the slow tick is long gone

    FlightRecorder r2("x");
    for (int i = 0; i < 600; ++i) {
        FlightRecord rec;
        rec.qpc       = static_cast<int64_t>(i) * f / 60;
        rec.encode_us = i == 590 ? 41000 : 4000;
        if (i == 590) rec.flags = FlightRecord::kKeyframe | FlightRecord::kLate;
        if (i == 595) {
            rec.flags   = FlightRecord::kResync;
            rec.skipped = 3;
        }
        r2.Push(rec);
    }
    const std::string t = r2.Summary(2.0);
    INFO(t);
    CHECK(t.find("121 ticks, 1 late, 0 duplicate, 1 resyncs (3 ticks dropped), 1 keyframes") != std::string::npos);
    CHECK(t.find("max 41.0 at -0.15 s") != std::string::npos);
    CHECK(t.find(" 41LK") != std::string::npos);
    CHECK(t.find(" 4R") != std::string::npos);
    CHECK(t.find(" 4F") == std::string::npos);  // the first tick is outside the window
}

// Measure the cost of Push on the pacer's tick path.
TEST_CASE("flight recorder: Push costs nanoseconds") {
    FlightRecorder r("bench");
    constexpr int  kN = 1'000'000;
    LARGE_INTEGER  a{}, b{};
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kN; ++i) {
        FlightRecord rec;
        rec.qpc       = i;
        rec.encode_us = static_cast<uint32_t>(i);
        r.Push(rec);
    }
    QueryPerformanceCounter(&b);
    const double ns = static_cast<double>(b.QuadPart - a.QuadPart) * 1e9 / static_cast<double>(Freq()) / kN;
    MESSAGE("FlightRecorder::Push: " << ns << " ns per call");
    CHECK(ns < 1000.0);
}

TEST_CASE("flight recorder: RawViews lists live recorders only") {
    FlightRecorder::RawView v[8];
    const size_t            before = FlightRecorder::RawViews(v, 8);
    {
        FlightRecorder r("live");
        CHECK(FlightRecorder::RawViews(v, 8) == before + 1);
    }
    CHECK(FlightRecorder::RawViews(v, 8) == before);
}

}  // namespace ombra::test
