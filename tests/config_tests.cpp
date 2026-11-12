// config.cpp's WarnIfUnimplemented dedup. Headless, no GPU.
//
// ApplyConfigPatch runs on every get_config/set_config round trip, not
// just at startup, so every Settings interaction re-logged all "not yet wired" warnings for the
// keys present in the patch (5x in 2 minutes, observed live). Fixed by warning once per key per
// process instead of once per ApplyConfigPatch call.

#include <doctest/doctest.h>

#include "ombra/config.h"
#include "ombra/json.h"
#include "ombra/log.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>

namespace ombra::test {

namespace {

std::string ReadWholeFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

// A live config.json had two
// different sources (Code.exe and Discord.exe) both carrying a stray "track": 4 -- a field that
// was never read anywhere (config.h's AudioSourceSpec comment) and was deleted. A
// config.json already on disk with that key must still load cleanly, same as any other unknown
// key (SaveConfig's own file-header comment: "unrecognized keys ... are left alone, never
// deleted").
TEST_CASE("config: LoadConfig tolerates a legacy \"track\" key in audio.sources") {
    const std::wstring dir = ConfigDirW();
    REQUIRE(!dir.empty());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\config.json";

    {
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f);
        f << R"JSON({
  "audio": {
    "sources": [
      {"id": "desktop", "kind": "output_loopback", "device": "default", "mode": "all",
       "track": 4, "enabled": true},
      {"id": "only_Discord.exe", "kind": "output_loopback", "device": "default", "mode": "only",
       "target": "Discord.exe", "track": 4, "enabled": true}
    ]
  }
})JSON";
    }

    const ConfigLoadResult result = LoadConfig();
    CHECK(result.file_existed);
    CHECK(result.issues.empty());

    const auto specs = ResolveAudioSources(result.raw.at("audio.sources"));
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].id == "desktop");
    CHECK(specs[0].mode == "all");
    CHECK(specs[1].id == "only_Discord.exe");
    CHECK(specs[1].target == "Discord.exe");
}

// DiffAudioSources (config.h) is how the audio target watcher applies a new audio.sources list
// live: which running source each new entry is, and which captures reopen. main.cpp's watcher
// isn't reachable headless, so this pins the contract it relies on.
TEST_CASE("config: DescribeNonDefaultSettings lists only changed keys and shortens long raw values") {
    Config       cfg;
    RawConfigMap raw;
    for (const std::string& key : RawConfigKeys()) raw[key] = JsonValue{};
    CHECK(DescribeNonDefaultSettings(cfg, raw).empty());

    cfg.buffer_seconds = Config{}.buffer_seconds + 30;
    cfg.engine_autostart = !Config{}.engine_autostart;
    JsonValue games = JsonValue::MakeArray();
    for (int i = 0; i < 50; ++i) games.arr.push_back(JsonValue::MakeStr("some-long-game-name-" + std::to_string(i)));
    raw["games.list"] = games;
    const std::string text = DescribeNonDefaultSettings(cfg, raw);
    CHECK(text.find("buffer.seconds=") != std::string::npos);
    CHECK(text.find("engine.autostart=") != std::string::npos);
    CHECK(text.find("games.list=[50 entries]") != std::string::npos);
    CHECK(text.find("capture.monitors") == std::string::npos);
    CHECK(text.find("hotkeys.") == std::string::npos);
}

TEST_CASE("config: DiffAudioSources matches sources by id") {
    AudioSourceSpec desktop;
    desktop.id = "desktop";
    desktop.group_id = "desktop";

    AudioSourceSpec discord;
    discord.id = "only_Discord.exe";
    discord.mode = "only";
    discord.target = "Discord.exe";
    discord.group_id = "only_Discord.exe";

    const std::vector<AudioSourceSpec> before = {desktop, discord};

    SUBCASE("gain, grouping and on/off keep every capture open") {
        std::vector<AudioSourceSpec> after = before;
        after[1].gain_db  = -6.0;
        after[1].group_id = "desktop";
        after[0].enabled  = false;
        const auto d = DiffAudioSources(before, after);
        CHECK(d.old_index == std::vector<int>{0, 1});
        CHECK(d.reopen == std::vector<bool>{false, false});
        CHECK(d.removed.empty());
    }
    SUBCASE("an added source is new; the others are untouched") {
        std::vector<AudioSourceSpec> after = before;
        AudioSourceSpec brave;
        brave.id     = "only_brave.exe";
        brave.mode   = "only";
        brave.target = "brave.exe";
        after.insert(after.begin() + 1, brave);
        const auto d = DiffAudioSources(before, after);
        CHECK(d.old_index == std::vector<int>{0, -1, 1});
        CHECK(d.reopen == std::vector<bool>{false, false, false});
    }
    SUBCASE("a removed source is listed; reordering is not a change") {
        const auto d = DiffAudioSources(before, {discord});
        CHECK(d.old_index == std::vector<int>{1});
        CHECK(d.removed == std::vector<size_t>{0});
        const auto r = DiffAudioSources(before, {discord, desktop});
        CHECK(r.old_index == std::vector<int>{1, 0});
        CHECK(r.reopen == std::vector<bool>{false, false});
    }
    SUBCASE("a retarget or a device change reopens that capture only") {
        std::vector<AudioSourceSpec> after = before;
        after[1].target = "brave.exe";
        after[0].device = "{0.0.0.00000000}.{earphones}";
        CHECK(DiffAudioSources(before, after).reopen == std::vector<bool>{true, true});
    }
}

