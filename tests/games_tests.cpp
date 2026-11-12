// games.h / game_library.h: the Games list. Headless, no GPU. The launcher scanners run on fixture
// folders built under the test root, shaped like real launcher files (Riot
// product_settings.yaml, Steam libraryfolders.vdf + appmanifest_*.acf, Epic .item manifests).

#include <doctest/doctest.h>

#include "support/test_env.h"
#include "ombra/config.h"
#include "ombra/controller.h"
#include "ombra/game_library.h"
#include "ombra/games.h"
#include "ombra/json.h"
#include "ombra/util.h"
#include "ombra/window_target.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ombra::test {

namespace {

namespace fs = std::filesystem;

void WriteText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    REQUIRE(f);
    f << text;
}

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A fresh fixture root per test case.
fs::path FixtureRoot(const wchar_t* name) {
    const fs::path root = TestOutputPath(std::wstring(L"games\\") + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

std::string Fwd(const fs::path& p) {  // Riot writes forward slashes
    std::string s = p.string();
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

std::string VdfPath(const fs::path& p) {  // Steam escapes backslashes
    std::string out;
    for (char c : p.string()) {
        out.push_back(c);
        if (c == '\\') out.push_back('\\');
    }
    return out;
}

const GameEntry* FindByName(const std::vector<GameEntry>& games, const std::wstring& name) {
    for (const GameEntry& g : games)
        if (g.name == name) return &g;
    return nullptr;
}

GameEntry Entry(const wchar_t* name, const wchar_t* path, bool is_game = true, GameSource source = GameSource::User) {
    GameEntry g;
    g.name    = name;
    g.path    = path;
    g.is_game = is_game;
    g.source  = source;
    return g;
}

std::string Acf(const char* appid, const char* name, const char* installdir, const char* flags = "4") {
    return std::string("\"AppState\"\n{\n\t\"appid\"\t\t\"") + appid + "\"\n\t\"name\"\t\t\"" + name +
           "\"\n\t\"StateFlags\"\t\t\"" + flags + "\"\n\t\"installdir\"\t\t\"" + installdir + "\"\n}\n";
}

constexpr wchar_t kValorantExe[] =
    L"E:\\Games\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe";

}  // namespace

TEST_SUITE("games") {

TEST_CASE("Riot: each product's own folder, named by its shortcut or folder; the Riot Client and uninstalled products are skipped") {
    const fs::path root  = FixtureRoot(L"riot");
    const fs::path games = root / "Games" / "Riot Games";
    fs::create_directories(games / "League of Legends");
    fs::create_directories(games / "VALORANT" / "live");
    fs::create_directories(games / "Teamfight Tactics" / "Live");
    const fs::path meta = root / "Riot Games" / "Metadata";
    WriteText(meta / "league_of_legends.live" / "league_of_legends.live.product_settings.yaml",
              "product_install_full_path: \"" + Fwd(games / "League of Legends") +
                  "\"\nproduct_install_root: \"" + Fwd(games) + "\"\nshortcut_name: \"League of Legends.lnk\"\n");
    WriteText(meta / "league_of_legends.live" / "league_of_legends.live.ico", "ico");
    WriteText(meta / "valorant.live" / "valorant.live.product_settings.yaml",
              "product_install_full_path: \"" + Fwd(games / "VALORANT" / "live") + "\"\nshortcut_name: \"VALORANT.lnk\"\n");
    // No shortcut_name: named by the folder above "Live".
    WriteText(meta / "teamfighttactics.live" / "teamfighttactics.live.product_settings.yaml",
              "product_install_full_path: \"" + Fwd(games / "Teamfight Tactics" / "Live") + "\"\n");
    // Installed once, folder gone since.
    WriteText(meta / "league_of_legends.pbe" / "league_of_legends.pbe.product_settings.yaml",
              "product_install_full_path: \"" + Fwd(games / "League of Legends (PBE)") + "\"\n");
    // The launcher's own metadata folder has a settings.yaml, not a product_settings.yaml.
    WriteText(meta / "Riot Client" / "Riot Client.settings.yaml", "whatever: 1\n");

    const auto found = ScanRiot(root.wstring());
    REQUIRE(found.size() == 3);
    const GameEntry* lol = FindByName(found, L"League of Legends");
    REQUIRE(lol);
    CHECK(GameKey(lol->path) == GameKey((games / "League of Legends").wstring()));
    CHECK(lol->source == GameSource::Riot);
    CHECK(lol->icon == (meta / "league_of_legends.live" / "league_of_legends.live.ico").wstring());
    const GameEntry* val = FindByName(found, L"VALORANT");
    REQUIRE(val);
    CHECK(GameKey(val->path) == GameKey((games / "VALORANT" / "live").wstring()));
    CHECK(val->icon.empty());  // no .ico beside it: the page draws the controller
    CHECK(FindByName(found, L"Teamfight Tactics"));
}

TEST_CASE("Steam: every library in libraryfolders.vdf, installed apps only, redistributables and runtimes skipped") {
    const fs::path root  = FixtureRoot(L"steam");
    const fs::path steam = root / "Steam";
    const fs::path lib2  = root / "SteamLibrary";
    WriteText(steam / "steamapps" / "libraryfolders.vdf",
              "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + VdfPath(steam) +
                  "\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"730\"\t\t\"1\"\n\t\t}\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
                  VdfPath(lib2) + "\"\n\t}\n}\n");
    fs::create_directories(steam / "steamapps" / "common" / "Counter-Strike Global Offensive");
    WriteText(steam / "steamapps" / "appmanifest_730.acf", Acf("730", "Counter-Strike 2", "Counter-Strike Global Offensive"));
    fs::create_directories(steam / "steamapps" / "common" / "Steamworks Shared");
    WriteText(steam / "steamapps" / "appmanifest_228980.acf", Acf("228980", "Steamworks Common Redistributables", "Steamworks Shared"));
    fs::create_directories(steam / "steamapps" / "common" / "SteamLinuxRuntime_sniper");
    WriteText(steam / "steamapps" / "appmanifest_1628350.acf", Acf("1628350", "Steam Linux Runtime 3.0 (sniper)", "SteamLinuxRuntime_sniper"));
    // Mid-download: StateFlags without bit 4.
    fs::create_directories(steam / "steamapps" / "common" / "Downloading");
    WriteText(steam / "steamapps" / "appmanifest_111.acf", Acf("111", "Downloading", "Downloading", "1026"));
    // A manifest whose folder is gone.
    WriteText(steam / "steamapps" / "appmanifest_222.acf", Acf("222", "Gone", "Gone"));
    // A temp manifest Steam leaves while writing.
    WriteText(steam / "steamapps" / "appmanifest_333.acf.tmp", Acf("333", "Temp", "Counter-Strike Global Offensive"));
    // Icons live in the Steam install's cache, whatever library the game is in.
    const fs::path cache = steam / "appcache" / "librarycache";
    const fs::path cs_icon = cache / "730" / "b6e290dd5a92ce98f89089a207733c70c41a1871.jpg";
    WriteText(cs_icon, "jpg");
    WriteText(cache / "730" / "header.jpg", "jpg");
    WriteText(cache / "730" / "12351fd67b63a03971952ca142dba8b58d811b18" / "a.jpg", "jpg");  // other art
    WriteText(cache / "546560_icon.jpg", "jpg");  // the pre-2024 layout
    fs::create_directories(lib2 / "steamapps" / "common" / "Half-Life Alyx");
    WriteText(lib2 / "steamapps" / "appmanifest_546560.acf", Acf("546560", "Half-Life: Alyx", "Half-Life Alyx", "6"));

    const auto found = ScanSteam(steam.wstring());
    REQUIRE(found.size() == 2);
    const GameEntry* cs = FindByName(found, L"Counter-Strike 2");
    REQUIRE(cs);
    CHECK(GameKey(cs->path) == GameKey((steam / "steamapps" / "common" / "Counter-Strike Global Offensive").wstring()));
    CHECK(cs->source == GameSource::Steam);
    CHECK(cs->icon == cs_icon.wstring());
    const GameEntry* alyx = FindByName(found, L"Half-Life: Alyx");
    REQUIRE(alyx);
    CHECK(alyx->icon == (cache / "546560_icon.jpg").wstring());
}

TEST_CASE("Steam: two hashed files in an app's cache folder give no icon rather than a guess") {
    const fs::path root  = FixtureRoot(L"steam_icon_ambiguous");
    const fs::path steam = root / "Steam";
    fs::create_directories(steam / "steamapps" / "common" / "Game");
    WriteText(steam / "steamapps" / "appmanifest_9.acf", Acf("9", "Game", "Game"));
    WriteText(steam / "appcache" / "librarycache" / "9" / "b6e290dd5a92ce98f89089a207733c70c41a1871.jpg", "jpg");
    WriteText(steam / "appcache" / "librarycache" / "9" / "f6da1420a173324d49bcd470fa3eee781ad0fa5e.jpg", "jpg");
    const auto found = ScanSteam(steam.wstring());
    REQUIRE(found.size() == 1);
    CHECK(found[0].icon.empty());
}

TEST_CASE("Steam: the pre-2021 libraryfolders.vdf format still lists extra libraries") {
    const fs::path root  = FixtureRoot(L"steam_old");
    const fs::path steam = root / "Steam";
    const fs::path lib2  = root / "Lib";
    WriteText(steam / "steamapps" / "libraryfolders.vdf",
              "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1\"\n\t\"1\"\t\t\"" + VdfPath(lib2) + "\"\n}\n");
    fs::create_directories(lib2 / "steamapps" / "common" / "Game");
    WriteText(lib2 / "steamapps" / "appmanifest_9.acf", Acf("9", "Old Library Game", "Game"));
    const auto found = ScanSteam(steam.wstring());
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == L"Old Library Game");
}

TEST_CASE("Epic: complete base-game installs only") {
    const fs::path root = FixtureRoot(L"epic");
    const fs::path manifests = root / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
    auto item = [&](const char* file, const std::string& json) { WriteText(manifests / file, json); };
    auto folder = [&](const char* name) {
        fs::create_directories(root / "Games" / name);
        std::string p = (root / "Games" / name).string(), out;
        for (char c : p) {
            out.push_back(c);
            if (c == '\\') out.push_back('\\');
        }
        return out;
    };
    item("A.item", R"({"DisplayName":"Fortnite","AppName":"Fortnite","MainGameAppName":"Fortnite","InstallLocation":")" +
                       folder("Fortnite") +
                       R"(","LaunchExecutable":"FortniteGame/Binaries/Win64/FortniteBootstrapper.exe","AppCategories":["public","games","applications"]})");
    const fs::path fortnite_exe = root / "Games" / "Fortnite" / "FortniteGame" / "Binaries" / "Win64" / "FortniteBootstrapper.exe";
    WriteText(fortnite_exe, "MZ");
    item("B.item", R"({"DisplayName":"Some DLC","AppName":"Dlc1","MainGameAppName":"Base","InstallLocation":")" +
                       folder("Dlc") + R"("})");
    item("C.item", R"({"DisplayName":"Half done","AppName":"H","MainGameAppName":"H","bIsIncompleteInstall":true,"InstallLocation":")" +
                       folder("Half") + R"("})");
    item("D.item", R"({"DisplayName":"Engine plugin","AppName":"P","InstallLocation":")" + folder("Plugin") +
                       R"(","AppCategories":["plugins","plugins/engine"]})");
    item("E.item", "{ not json");
    item("F.txt", "{}");

    const auto found = ScanEpic(root.wstring());
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == L"Fortnite");
    CHECK(found[0].source == GameSource::Epic);
    CHECK(found[0].icon == fortnite_exe.wstring());
}

