// HandlePipeRequest (engine_pipe.h) against an unstarted, disarmed supervisor: every request the
// engine answers that doesn't need footage, with no GPU and no named pipe. A save against real
// footage is the gpu suite's ("gpu: save_now through the pipe handler ...").
//
// tray and autotune_ctx stay null throughout: a null Tray is what keeps a capture.monitors change
// from self-restarting (EngineSelfRestart would relaunch this test binary), and a null
// AutotuneContext refuses engine.autotune instead of spawning capturebench.

#include <doctest/doctest.h>

#include "ombra/config.h"
#include "ombra/capturebench.h"
#include "ombra/clip.h"
#include "ombra/controller.h"
#include "ombra/engine_pipe.h"
#include "ombra/games.h"
#include "ombra/json.h"
#include "ombra/pipe.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/supervisor.h"
#include "ombra/util.h"
#include "support/test_env.h"

#include <string>
#include <fstream>
#include <limits>
#include <vector>

namespace ombra::test {

namespace {

class HandlerRig {
public:
    HandlerRig() : supervisor(60, 10000, 2, &ring), controller(&supervisor) {
        ring.Init(1u << 20, 64u << 10);
        supervisor.SetArmed(false);
        save_ctx.ring       = &ring;
        save_ctx.supervisor = &supervisor;
        save_ctx.controller = &controller;
        save_ctx.markers    = &markers;
    }

    JsonValue Request(const std::string& type, JsonValue fields = JsonValue::MakeObject()) {
        fields.Set("type", JsonValue::MakeStr(type));
        return HandlePipeRequest(fields, cfg, supervisor, controller, save_ctx, StartupGpuSummary{},
                                 nullptr, nullptr, nullptr, nullptr, &raw_store);
    }

    JsonValue ApiRequest(const std::string& type, JsonValue fields = JsonValue::MakeObject(),
                         const PipeCaller* caller = nullptr) {
        fields.Set("type", JsonValue::MakeStr(type));
        return HandlePipeRequest(fields, cfg, supervisor, controller, save_ctx, StartupGpuSummary{},
                                 nullptr, nullptr, nullptr, nullptr, &raw_store, nullptr, nullptr, {},
                                 PipeSurface::kApi, &app_clips, caller);
    }

    JsonValue SetConfig(JsonValue patch) {
        JsonValue req = JsonValue::MakeObject();
        req.Set("config", std::move(patch));
        return Request("set_config", std::move(req));
    }

