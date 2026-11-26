// The engine's marker timeline (marker.h), headless: labels, the per-app cap, what a writer's hold
// keeps, and how a marker's moment maps onto a file's frames.

#include <doctest/doctest.h>

#include "ombra/json.h"
#include "ombra/marker.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace ombra;

namespace {

constexpr int64_t  kFreq = 10'000'000;  // QPC ticks per second, as on this machine
constexpr uint32_t kFps  = 60;
constexpr int64_t  kFrame = kFreq / kFps;

Marker At(int64_t qpc, MarkerType type = MarkerType::kGeneric, std::string label = {}) {
    Marker m;
    m.qpc   = qpc;
    m.type  = type;
    m.label = std::move(label);
    m.owner = "Tool";
    return m;
}

}  // namespace

TEST_CASE("marker: types round-trip by wire name, unknown names refuse") {
    for (MarkerType t : {MarkerType::kGeneric, MarkerType::kKill, MarkerType::kRoundStart, MarkerType::kRoundEnd})
        CHECK(ParseMarkerType(MarkerTypeName(t)) == t);
    CHECK(!ParseMarkerType("Kill"));
    CHECK(!ParseMarkerType(""));
    // Ombra's own have wire names (the file's tag) but no app may pick them.
    for (MarkerType t : {MarkerType::kClipSaved, MarkerType::kGameStart, MarkerType::kGameEnd}) {
        CHECK(IsOmbraMarkerType(t));
        CHECK(!ParseMarkerType(MarkerTypeName(t)));
    }
    CHECK(std::string(MarkerTypeName(MarkerType::kGameStart)) == "game_start");
    CHECK(!IsOmbraMarkerType(kLastAppType));
}

TEST_CASE("marker: labels count code points, drop control characters, trim spaces") {
    CHECK(CleanMarkerLabel("  Pentakill\r\n ") == std::string("Pentakill"));
    CHECK(CleanMarkerLabel("   ") == std::string());
    // 64 letters fit whatever their byte length; 65 don't.
    std::string cyrillic;
    for (int i = 0; i < 64; ++i) cyrillic += "\xD0\x96";  // Ж, two bytes
    CHECK(CleanMarkerLabel(cyrillic) == cyrillic);
    CHECK(!CleanMarkerLabel(cyrillic + "a"));
    CHECK(CleanMarkerLabel(std::string(64, 'a') + "   ").has_value());
    CHECK(!CleanMarkerLabel(std::string(65, 'a')));
    // Broken UTF-8: a lone continuation byte, a cut sequence.
    CHECK(!CleanMarkerLabel("a\x80"));
    CHECK(!CleanMarkerLabel("a\xE2\x82"));
}

TEST_CASE("marker: chapter title names the label or the type, then the app") {
    CHECK(MarkerChapterTitle(At(0, MarkerType::kKill)) == "Kill \xC2\xB7 Tool");
    CHECK(MarkerChapterTitle(At(0, MarkerType::kKill, "Ace")) == "Ace \xC2\xB7 Tool");
    Marker anonymous = At(0, MarkerType::kRoundStart);
    anonymous.owner.clear();
    CHECK(MarkerChapterTitle(anonymous) == "Round start");
    // Ombra's own: the type, then the detail (the game, the asking app), never the owner.
    Marker game = At(0, MarkerType::kGameStart, "VALORANT");
    game.owner = kOmbraMarkerOwner;
    CHECK(MarkerChapterTitle(game) == "Game start \xC2\xB7 VALORANT");
    Marker saved = At(0, MarkerType::kClipSaved);
    saved.owner = kOmbraMarkerOwner;
    CHECK(MarkerChapterTitle(saved) == "Clip saved");
}

