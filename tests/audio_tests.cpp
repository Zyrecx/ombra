// The audio target watcher's (main.cpp) start/absent/relaunch/retry state machine
// (DecideTargetWatcherAction, NextTargetWatcherState, audio.h/.cpp) — pulled out of the watcher's
// thread loop specifically so this is testable without a live WASAPI process-loopback client.
// Headless, no GPU, no audio hardware. FindTargetRootPid/WantedTargetCapture (the process-snapshot
// resolution half of the same watcher) are already covered in schema_tests.cpp; this file covers
// the other half: what the watcher does with the pid those functions resolve, across polls.

#include <doctest/doctest.h>

#include "ombra/audio.h"

#include <string>

namespace ombra::test {

using C = AudioTargetCapture;

TEST_CASE("audio watcher: a fresh (never-started) source always acts") {
    TargetWatcherState fresh;  // started=false
    auto d = DecideTargetWatcherAction(fresh, C::kNothing, 0, /*poll=*/0, /*snapshot_every_polls=*/3);
    CHECK(d.act);
    CHECK_FALSE(d.retrying);
}

TEST_CASE("audio watcher: nothing to do when already running exactly what's wanted") {
    TargetWatcherState cur{true, C::kTargetOnly, 1234, 0};
    auto d = DecideTargetWatcherAction(cur, C::kTargetOnly, 1234, 7, 3);
    CHECK_FALSE(d.act);
    CHECK_FALSE(d.retrying);
}

TEST_CASE("audio watcher: target absent (mode only/game_only) settles on kNothing, no track") {
    // pid=0 (target not running) -> WantedTargetCapture would already say kNothing/kAllDesktop
    // upstream; this checks the watcher settles once it gets there and doesn't keep re-acting.
    TargetWatcherState cur;  // not started
    auto d1 = DecideTargetWatcherAction(cur, C::kNothing, 0, 0, 3);
    CHECK(d1.act);
    TargetWatcherState next = NextTargetWatcherState(C::kNothing, 0, /*activation_failed=*/false);
    CHECK(next.started);
    CHECK(next.capture == C::kNothing);
    CHECK(next.pid == 0);
    CHECK(next.failed_pid == 0);

    // Same pid==0 the next poll: settled, no further action.
    auto d2 = DecideTargetWatcherAction(next, C::kNothing, 0, 1, 3);
    CHECK_FALSE(d2.act);
}

TEST_CASE("audio watcher: target relaunches with a new pid -> reactivates") {
    TargetWatcherState cur{true, C::kTargetOnly, 1111, 0};  // was capturing pid 1111
    // App exited: FindTargetRootPid now returns 0 for it upstream, so want becomes kNothing.
    auto d_exit = DecideTargetWatcherAction(cur, C::kNothing, 0, 3, 3);
    CHECK(d_exit.act);
    TargetWatcherState stopped = NextTargetWatcherState(C::kNothing, 0, false);
    CHECK(stopped.capture == C::kNothing);
    CHECK(stopped.pid == 0);

    // App relaunches under a new pid (2222): want flips back to kTargetOnly/2222.
    auto d_relaunch = DecideTargetWatcherAction(stopped, C::kTargetOnly, 2222, 6, 3);
    CHECK(d_relaunch.act);
    CHECK_FALSE(d_relaunch.retrying);
    TargetWatcherState restarted = NextTargetWatcherState(C::kTargetOnly, 2222, false);
    CHECK(restarted.capture == C::kTargetOnly);
    CHECK(restarted.pid == 2222);
}

TEST_CASE("audio watcher: a failed activation falls back and is retried only on the snapshot cadence") {
    // want=kTargetOnly for pid 42, but StartProcessLoopback fails -> falls back to kNothing.
    TargetWatcherState cur;  // fresh
    auto d1 = DecideTargetWatcherAction(cur, C::kTargetOnly, 42, 0, 3);
    CHECK(d1.act);
    CHECK_FALSE(d1.retrying);  // first attempt, not yet a retry
    TargetWatcherState failed = NextTargetWatcherState(C::kTargetOnly, 42, /*activation_failed=*/true);
    CHECK(failed.capture == C::kNothing);  // "only" excludes -> nothing on failure
    CHECK(failed.pid == 0);
    CHECK(failed.failed_pid == 42);

    // Same poll's still-wanted target (42) at a non-cadence poll: don't retry yet.
    auto d_wait = DecideTargetWatcherAction(failed, C::kTargetOnly, 42, 1, 3);
    CHECK_FALSE(d_wait.act);
    auto d_wait2 = DecideTargetWatcherAction(failed, C::kTargetOnly, 42, 2, 3);
    CHECK_FALSE(d_wait2.act);

    // On the 3-poll snapshot cadence, retry.
    auto d_retry = DecideTargetWatcherAction(failed, C::kTargetOnly, 42, 3, 3);
    CHECK(d_retry.act);
    CHECK(d_retry.retrying);
}

TEST_CASE("audio watcher: all_except falls back to the whole desktop mix on activation failure") {
    TargetWatcherState cur;
    TargetWatcherState failed = NextTargetWatcherState(C::kAllExceptTarget, 99, /*activation_failed=*/true);
    CHECK(failed.capture == C::kAllDesktop);  // "all_except" falls back to the mix, not silence
    CHECK(failed.pid == 0);
    CHECK(failed.failed_pid == 99);
}

TEST_CASE("audio watcher: a successful retry clears failed_pid and records the real pid") {
    TargetWatcherState failed{true, C::kNothing, 0, 42};
    auto d = DecideTargetWatcherAction(failed, C::kTargetOnly, 42, 3, 3);
    REQUIRE(d.act);
    TargetWatcherState recovered = NextTargetWatcherState(C::kTargetOnly, 42, /*activation_failed=*/false);
    CHECK(recovered.capture == C::kTargetOnly);
    CHECK(recovered.pid == 42);
    CHECK(recovered.failed_pid == 0);

    // Settled: no more action once recovered.
    auto d_settled = DecideTargetWatcherAction(recovered, C::kTargetOnly, 42, 4, 3);
    CHECK_FALSE(d_settled.act);
}


// What the Audio page shows per card (ClassifyAudioSource / AudioSourceBoard).
TEST_CASE("audio standby gate: holds only after the display is off and every output silent that long") {
    AudioStandbyGate gate;
    const int n = AudioStandbyGate::kHoldPolls;
    CHECK_FALSE(gate.Update(false, false));  // display on: never held
    for (int i = 1; i < n; ++i) CHECK_FALSE(gate.Update(true, false));
    CHECK(gate.Update(true, false));         // the kHoldPolls-th quiet, dark poll
    CHECK(gate.Update(true, false));
}

TEST_CASE("audio standby gate: sound or the display coming back releases at once and restarts the count") {
    AudioStandbyGate gate;
    const int n = AudioStandbyGate::kHoldPolls;
    for (int i = 0; i < n; ++i) gate.Update(true, false);
    REQUIRE(gate.Update(true, false));
    CHECK_FALSE(gate.Update(true, true));    // a call starts: released, no hysteresis
    for (int i = 1; i < n; ++i) CHECK_FALSE(gate.Update(true, false));  // must wait the full time again
    CHECK(gate.Update(true, false));
    CHECK_FALSE(gate.Update(false, false));  // display on: released
    for (int i = 1; i < n; ++i) CHECK_FALSE(gate.Update(true, false));
}

namespace {

constexpr int64_t kRepairFreq   = 10'000'000;
constexpr int64_t kTenMs        = kRepairFreq / 100;

PcmPacket TenMsPacket(int64_t qpc) {
    PcmPacket p;
    p.samples.assign(480 * 2, 0);
    p.qpc_ticks = qpc;
    return p;
}

LoopbackStampRepair RepairWith200msBuffer() {
    LoopbackStampRepair repair;
    repair.SetCapacity(20 * kTenMs, kRepairFreq);
    return repair;
}

}  // namespace

TEST_CASE("loopback stamp repair: true stamps pass through untouched, even after a stall") {
    LoopbackStampRepair    repair = RepairWith200msBuffer();
    std::vector<int64_t>   got;
    const auto emit = [&](const PcmPacket& p) { got.push_back(p.qpc_ticks); };
    const int64_t now = 1000 * kTenMs;
    // A stalled thread finds a full 200 ms buffer: every stamp is within the buffer's capacity.
    repair.BeginPass(now);
    for (int i = 0; i < 20; ++i) repair.Push(TenMsPacket(now - (20 - i) * kTenMs), emit);
    repair.EndPass(emit);
    REQUIRE(got.size() == 20);
    for (int i = 0; i < 20; ++i) CHECK(got[i] == now - (20 - i) * kTenMs);
    LoopbackStampRepair::Run run;
    CHECK_FALSE(repair.TakeFinishedRun(run));
}

TEST_CASE("loopback stamp repair: a stale run in one pass is laid back to back, ending at the pass") {
    LoopbackStampRepair    repair = RepairWith200msBuffer();
    std::vector<int64_t>   got;
    const auto emit = [&](const PcmPacket& p) { got.push_back(p.qpc_ticks); };
    // The 2026-10-12 shape: the last packet before the silence at t, then 20 packets stamped
    // straight on from t, arriving 14 s later.
    const int64_t t   = 1000 * kTenMs;
    const int64_t now = t + 1400 * kTenMs;
    repair.BeginPass(now);
    for (int i = 0; i < 20; ++i) repair.Push(TenMsPacket(t + (i + 1) * kTenMs), emit);
    CHECK(got.empty());  // held until the run's end is known
    repair.EndPass(emit);
    REQUIRE(got.size() == 20);
    for (int i = 0; i < 20; ++i) CHECK(got[i] == now - (20 - i) * kTenMs);

    LoopbackStampRepair::Run run;
    REQUIRE(repair.TakeFinishedRun(run) == false);  // still open: the next pass may continue it
    repair.BeginPass(now + kTenMs);
    repair.Push(TenMsPacket(now), emit);  // a true stamp ends it, untouched
    repair.EndPass(emit);
    CHECK(got.back() == now);
    REQUIRE(repair.TakeFinishedRun(run));
    CHECK(run.packets == 20);
    CHECK(run.frames == 20 * 480);
    CHECK(run.passes == 1);
    CHECK(run.span_ticks == 0);
    CHECK(run.raw_age_ticks == 1399 * kTenMs);
    CHECK_FALSE(repair.TakeFinishedRun(run));
}

TEST_CASE("loopback stamp repair: a run trickling in one packet a pass ends each packet at its pass") {
    LoopbackStampRepair    repair = RepairWith200msBuffer();
    std::vector<int64_t>   got;
    const auto emit = [&](const PcmPacket& p) { got.push_back(p.qpc_ticks); };
    const int64_t t   = 1000 * kTenMs;
    const int64_t now = t + 4700 * kTenMs;
    for (int i = 0; i < 20; ++i) {
        repair.BeginPass(now + i * kTenMs);
        repair.Push(TenMsPacket(t + (i + 1) * kTenMs), emit);
        repair.EndPass(emit);
        REQUIRE(got.size() == size_t(i + 1));
        CHECK(got.back() == now + (i - 1) * kTenMs);  // 10 ms long, ending where its pass began
    }
    repair.BeginPass(now + 20 * kTenMs);
    repair.Push(TenMsPacket(now + 19 * kTenMs), emit);
    repair.EndPass(emit);
    LoopbackStampRepair::Run run;
    REQUIRE(repair.TakeFinishedRun(run));
    CHECK(run.packets == 20);
    CHECK(run.passes == 20);
    CHECK(run.span_ticks == 19 * kTenMs);
}

TEST_CASE("loopback stamp repair: a true stamp later in the same pass caps the run and is kept") {
    LoopbackStampRepair    repair = RepairWith200msBuffer();
    std::vector<int64_t>   got;
    const auto emit = [&](const PcmPacket& p) { got.push_back(p.qpc_ticks); };
    const int64_t now = 5000 * kTenMs;
    repair.BeginPass(now);
    repair.Push(TenMsPacket(10 * kTenMs), emit);
    repair.Push(TenMsPacket(11 * kTenMs), emit);
    repair.Push(TenMsPacket(now - 3 * kTenMs), emit);  // fresh: the stale two end where it starts
    repair.EndPass(emit);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == now - 5 * kTenMs);
    CHECK(got[1] == now - 4 * kTenMs);
    CHECK(got[2] == now - 3 * kTenMs);
}