TEST_CASE("GOG and Ubisoft: registry installs, scratch key") {
    const fs::path root = FixtureRoot(L"registry");
    fs::create_directories(root / "Games" / "Witcher");
    fs::create_directories(root / "Games" / "Anno");
    const fs::path exe = root / "Games" / "Witcher" / "witcher.exe";
    WriteText(exe, "MZ");

    const std::wstring base = L"Software\\OmbraTests\\launchers";
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    auto put = [&](const std::wstring& sub, const wchar_t* value, const std::wstring& data) {
        HKEY k = nullptr;
        REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\" + sub).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                                nullptr) == ERROR_SUCCESS);
        RegSetValueExW(k, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                       static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    };
    const std::wstring witcher = (root / "Games" / "Witcher").wstring();
    put(L"gog\\1207664643_is1", L"Publisher", L"GOG.com");
    put(L"gog\\1207664643_is1", L"InstallLocation", witcher);
    put(L"gog\\1207664643_is1", L"DisplayName", L"The Witcher");
    put(L"gog\\1207664643_is1", L"DisplayIcon", L"\"" + exe.wstring() + L"\",0");
    put(L"gog\\GOGPACK_is1", L"Publisher", L"GOG.com");  // not "<digits>_is1"
    put(L"gog\\GOGPACK_is1", L"InstallLocation", witcher);
    put(L"gog\\99_is1", L"Publisher", L"Someone Else");  // another publisher's Inno Setup entry
    put(L"gog\\99_is1", L"InstallLocation", witcher);
    put(L"gog\\55_is1", L"Publisher", L"GOG.com");  // folder gone
    put(L"gog\\55_is1", L"InstallLocation", (root / "Gone").wstring());
    std::wstring fwd = (root / "Games" / "Anno").wstring();
    for (wchar_t& c : fwd)
        if (c == L'\\') c = L'/';
    put(L"ubi\\Installs\\1", L"InstallDir", fwd + L"/");  // the launcher writes forward slashes
    put(L"ubi\\Installs\\2", L"InstallDir", (root / "Gone").wstring());

    const auto gog = ScanGog(HKEY_CURRENT_USER, (base + L"\\gog").c_str());
    REQUIRE(gog.size() == 1);
    CHECK(gog[0].name == L"The Witcher");
    CHECK(gog[0].source == GameSource::Gog);
    CHECK(gog[0].icon == exe.wstring());

    const auto ubi = ScanUbisoft(HKEY_CURRENT_USER, (base + L"\\ubi\\Installs").c_str());
    REQUIRE(ubi.size() == 1);
    CHECK(ubi[0].name == L"Anno");
    CHECK(ubi[0].source == GameSource::Ubisoft);

    CHECK(ScanGog(HKEY_CURRENT_USER, L"Software\\OmbraTests\\nothing").empty());
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
}

TEST_CASE("scanners return nothing, without throwing, when a launcher isn't installed") {
    const fs::path root = FixtureRoot(L"empty");
    CHECK(ScanRiot(root.wstring()).empty());
    CHECK(ScanSteam((root / "NoSteam").wstring()).empty());
    CHECK(ScanEpic(root.wstring()).empty());
    CHECK(ScanRiot(L"").empty());
    CHECK(ScanLaunchers(LauncherRoots{}).empty());
}

TEST_CASE("merge: a decision on a detected game overrides it and keeps the detected name; other decisions follow") {
    std::vector<GameEntry> detected = {
        Entry(L"VALORANT", L"E:\\Games\\Riot Games\\VALORANT\\live", true, GameSource::Riot),
        Entry(L"SteamVR", L"E:\\Games\\Steam\\steamapps\\common\\SteamVR", true, GameSource::Steam),
    };
    detected[1].icon = L"E:\\Games\\Steam\\appcache\\librarycache\\250820_icon.jpg";
    const std::vector<GameEntry> decisions = {
        Entry(L"whatever", L"e:/games/steam/steamapps/common/SteamVR/", false),
        Entry(L"Discord", L"C:\\Users\\x\\AppData\\Local\\Discord", false),
    };
    const auto merged = MergeGames(detected, decisions);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].name == L"VALORANT");
    CHECK(merged[0].is_game);
    CHECK(merged[0].detected);
    CHECK(merged[1].name == L"SteamVR");
    CHECK(merged[1].source == GameSource::Steam);
    CHECK_FALSE(merged[1].is_game);
    CHECK(merged[1].icon == detected[1].icon);  // a decision carries no icon; the scan's stays
    CHECK(merged[2].name == L"Discord");
    CHECK_FALSE(merged[2].detected);
}