TEST_CASE("marker: 256 per app within the held footage, then too many") {
    MarkerTimeline timeline;
    for (size_t i = 0; i < kMarkersPerApp; ++i) {
        Marker m = At(1000 + static_cast<int64_t>(i));
        REQUIRE(timeline.Add(m, "a", 0) == MarkerTimeline::AddResult::kAdded);
    }
    Marker over = At(5000);
    CHECK(timeline.Add(over, "a", 0) == MarkerTimeline::AddResult::kAppFull);
    // Another app has its own allowance.
    Marker other = At(5000);
    CHECK(timeline.Add(other, "b", 0) == MarkerTimeline::AddResult::kAdded);
    // Once the buffer has moved past the old ones, they stop counting.
    Marker later = At(6000);
    CHECK(timeline.Add(later, "a", 2000) == MarkerTimeline::AddResult::kAdded);
    CHECK(timeline.Size() == 2);  // b's and a's latest: a's first 256 fell behind the buffer
}

TEST_CASE("marker: a hold keeps markers the buffer has left behind") {
    MarkerTimeline timeline;
    Marker early = At(100);
    REQUIRE(timeline.Add(early, "a", 0) == MarkerTimeline::AddResult::kAdded);
    {
        MarkerTimeline::Hold hold = timeline.HoldFrom(50);
        Marker m = At(900);
        REQUIRE(timeline.Add(m, "a", 800) == MarkerTimeline::AddResult::kAdded);
        CHECK(timeline.Between(0, 1000).size() == 2);  // the save from 50 still gets 100
        MarkerTimeline::Hold moved = std::move(hold);  // a moved hold still holds
        Marker n = At(950);
        REQUIRE(timeline.Add(n, "a", 800) == MarkerTimeline::AddResult::kAdded);
        CHECK(timeline.Size() == 3);
    }
    Marker m = At(1000);
    REQUIRE(timeline.Add(m, "a", 800) == MarkerTimeline::AddResult::kAdded);
    const std::vector<Marker> left = timeline.Between(0, 2000);
    REQUIRE(left.size() == 3);
    CHECK(left.front().qpc == 900);
}

TEST_CASE("marker: a marker placed in the past keeps the list in time order") {
    MarkerTimeline timeline;
    Marker late = At(500), early = At(200), mid = At(300);
    timeline.Add(late, "a", 0);
    timeline.Add(early, "a", 0);
    timeline.Add(mid, "a", 0);
    const std::vector<Marker> all = timeline.Between(0, 1000);
    REQUIRE(all.size() == 3);
    CHECK(all[0].qpc == 200);
    CHECK(all[1].qpc == 300);
    CHECK(all[2].qpc == 500);
    CHECK(all[0].id != all[1].id);
    CHECK(timeline.Between(250, 500).size() == 2);  // both ends inclusive
}

TEST_CASE("marker: a file clock lands on the frame at or before the moment") {
    const int64_t base = 1'000'000'000;
    FileClock clock(kFreq, kFps);
    CHECK(!clock.MsAt(base));
    // 60 frames on time, with jitter under half a frame; then the pacer stalls 2 s and the next
    // frames carry on at tick 60 (the file has no gap; the qpc timeline does).
    for (uint64_t t = 0; t < 60; ++t)
        clock.Add(base + static_cast<int64_t>(t) * kFrame + (t % 2 ? kFrame / 4 : 0), 1000 + t);
    const int64_t resume = base + 3 * kFreq;
    for (uint64_t t = 60; t < 120; ++t) clock.Add(resume + static_cast<int64_t>(t - 60) * kFrame, 1000 + t);

    CHECK(clock.MsAt(base) == 0u);
    CHECK(!clock.MsAt(base - 1));
    CHECK(clock.MsAt(base + kFrame - kFreq / 500) == 0u);  // 2 ms before frame 1: still frame 0
    CHECK(clock.MsAt(base + 30 * kFrame) == 500u);
    // A moment inside the stall belongs to the last frame before it, never the frame after.
    CHECK(clock.MsAt(base + 2 * kFreq) == 59u * 1000 / kFps);
    CHECK(clock.MsAt(resume) == 1000u);
    CHECK(clock.MsAt(resume + 59 * kFrame) == 119u * 1000 / kFps);
    // Up to kMarkerTrailSeconds past the last frame lands on it (a "now" marker runs ahead of the
    // encode); later, the file's footage had stopped and the marker isn't its.
    const int64_t last = resume + 59 * kFrame;
    CHECK(clock.MsAt(last + kFrame) == 119u * 1000 / kFps);
    CHECK(clock.MsAt(last + kFreq * 19 / 10) == 119u * 1000 / kFps);
    CHECK(!clock.MsAt(last + kFreq * 21 / 10));
}