TEST_CASE("loopback stamp repair: unknown buffer capacity leaves every stamp alone") {
    LoopbackStampRepair    repair;
    repair.SetCapacity(0, kRepairFreq);
    std::vector<int64_t>   got;
    const auto emit = [&](const PcmPacket& p) { got.push_back(p.qpc_ticks); };
    repair.BeginPass(5000 * kTenMs);
    repair.Push(TenMsPacket(kTenMs), emit);
    repair.EndPass(emit);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == kTenMs);
}

TEST_CASE("audio standby gate: a long call with the screen off is never held") {
    AudioStandbyGate gate;
    for (int i = 0; i < 3 * AudioStandbyGate::kHoldPolls; ++i) CHECK_FALSE(gate.Update(true, true));
}

TEST_CASE("audio status: a target source reads waiting until it's attached, then by audibility") {
    constexpr int64_t kFreq = 10'000'000;
    const int64_t     now   = 100 * kFreq;
    const TargetWatcherState fresh{};
    CHECK(ClassifyAudioSource(true, "only", &fresh, 0, now, kFreq) == AudioSourceState::kWaiting);
    CHECK(ClassifyAudioSource(true, "game_only", &fresh, 0, now, kFreq) == AudioSourceState::kNoGame);

    const TargetWatcherState absent = NextTargetWatcherState(C::kNothing, 0, false);
    CHECK(ClassifyAudioSource(true, "only", &absent, 0, now, kFreq) == AudioSourceState::kWaiting);

    const TargetWatcherState attached = NextTargetWatcherState(C::kTargetOnly, 42, false);
    CHECK(ClassifyAudioSource(true, "only", &attached, 0, now, kFreq) == AudioSourceState::kQuiet);
    CHECK(ClassifyAudioSource(true, "only", &attached, now - kFreq, now, kFreq) == AudioSourceState::kLive);
    CHECK(ClassifyAudioSource(true, "only", &attached, now - 3 * kFreq, now, kFreq) == AudioSourceState::kQuiet);
}