TEST_CASE("classify: a folder covers everything under it; the most specific entry wins; the shell is never a game") {
    const std::vector<GameEntry> games = {
        Entry(L"VALORANT", L"E:\\Games\\Riot Games\\VALORANT\\live", true, GameSource::Riot),
        Entry(L"League", L"E:\\Games\\Riot Games\\League of Legends", true, GameSource::Riot),
        Entry(L"League client", L"E:\\Games\\Riot Games\\League of Legends\\LeagueClientUx.exe", false),
        Entry(L"Discord", L"C:\\Users\\x\\AppData\\Local\\Discord", false),
        Entry(L"explorer", L"C:\\Windows", true),  // nonsense on purpose: the floor still wins
    };
    const GameEntry* match = nullptr;
    CHECK(ClassifyImage(games, kValorantExe, &match) == GameVerdict::Game);
    REQUIRE(match);
    CHECK(match->name == L"VALORANT");
    CHECK(ClassifyImage(games, L"E:\\Games\\Riot Games\\League of Legends\\Game\\League of Legends.exe") == GameVerdict::Game);
    CHECK(ClassifyImage(games, L"E:\\Games\\Riot Games\\League of Legends\\LeagueClientUx.exe") == GameVerdict::NotGame);
    // Discord updates into app-<version>\: a folder entry survives that.
    CHECK(ClassifyImage(games, L"C:\\Users\\x\\AppData\\Local\\Discord\\app-1.0.9259\\Discord.exe") == GameVerdict::NotGame);
    // Shares a prefix, isn't under it.
    CHECK(ClassifyImage(games, L"E:\\Games\\Riot Games\\League of Legends (PBE)\\LeagueClientUx.exe") == GameVerdict::Unknown);
    CHECK(ClassifyImage(games, L"E:\\Games\\Riot Games\\Riot Client\\RiotClientServices.exe") == GameVerdict::Unknown);
    CHECK(ClassifyImage(games, L"C:\\Windows\\explorer.exe", &match) == GameVerdict::NotGame);
    CHECK(match == nullptr);
    CHECK(ClassifyImage(games, L"") == GameVerdict::Unknown);
    CHECK(ClassifyImage({}, kValorantExe) == GameVerdict::Unknown);
}