TEST_CASE("marker: a file's markers become chapters and the tag's JSON") {
    const int64_t base = 5'000'000;
    FileClock clock(kFreq, kFps);
    for (uint64_t t = 0; t < 600; ++t) clock.Add(base + static_cast<int64_t>(t) * kFrame, t);

    Marker kill = At(base + 2 * kFreq, MarkerType::kKill, "Ace");
    kill.important = true;
    const std::vector<Marker> markers = {At(base - kFreq), kill, At(base + 5 * kFreq, MarkerType::kRoundEnd)};
    const MuxMarkers out = MarkersForFile(markers, clock);
    REQUIRE(out.chapters.size() == 2);  // the one before the file is left out
    CHECK(out.chapters[0].ms == 2000);
    CHECK(out.chapters[0].title == "Ace \xC2\xB7 Tool");
    CHECK(out.chapters[1].ms == 5000);
    CHECK(out.chapters[1].title == "Round end \xC2\xB7 Tool");

    const JsonValue root = ParseJson(out.json);
    REQUIRE(root.Find("v"));
    CHECK(root.Find("v")->num == 1);
    const JsonValue* list = root.Find("markers");
    REQUIRE(list);
    REQUIRE(list->arr.size() == 2);
    const JsonValue& first = list->arr[0];
    CHECK(first.Find("ms")->num == 2000);
    CHECK(first.Find("kind")->str == "kill");
    CHECK(first.Find("label")->str == "Ace");
    CHECK(first.Find("app")->str == "Tool");
    CHECK(first.Find("important")->b);
    CHECK(!list->arr[1].Find("label"));
    CHECK(!list->arr[1].Find("important"));

    CHECK(MarkersForFile({At(base - kFreq)}, clock).json.empty());
}

TEST_CASE("marker: a game start just before a file's first frame lands on it") {
    const int64_t base = 50'000'000;
    FileClock clock(kFreq, kFps);
    for (uint64_t t = 0; t < 600; ++t) clock.Add(base + static_cast<int64_t>(t) * kFrame, t);
    // The game armed the buffer; footage began 0.4 s later. An app's marker there isn't the file's.
    const std::vector<Marker> markers = {At(base - kFreq * 4 / 10, MarkerType::kGameStart, "VALORANT"),
                                         At(base - kFreq * 4 / 10, MarkerType::kKill)};
    const MuxMarkers out = MarkersForFile(markers, clock);
    REQUIRE(out.chapters.size() == 1);
    CHECK(out.chapters[0].ms == 0);
    // Further back than the lead, the game started before this file's footage: not its start.
    CHECK(MarkersForFile({At(base - kFreq * 21 / 10, MarkerType::kGameStart)}, clock).chapters.empty());
    CHECK(clock.MsAtOrFirst(base + 30 * kFrame) == 500u);
}

TEST_CASE("marker: the timeline keeps and hands a file the lead before its first frame") {
    MarkerTimeline timeline(100);
    Marker before = At(950, MarkerType::kGameStart);
    REQUIRE(timeline.Add(before, "ombra", 0) == MarkerTimeline::AddResult::kAdded);
    // The buffer's oldest frame is at 1000: 950 is within the lead, kept; 850 isn't.
    Marker old = At(850);
    REQUIRE(timeline.Add(old, "a", 1000) == MarkerTimeline::AddResult::kAdded);
    Marker now = At(1500);
    REQUIRE(timeline.Add(now, "a", 1000) == MarkerTimeline::AddResult::kAdded);
    CHECK(timeline.Size() == 2);
    CHECK(timeline.Between(1000, 2000).size() == 1);
    CHECK(timeline.ForFile(1000, 2000).size() == 2);
    CHECK(timeline.Remove(before.id));
    CHECK(!timeline.Remove(before.id));
    CHECK(timeline.ForFile(1000, 2000).size() == 1);
}