    Config             cfg{};
    RawConfigMap       raw_store;
    Ring               ring;
    PipelineSupervisor supervisor;
    BufferController   controller;
    MarkerTimeline     markers;
    SaveContext        save_ctx;
    AppClips           app_clips;
};

bool IsBool(const JsonValue* v, bool want) {
    return v && v->type == JsonValue::Type::Bool && v->b == want;
}

}  // namespace

TEST_CASE("ScreenshotPathsFor: the fallback keeps the name and the game folder") {
    const auto folder_of = [](const std::wstring& p) { return p.substr(0, p.find_last_of(L'\\')); };
    const auto name_of = [](const std::wstring& p) { return p.substr(p.find_last_of(L'\\') + 1); };

    const auto with_game = ScreenshotPathsFor(L"Z:\\clips", L"C:\\fallback", L"League", true, 0);
    CHECK(folder_of(with_game.path) == L"Z:\\clips\\League");
    CHECK(folder_of(with_game.fallback) == L"C:\\fallback\\League");
    CHECK(name_of(with_game.path) == name_of(with_game.fallback));
    CHECK(name_of(with_game.path).ends_with(L".png"));

    const auto no_folders = ScreenshotPathsFor(L"Z:\\clips", L"C:\\fallback", L"League", false, 0);
    CHECK(folder_of(no_folders.path) == L"Z:\\clips");
    CHECK(folder_of(no_folders.fallback) == L"C:\\fallback");

    CHECK(ScreenshotPathsFor(L"Z:\\clips", L"", L"League", true, 0).fallback.empty());
}

namespace {

// One job through the real worker; returns the toasts it asked for.
std::vector<std::wstring> ScreenshotToasts(const std::wstring& path, const std::wstring& fallback,
                                           std::vector<uint8_t> bgra) {
    std::vector<std::wstring> toasts;
    ScreenshotContext ctx;
    ctx.notify = [&toasts](const wchar_t* event) { toasts.emplace_back(event); };
    ctx.job_path = path;
    ctx.job_fallback_path = fallback;
    ctx.job_width = ctx.job_height = 2;
    ctx.job_row_pitch = 8;
    ctx.job_bgra = std::move(bgra);
    ctx.job_ready = true;
    ctx.running = false;  // the worker finishes the job, then leaves
    ScreenshotWorkerLoop(&ctx);
    return toasts;
}

}  // namespace

TEST_CASE("screenshot worker: a failure or a fallback reaches the user, a plain success doesn't") {
    const std::vector<uint8_t> pixels(16, 0x80);
    const std::wstring blocker = TestOutputPath(L"shot_blocker");
    std::ofstream(blocker) << "a file, not a folder";
    const std::wstring dead = blocker + L"\\clips\\a.png";
    const std::wstring good = TestOutputPath(L"shot_good.png");
    const std::wstring other = TestOutputPath(L"shot_fallback.png");
    DeleteFileW(good.c_str());
    DeleteFileW(other.c_str());

    CHECK(ScreenshotToasts(good, other, pixels).empty());
    CHECK(GetFileAttributesW(good.c_str()) != INVALID_FILE_ATTRIBUTES);

    CHECK(ScreenshotToasts(dead, other, pixels) == std::vector<std::wstring>{L"shot_elsewhere"});
    CHECK(GetFileAttributesW(other.c_str()) != INVALID_FILE_ATTRIBUTES);

    CHECK(ScreenshotToasts(dead, L"", pixels) == std::vector<std::wstring>{L"shot_failed"});
    // The GPU refused the readback (the pacer passes it on empty): no file, and the user hears.
    const std::wstring unwritten = TestOutputPath(L"shot_unwritten.png");
    DeleteFileW(unwritten.c_str());
    CHECK(ScreenshotToasts(unwritten, other, {}) == std::vector<std::wstring>{L"shot_failed"});
    CHECK(GetFileAttributesW(unwritten.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("engine pipe: an invalid nested raw key is rejected without dropping the buffer") {
    HandlerRig rig;
    JsonValue group = JsonValue::MakeObject();
    group.Set("monitors", JsonValue::MakeBool(false));
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("capture", std::move(group));
    const JsonValue resp = rig.SetConfig(std::move(patch));

    CHECK(IsBool(resp.Find("ok"), false));
    CHECK_FALSE(IsBool(resp.Find("buffer_dropped"), true));
    const JsonValue* accepted = resp.Find("accepted");
    if (accepted)
        for (const JsonValue& key : accepted->arr) CHECK(key.str != "capture.monitors");
}

TEST_CASE("engine pipe: autotune ack clears a result, never a run") {
    HandlerRig rig;
    AutotuneContext ctx;
    const auto ack = [&] {
        JsonValue patch = JsonValue::MakeObject();
        patch.Set("engine.autotune", JsonValue::MakeStr("ack"));
        JsonValue req = JsonValue::MakeObject();
        req.Set("type", JsonValue::MakeStr("set_config"));
        req.Set("config", std::move(patch));
        return HandlePipeRequest(req, rig.cfg, rig.supervisor, rig.controller, rig.save_ctx, StartupGpuSummary{},
                                 nullptr, nullptr, &ctx, nullptr, &rig.raw_store);
    };
    ctx.state = AutotuneContext::State::Running;
    CHECK(IsBool(ack().Find("ok"), false));
    CHECK(ctx.state.load() == AutotuneContext::State::Running);
    ctx.state = AutotuneContext::State::Done;
    CHECK(IsBool(ack().Find("ok"), true));
    CHECK(ctx.state.load() == AutotuneContext::State::Idle);
    ctx.state = AutotuneContext::State::Failed;
    CHECK(IsBool(ack().Find("ok"), true));
    CHECK(ctx.state.load() == AutotuneContext::State::Idle);
}

TEST_CASE("engine pipe: get_schema lists every setting") {
    HandlerRig rig;
    const JsonValue  resp     = rig.Request("get_schema");
    const JsonValue* settings = resp.Find("settings");
    REQUIRE(settings);
    REQUIRE(settings->type == JsonValue::Type::Array);
    CHECK(settings->arr.size() == AllSettings().size());
}

TEST_CASE("engine pipe: get_config reports the live Config") {
    HandlerRig rig;
    rig.cfg.buffer_seconds = 42;
    const JsonValue  resp   = rig.Request("get_config");
    const JsonValue* config = resp.Find("config");
    REQUIRE(config);
    const JsonValue* bs = config->Find("buffer.seconds");
    REQUIRE(bs);
    REQUIRE(bs->type == JsonValue::Type::Number);
    CHECK(bs->num == 42);
}

// set_armed is the Instant Replay switch: it sets the controller's user switch, and the controller
// (not the handler) decides on its next poll whether the pipeline records.
TEST_CASE("engine pipe: set_armed flips the Instant Replay switch and get_status reports it") {
    HandlerRig rig;
    const bool before = rig.controller.UserEnabled();
    CHECK(IsBool(rig.Request("get_status").Find("enabled"), before));

    JsonValue req = JsonValue::MakeObject();
    req.Set("armed", JsonValue::MakeBool(!before));
    CHECK(IsBool(rig.Request("set_armed", std::move(req)).Find("ok"), true));
    CHECK(rig.controller.UserEnabled() == !before);
    CHECK(IsBool(rig.Request("get_status").Find("enabled"), !before));
    CHECK(IsBool(rig.Request("get_status").Find("armed"), rig.supervisor.Armed()));

    CHECK(IsBool(rig.Request("set_armed").Find("ok"), false));  // no "armed" field
}

TEST_CASE("engine pipe api: status answers, internal commands don't exist there") {
    HandlerRig rig;
    const JsonValue status = rig.ApiRequest("status");
    CHECK(IsBool(status.Find("replay"), rig.controller.UserEnabled()));
    CHECK(IsBool(status.Find("buffering"), rig.supervisor.Armed()));
    const JsonValue* game = status.Find("game");
    CHECK((game && game->type == JsonValue::Type::Null));
    CHECK(status.Find("buffer_seconds"));
    CHECK_FALSE(status.Find("rss_ws_mb"));

    for (const char* internal : {"get_status", "set_config", "shutdown", "update_install"}) {
        const JsonValue resp = rig.ApiRequest(internal);
        CHECK(IsBool(resp.Find("ok"), false));
        const JsonValue* error = resp.Find("error");
        CHECK((error && error->type == JsonValue::Type::String && error->str == "unknown_type"));
    }
    // ...and the API's names don't exist on the internal surface.
    CHECK(IsBool(rig.Request("status").Find("ok"), false));
}

namespace {

std::string ErrorOf(const JsonValue& resp) {
    const JsonValue* ok = resp.Find("ok");
    const JsonValue* error = resp.Find("error");
    if (!ok || ok->type != JsonValue::Type::Bool || ok->b) return "";
    return error && error->type == JsonValue::Type::String ? error->str : "?";
}

}  // namespace

TEST_CASE("engine pipe api: clip answers only Ombra's command line until consent exists") {
    HandlerRig rig;
    CHECK(ErrorOf(rig.ApiRequest("clip")) == "not_allowed");  // no caller
    PipeCaller app;
    app.app = "Command line";  // the name alone grants nothing
    CHECK(ErrorOf(rig.ApiRequest("clip", JsonValue::MakeObject(), &app)) == "not_allowed");
    // The verdict comes from the image: this process (the tests) passes, another program doesn't.
    CHECK(IsCommandLineClient(GetCurrentProcessId(), L""));
    CHECK_FALSE(IsCommandLineClient(0, L"C:\\Windows\\System32\\notepad.exe"));
}

TEST_CASE("engine pipe api: clip checks its fields, Instant Replay and the footage") {
    HandlerRig rig;
    PipeCaller cli;
    cli.app          = "Command line";
    cli.command_line = true;
    const auto clip = [&](JsonValue fields = JsonValue::MakeObject()) {
        return rig.ApiRequest("clip", std::move(fields), &cli);
    };
    const auto with = [](const char* key, JsonValue value) {
        JsonValue fields = JsonValue::MakeObject();
        fields.Set(key, std::move(value));
        return fields;
    };
    CHECK(ErrorOf(clip(with("before", JsonValue::MakeNum(-1)))) == "bad_request");
    CHECK(ErrorOf(clip(with("after", JsonValue::MakeStr("5")))) == "bad_request");
    CHECK(ErrorOf(clip(with("after", JsonValue::MakeNum(std::numeric_limits<double>::infinity())))) == "bad_request");
    CHECK(ErrorOf(clip(with("title", JsonValue::MakeNum(1)))) == "bad_request");
    CHECK(ErrorOf(clip(with("wait", JsonValue::MakeStr("yes")))) == "bad_request");

    JsonValue on = JsonValue::MakeObject();
    on.Set("armed", JsonValue::MakeBool(true));
    REQUIRE(IsBool(rig.Request("set_armed", on).Find("ok"), true));
    // Switched on with an empty ring: refused at once, with the clip's id, even without "wait".
    const JsonValue empty = clip();
    CHECK(ErrorOf(empty) == "nothing_new");
    CHECK(empty.Find("clip"));
    const JsonValue* monitors = empty.Find("monitors");
    CHECK((monitors && monitors->type == JsonValue::Type::Array && monitors->arr.size() == 1));

    JsonValue off = JsonValue::MakeObject();
    off.Set("armed", JsonValue::MakeBool(false));
    REQUIRE(IsBool(rig.Request("set_armed", off).Find("ok"), true));
    CHECK(ErrorOf(clip()) == "replay_off");
}

TEST_CASE("engine pipe api: marker checks its fields, the footage and the per-app cap") {
    HandlerRig rig;
    CHECK(ErrorOf(rig.ApiRequest("marker")) == "not_allowed");  // no caller
    PipeCaller cli;
    cli.app          = "Script";
    cli.command_line = true;
    const auto marker = [&](JsonValue fields = JsonValue::MakeObject()) {
        return rig.ApiRequest("marker", std::move(fields), &cli);
    };
    const auto with = [](const char* key, JsonValue value) {
        JsonValue fields = JsonValue::MakeObject();
        fields.Set(key, std::move(value));
        return fields;
    };
    const auto at = [&](double seconds) { return with("at", JsonValue::MakeNum(seconds)); };
    CHECK(ErrorOf(marker(with("label", JsonValue::MakeNum(1)))) == "bad_request");
    CHECK(ErrorOf(marker(with("label", JsonValue::MakeStr(std::string(65, 'a'))))) == "bad_request");
    CHECK(ErrorOf(marker(with("kind", JsonValue::MakeStr("Kill")))) == "bad_request");
    CHECK(ErrorOf(marker(with("kind", JsonValue::MakeNum(1)))) == "bad_request");
    CHECK(ErrorOf(marker(with("important", JsonValue::MakeStr("yes")))) == "bad_request");
    CHECK(ErrorOf(marker(at(1))) == "bad_request");
    CHECK(ErrorOf(marker(at(-rig.cfg.buffer_seconds - 1))) == "bad_request");
    CHECK(ErrorOf(marker(at(-std::numeric_limits<double>::infinity()))) == "bad_request");

    // Instant Replay off: nothing to mark, whatever the ring holds.
    JsonValue off = JsonValue::MakeObject();
    off.Set("armed", JsonValue::MakeBool(false));
    REQUIRE(IsBool(rig.Request("set_armed", off).Find("ok"), true));
    CHECK(ErrorOf(marker()) == "replay_off");
    JsonValue on = JsonValue::MakeObject();
    on.Set("armed", JsonValue::MakeBool(true));
    REQUIRE(IsBool(rig.Request("set_armed", on).Find("ok"), true));
    CHECK(ErrorOf(marker()) == "replay_off");  // an empty ring

    // A disarmed ring holding a second of footage from 2 s ago covers that second only.
    const int64_t freq = QpcFrequency(), now = QpcNow();
    const uint8_t au[64] = {};
    for (uint64_t i = 0; i < 60; ++i)
        rig.ring.Append(au, sizeof(au), now - 2 * freq + static_cast<int64_t>(i) * freq / 60, i, i % 30 == 0);
    rig.supervisor.SetArmed(false);  // the user switch on, but nothing recording (game mode, no game)
    CHECK(ErrorOf(marker()) == "replay_off");      // now: the ring stopped a second ago
    CHECK(ErrorOf(marker(at(-3))) == "replay_off");  // before its oldest frame
    JsonValue past = at(-1.5);
    past.Set("label", JsonValue::MakeStr("  Ace\n"));
    past.Set("kind", JsonValue::MakeStr("kill"));
    past.Set("important", JsonValue::MakeBool(true));
    const JsonValue placed = marker(std::move(past));
    CHECK(ErrorOf(placed) == "");
    REQUIRE(placed.Find("marker"));
    const std::vector<Marker> held = rig.markers.Between(0, (std::numeric_limits<int64_t>::max)());
    REQUIRE(held.size() == 1);
    CHECK(held[0].id == static_cast<uint64_t>(placed.Find("marker")->num));
    CHECK(held[0].label == "Ace");
    CHECK(held[0].type == MarkerType::kKill);
    CHECK(held[0].owner == "Script");
    CHECK(held[0].important);

    // Recording: now is covered, though the newest frame trails it.
    rig.supervisor.SetArmed(true);
    CHECK(ErrorOf(marker()) == "");
    // Every command-line script is one app: its 257th marker in the held footage is refused.
    for (size_t i = 2; i < kMarkersPerApp; ++i) REQUIRE(ErrorOf(marker()) == "");
    CHECK(ErrorOf(marker()) == "too_many_markers");
    rig.supervisor.SetArmed(false);
}

TEST_CASE("engine pipe: set_config accepts a valid key and rejects a bad value and an unknown key") {
    HandlerRig rig;
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("output.filename", JsonValue::MakeStr("pipe_{date}.mp4"));
    patch.Set("capture.mode", JsonValue::MakeStr("not_a_real_mode"));
    patch.Set("totally.unknown.key", JsonValue::MakeNum(1));
    const JsonValue  resp     = rig.SetConfig(std::move(patch));
    const JsonValue* accepted = resp.Find("accepted");
    const JsonValue* rejected = resp.Find("rejected");
    REQUIRE(accepted);
    REQUIRE(rejected);
    REQUIRE(accepted->arr.size() == 1);
    CHECK(accepted->arr[0].str == "output.filename");
    CHECK(rejected->arr.size() == 2);
    CHECK(rig.cfg.output_filename == "pipe_{date}.mp4");
    CHECK(rig.save_ctx.output_filename == "pipe_{date}.mp4");
}

TEST_CASE("engine pipe: save_now while a save is in flight reports busy instead of racing it") {
    HandlerRig rig;
    rig.save_ctx.save_in_flight.store(true);
    const JsonValue resp = rig.Request("save_now");
    rig.save_ctx.save_in_flight.store(false);
    CHECK(IsBool(resp.Find("saved"), false));
    const JsonValue* reason = resp.Find("reason");
    REQUIRE(reason);
    CHECK(reason->str == "busy");
}

TEST_CASE("engine pipe: replay includes every monitor and resolves feedback once") {
    HandlerRig rig;
    std::vector<std::unique_ptr<MonitorPipeline>> pipelines;
    for (int i = 0; i < 2; ++i) {
        auto p = std::make_unique<MonitorPipeline>();
        p->ring.Init(1u << 20, 64u << 10);
        p->save_ctx.ring = &p->ring;
        p->save_ctx.monitor_device = "display" + std::to_string(i);
        pipelines.push_back(std::move(p));
    }
    int started = 0, finished = 0;
    JsonValue req = JsonValue::MakeObject();
    req.Set("type", JsonValue::MakeStr("save_now"));
    const auto response = HandlePipeRequest(req, rig.cfg, rig.supervisor, rig.controller, rig.save_ctx,
        {}, nullptr, nullptr, nullptr, &pipelines, nullptr, nullptr, nullptr,
        [&]() -> SaveFeedback {
            ++started;
            return [&](const ReplaySaveResults& r) { CHECK(r.outcome == SaveOutcome::kNothingNew); ++finished; };
        });
    REQUIRE(response.Find("monitors"));
    REQUIRE(response.Find("monitors")->arr.size() == 2);
    for (int i = 0; i < 2; ++i) {
        const auto& entry = response.Find("monitors")->arr[i];
        CHECK(entry.Find("monitor")->str == "display" + std::to_string(i));
        CHECK(entry.Find("reason")->str == "nothing_new");
    }
    CHECK(started == 1);
    CHECK(finished == 1);
}

TEST_CASE("engine pipe: notify with no tray answers ok") {
    HandlerRig rig;
    JsonValue req = JsonValue::MakeObject();
    req.Set("title", JsonValue::MakeStr("test"));
    req.Set("message", JsonValue::MakeStr("message"));
    CHECK(IsBool(rig.Request("notify", std::move(req)).Find("ok"), true));
}

TEST_CASE("engine pipe: a failed Auto-Tune run cannot reuse an earlier recommendation") {
    bool success = false;
    SUBCASE("failed child ignores stale advice") {}
    SUBCASE("successful child reads its result") { success = true; }
    {
        std::ofstream file(AutotuneResultFilePath());
        file << R"({"recommended_preset":"p3","reason":"previous result"})";
    }
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    wchar_t failed[] = L"cmd.exe /c exit 7";
    wchar_t passed[] = L"cmd.exe /c exit 0";
    REQUIRE(CreateProcessW(nullptr, success ? passed : failed, nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
    CloseHandle(child.hThread);
    AutotuneContext ctx;
    ctx.process = child.hProcess;
    ctx.state = AutotuneContext::State::Running;
    WaitForAutotuneCompletion(&ctx);
    CHECK(ctx.state.load() == (success ? AutotuneContext::State::Done : AutotuneContext::State::Failed));
    CHECK((ctx.last_result.Find("recommended_preset") != nullptr) == success);
    DeleteFileW(AutotuneResultFilePath().c_str());
}

TEST_CASE("engine pipe: an unknown request type answers ok:false") {
    HandlerRig rig;
    const JsonValue resp = rig.Request("not_a_valid_type_name");
    CHECK(IsBool(resp.Find("ok"), false));
    CHECK(resp.Find("error"));
}

TEST_CASE("engine pipe: preview_config prices a quality change without applying it") {
    HandlerRig rig;
    const std::string before = rig.cfg.encode_quality_preset;
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("encode.quality_preset", JsonValue::MakeStr("ultra"));
    JsonValue req = JsonValue::MakeObject();
    req.Set("config", std::move(patch));
    const JsonValue resp = rig.Request("preview_config", std::move(req));
    CHECK(IsBool(resp.Find("ok"), true));
    const JsonValue* tier = resp.Find("apply_tier");
    REQUIRE(tier);
    CHECK(tier->str == "realloc");
    const JsonValue* ring_bytes = resp.Find("ring_bytes");
    REQUIRE(ring_bytes);
    CHECK(ring_bytes->num > 0);
    CHECK(rig.cfg.encode_quality_preset == before);
}

TEST_CASE("engine pipe: raw keys set through set_config come back from get_config") {
    HandlerRig rig;
    JsonValue monitors = JsonValue::MakeArray();
    monitors.arr.push_back(JsonValue::MakeStr("\\\\.\\DISPLAY9"));
    JsonValue source = JsonValue::MakeObject();
    source.Set("kind", JsonValue::MakeStr("output_loopback"));
    source.Set("device", JsonValue::MakeStr("default"));
    JsonValue sources = JsonValue::MakeArray();
    sources.arr.push_back(std::move(source));
    JsonValue game = JsonValue::MakeObject();
    game.Set("name", JsonValue::MakeStr("pipetest"));
    game.Set("path", JsonValue::MakeStr("C:\\Games\\pipetest"));
    JsonValue games = JsonValue::MakeArray();
    games.arr.push_back(std::move(game));

    JsonValue patch = JsonValue::MakeObject();
    patch.Set("capture.monitors", std::move(monitors));
    patch.Set("audio.sources", std::move(sources));
    patch.Set("games.list", std::move(games));
    REQUIRE(IsBool(rig.SetConfig(std::move(patch)).Find("ok"), true));

    const JsonValue  resp   = rig.Request("get_config");
    const JsonValue* config = resp.Find("config");
    REQUIRE(config);
    const JsonValue* got_monitors = config->Find("capture.monitors");
    const JsonValue* got_sources  = config->Find("audio.sources");
    const JsonValue* got_games    = config->Find("games.list");
    REQUIRE(got_monitors);
    REQUIRE(got_sources);
    REQUIRE(got_games);
    REQUIRE(got_monitors->arr.size() == 1);
    CHECK(got_monitors->arr[0].str == "\\\\.\\DISPLAY9");
    CHECK(got_sources->arr.size() == 1);
    const auto parsed_games = ParseGamesList(*got_games);
    REQUIRE(parsed_games.size() == 1);
    CHECK(parsed_games[0].path == L"C:\\Games\\pipetest");
}

}  // namespace ombra::test