TEST_CASE("classify: a bare exe name (only from migrated old lists) matches that file name exactly") {
    const std::vector<GameEntry> games = {Entry(L"obs64.exe", L"obs64.exe", false)};
    CHECK(ClassifyImage(games, L"E:\\Programs\\obs-studio\\bin\\64bit\\obs64.exe") == GameVerdict::NotGame);
    CHECK(ClassifyImage(games, L"E:\\Programs\\obs-studio\\bin\\64bit\\obs64-helper.exe") == GameVerdict::Unknown);
}

TEST_CASE("games.list validates, parses and round-trips") {
    std::string err;
    const JsonValue ok = ParseJson(
        R"([{"name":"VALORANT","path":"E:\\Games\\Riot Games\\VALORANT\\live","source":"riot","game":true},)"
        R"({"path":"C:\\Tools\\thing.exe","game":false}])");
    CHECK(ValidateGamesList(ok, err));
    const auto parsed = ParseGamesList(ok);
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].source == GameSource::Riot);
    CHECK(parsed[1].name == L"thing.exe");  // a missing name falls back to the file name
    CHECK(parsed[1].source == GameSource::User);
    CHECK_FALSE(parsed[1].is_game);
    const auto again = ParseGamesList(SerializeGamesList(parsed));
    REQUIRE(again.size() == 2);
    CHECK(again[0].path == parsed[0].path);
    CHECK(again[1].is_game == parsed[1].is_game);

    CHECK_FALSE(ValidateGamesList(ParseJson(R"([{"name":"no path"}])"), err));
    CHECK_FALSE(ValidateGamesList(ParseJson(R"([{"path":""}])"), err));
    CHECK_FALSE(ValidateGamesList(ParseJson(R"([{"path":"x","source":"origin"}])"), err));
    CHECK_FALSE(ValidateGamesList(ParseJson(R"([{"path":"x","game":"yes"}])"), err));
    CHECK_FALSE(ValidateGamesList(ParseJson(R"({"path":"x"})"), err));
    CHECK(ParseGamesList(ParseJson(R"([{"path":3}])")).empty());
}