TEST_CASE("marker: an app's markers go only into its own files and the user's") {
    MarkerTimeline timeline;
    Marker ombra = At(100, MarkerType::kGameStart);
    Marker user  = At(200);
    Marker a     = At(300, MarkerType::kKill);
    Marker b     = At(400, MarkerType::kDeath);
    REQUIRE(timeline.Add(ombra, kOmbraMarkerKey, 0) == MarkerTimeline::AddResult::kAdded);
    REQUIRE(timeline.Add(user, kUserMarkerKey, 0) == MarkerTimeline::AddResult::kAdded);
    REQUIRE(timeline.Add(a, "a", 0) == MarkerTimeline::AddResult::kAdded);
    REQUIRE(timeline.Add(b, "b", 0) == MarkerTimeline::AddResult::kAdded);
    const auto ids = [&](std::string_view file_app) {
        std::vector<uint64_t> out;
        for (const Marker& m : timeline.ForFile(0, 1000, file_app)) out.push_back(m.id);
        return out;
    };
    CHECK(ids("a") == std::vector<uint64_t>{ombra.id, user.id, a.id});
    CHECK(ids("b") == std::vector<uint64_t>{ombra.id, user.id, b.id});
    CHECK(ids("c") == std::vector<uint64_t>{ombra.id, user.id});
    CHECK(ids({}) == std::vector<uint64_t>{ombra.id, user.id, a.id, b.id});  // the user's file
}

TEST_CASE("marker: game sessions become start and end markers, debounced") {
    MarkerTimeline timeline;
    GameMarkers games(timeline, kFreq);
    const auto s = [](double seconds) { return static_cast<int64_t>(seconds * kFreq); };
    const auto kinds = [&] {
        std::string out;
        for (const Marker& m : timeline.Between(0, s(100000))) {
            out += std::string(out.empty() ? "" : " ") + MarkerTypeName(m.type);
            if (!m.label.empty()) out += ":" + m.label;
            CHECK(m.owner == kOmbraMarkerOwner);
        }
        return out;
    };

    // A client flashing past (under kGameMarkerMinSeconds) leaves nothing, twice over.
    games.Update("League", s(100), 0);
    games.Update("", s(110), 0);
    games.Update("League", s(200), 0);
    games.Update("", s(216), 0);
    CHECK(kinds().empty());

    // A real session: start and end, labelled with the game.
    games.Update("League", s(300), 0);
    games.Update("League", s(400), 0);  // another exe of the same game (client to match): no change
    games.Update("", s(1000), 0);
    CHECK(kinds() == "game_start:League game_end:League");
    // Back within kGameMarkerJoinSeconds: the session goes on, its end taken back.
    games.Update("League", s(1068), 0);
    CHECK(kinds() == "game_start:League");
    games.Update("", s(2000), 0);
    // Back later than that: a new session.
    games.Update("League", s(2000 + kGameMarkerJoinSeconds + 1), 0);
    CHECK(kinds() == "game_start:League game_end:League game_start:League");

    // Straight to another game: this one ends where the other starts.
    games.Update("VALORANT", s(3000), 0);
    CHECK(kinds() == "game_start:League game_end:League game_start:League game_end:League game_start:VALORANT");
    const std::vector<Marker> all = timeline.Between(0, s(100000));
    CHECK(all[3].qpc == s(3000));
    CHECK(all[4].qpc == s(3000));
    // Another game ending its session doesn't rejoin the earlier one.
    games.Update("", s(3100), 0);
    games.Update("League", s(3110), 0);
    CHECK(kinds() == "game_start:League game_end:League game_start:League game_end:League game_start:VALORANT "
                     "game_end:VALORANT game_start:League");
}