TEST_CASE("config: ResolveAudioSources gives a duplicated id a suffix") {
    JsonValue raw = JsonValue::MakeArray();
    for (int i = 0; i < 2; ++i) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("id", JsonValue::MakeStr("only_Discord.exe"));
        raw.arr.push_back(std::move(o));
    }
    const auto specs = ResolveAudioSources(raw);
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].id == "only_Discord.exe");
    CHECK(specs[1].id == "only_Discord.exe_2");
    CHECK(specs[1].group_id == "only_Discord.exe_2");
}

// Report, 2026-09-17: toggling audio.per_app_mode off, saving, and reopening Settings still
// showed it on. Suspected the uncommitted WriteConfigFileAtomic (config.cpp) swallowing a write
// failure -- every call site (main.cpp, reconfig.cpp) drops its bool return. This is the
// discriminator: round-trip both writers for real, exactly as HandlePipeRequest calls them
// (SaveRawConfigValues for audio.sources, then SaveConfig for the typed audio.per_app_mode), and
// confirm LoadConfig sees both afterward.
TEST_CASE("config: SaveRawConfigValues and SaveConfig both persist and round-trip via LoadConfig") {
    JsonValue sources = JsonValue::MakeArray();
    JsonValue desktop = JsonValue::MakeObject();
    desktop.Set("id", JsonValue::MakeStr("desktop"));
    desktop.Set("kind", JsonValue::MakeStr("output_loopback"));
    desktop.Set("device", JsonValue::MakeStr("default"));
    desktop.Set("mode", JsonValue::MakeStr("all"));
    desktop.Set("enabled", JsonValue::MakeBool(true));
    sources.arr.push_back(desktop);

    RawConfigMap raw;
    raw["audio.sources"] = sources;
    REQUIRE(SaveRawConfigValues(raw));

    Config cfg{};
    cfg.audio_per_app_mode = false;
    REQUIRE(SaveConfig(cfg));

    const ConfigLoadResult result = LoadConfig();
    CHECK(result.file_existed);
    CHECK(result.issues.empty());
    CHECK_FALSE(result.config.audio_per_app_mode);
    const auto specs = ResolveAudioSources(result.raw.at("audio.sources"));
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].id == "desktop");
}

// A config.json carried five retired keys and logged a WARN for each on every start:
// LoadConfig must skip them quietly and SaveConfig must drop them, while a key it doesn't know at
// all survives the save untouched.
// LoadConfig prefers a flat dotted key, so a save that added a nested copy beside it was never read.
TEST_CASE("config: a save updates a key written flat in a hand-made file") {
    CreateDirectoryW(ConfigDirW().c_str(), nullptr);
    const std::wstring path = ConfigDirW() + L"\\config.json";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << R"({"capture.monitors":["old"],"capture.framerate":30})";
    }
    JsonValue monitors = JsonValue::MakeArray();
    monitors.arr.push_back(JsonValue::MakeStr("new"));
    RawConfigMap raw;
    raw["capture.monitors"] = monitors;
    REQUIRE(SaveRawConfigValues(raw));
    ConfigLoadResult loaded = LoadConfig();
    REQUIRE(loaded.raw.at("capture.monitors").arr.size() == 1);
    CHECK(loaded.raw.at("capture.monitors").arr[0].str == "new");

    loaded.config.capture_framerate = 45;
    REQUIRE(SaveConfig(loaded.config));
    CHECK(LoadConfig().config.capture_framerate == 45);
    DeleteFileW(path.c_str());
}

TEST_CASE("config: retired keys load silently and SaveConfig drops them") {
    const std::wstring path = ConfigDirW() + L"\\config.json";
    CreateDirectoryW(ConfigDirW().c_str(), nullptr);
    {
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f);
        f << R"JSON({
  "audio": {"layout": "mixed"},
  "encode": {"bframes": 2, "lookahead": 8, "tuning": "hq"},
  "ui": {"panel_edge_style": "floating"},
  "someone.elses_key": 1
})JSON";
    }

    const ConfigLoadResult loaded = LoadConfig();
    REQUIRE(loaded.issues.size() == 1);
    CHECK(loaded.issues[0].key == "someone.elses_key");

    REQUIRE(SaveConfig(loaded.config));
    const JsonValue saved = ParseJson(ReadWholeFile(path));
    CHECK(saved.Find("someone.elses_key") != nullptr);
    CHECK(saved.Find("audio")->Find("layout") == nullptr);
    CHECK(saved.Find("encode")->Find("bframes") == nullptr);
    CHECK(saved.Find("encode")->Find("lookahead") == nullptr);
    CHECK(saved.Find("encode")->Find("tuning") == nullptr);
    CHECK(saved.Find("ui")->Find("panel_edge_style") == nullptr);
    CHECK(LoadConfig().issues.size() == 1);
}

// The engine reads Raw keys with raw.at(): a first run (no file) and an unparsable file both
// returned before filling them, and the engine died with std::out_of_range on start (2026-10-21).
TEST_CASE("config: every Raw key is present after a first run and after an unparsable file") {
    const std::wstring dir = ConfigDirW();
    REQUIRE(!dir.empty());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\config.json";
    auto has_every_raw_key = [](const ConfigLoadResult& r) {
        for (const std::string& key : RawConfigKeys()) {
            CAPTURE(key);
            if (r.raw.find(key) == r.raw.end()) return false;
        }
        return true;
    };

    DeleteFileW(path.c_str());
    const ConfigLoadResult first = LoadConfig();
    CHECK_FALSE(first.file_existed);
    CHECK(has_every_raw_key(first));
    CHECK(first.raw.at("capture.monitors").type == JsonValue::Type::Null);

    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        REQUIRE(f);
        f << "{ not json";
    }
    const ConfigLoadResult broken = LoadConfig();
    CHECK(broken.file_existed);
    CHECK(broken.issues.size() == 1);
    CHECK(has_every_raw_key(broken));
    DeleteFileW(path.c_str());
}

}  // namespace ombra::test