TEST_CASE("migration: exclusions become \"not a game\", overrides \"a game\", an exclusion wins a tie, existing entries stay") {
    const JsonValue excl = ParseJson(R"(["obs64.exe","Discord.exe",""])");
    const JsonValue over = ParseJson(R"(["VALORANT-Win64-Shipping.exe","discord.exe"])");
    const std::vector<GameEntry> existing = {Entry(L"OBS", L"OBS64.EXE", true)};
    const auto out = MigrateLegacyGameLists(excl, over, existing);
    REQUIRE(out.size() == 2);
    CHECK(out[0].path == L"Discord.exe");
    CHECK_FALSE(out[0].is_game);
    CHECK(out[1].path == L"VALORANT-Win64-Shipping.exe");
    CHECK(out[1].is_game);
    CHECK(ClassifyImage(out, kValorantExe) == GameVerdict::Game);
}

TEST_CASE("migration: LoadConfig rewrites the old keys into games.list and removes them") {
    const std::wstring dir = ConfigDirW();
    REQUIRE(!dir.empty());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\config.json";
    WriteText(fs::path(path), R"JSON({
  "capture": {"target": "window_with_fallback", "target_window": "Discord.exe",
              "target_programs": [{"exe": "League of Legends.exe"}]},
  "engine": {"exclusions": ["obs64.exe"], "game_overrides": ["VALORANT-Win64-Shipping.exe"], "game_grace_sec": 45}
})JSON");

    const ConfigLoadResult result = LoadConfig();
    CHECK(result.issues.empty());
    CHECK(result.config.capture_target == "window");
    const auto list = ParseGamesList(result.raw.at("games.list"));
    REQUIRE(list.size() == 2);
    CHECK(list[0].path == L"obs64.exe");
    CHECK_FALSE(list[0].is_game);
    CHECK(list[1].is_game);

    // Written back once: the old keys are gone from the file, the rest is kept.
    const std::string text = ReadText(fs::path(path));
    CHECK(text.find("exclusions") == std::string::npos);
    CHECK(text.find("game_overrides") == std::string::npos);
    CHECK(text.find("target_programs") == std::string::npos);
    CHECK(text.find("window_with_fallback") == std::string::npos);
    CHECK(text.find("games") != std::string::npos);
    CHECK(text.find("Discord.exe") != std::string::npos);  // target_window untouched

    // A second load finds nothing to migrate and leaves the list as it is.
    const ConfigLoadResult again = LoadConfig();
    CHECK(ParseGamesList(again.raw.at("games.list")).size() == 2);
    DeleteFileW(path.c_str());
}

TEST_CASE("arm decision: the list decides when it knows the program, the heuristic only when it doesn't") {
    CHECK(DecideGameArm(GameVerdict::Game, 0.0).arm);
    CHECK_FALSE(DecideGameArm(GameVerdict::Game, 0.0).heuristic);
    CHECK_FALSE(DecideGameArm(GameVerdict::NotGame, 100.0).arm);  // fullscreen YouTube, marked
    CHECK_FALSE(DecideGameArm(GameVerdict::Unknown, kGameArmThreshold - 1).arm);
    const GameArmDecision guess = DecideGameArm(GameVerdict::Unknown, kGameArmThreshold);
    CHECK(guess.arm);
    CHECK(guess.heuristic);
}

