// window_target.h: which window a window-mode capture records. Headless, no GPU: the matcher runs
// on hand-built snapshots, never on the real desktop.

#include <doctest/doctest.h>

#include "ombra/games.h"
#include "ombra/window_target.h"

#include <windows.h>

#include <string>
#include <vector>

namespace ombra::test {

namespace {

constexpr wchar_t kLeagueDir[]    = L"E:\\Games\\Riot Games\\League of Legends";
constexpr wchar_t kClientExe[]    = L"E:\\Games\\Riot Games\\League of Legends\\LeagueClientUx.exe";
constexpr wchar_t kGameExe[]      = L"E:\\Games\\Riot Games\\League of Legends\\Game\\League of Legends.exe";
constexpr wchar_t kChromeExe[]    = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
constexpr int64_t kFullHd         = 1920LL * 1080;
constexpr int64_t kClientArea     = 1280LL * 720;

WindowCandidate Win(uint64_t id, const wchar_t* title, const wchar_t* image, int64_t area = kClientArea) {
    WindowCandidate w;
    w.id         = id;
    w.title      = title;
    w.image_path = image;
    w.area       = area;
    return w;
}

WindowCandidate Client() { return Win(1, L"League of Legends", kClientExe); }
WindowCandidate Game() {
    WindowCandidate w = Win(2, L"League of Legends (TM) Client", kGameExe, kFullHd);
    w.covers_monitor  = true;
    return w;
}

// "The game" with one listed game, the way the supervisor builds it.
WindowTarget LeagueFolder() {
    GameEntry league;
    league.name = L"League of Legends";
    league.path = L"E:/Games/Riot Games/League of Legends/";
    WindowTarget t;
    t.is_game = [games = std::vector<GameEntry>{league}](const std::wstring& image) {
        return ClassifyImage(games, image) == GameVerdict::Game;
    };
    return t;
}

WindowTarget Legacy(const wchar_t* match) {
    WindowTarget t;
    t.match = match;
    return t;
}

}  // namespace

TEST_SUITE("window_target") {

TEST_CASE("legacy substrings still match by title and by image name") {
    const std::vector<WindowCandidate> windows = {Win(7, L"Friends - Discord", L"C:\\Apps\\Discord\\Discord.exe"),
                                                  Game()};
    CHECK(ChooseTargetWindow(Legacy(L"discord.exe"), windows, 0) == 0);
    CHECK(ChooseTargetWindow(Legacy(L"League of Legends (TM) Client"), windows, 0) == 1);
    CHECK(ChooseTargetWindow(Legacy(L"not running"), windows, 0) == -1);
    CHECK(ChooseTargetWindow(WindowTarget{}, windows, 0) == -1);
}

TEST_CASE("a listed game's folder matches every program under it and nothing that only shares a prefix") {
    const WindowTarget t = LeagueFolder();
    CHECK(WindowMatchesTarget(t, Client()));
    CHECK(WindowMatchesTarget(t, Game()));
    CHECK_FALSE(WindowMatchesTarget(t, Win(3, L"League of Legends", L"E:\\Games\\Riot Games\\League of Legends PBE\\LeagueClientUx.exe")));
    CHECK_FALSE(WindowMatchesTarget(t, Win(4, L"League of Legends", L"E:\\Games\\Riot Games\\Riot Client\\RiotClientServices.exe")));
    // A browser tab about the game: right title, wrong program.
    CHECK_FALSE(WindowMatchesTarget(t, Win(5, L"League of Legends patch notes - Google Chrome", kChromeExe)));
    // An unqueryable process (empty path) is never a game.
    CHECK_FALSE(WindowMatchesTarget(t, Win(6, L"League of Legends", L"")));
}

TEST_CASE("\"not a game\" on a tool inside a game's folder keeps its window out of The game") {
    GameEntry league;
    league.path = L"E:\\Games\\Riot Games\\League of Legends";
    GameEntry client;
    client.path    = kClientExe;
    client.is_game = false;
    WindowTarget t;
    t.is_game = [games = std::vector<GameEntry>{league, client}](const std::wstring& image) {
        return ClassifyImage(games, image) == GameVerdict::Game;
    };
    CHECK(WindowMatchesTarget(t, Game()));
    CHECK_FALSE(WindowMatchesTarget(t, Client()));
}

TEST_CASE("The game ignores One window's match string") {
    WindowTarget t = LeagueFolder();
    t.match        = L"discord";
    CHECK_FALSE(WindowMatchesTarget(t, Win(7, L"Discord", L"C:\\Apps\\Discord\\Discord.exe")));
    CHECK(WindowMatchesTarget(t, Client()));
}

TEST_CASE("Ombra's own windows never match") {
    CHECK_FALSE(WindowMatchesTarget(Legacy(L"ombra"), Win(8, L"Ombra", L"E:\\Projects\\Ombra\\build\\Release\\ombra-ui.exe")));
}

TEST_CASE("lobby to match: the fullscreen game in front takes over from the client") {
    WindowCandidate game = Game();
    game.foreground      = true;
    const std::vector<WindowCandidate> windows = {game, Client()};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, /*current=*/1) == 0);
}

TEST_CASE("alt-tab to the client mid-match keeps recording the match") {
    WindowCandidate client = Client();
    client.foreground      = true;
    const std::vector<WindowCandidate> windows = {client, Game()};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, /*current=*/2) == 1);
}

TEST_CASE("the game closing hands back to the client") {
    const std::vector<WindowCandidate> windows = {Client()};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, /*current=*/2) == 0);
}