TEST_CASE("audio status: a failed activation is failed, not waiting; all_except with its target closed records") {
    constexpr int64_t kFreq = 10'000'000;
    const TargetWatcherState failed = NextTargetWatcherState(C::kTargetOnly, 42, /*activation_failed=*/true);
    CHECK(ClassifyAudioSource(true, "only", &failed, 0, kFreq, kFreq) == AudioSourceState::kFailed);
    const TargetWatcherState except_failed = NextTargetWatcherState(C::kAllExceptTarget, 42, true);
    CHECK(ClassifyAudioSource(true, "all_except", &except_failed, 0, kFreq, kFreq) == AudioSourceState::kFailed);
    const TargetWatcherState except_absent = NextTargetWatcherState(C::kAllDesktop, 0, false);
    CHECK(ClassifyAudioSource(true, "all_except", &except_absent, 0, kFreq, kFreq) == AudioSourceState::kQuiet);
}

TEST_CASE("audio status: the board reports every source by id; untargeted sources never wait") {
    AudioSourceBoard board;
    board.Init({{"desktop", "all", false, true, 0}, {"only_Discord.exe", "only", true, true, 1}, {"mic", "all", false, true, 2}});
    constexpr int64_t kFreq = 10'000'000;
    const int64_t     now   = 50 * kFreq;
    auto audible = [&](size_t i) { return i == 2 ? now - kFreq / 2 : int64_t{0}; };

    auto st = board.Snapshot(audible, now, kFreq);
    REQUIRE(st.size() == 3);
    CHECK(st[0].id == "desktop");
    CHECK(st[0].state == AudioSourceState::kQuiet);
    CHECK(st[1].state == AudioSourceState::kWaiting);
    CHECK(st[2].state == AudioSourceState::kLive);

    board.SetWatch(1, NextTargetWatcherState(C::kTargetOnly, 7, false));
    CHECK(board.Snapshot(audible, now, kFreq)[1].state == AudioSourceState::kQuiet);
    CHECK(std::string(AudioSourceStateName(AudioSourceState::kNoGame)) == "no_game");
}