TEST_CASE("library: heuristic arms are remembered until the list decides them, and persist across restarts") {
    const fs::path root = FixtureRoot(L"library");
    const std::wstring seen_file = (root / "games_seen.json").wstring();
    const std::wstring tool = L"C:\\Tools\\Renderer\\renderer.exe";
    {
        GameLibrary lib(LauncherRoots{}, seen_file);
        lib.Rescan();
        lib.NoteHeuristicArm(L"Renderer", tool);
        lib.NoteHeuristicArm(L"Renderer", tool);  // once, not twice
        lib.NoteHeuristicArm(L"Explorer", L"C:\\Windows\\explorer.exe");  // the floor already decides it
        REQUIRE(lib.Seen().size() == 1);
        CHECK(lib.Seen()[0].name == L"Renderer");
    }
    GameLibrary lib(LauncherRoots{}, seen_file);
    REQUIRE(lib.Seen().size() == 1);
    CHECK(lib.Classify(tool) == GameVerdict::Unknown);

    lib.SetDecisions({Entry(L"Renderer", tool.c_str(), false)});  // "Not a game"
    CHECK(lib.Seen().empty());
    std::wstring name;
    CHECK(lib.Classify(tool, &name) == GameVerdict::NotGame);
    CHECK(name == L"Renderer");
    CHECK_FALSE(lib.GameFilter()(tool));
    CHECK(GameLibrary(LauncherRoots{}, seen_file).Seen().empty());
}

TEST_CASE("library: a heuristic arm counts as a game for this run only, and a decision overrides it") {
    const std::wstring tool = L"C:\\Tools\\Renderer\\renderer.exe";
    GameLibrary lib(LauncherRoots{}, L"");
    lib.Rescan();
    CHECK_FALSE(lib.GameFilter()(tool));
    lib.NoteHeuristicArm(L"Renderer", tool);
    // Arming and "The game" capture ask the same filter, so a heuristic game is also what gets recorded.
    CHECK(lib.GameFilter()(tool));
    CHECK(lib.GameFilter()(L"c:/tools/renderer/RENDERER.EXE"));
    CHECK_FALSE(lib.GameFilter()(L"C:\\Tools\\Renderer\\other.exe"));

    lib.SetDecisions({Entry(L"Renderer", tool.c_str(), false)});  // "Not a game" ends it now
    CHECK_FALSE(lib.GameFilter()(tool));

    // Not persisted: the next run has to see it look like a game again.
    const fs::path root = FixtureRoot(L"library-run");
    const std::wstring seen_file = (root / "games_seen.json").wstring();
    { GameLibrary a(LauncherRoots{}, seen_file); a.Rescan(); a.NoteHeuristicArm(L"Renderer", tool); }
    GameLibrary b(LauncherRoots{}, seen_file);
    b.Rescan();
    CHECK(b.Seen().size() == 1);  // still listed under "Looked like a game"
    CHECK_FALSE(b.GameFilter()(tool));
}

namespace {
ArmInputs Inputs(bool enabled, bool only_games, bool window, bool present) {
    ArmInputs in;
    in.user_enabled   = enabled;
    in.only_in_games  = only_games;
    in.window_target  = window;
    in.target_present = present;
    return in;
}
}  // namespace

TEST_CASE("arming: When x What x the switch") {
    // Always records whether or not a game is open; Only in games only while one is.
    CHECK(WantArmed(Inputs(true, false, false, false)));
    CHECK(WantArmed(Inputs(true, false, false, true)));
    CHECK_FALSE(WantArmed(Inputs(true, true, false, false)));
    CHECK(WantArmed(Inputs(true, true, false, true)));
    // One window ignores When: it records while its program is open.
    CHECK_FALSE(WantArmed(Inputs(true, false, true, false)));
    CHECK(WantArmed(Inputs(true, false, true, true)));
    CHECK_FALSE(WantArmed(Inputs(true, true, true, false)));
    // Switched off, nothing records.
    CHECK_FALSE(WantArmed(Inputs(false, false, false, true)));
    CHECK_FALSE(WantArmed(Inputs(false, true, true, true)));

    CHECK_FALSE(NeedsWindowPoll("desktop", "monitor"));
    CHECK(NeedsWindowPoll("desktop", "game"));  // to name the game it records
    CHECK(NeedsWindowPoll("desktop", "window"));
    CHECK(NeedsWindowPoll("game", "monitor"));

    CHECK(BufferController::InitialArmed("desktop", "monitor"));
    CHECK(BufferController::InitialArmed("desktop", "game"));  // the display until a game opens
    CHECK_FALSE(BufferController::InitialArmed("desktop", "window"));
    CHECK_FALSE(BufferController::InitialArmed("game", "monitor"));
    CHECK_FALSE(BufferController::InitialArmed("game", "game"));
}