TEST_CASE("alt-tab out of an exclusive-fullscreen game (it minimizes) stays on the game") {
    WindowCandidate game = Game();
    game.minimized       = true;
    game.covers_monitor  = false;
    game.area            = 0;
    WindowCandidate client = Client();
    client.foreground      = true;
    const std::vector<WindowCandidate> windows = {game, client};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, /*current=*/2) == 0);
}

TEST_CASE("a minimized lobby still hands off when the match starts") {
    WindowCandidate client = Client();
    client.minimized       = true;
    client.area            = 0;
    WindowCandidate game   = Game();
    game.foreground        = true;
    const std::vector<WindowCandidate> windows = {game, client};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, /*current=*/1) == 0);
}

TEST_CASE("switch debounce: a pick must hold before it's acted on") {
    WindowSwitchState st;
    constexpr int64_t kSettle = 2000;
    // Same window: nothing to do.
    CHECK(StepWindowSwitch(st, 5, 5, true, 0, kSettle) == WindowSwitchAction::None);
    // A new pick starts the clock, holds, then switches once.
    CHECK(StepWindowSwitch(st, 6, 5, true, 1000, kSettle) == WindowSwitchAction::Wait);
    CHECK(StepWindowSwitch(st, 6, 5, true, 2999, kSettle) == WindowSwitchAction::Wait);
    CHECK(StepWindowSwitch(st, 6, 5, true, 3000, kSettle) == WindowSwitchAction::Switch);
    CHECK_FALSE(st.pending);
    // A flicker back to the current window cancels a pending switch.
    CHECK(StepWindowSwitch(st, 7, 5, true, 4000, kSettle) == WindowSwitchAction::Wait);
    CHECK(StepWindowSwitch(st, 5, 5, true, 5000, kSettle) == WindowSwitchAction::None);
    CHECK(StepWindowSwitch(st, 7, 5, true, 6500, kSettle) == WindowSwitchAction::Wait);  // clock restarted
    CHECK(StepWindowSwitch(st, 7, 5, true, 8000, kSettle) == WindowSwitchAction::Wait);
    // A different pick while pending restarts the clock too.
    CHECK(StepWindowSwitch(st, 8, 5, true, 8100, kSettle) == WindowSwitchAction::Wait);
    CHECK(StepWindowSwitch(st, 8, 5, true, 10100, kSettle) == WindowSwitchAction::Switch);
    // The recorded window closed: the item-closed fault owns that, not a switch.
    CHECK(StepWindowSwitch(st, 0, 5, false, 11000, kSettle) == WindowSwitchAction::None);
    CHECK(StepWindowSwitch(st, 0, 5, false, 20000, kSettle) == WindowSwitchAction::None);
    // It still exists but stopped matching (the target changed): switch away after the settle.
    CHECK(StepWindowSwitch(st, 0, 5, true, 21000, kSettle) == WindowSwitchAction::Wait);
    CHECK(StepWindowSwitch(st, 0, 5, true, 23000, kSettle) == WindowSwitchAction::Switch);
    // Monitor fallback (no window recorded) and a window appears.
    CHECK(StepWindowSwitch(st, 9, 0, false, 24000, kSettle) == WindowSwitchAction::Wait);
    CHECK(StepWindowSwitch(st, 9, 0, false, 26000, kSettle) == WindowSwitchAction::Switch);
}

TEST_CASE("a minimized window is still picked when it is the only match") {
    WindowCandidate client = Client();
    client.minimized       = true;
    CHECK(ChooseTargetWindow(LeagueFolder(), {client}, 0) == 0);
    CHECK(ChooseTargetWindow(LeagueFolder(), {client}, 1) == 0);
}

TEST_CASE("a fresh pick prefers the larger window over a small helper from the same folder") {
    const std::vector<WindowCandidate> windows = {
        Win(9, L"Bug Splat", L"E:\\Games\\Riot Games\\League of Legends\\BugSplat.exe", 400LL * 300), Client()};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, 0) == 1);
}

TEST_CASE("a fresh pick prefers the foreground window") {
    WindowCandidate client = Client();
    client.foreground      = true;
    const std::vector<WindowCandidate> windows = {Win(10, L"League", kGameExe, kFullHd), client};
    CHECK(ChooseTargetWindow(LeagueFolder(), windows, 0) == 1);
}

TEST_CASE("a current window that stopped matching is dropped") {
    // Target changed from one game to Discord while its client was recorded.
    const std::vector<WindowCandidate> windows = {Client(), Win(7, L"Discord", L"C:\\Apps\\Discord\\Discord.exe")};
    CHECK(ChooseTargetWindow(Legacy(L"discord"), windows, /*current=*/1) == 1);
}

TEST_CASE("the real snapshot is cheap enough to take every second") {
    // Read-only walk of this desktop's windows; the supervisor takes one per second in window mode.
    SnapshotTargetWindows();  // warm-up
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    constexpr int kRuns = 20;
    size_t count = 0;
    for (int i = 0; i < kRuns; ++i) count = SnapshotTargetWindows().size();
    QueryPerformanceCounter(&b);
    const double ms = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / kRuns;
    MESSAGE("SnapshotTargetWindows: " << count << " windows, " << ms << " ms per snapshot");
    CHECK(ms < 20.0);
}

TEST_CASE("folder paths normalize slashes, case and trailing separators") {
    CHECK(NormalizeFolderPath(L"E:/Games/Riot Games/League of Legends/") == L"e:\\games\\riot games\\league of legends");
    CHECK(NormalizeFolderPath(L"E:\\Games\\\\") == L"e:\\games");
}

}  // TEST_SUITE

}  // namespace ombra::test