TEST_CASE("audio status: with every ring released sources read paused, not quiet or not running") {
    AudioSourceBoard board;
    board.Init({{"mic", "all", false, true, 0},
                {"only_Discord.exe", "only", true, true, 1},
                {"game", "game_only", true, true, 2},
                {"off", "all", false, false, 3}});
    constexpr int64_t kFreq = 10'000'000;
    auto never = [](size_t) { return int64_t{0}; };
    board.SetPaused(true);
    auto st = board.Snapshot(never, 50 * kFreq, kFreq);
    CHECK(st[0].state == AudioSourceState::kPaused);
    CHECK(st[1].state == AudioSourceState::kPaused);
    CHECK(st[2].state == AudioSourceState::kNoGame);
    CHECK(st[3].state == AudioSourceState::kOff);
    CHECK(std::string(AudioSourceStateName(AudioSourceState::kPaused)) == "paused");
    board.SetPaused(false);
    CHECK(board.Snapshot(never, 50 * kFreq, kFreq)[0].state == AudioSourceState::kQuiet);
}

TEST_CASE("audio on/off: a switched-off source captures nothing and reads off, whatever its target does") {
    // Every kind of source, off: nothing to capture, so the watcher stops (or never starts) it.
    CHECK(WantedSourceCapture(false, "output_loopback", "all", 0) == C::kNothing);
    CHECK(WantedSourceCapture(false, "output_loopback", "only", 42) == C::kNothing);
    CHECK(WantedSourceCapture(false, "output_loopback", "all_except", 42) == C::kNothing);
    CHECK(WantedSourceCapture(false, "input", "all", 0) == C::kNothing);
    // On: a mic and plain Desktop capture their own endpoint; a target source follows its target.
    CHECK(WantedSourceCapture(true, "input", "all", 0) == C::kAllDesktop);
    CHECK(WantedSourceCapture(true, "output_loopback", "all", 0) == C::kAllDesktop);
    CHECK(WantedSourceCapture(true, "output_loopback", "only", 0) == C::kNothing);
    CHECK(WantedSourceCapture(true, "output_loopback", "only", 42) == C::kTargetOnly);
    CHECK(WantedSourceCapture(true, "output_loopback", "all_except", 42) == C::kAllExceptTarget);
    CHECK(AudioSourceHasTarget("output_loopback", "game_only"));
    CHECK_FALSE(AudioSourceHasTarget("output_loopback", "all"));
    CHECK_FALSE(AudioSourceHasTarget("input", "only"));

    // Switching a running source off is an action; staying off is not.
    const TargetWatcherState running = NextTargetWatcherState(C::kAllDesktop, 0, false);
    CHECK(DecideTargetWatcherAction(running, C::kNothing, 0, 1, 3).act);
    const TargetWatcherState stopped = NextTargetWatcherState(C::kNothing, 0, false);
    CHECK_FALSE(DecideTargetWatcherAction(stopped, C::kNothing, 0, 2, 3).act);
    CHECK(DecideTargetWatcherAction(stopped, C::kAllDesktop, 0, 2, 3).act);

    // Off wins over every watcher state, audible audio included.
    constexpr int64_t kFreq = 10'000'000;
    const TargetWatcherState failed = NextTargetWatcherState(C::kTargetOnly, 42, true);
    CHECK(ClassifyAudioSource(false, "only", &failed, 0, kFreq, kFreq) == AudioSourceState::kOff);
    CHECK(ClassifyAudioSource(false, "all", nullptr, kFreq, kFreq, kFreq) == AudioSourceState::kOff);

    AudioSourceBoard board;
    board.Init({{"desktop", "all", false, true, 0}, {"mic", "all", false, false, 1}});
    auto never = [](size_t) { return int64_t{0}; };
    CHECK(board.Snapshot(never, kFreq, kFreq)[0].state == AudioSourceState::kQuiet);
    CHECK(board.Snapshot(never, kFreq, kFreq)[1].state == AudioSourceState::kOff);
    // The watcher re-publishes the list after a Save switched them the other way.
    board.Init({{"desktop", "all", false, false, 0}, {"mic", "all", false, true, 1}});
    CHECK(board.Snapshot(never, kFreq, kFreq)[0].state == AudioSourceState::kOff);
    CHECK(board.Snapshot(never, kFreq, kFreq)[1].state == AudioSourceState::kQuiet);
    CHECK(std::string(AudioSourceStateName(AudioSourceState::kOff)) == "off");
}

}  // namespace ombra::test