// Always + displays never needed a game to arm, and so never looked for one: every clip was "Desktop",
// per-game folders did nothing and game_only audio captured nothing.
TEST_CASE("Always + displays still names the game it's recording") {
    const std::wstring exe = L"C:\\Games\\Renderer\\renderer.exe";
    GameLibrary lib(LauncherRoots{}, L"");
    lib.SetDecisions({Entry(L"Renderer", exe.c_str(), true)});
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    REQUIRE(hwnd);
    std::mutex mutex;
    std::vector<WindowCandidate> windows;
    WindowCandidate candidate;
    candidate.id         = reinterpret_cast<uint64_t>(hwnd);
    candidate.image_path = exe;
    candidate.foreground = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        windows = {candidate};
    }
    const auto wait_for_pid = [&](BufferController& c, DWORD pid) {
        for (int i = 0; i < 300 && c.CurrentGamePid() != pid; ++i) Sleep(10);
        return c.CurrentGamePid() == pid;
    };
    {
        BufferController controller("desktop", "monitor", "", {}, &lib);
        controller.SetWindowSnapshotForTest([&] {
            std::lock_guard<std::mutex> lock(mutex);
            return windows;
        });
        std::vector<std::wstring> sessions;  // what the usage sink was told
        controller.SetGameSessionSink([&](const std::wstring& path) {
            std::lock_guard<std::mutex> lock(mutex);
            sessions.push_back(path);
        });
        const auto sessions_now = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return sessions;
        };
        controller.Start();
        CHECK(wait_for_pid(controller, GetCurrentProcessId()));
        CHECK(controller.CurrentGameFolder() == L"Renderer");
        CHECK(sessions_now() == std::vector<std::wstring>{exe});
        // It closes: the next foreground change clears it. A target change wakes the same pass.
        {
            std::lock_guard<std::mutex> lock(mutex);
            windows.clear();
        }
        controller.SetTarget("monitor", "wake");
        CHECK(wait_for_pid(controller, 0));
        for (int i = 0; i < 300 && !controller.CurrentGameFolder().empty(); ++i) Sleep(10);
        CHECK(controller.CurrentGameFolder().empty());
        CHECK(sessions_now() == std::vector<std::wstring>{exe, L""});
        // Back, then the engine stops mid-game: Stop ends the session.
        {
            std::lock_guard<std::mutex> lock(mutex);
            windows = {candidate};
        }
        controller.SetTarget("monitor", "wake again");
        CHECK(wait_for_pid(controller, GetCurrentProcessId()));
        controller.Stop();
        CHECK(sessions_now() == std::vector<std::wstring>{exe, L"", exe, L""});
    }
    DestroyWindow(hwnd);
}

TEST_CASE("arm rule: every display off stops recording after the settle, and coming back is immediate") {
    ArmInputs off = Inputs(true, false, false, false);
    off.display_off = true;
    CHECK_FALSE(WantArmed(off));

    ArmState s;
    CHECK(StepArm(s, Inputs(true, false, false, false), 0, 10'000));
    CHECK(StepArm(s, off, 1'000, 10'000));  // a brief blank costs no barrier
    CHECK(StepArm(s, Inputs(true, false, false, false), 5'000, 10'000));
    CHECK_FALSE(s.leaving);
    CHECK(StepArm(s, off, 6'000, 10'000));
    CHECK_FALSE(StepArm(s, off, 16'000, 10'000));
    CHECK(StepArm(s, Inputs(true, false, false, false), 16'001, 10'000));
}

TEST_CASE("controller: Always + a display disarms with the display off and re-arms with it on, with no poll") {
    Ring ring;
    ring.Init(1024 * 1024, 64 * 1024);
    PipelineSupervisor sup(60, 10000, 2, &ring);
    sup.SetArmed(true);
    BufferController controller("desktop", "monitor", "", {&sup});
    controller.SetWindowSnapshotForTest([] { return std::vector<WindowCandidate>{}; });
    controller.SetDisarmSettleForTest(50);
    controller.Start();
    const auto wait_for = [&](bool armed) {
        for (int i = 0; i < 300 && sup.Armed() != armed; ++i) Sleep(10);
        return sup.Armed() == armed;
    };
    controller.SetDisplayOff(true);
    CHECK(wait_for(false));
    // Switching on while it's off doesn't start a recording.
    controller.SetUserEnabled(false);
    controller.SetUserEnabled(true);
    Sleep(50);
    CHECK_FALSE(sup.Armed());
    controller.SetDisplayOff(false);
    CHECK(wait_for(true));
    controller.Stop();
}

// Two games open while a display records: the one in front is the one on screen.
TEST_CASE("Always + displays with two games: the clip follows the one in front") {
    const std::wstring first = L"C:\\Games\\First\\first.exe", second = L"C:\\Games\\Second\\second.exe";
    GameLibrary lib(LauncherRoots{}, L"");
    lib.SetDecisions({Entry(L"First", first.c_str(), true), Entry(L"Second", second.c_str(), true)});
    HWND a = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND b = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    REQUIRE(a);
    REQUIRE(b);
    std::mutex mutex;
    bool second_in_front = false;
    const auto snapshot = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        WindowCandidate one, two;
        one.id = reinterpret_cast<uint64_t>(a);
        one.image_path = first;
        one.foreground = !second_in_front;
        two.id = reinterpret_cast<uint64_t>(b);
        two.image_path = second;
        two.foreground = second_in_front;
        return std::vector<WindowCandidate>{one, two};
    };
    {
        BufferController controller("desktop", "monitor", "", {}, &lib);
        controller.SetWindowSnapshotForTest(snapshot);
        controller.Start();
        const auto wait_for_folder = [&](const std::wstring& want) {
            for (int i = 0; i < 300 && controller.CurrentGameFolder() != want; ++i) Sleep(10);
            return controller.CurrentGameFolder() == want;
        };
        CHECK(wait_for_folder(L"First"));
        {
            std::lock_guard<std::mutex> lock(mutex);
            second_in_front = true;
        }
        controller.SetTarget("monitor", "wake");
        CHECK(wait_for_folder(L"Second"));
        controller.Stop();
    }
    DestroyWindow(a);
    DestroyWindow(b);
}

TEST_CASE("arming: a game that goes away disarms only after the settle; one that comes back costs nothing") {
    constexpr int64_t kSettle = 10'000;
    ArmState s;
    CHECK(StepArm(s, Inputs(true, true, false, true), 0, kSettle));  // arms at once

    // The window is recreated (fullscreen toggle): gone for 3 s, back. No disarm, so no barrier.
    CHECK(StepArm(s, Inputs(true, true, false, false), 1'000, kSettle));
    CHECK(StepArm(s, Inputs(true, true, false, false), 4'000, kSettle));
    CHECK(StepArm(s, Inputs(true, true, false, true), 4'500, kSettle));
    CHECK_FALSE(s.leaving);

    // The game quits: still armed until the settle has passed since it went, counted afresh.
    CHECK(StepArm(s, Inputs(true, true, false, false), 20'000, kSettle));
    CHECK(StepArm(s, Inputs(true, true, false, false), 29'999, kSettle));
    CHECK_FALSE(StepArm(s, Inputs(true, true, false, false), 30'000, kSettle));
    CHECK_FALSE(StepArm(s, Inputs(true, true, false, false), 90'000, kSettle));

    // The switch turned off disarms at once, no settle.
    CHECK(StepArm(s, Inputs(true, true, false, true), 100'000, kSettle));
    CHECK_FALSE(StepArm(s, Inputs(false, true, false, true), 100'001, kSettle));
    // And back on with the game still open: at once.
    CHECK(StepArm(s, Inputs(true, true, false, true), 100'002, kSettle));
}

TEST_CASE("arming: the window poll is cheap on this desktop (read-only)") {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    SnapshotTargetWindows();  // first call loads whatever it loads
    constexpr int kRuns = 20;
    QueryPerformanceCounter(&a);
    size_t windows = 0;
    for (int i = 0; i < kRuns; ++i) windows = SnapshotTargetWindows().size();
    QueryPerformanceCounter(&b);
    const double ms = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / kRuns;
    MESSAGE("SnapshotTargetWindows: " << windows << " windows, " << ms << " ms per call");
    CHECK(ms < 20.0);
}

TEST_CASE("library: this machine's real launchers scan in milliseconds (read-only)") {
    GameLibrary lib(DefaultLauncherRoots(), L"");
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    lib.Rescan();
    QueryPerformanceCounter(&b);
    const double ms = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
    const auto games = lib.Games();
    std::string names;
    for (const GameEntry& g : games) names += " [" + std::string(GameSourceName(g.source)) + "] " + NarrowUtf8(g.name) + ";";
    MESSAGE("launcher scan: " << games.size() << " games in " << ms << " ms:" << names);
    CHECK(ms < 500.0);
}

}  // TEST_SUITE

TEST_CASE("SanitizeFolderName: reserved device names, with or without an extension") {
    CHECK(SanitizeFolderName(L"CON") == L"CON_");
    CHECK(SanitizeFolderName(L"con.foo") == L"con_.foo");
    CHECK(SanitizeFolderName(L"LPT1.tar.gz") == L"LPT1_.tar.gz");
    CHECK(SanitizeFolderName(L"NUL .x") == L"NUL_ .x");
    CHECK(SanitizeFolderName(L"Control") == L"Control");
    CHECK(SanitizeFolderName(L"CONSOLE.foo") == L"CONSOLE.foo");
    CHECK(SanitizeFolderName(std::wstring(100, L'x')).size() == 64);
}

TEST_CASE("ForegroundRescoreDue: every 5 s for the first minute of focus") {
    CHECK_FALSE(ForegroundRescoreDue(4'000, 4'000));
    CHECK(ForegroundRescoreDue(5'000, 5'000));
    CHECK_FALSE(ForegroundRescoreDue(7'000, 2'000));
    CHECK(ForegroundRescoreDue(59'000, 5'000));
    CHECK_FALSE(ForegroundRescoreDue(60'000, 5'000));
}

}  // namespace ombra::test
