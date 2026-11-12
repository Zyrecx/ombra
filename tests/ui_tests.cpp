// Headless UI tests: SettingsScreen/HomeScreen against a FakeScreenHost + FakePipeClient that serve
// canned JSON instead of a real ombra.exe over a real pipe. Layout()/InitSchema()/RebuildGroupRows
// take no ID2D1DeviceContext, so this runs with no window, no GPU, no engine.

//
// What's NOT covered here: anything a widget only computes inside Draw() (ModalDialog's button
// rects, ...) -- those stay zero until a real dc
// runs, so asserting on them here would pass or fail vacuously. See widget.h's own comments on each.

#include <doctest/doctest.h>

#include <cmath>

#include "ombra-ui/clips_screen.h"
#include "ombra-ui/overlay_window.h"
#include "ombra-ui/screens.h"
#include "ombra-ui/setup_screen.h"
#include "ombra-ui/pipe_client.h"
#include "ombra-ui/ui/attached_panel.h"
#include "ombra-ui/ui/disk_location.h"
#include "ombra-ui/ui/flex.h"
#include "ombra-ui/ui/theme.h"
#include "support/h264_fixture.h"
#include "support/test_env.h"
#include "ombra/clip_file.h"
#include "ombra/config.h"
#include "ombra/mux.h"
#include "ombra/json.h"
#include "ombra/paths.h"
#include "ombra/screenshot.h"
#include "ombra/util.h"

#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <shlwapi.h>

#include "ombra/brand_mark.h"
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <map>
#include <typeinfo>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace ombra_ui::test {

namespace {

using ombra::JsonValue;

// Overrides PipeClient::Request (made virtual for exactly this) to serve fixed responses per
// request "type" instead of touching a real pipe, and counts calls per type so tests can assert
// on request volume -- the "no pipe calls on the UI thread" half of the item, read as a
// census rather than a blanket zero (RebuildGroupRows does make a handful of synchronous pipe
// calls today; that is a separate navigation stall, not
// something to paper over here).
class FakePipeClient : public PipeClient {
public:
    std::map<std::string, int> counts;
    JsonValue last_set_config;
    bool        set_config_ok = true;
    bool        set_config_no_answer = false;
    std::string set_config_error;

    std::optional<JsonValue> Request(JsonValue req, DWORD /*timeout_ms*/) override {
        const auto* type_v = req.Find("type");
        const std::string type = (type_v && type_v->type == JsonValue::Type::String) ? type_v->str : "";
        ++counts[type];

        if (type == "preview_config") {
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("ring_bytes", JsonValue::MakeNum(60.0 * 1024 * 1024));
            resp.Set("mb_per_minute", JsonValue::MakeNum(60.0));
            resp.Set("bitrate_kbps", JsonValue::MakeNum(25000.0));
            return resp;
        }
        if (type == "get_status") {
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("armed", JsonValue::MakeBool(false));
            return resp;
        }
        if (type == "list_monitors") {
            JsonValue mon = JsonValue::MakeObject();
            mon.Set("device", JsonValue::MakeStr("\\\\.\\DISPLAY1"));
            mon.Set("label", JsonValue::MakeStr("Built-in Display"));
            mon.Set("width", JsonValue::MakeNum(1920));
            mon.Set("height", JsonValue::MakeNum(1080));
            mon.Set("primary", JsonValue::MakeBool(true));
            JsonValue arr = JsonValue::MakeArray();
            arr.arr.push_back(std::move(mon));
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("monitors", std::move(arr));
            return resp;
        }
        if (type == "capture_monitor_thumbnail") {
            // ok:false -- BuildMonitorRows treats a miss here as "no thumbnail yet," not a
            // failure, so this exercises that path without needing a real PNG fixture.
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("ok", JsonValue::MakeBool(false));
            return resp;
        }
        if (type == "list_processes") {
            JsonValue proc = JsonValue::MakeObject();
            proc.Set("name", JsonValue::MakeStr("game.exe"));
            proc.Set("display_name", JsonValue::MakeStr("Some Game"));
            JsonValue arr = JsonValue::MakeArray();
            arr.arr.push_back(std::move(proc));
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("processes", std::move(arr));
            return resp;
        }
        if (type == "list_audio_sessions") {
            JsonValue sess = JsonValue::MakeObject();
            sess.Set("name", JsonValue::MakeStr("discord.exe"));
            sess.Set("display_name", JsonValue::MakeStr("Discord"));
            JsonValue arr = JsonValue::MakeArray();
            arr.arr.push_back(std::move(sess));
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("sessions", std::move(arr));
            return resp;
        }
        if (type == "set_config") {
            last_set_config = req;
            if (set_config_no_answer) return std::nullopt;
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("ok", JsonValue::MakeBool(set_config_ok));
            if (!set_config_error.empty()) resp.Set("error", JsonValue::MakeStr(set_config_error));
            return resp;
        }
        return std::nullopt;
    }
};

// Shared fake host for the UI tests. Its async list calls resolve *synchronously with an empty
// reply* by design, and that is load-bearing: other cases in this file depend on the empty
// result (a non-empty one makes row builders call back into RebuildGroupRows, which made
// unrelated cases hang when this was tried). Need real data? Subclass it for that one test,
// the way ReentrantFakeScreenHost below does -- don't change the defaults here.
class FakeScreenHost : public ScreenHost {
public:
    FakePipeClient pipe;
    Theme          theme;
    int            repaint_count = 0;
    int            modal_count = 0;  // how many times ShowModal was invoked -- lets a test pin "one
                                      // confirm for a whole batch," not just that it eventually fires

    std::vector<bool> hotkey_suspends;  // every SetHotkeysSuspended call, in order
    void SetHotkeysSuspended(bool suspended) override { hotkey_suspends.push_back(suspended); }

    void RequestRepaint() override { ++repaint_count; }
    void SwitchScreen(ActiveScreen) override {}
    ModalButtons   last_modal_buttons;
    void ShowModal(const std::wstring&, const std::wstring&, bool, std::function<void()> on_confirm,
                   std::function<void()>, ModalButtons buttons) override {
        ++modal_count;
        last_modal_buttons = std::move(buttons);
        // Tests never need to decline a confirm-gated path -- auto-confirming keeps
        // StageAudioGainForTest-style tests simple.
        if (on_confirm) on_confirm();
    }
    // The last menu asked for; a test picks from it with `pick` and closes it with `close`.
    std::vector<std::wstring> dropdown_options;
    int dropdown_selected = -1;
    std::function<void(int)> dropdown_pick;
    std::function<void()> dropdown_close;
    void ShowDropdown(const D2D1_RECT_F&, const std::vector<std::wstring>& options, int selected,
                      std::function<void(int)> on_select, std::function<void()> on_close) override {
        dropdown_options = options;
        dropdown_selected = selected;
        dropdown_pick = std::move(on_select);
        dropdown_close = std::move(on_close);
    }
    // The last popup asked for, kept open until a test drives it and drops it once it Closed().
    std::unique_ptr<Popup> popup;
    D2D1_RECT_F popup_anchor{};
    void ShowPopup(std::unique_ptr<Popup> p, const D2D1_RECT_F& anchor) override {
        popup_anchor = anchor;
        if (p) p->Place(anchor, D2D1::RectF(0, 0, 1920, 1080), theme, 1.0f);
        popup = std::move(p);
    }
    void RequestWindowListAsync(std::function<void(std::vector<WindowEntry>)> on_result) override {
        if (on_result) on_result({});
    }
    void RequestMonitorListAsync(std::function<void(std::vector<MonitorEntry>)> on_result) override {
        if (on_result) on_result({});
    }
    void RequestMonitorThumbnailsAsync(
        std::vector<std::string> devices,
        std::function<void(std::string, bool, UINT, UINT, std::vector<uint8_t>)> on_result) override {
        for (auto& device : devices) {
            if (on_result) on_result(std::move(device), false, 0, 0, {});
        }
    }
    void RequestWindowThumbnailsAsync(
        std::vector<uint64_t> hwnds,
        std::function<void(uint64_t, bool, UINT, UINT, std::vector<uint8_t>)> on_result) override {
        for (uint64_t h : hwnds) {
            if (on_result) on_result(h, false, 0, 0, {});
        }
    }
    // Same "resolve synchronously" contract as RequestWindowListAsync/RequestMonitorListAsync above
    // -- SettingsScreen's re-entrancy guards (process_list_pending_/audio_sessions_pending_,
    // screens.h) exist specifically so this doesn't recurse into RebuildGroupRows unboundedly.
    void RequestProcessListAsync(std::function<void(std::vector<ProcessEntry>)> on_result) override {
        if (on_result) on_result({});
    }
    void RequestAudioSessionListAsync(
        std::function<void(std::vector<AudioSessionEntry>)> on_result) override {
        if (on_result) on_result({});
    }
    void RequestAudioDeviceListAsync(
        std::function<void(std::vector<AudioDeviceEntry>)> on_result) override {
        if (on_result) on_result({});
    }
    // Recorded, and answered only when a test flushes `pending_file_icons`: answering from inside
    // the page build would rebuild the page in the middle of building it.
    std::vector<std::vector<std::wstring>> file_icon_requests;
    std::vector<std::function<void()>>     pending_file_icons;
    void RequestFileIconsAsync(std::vector<std::wstring> paths, UINT px,
                               std::function<void(std::vector<FileIcon>)> on_result) override {
        file_icon_requests.push_back(paths);
        pending_file_icons.push_back([paths = std::move(paths), px, on_result = std::move(on_result)]() {
            std::vector<FileIcon> icons;
            for (const auto& p : paths) icons.push_back(LoadFileIcon(p, px));
            if (on_result) on_result(std::move(icons));
        });
    }
    // Resolves synchronously through the same FakePipeClient every other request in this fake goes
    // through, same "resolve synchronously" contract as the other Async methods above -- real
    // set_config/preview_config/get_status parsing, just not off a real worker thread. Unless
    // `defer_set_config_replies` is set (below): a synchronously-resolving fake can otherwise never
    // have two set_config calls to the same key genuinely overlap, which is exactly the case
    // set_config_seq_by_key_/audio_sources_generation_ (screens.h) exist to guard -- deferring lets
    // a test call two requests, then flush their replies in whichever order it wants to simulate the
    // real out-of-order-arrival hazard async introduces.
    bool                          defer_set_config_replies = false;
    std::vector<std::function<void()>> pending_set_config_replies;
    void RequestAsync(JsonValue req, DWORD timeout_ms,
                      std::function<void(std::optional<JsonValue>)> on_result) override {
        const JsonValue* type = req.Find("type");
        const bool is_set_config = type && type->str == "set_config";
        auto run = [this, req, timeout_ms, on_result]() {
            auto resp = pipe.Request(req, timeout_ms);
            if (on_result) on_result(resp);
        };
        if (defer_set_config_replies && is_set_config) {
            pending_set_config_replies.push_back(std::move(run));
        } else {
            run();
        }
    }
    void RequestPreviewAsync(
        const JsonValue& config,
        std::function<void(std::optional<JsonValue>, std::optional<JsonValue>)> on_result) override {
        JsonValue req = JsonValue::MakeObject();
        req.Set("type", JsonValue::MakeStr("preview_config"));
        req.Set("config", config);
        auto preview_resp = pipe.Request(req, 3000);
        JsonValue status_req = JsonValue::MakeObject();
        status_req.Set("type", JsonValue::MakeStr("get_status"));
        auto status_resp = pipe.Request(status_req, 1500);
        if (on_result) on_result(preview_resp, status_resp);
    }
    void CloseOverlay() override {}
    PipeClient& GetPipeClient() override { return pipe; }
    Theme& GetTheme() override { return theme; }
};

// Real schema/default-config JSON -- same ombra::AllSettings()-backed payload the real UI gets
// over get_schema/get_config, not a fabricated subset that could drift from the real schema shape.
std::vector<FieldSpec> RealSpecs() {
    return ParseSchema(ombra::SerializeSettingsSchema());
}

JsonValue DefaultConfigJson() {
    ombra::Config cfg{};
    return ombra::SerializeConfigFlat(cfg);
}


}  // namespace

TEST_CASE("ui: an edit made while a Save is on its way stays staged") {
    FakeScreenHost host;
    host.defer_set_config_replies = true;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(90.0));
    screen.SaveAllForTest();
    REQUIRE(host.pending_set_config_replies.size() == 1);
    screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(150.0));  // no second Save
    host.pending_set_config_replies[0]();

    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.SettingsBarForTest().visible);  // this reply used to erase the 150
    CHECK_FALSE(screen.SettingsBarForTest().failed);
}

TEST_CASE("ui: an edit put back to the saved value while a Save is on its way isn't lost") {
    FakeScreenHost host;
    host.defer_set_config_replies = true;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    const double original = config.Find("buffer.seconds")->num;

    SUBCASE("the Save lands: the put-back is the next change") {
        screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(original + 30.0));
        screen.SaveAllForTest();
        REQUIRE(host.pending_set_config_replies.size() == 1);
        screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(original));
        host.pending_set_config_replies[0]();
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
        CHECK(screen.HasPendingChangesForTest());  // the engine has +30, the user asked for the original
    }
}

TEST_CASE("ui: Discard after a Save's reply goes back to the audio that was sent, not a later edit") {
    FakeScreenHost host;
    host.defer_set_config_replies = true;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    screen.StageAudioGainForTest(-6.0);
    screen.SaveAudioSourcesForTest();
    REQUIRE(host.pending_set_config_replies.size() == 1);
    screen.StageAudioGainForTest(-9.0);
    host.pending_set_config_replies[0]();
    screen.DiscardAudioSourcesForTest();
    CHECK(screen.FirstAudioGainForTest() == doctest::Approx(-6.0));
}

TEST_CASE("ui: a Save that fails says why in the bar and keeps the edits") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    const auto layout = [&] { screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f); };
    layout();
    const auto& bar = screen.SettingsBarForTest();

    screen.ApplySettingChangeForTest("output.root", JsonValue::MakeStr("D:\\Clips"));
    SUBCASE("the engine refuses") {
        host.pipe.set_config_ok = false;
        host.pipe.set_config_error = "can't keep the buffer on disk there: no room";
        screen.SaveAllForTest();
        layout();
        CHECK(bar.detail == L"Can't keep the buffer on disk there: no room");
    }
    SUBCASE("the engine is gone") {
        host.pipe.set_config_no_answer = true;
        screen.SaveAllForTest();
        layout();
        CHECK(bar.detail.find(L"didn't answer") != std::wstring::npos);
    }
    SUBCASE("live, but config.json wasn't written") {
        host.pipe.set_config_error = "applied, but config.json could not be written";
        screen.SaveAllForTest();
        layout();
        CHECK(bar.detail == L"Applied, but config.json could not be written");
    }
    CHECK(bar.visible);
    CHECK(bar.failed);
    CHECK(bar.title == L"Couldn't save");

    // A retry that works clears it all.
    host.pipe.set_config_ok = true;
    host.pipe.set_config_no_answer = false;
    host.pipe.set_config_error.clear();
    screen.SaveAllForTest();
    layout();
    CHECK_FALSE(bar.visible);
}

TEST_CASE("ui: setup finishes with one set_config of the recommendation, and marks itself seen") {
    FakeScreenHost host;
    DeleteFileW(FirstRunMarkerPath().c_str());
    DeleteFileW(ombra::SetupCompletedPath().c_str());
    REQUIRE_FALSE(FirstRunComplete());
    REQUIRE_FALSE(ombra::SetupCompleted());
    SetupScreen setup(&host);
    setup.Open(nullptr, nullptr);
    setup.FinishForTest();
    CHECK(ombra::SetupCompleted());
    DeleteFileW(ombra::SetupCompletedPath().c_str());
    CHECK(host.pipe.counts["set_config"] == 1);
    const JsonValue* patch = host.pipe.last_set_config.Find("config");
    REQUIRE(patch);
    CHECK(patch->Find("encode.quality_preset")->str == "high");
    CHECK(patch->Find("buffer.seconds")->num == 60.0);
    CHECK(patch->Find("audio.sources")->arr.size() == 2);
    CHECK(FirstRunComplete());
    DeleteFileW(FirstRunMarkerPath().c_str());
}

TEST_CASE("ui: an unfinished setup keeps coming back, until its last appearance") {
    const std::wstring shown = FirstRunMarkerPath() + L".shown";
    DeleteFileW(FirstRunMarkerPath().c_str());
    DeleteFileW(shown.c_str());
    for (int i = 1; i < kFirstRunMaxShows; ++i) {
        NoteFirstRunShown();
        CHECK_FALSE(FirstRunComplete());
    }
    NoteFirstRunShown();
    CHECK(FirstRunComplete());
    CHECK_FALSE(ombra::SetupCompleted());  // it stopped asking; nobody chose a diagnostics level
    CHECK(GetFileAttributesW(shown.c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(FirstRunMarkerPath().c_str());
}

TEST_CASE("ui: finishing the setup partway through the appearances ends them") {
    const std::wstring shown = FirstRunMarkerPath() + L".shown";
    DeleteFileW(FirstRunMarkerPath().c_str());
    DeleteFileW(shown.c_str());
    NoteFirstRunShown();
    FakeScreenHost host;
    SetupScreen setup(&host);
    setup.Open(nullptr, nullptr);
    setup.FinishForTest();
    CHECK(FirstRunComplete());
    CHECK(GetFileAttributesW(shown.c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(FirstRunMarkerPath().c_str());
}

TEST_CASE("ui: SettingsScreen builds non-empty rows on every rail") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);

    for (int rail : {kRailShortcuts, kRailCapture, kRailGames, kRailReplay, kRailAppearance, kRailAdvanced}) {
        screen.SetActiveRail(rail);
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    }

    // TotalWiredControls sums every wired row across every group regardless of which rail is
    // active (screens.h: RebuildGroupRows builds all groups up front) -- a real non-zero count
    // means the schema-driven build actually produced controls, not just compiled.
    CHECK(screen.TotalWiredControls() > 0);
    CHECK(screen.GroupHeadingCount() > 0);
}

// BuildMonitorRows/BuildAudioSourceRows only call back into RebuildGroupRows when their async
// reply actually has data ("only rebuild when there's something new to show" -- screens.cpp), so
// FakeScreenHost's normal empty replies never reach the hazard at all. This host is scoped to just
// this test (not the shared FakeScreenHost every other test uses) and returns one fake entry from
// each list call so the callback actually re-enters mid-rebuild, resolving synchronously the same
// way FakeScreenHost always has.
class ReentrantFakeScreenHost : public FakeScreenHost {
public:
    void RequestMonitorListAsync(std::function<void(std::vector<MonitorEntry>)> on_result) override {
        if (on_result) on_result({MonitorEntry{"\\\\.\\DISPLAY1", "Fake Monitor", 1920, 1080, true}});
    }
    void RequestProcessListAsync(std::function<void(std::vector<ProcessEntry>)> on_result) override {
        if (on_result) on_result({ProcessEntry{"fake.exe", "Fake App"}});
    }
    void RequestAudioSessionListAsync(
        std::function<void(std::vector<AudioSessionEntry>)> on_result) override {
        if (on_result) on_result({AudioSessionEntry{"fake.exe", "Fake App"}});
    }
};

// Pins that the re-entrancy guard (screens.h, comment above rebuilding_group_rows_) actually
// engages rather than recursing -- not just that nothing crashes, which the shipped Monitors code
// already didn't do even before this guard existed.
TEST_CASE("ui: RebuildGroupRows never re-enters its own body") {
    ReentrantFakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);

    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(0);  // Monitors -- BuildMonitorRows fires RequestMonitorListAsync
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.MaxRebuildBodyDepthForTest() == 1);

    screen.SetActiveSubTab(1);  // Audio -- BuildAudioSourceRows fires RequestProcessListAsync etc.
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.MaxRebuildBodyDepthForTest() == 1);
}

// The Displays diagram has no real monitor in --selftest, so this is the only place its layout is
// exercised: screens sit where the desktop puts them, scaled together.
class MultiMonitorFakeScreenHost : public FakeScreenHost {
public:
    int count = 1;
    std::vector<MonitorEntry> layout;  // when set, replied as is
    void RequestMonitorListAsync(std::function<void(std::vector<MonitorEntry>)> on_result) override {
        std::vector<MonitorEntry> list = layout;
        if (list.empty())
            for (int i = 0; i < count; ++i)
                list.push_back(MonitorEntry{"\\\\.\\DISPLAY" + std::to_string(i + 1), "Fake Monitor", 1920, 1080, i == 0});
        if (on_result) on_result(std::move(list));
    }
};

std::vector<D2D1_RECT_F> MonitorBoxes(MultiMonitorFakeScreenHost& host) {
    SettingsScreen screen(&host);
    const auto config = DefaultConfigJson();
    screen.InitSchema(RealSpecs(), &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    return screen.MonitorCardBoundsForTest();
}

MonitorEntry PlacedMonitor(int n, int left, int top, int w, int h) {
    MonitorEntry m{"\\\\.\\DISPLAY" + std::to_string(n), "", w, h, n == 1};
    m.left = left;
    m.top = top;
    m.positioned = true;
    return m;
}

TEST_CASE("ui: Displays diagram puts screens where the desktop does") {
    MultiMonitorFakeScreenHost host;
    // A panel left of the primary (negative x), a portrait screen raised on its right (negative y).
    host.layout = {PlacedMonitor(1, 0, 0, 2560, 1440), PlacedMonitor(2, -1920, 360, 1920, 1080),
                   PlacedMonitor(3, 2560, -240, 1080, 1920)};
    const auto t = MonitorBoxes(host);
    REQUIRE(t.size() == 3);
    auto w = [](const D2D1_RECT_F& r) { return r.right - r.left; };
    auto h = [](const D2D1_RECT_F& r) { return r.bottom - r.top; };
    CHECK(t[1].right <= t[0].left);   // the panel is left of the primary
    CHECK(t[2].left >= t[0].right);   // the portrait one right of it
    CHECK(t[2].top < t[0].top);       // and raised above it
    CHECK(t[1].top > t[0].top);       // the panel sits lower
    CHECK(h(t[2]) > w(t[2]));         // portrait stays portrait
    CHECK(w(t[0]) / h(t[0]) == doctest::Approx(16.0f / 9.0f).epsilon(0.05));
    CHECK(w(t[0]) > w(t[1]));         // 2560 wide draws wider than 1920: one scale for all
    CHECK(t[1].left >= 0.0f);         // negative coordinates normalized into the card
    CHECK(t[2].top >= 0.0f);
}

TEST_CASE("ui: Displays diagram rows up screens without positions, one size each") {
    float lone_w = 0.0f;
    for (const int n : {1, 3}) {
        MultiMonitorFakeScreenHost host;
        host.count = n;
        const auto t = MonitorBoxes(host);
        REQUIRE(t.size() == static_cast<size_t>(n));
        CHECK(t[0].right - t[0].left > 100.0f);
        if (n == 1) { lone_w = t[0].right - t[0].left; continue; }
        CHECK(t[0].right - t[0].left <= lone_w + 0.5f);  // three share the width, never grow
        for (int i = 1; i < n; ++i) {
            CHECK(t[i].left >= t[i - 1].right);            // in a row, left to right
            CHECK(t[i].top == doctest::Approx(t[0].top));
            CHECK(t[i].right - t[i].left == doctest::Approx(t[0].right - t[0].left));
        }
    }
}

// ombra-ui is spawned per overlay open, so the only thing that survives between opens is the
// engine's last grab on disk. The tile must draw it at page build, and a failed fresh grab (the
// fake host fails every one) must not wipe it back to the placeholder.
TEST_CASE("ui: a display draws the engine's previous preview until a fresh one lands") {
    const std::string device = "\\\\.\\DISPLAY1";  // what the fake host lists
    const std::wstring path = ombra::MonitorThumbPath(device);
    REQUIRE(!path.empty());
    REQUIRE(ombra::EnsureParentDir(path));
    ombra::ComInit com;
    std::vector<uint8_t> bgra(64 * 36 * 4, 0x80);
    REQUIRE(ombra::WriteBgraPng(path, 64, 36, 64 * 4, bgra));

    MultiMonitorFakeScreenHost host;
    host.count = 1;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.MonitorThumbWidthForTest(device) == 64);

    DeleteFileW(path.c_str());
}

// Capture > Video, built by hand: a Recording card, then the target's own card.
std::vector<std::wstring> RowTitles(const CardContainer& card) {
    std::vector<std::wstring> out;
    for (const auto& r : card.rows)
        if (auto row = std::dynamic_pointer_cast<SettingRow>(r)) out.push_back(row->title);
    return out;
}

// Finds the card titled `title` on the current page, and a row in it by title.
std::shared_ptr<CardContainer> ContainerTitled(const SettingsScreen& screen, const wchar_t* title) {
    for (const auto& w : screen.PageChildrenForTest())
        if (auto c = std::dynamic_pointer_cast<CardContainer>(w); c && c->title == title) return c;
    return nullptr;
}

std::shared_ptr<SettingRow> RowTitled(const CardContainer& card, const wchar_t* title) {
    for (const auto& w : card.rows)
        if (auto r = std::dynamic_pointer_cast<SettingRow>(w); r && r->title == title) return r;
    return nullptr;
}

std::vector<TileControl*> CardTiles(const std::shared_ptr<CardContainer>& card) {
    std::vector<TileControl*> out;
    if (!card) return out;
    for (const auto& r : card->rows)
        if (auto g = std::dynamic_pointer_cast<TileGrid>(r))
            for (const auto& t : g->tiles) out.push_back(t.get());
    return out;
}

std::vector<ScreenControl*> CardScreens(const std::shared_ptr<CardContainer>& card) {
    std::vector<ScreenControl*> out;
    if (!card) return out;
    for (const auto& r : card->rows)
        if (auto d = std::dynamic_pointer_cast<MonitorDiagram>(r))
            for (const auto& sc : d->screens) out.push_back(sc.get());
    return out;
}

void OpenCaptureVideo(SettingsScreen& screen) {
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
}

TEST_CASE("ui: Capture > Video is a Recording card, then the Displays card; When is in Recording") {
    MultiMonitorFakeScreenHost host;
    host.count = 2;
    SettingsScreen screen(&host);
    const auto config = DefaultConfigJson();
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);

    const auto& children = screen.PageChildrenForTest();
    REQUIRE(children.size() == 2);
    auto rec = std::dynamic_pointer_cast<CardContainer>(children[0]);
    auto displays = std::dynamic_pointer_cast<CardContainer>(children[1]);
    REQUIRE(rec);
    REQUIRE(displays);
    CHECK(rec->title == L"Recording");
    CHECK(RowTitles(*rec) == std::vector<std::wstring>{L"Record", L"When to record", L"Mouse cursor"});
    auto what = std::dynamic_pointer_cast<SegmentedControl>(std::dynamic_pointer_cast<SettingRow>(rec->rows[0])->controls[0]);
    REQUIRE(what);
    CHECK(what->options.front() == L"Displays");
    CHECK(displays->title == L"Displays");
    CHECK(displays->help.find(L"restarts Ombra") != std::wstring::npos);
    CHECK(CardScreens(displays).size() == 2);

    auto when_to_record_shown = [&] {
        bool shown = false;
        for (const auto& c : screen.PageChildrenForTest())
            if (auto card = std::dynamic_pointer_cast<CardContainer>(c))
                for (const auto& t : RowTitles(*card)) shown |= t == L"When to record";
        return shown;
    };
    CHECK(when_to_record_shown());
    screen.SetActiveRail(kRailGames);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK_FALSE(when_to_record_shown());
}

TEST_CASE("ui: The game with Only in games has no displays to pick") {
    MultiMonitorFakeScreenHost host;
    host.count = 2;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    config.Set("capture.target", JsonValue::MakeStr("game"));
    config.Set("capture.mode", JsonValue::MakeStr("game"));
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);
    REQUIRE(screen.PageChildrenForTest().size() == 2);  // Recording, then the game alone
    auto what = std::dynamic_pointer_cast<CardContainer>(screen.PageChildrenForTest()[1]);
    REQUIRE(what);
    CHECK(what->title == L"What gets recorded");
    CHECK(screen.MonitorCardBoundsForTest().empty());
}

TEST_CASE("ui: a game tile says Recording now only when the engine records that target") {
    MultiMonitorFakeScreenHost host;
    host.count = 2;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    config.Set("capture.target", JsonValue::MakeStr("game"));
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);
    auto game_detail = [&] {
        auto card = std::dynamic_pointer_cast<CardContainer>(screen.PageChildrenForTest().back());
        REQUIRE(card);
        auto sources = std::dynamic_pointer_cast<RecordingSources>(card->rows.front());
        REQUIRE(sources);
        return sources->game->title + L"|" + sources->game->detail;
    };
    CHECK(game_detail() == L"No game running|Starts when a game opens");
    auto status = JsonValue::MakeObject();
    status.Set("current_game", JsonValue::MakeStr("VALORANT"));
    status.Set("target", JsonValue::MakeStr("monitor"));
    status.Set("mode", JsonValue::MakeStr("desktop"));
    status.Set("enabled", JsonValue::MakeBool(true));
    screen.SeedRunStatusForSnapshot(status);
    CHECK(game_detail() == L"VALORANT|Open now");  // staged, not saved yet
    status.Set("target", JsonValue::MakeStr("game"));
    screen.SeedRunStatusForSnapshot(status);
    CHECK(game_detail() == L"VALORANT|Recording now");
    status.Set("enabled", JsonValue::MakeBool(false));
    screen.SeedRunStatusForSnapshot(status);
    CHECK(game_detail() == L"VALORANT|Open now");
}

TEST_CASE("ui: Displays with Only in games says whether a game has it recording") {
    MultiMonitorFakeScreenHost host;
    host.count = 2;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    config.Set("capture.mode", JsonValue::MakeStr("game"));
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);
    auto strip_at_1 = [&] { return std::dynamic_pointer_cast<StatusStrip>(screen.PageChildrenForTest()[1]); };
    CHECK_FALSE(strip_at_1());  // no status yet: claim nothing
    auto status = JsonValue::MakeObject();
    status.Set("target", JsonValue::MakeStr("monitor"));
    status.Set("mode", JsonValue::MakeStr("game"));
    status.Set("enabled", JsonValue::MakeBool(true));
    screen.SeedRunStatusForSnapshot(status);
    REQUIRE(strip_at_1());
    CHECK(strip_at_1()->text == L"No game open: not recording");
    CHECK_FALSE(strip_at_1()->live);
    status.Set("current_game", JsonValue::MakeStr("VALORANT"));
    screen.SeedRunStatusForSnapshot(status);
    REQUIRE(strip_at_1());
    CHECK(strip_at_1()->text == L"VALORANT is open: recording your displays");
    CHECK(strip_at_1()->live);
}

TEST_CASE("ui: a staged monitor toggle survives the rebuild it triggers; the last one can't go") {
    // capture.monitors is staged, not committed. The tiles used to re-read current_config_ on the
    // rebuild ApplySettingChange runs, so a just-checked screen flipped back until Save.
    MultiMonitorFakeScreenHost host;
    host.count = 2;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    JsonValue only_first = JsonValue::MakeArray();
    only_first.arr.push_back(JsonValue::MakeStr("\\\\.\\DISPLAY1"));
    config.Set("capture.monitors", only_first);
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);

    auto tiles = CardScreens(ContainerTitled(screen, L"Displays"));
    REQUIRE(tiles.size() == 2);
    CHECK(tiles[0]->selected);
    CHECK_FALSE(tiles[1]->selected);
    // Unchecking the only checked display is refused and the screen stays checked.
    tiles[0]->selected = false;
    tiles[0]->on_change(false);
    CHECK(tiles[0]->selected);
    CHECK(screen.StagedValueForTest("capture.monitors") == nullptr);

    tiles[1]->selected = true;
    tiles[1]->on_change(true);
    tiles = CardScreens(ContainerTitled(screen, L"Displays"));
    REQUIRE(tiles.size() == 2);
    CHECK(tiles[0]->selected);
    CHECK(tiles[1]->selected);
}

TEST_CASE("ui: hiding a display takes it out of the diagram, which rescales to the rest") {
    MultiMonitorFakeScreenHost host;
    host.layout = {PlacedMonitor(1, 0, 0, 2560, 1440), PlacedMonitor(2, -1920, 360, 1920, 1080)};
    SettingsScreen screen(&host);
    const auto config = DefaultConfigJson();
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);
    auto screens = CardScreens(ContainerTitled(screen, L"Displays"));
    REQUIRE(screens.size() == 2);
    const float before = screens[0]->bounds.right - screens[0]->bounds.left;
    REQUIRE(screens[1]->on_hide);
    screens[1]->on_hide();
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    screens = CardScreens(ContainerTitled(screen, L"Displays"));
    REQUIRE(screens.size() == 1);
    CHECK_FALSE(screens[0]->on_hide);  // the last visible one stays
    CHECK(screens[0]->bounds.right - screens[0]->bounds.left >= before);
}

// Window mode's grid: N fake windows from list_windows, synchronously.
class WindowListFakeScreenHost : public MultiMonitorFakeScreenHost {
public:
    std::vector<WindowEntry> windows;
    std::vector<uint64_t> thumbs_requested;
    void RequestWindowListAsync(std::function<void(std::vector<WindowEntry>)> on_result) override {
        if (on_result) on_result(windows);
    }
    void RequestWindowThumbnailsAsync(
        std::vector<uint64_t> hwnds,
        std::function<void(uint64_t, bool, UINT, UINT, std::vector<uint8_t>)> on_result) override {
        for (uint64_t h : hwnds) {
            thumbs_requested.push_back(h);
            if (on_result) on_result(h, false, 0, 0, {});
        }
    }
};

TEST_CASE("ui: window mode lists running windows, eight until Show all, saved one first") {
    WindowListFakeScreenHost host;
    for (int i = 0; i < 11; ++i)
        host.windows.push_back(WindowEntry{L"Window " + std::to_wstring(i), "app" + std::to_string(i) + ".exe",
                                           static_cast<uint64_t>(100 + i)});
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    config.Set("capture.target", JsonValue::MakeStr("window"));
    config.Set("capture.target_window", JsonValue::MakeStr("app7.exe"));
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);

    auto card = ContainerTitled(screen, L"Window");
    REQUIRE(card);
    CHECK(card->help.find(L"pauses recording") != std::wstring::npos);
    REQUIRE(card->actions.size() == 1);  // Show all
    auto tiles = CardTiles(card);
    REQUIRE(tiles.size() == 8);
    CHECK(tiles[0]->detail == L"Window 7");  // the saved window leads, even from past the fold
    CHECK(tiles[0]->selected);
    CHECK_FALSE(tiles[0]->dimmed);
    for (size_t i = 1; i < tiles.size(); ++i) CHECK_FALSE(tiles[i]->selected);
    CHECK(host.thumbs_requested.size() == 8);  // thumbnails only for tiles on screen
    CHECK(ContainerTitled(screen, L"Displays") == nullptr);

    // Picking a tile stages the process name, not the (volatile) window title.
    tiles[2]->on_change(true);
    const JsonValue* staged = screen.StagedValueForTest("capture.target_window");
    REQUIRE(staged);
    CHECK(staged->str.find(".exe") != std::string::npos);
}

TEST_CASE("ui: window tiles show games, and Mark as game stages the exe's full path") {
    WindowListFakeScreenHost host;
    WindowEntry game{L"League of Legends (TM) Client", "League of Legends.exe", 1};
    game.image_path = L"E:\\Games\\Riot Games\\League of Legends\\Game\\League of Legends.exe";
    game.is_game = true;
    WindowEntry tool{L"Renderer", "renderer.exe", 2};
    tool.image_path = L"C:\\Tools\\renderer.exe";
    WindowEntry chat{L"Discord", "Discord.exe", 3};
    chat.image_path = L"C:\\Users\\Me\\AppData\\Local\\Discord\\app-1.0\\Discord.exe";
    chat.not_game = true;
    host.windows = {game, tool, chat};
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    config.Set("capture.target", JsonValue::MakeStr("window"));
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);

    auto tiles = CardTiles(ContainerTitled(screen, L"Window"));
    REQUIRE(tiles.size() == 3);
    CHECK(tiles[0]->title_icon == IconType::Gamepad2);
    CHECK(tiles[0]->action_label.empty());
    CHECK(tiles[2]->action_label.empty());  // the list already says it isn't one
    REQUIRE(tiles[1]->on_action);
    CHECK(tiles[1]->title_icon == IconType::None);

    auto mark = tiles[1]->on_action;
    mark();
    const auto decisions = screen.StagedGameDecisions();
    REQUIRE(decisions.size() == 1);
    CHECK(decisions[0].path == L"C:\\Tools\\renderer.exe");
    CHECK(decisions[0].is_game);
    // The staged answer shows on the rebuilt tile straight away.
    tiles = CardTiles(ContainerTitled(screen, L"Window"));
    REQUIRE(tiles.size() == 3);
    CHECK(tiles[1]->title_icon == IconType::Gamepad2);
    CHECK(tiles[1]->action_label.empty());
}

TEST_CASE("ui: a saved window that isn't running stays chosen, pale, and says so") {
    WindowListFakeScreenHost host;
    host.windows.push_back(WindowEntry{L"Other", "other.exe", 1});
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    config.Set("capture.target", JsonValue::MakeStr("window"));
    config.Set("capture.target_window", JsonValue::MakeStr("game.exe"));
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);

    auto tiles = CardTiles(ContainerTitled(screen, L"Window"));
    REQUIRE(tiles.size() == 2);
    CHECK(tiles[0]->title == L"game");
    CHECK(tiles[0]->detail == L"Not running");
    CHECK(tiles[0]->selected);
    CHECK(tiles[0]->dimmed);
    CHECK_FALSE(tiles[1]->selected);
}

TEST_CASE("ui: staging a window target swaps the display tiles out before Save") {
    MultiMonitorFakeScreenHost host;
    host.count = 2;
    SettingsScreen screen(&host);
    const auto config = DefaultConfigJson();
    screen.InitSchema(RealSpecs(), &config);
    OpenCaptureVideo(screen);
    CHECK(screen.MonitorCardBoundsForTest().size() == 2);

    screen.ApplySettingChangeForTest("capture.target", JsonValue::MakeStr("window"));
    CHECK(screen.MonitorCardBoundsForTest().empty());
    auto card = ContainerTitled(screen, L"Window");
    REQUIRE(card);
    // No window list from the fake host: the card says it's loading or empty.
    CHECK_FALSE(card->help.empty());

    // "The game" still needs the displays: they're recorded while no game is on screen.
    screen.ApplySettingChangeForTest("capture.target", JsonValue::MakeStr("game"));
    CHECK(screen.MonitorCardBoundsForTest().size() == 2);
}

TEST_CASE("ui: Advanced tab's Audio card isn't silently empty below the fold") {
    // screens.h's own comment on RowCountForGroup: the Advanced rail's Audio card sits below the
    // fold in a fixed-size snapshot, so a visual check alone wouldn't catch BuildAudioSourceRows
    // silently producing zero rows. This is the headless equivalent.
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);  // [Monitors, Audio] -- index 1 is Audio (kMonAudioGroups[1])
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    CHECK(screen.RowCountForGroup(L"Audio") > 0);
}

// The "Add app" menu showed an app closed long ago.
// The on-click pickers (BuildAudioSourceRows' "Add app",
// BuildExclusionRows'/BuildGameOverrideRows' "Add process") used to short-circuit on
// process_list_fetched_ and hand back whatever process_list_cache_ held from the overlay's first
// fetch -- stale for the rest of that overlay session even if the picker is clicked again later,
// after the real process list changed. OpenFreshProcessPicker fixes this by always re-fetching.
class CountingProcessHost : public FakeScreenHost {
public:
    std::vector<ProcessEntry> current = {ProcessEntry{"gameA.exe", "Game A"}};
    int request_count = 0;
    void RequestProcessListAsync(std::function<void(std::vector<ProcessEntry>)> on_result) override {
        ++request_count;
        if (on_result) on_result(current);
    }
};

TEST_CASE("ui: the process picker always re-fetches, never trusts a stale cache") {
    CountingProcessHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);

    std::vector<ProcessEntry> first_result;
    screen.OpenFreshProcessPickerForTest(
        [&](const std::vector<ProcessEntry>& processes) { first_result = processes; });
    CHECK(host.request_count == 1);
    REQUIRE(first_result.size() == 1);
    CHECK(first_result[0].name == "gameA.exe");

    // Simulates time passing with the overlay still open: gameA closed, gameB launched.
    host.current = {ProcessEntry{"gameB.exe", "Game B"}};
    std::vector<ProcessEntry> second_result;
    screen.OpenFreshProcessPickerForTest(
        [&](const std::vector<ProcessEntry>& processes) { second_result = processes; });
    CHECK(host.request_count == 2);
    REQUIRE(second_result.size() == 1);
    CHECK(second_result[0].name == "gameB.exe");
}

TEST_CASE("ui: rail/card geometry is non-degenerate and scales with dpi") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);

    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    const D2D1_RECT_F rail_1x = screen.RailBoundsForTest();
    const D2D1_RECT_F card_1x = screen.ViewBoundsForTest();

    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.5f);
    const D2D1_RECT_F rail_15x = screen.RailBoundsForTest();
    const D2D1_RECT_F card_15x = screen.ViewBoundsForTest();

    for (const auto& r : {rail_1x, card_1x, rail_15x, card_15x}) {
        CHECK(r.right > r.left);
        CHECK(r.bottom > r.top);
    }
    // The rail and the page share the panel side by side, never overlapping.
    CHECK(rail_1x.right <= card_1x.left);
    CHECK(rail_15x.right <= card_15x.left);
    // The rail's size comes from DIP tokens, so a real dpi change must widen it.
    CHECK((rail_15x.right - rail_15x.left) > (rail_1x.right - rail_1x.left));
}

TEST_CASE("ui: repeated Layout with unchanged bounds skips RebuildGroupRows (rows_dirty_ contract)") {
    // screens.h: Layout() runs every repaint, but RebuildGroupRows is skipped unless bounds/dpi
    // actually changed, so it doesn't destroy and recreate every row widget (which used to wipe
    // transient state, e.g. a text field's edit state, on the very next repaint). Asserts the
    // rebuild count directly, not a pipe-call proxy: list_monitors/list_processes/etc. are also
    // guarded by their own per-overlay-open cache flags, so a pipe-count-only check would pass even
    // if this gate broke and RebuildGroupRows ran on every Layout() call.
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);

    const D2D1_RECT_F bounds = D2D1::RectF(0, 0, 1280, 800);
    screen.Layout(bounds, 1.0f);
    const size_t count_after_first = screen.RebuildCountForTest();
    CHECK(count_after_first > 0);

    screen.Layout(bounds, 1.0f);
    screen.Layout(bounds, 1.0f);
    CHECK(screen.RebuildCountForTest() == count_after_first);

    // Changing bounds/dpi must still trigger a rebuild -- otherwise this would vacuously pass by a
    // gate that's stuck permanently closed, not one that's correctly comparing against last-seen
    // bounds.
    screen.Layout(D2D1::RectF(0, 0, 1000, 700), 1.0f);
    CHECK(screen.RebuildCountForTest() == count_after_first + 1);
}

// Capture > Audio fixtures: sources as config holds them, and the page's controls by type.
JsonValue AudioSource(const char* id, const char* kind, const char* mode = nullptr, const char* target = nullptr,
                      const char* group = nullptr) {
    JsonValue o = JsonValue::MakeObject();
    o.Set("id", JsonValue::MakeStr(id));
    o.Set("kind", JsonValue::MakeStr(kind));
    o.Set("device", JsonValue::MakeStr("default"));
    if (mode) o.Set("mode", JsonValue::MakeStr(mode));
    if (target) o.Set("target", JsonValue::MakeStr(target));
    if (group) o.Set("group_id", JsonValue::MakeStr(group));
    return o;
}
JsonValue AudioConfig(std::vector<JsonValue> list, bool per_app) {
    ombra::Config cfg{};
    JsonValue config = ombra::SerializeConfigFlat(cfg);
    JsonValue sources = JsonValue::MakeArray();
    for (auto& s : list) sources.arr.push_back(std::move(s));
    config.Set("audio.sources", std::move(sources));
    config.Set("audio.per_app_mode", JsonValue::MakeBool(per_app));
    return config;
}
void OpenAudioPage(SettingsScreen& screen, const JsonValue& config) {
    screen.InitSchema(RealSpecs(), &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
}
template <class T> std::shared_ptr<T> ControlOf(const std::shared_ptr<Widget>& w) {
    if (auto row = std::dynamic_pointer_cast<SettingRow>(w))
        for (const auto& c : row->controls)
            if (auto t = std::dynamic_pointer_cast<T>(c)) return t;
    return nullptr;
}
// Opens a card's ⋯ (the fake host records the menu) and picks `label`.
bool PickFromMore(FakeScreenHost& host, const std::shared_ptr<CardContainer>& card, const std::wstring& label) {
    if (!card || card->actions.empty()) return false;
    auto more = std::dynamic_pointer_cast<IconButtonControl>(card->actions[0]);
    if (!more || !more->on_click) return false;
    more->on_click();
    for (size_t i = 0; i < host.dropdown_options.size(); ++i) {
        if (host.dropdown_options[i] != label) continue;
        auto pick = host.dropdown_pick;
        pick(static_cast<int>(i));
        return true;
    }
    return false;
}

class AudioDevicesFakeScreenHost : public FakeScreenHost {
public:
    void RequestAudioDeviceListAsync(std::function<void(std::vector<AudioDeviceEntry>)> on_result) override {
        if (on_result)
            on_result({{"out1", "Speakers", true, false},
                       {"out2", "Headphones", false, false},
                       {"in2", "Headset Mic", false, true},  // the default isn't first: the menu reorders
                       {"in1", "Mic Array", true, true}});
    }
};

class MeterTimerFakeScreenHost : public FakeScreenHost {
public:
    std::vector<bool> timer_calls;
    void SetMeterTimer(bool on) override { timer_calls.push_back(on); }
};
// The level reader factory is process-wide: put it back so other tests see no meters move.
struct LevelReaderScope {
    explicit LevelReaderScope(std::map<std::string, float> peaks) {
        SetLevelReaderFactory([peaks] { return std::make_unique<FixedLevelReader>(peaks); });
    }
    ~LevelReaderScope() { SetLevelReaderFactory(nullptr); }
};
std::shared_ptr<LevelMeter> MeterOf(const std::shared_ptr<SettingRow>& row) {
    return row ? std::dynamic_pointer_cast<LevelMeter>(row->below) : nullptr;
}

TEST_CASE("ui: a level meter lights blocks from -60 dB up to 0, none when off") {
    LevelMeter m;
    CHECK(m.LitBlocks() == 0);
    m.db = -60.0f;
    CHECK(m.LitBlocks() == 0);
    m.db = -57.5f;  // one block's worth
    CHECK(m.LitBlocks() == 1);
    m.db = -12.0f;
    CHECK(m.LitBlocks() == 20);
    m.db = 0.0f;
    CHECK(m.LitBlocks() == LevelMeter::kBlocks);
    m.db = 6.0f;  // gain past full scale stays full
    CHECK(m.LitBlocks() == LevelMeter::kBlocks);
    m.enabled = false;
    CHECK(m.LitBlocks() == 0);
}

TEST_CASE("ui: Audio page meters show each source's level after its gain; an off source stays dark") {
    LevelReaderScope levels({{"desktop", 0.25f}, {"mic", 0.5f}, {"app:discord", 0.25f}});
    MeterTimerFakeScreenHost host;
    SettingsScreen screen(&host);
    JsonValue desktop = AudioSource("desktop", "output_loopback", "all");
    desktop.Set("gain_db", JsonValue::MakeNum(6.0));
    JsonValue mic = AudioSource("mic", "input");
    mic.Set("enabled", JsonValue::MakeBool(false));
    OpenAudioPage(screen, AudioConfig({std::move(desktop), std::move(mic),
                                       AudioSource("app:discord", "output_loopback", "only", "Discord.exe")}, true));
    CHECK(host.timer_calls == std::vector<bool>{true});
    const auto d = MeterOf(screen.AudioRowForTest("desktop"));
    const auto m = MeterOf(screen.AudioRowForTest("mic"));
    const auto a = MeterOf(screen.AudioRowForTest("app:discord"));
    REQUIRE(d);
    REQUIRE(m);
    REQUIRE(a);
    CHECK(d->db == doctest::Approx(PeakToDb(0.25f) + 6.0f));
    CHECK(d->LitBlocks() == 22);
    CHECK(a->LitBlocks() == 20);
    CHECK(m->LitBlocks() == 0);
    // A staged gain edit moves the meter before Save.
    const auto gain = ControlOf<StepperControl>(screen.AudioRowForTest("app:discord"));
    REQUIRE(gain);
    gain->on_change(6.0);
    REQUIRE(MeterOf(screen.AudioRowForTest("app:discord")));
    CHECK(MeterOf(screen.AudioRowForTest("app:discord"))->LitBlocks() == 22);
    // Hidden and shown again on the same page (no rebuild): the meters start again.
    screen.StopMeters();
    CHECK(host.timer_calls == std::vector<bool>{true, false});
    screen.SyncMeters();
    CHECK(host.timer_calls == std::vector<bool>{true, false, true});
    CHECK(MeterOf(screen.AudioRowForTest("desktop"))->LitBlocks() == 22);
    // The meters stop when the page goes.
    host.timer_calls.clear();
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(host.timer_calls == std::vector<bool>{false});
    CHECK_FALSE(screen.OnMeterTick());
}

TEST_CASE("ui: no level reader, no meter timer") {
    MeterTimerFakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all")}, false));
    CHECK(MeterOf(screen.AudioRowForTest("desktop")) != nullptr);
    CHECK(host.timer_calls.empty());
}

// Simple records Desktop whole whatever is saved (SelectActiveAudioSources), so the page says
// nothing about a Desktop exclusion there; the mic's device applies in both modes.
TEST_CASE("ui: Simple shows only what Simple records") {
    AudioDevicesFakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all_except", "Discord.exe"),
                                       AudioSource("mic", "input")}, false));
    const auto mic_device = ControlOf<SelectControl>(screen.AudioRowForTest("mic"));
    REQUIRE(mic_device);
    CHECK(mic_device->options.front() == L"Default: Mic Array");
    CHECK(screen.AudioRowForTest("desktop")->help.empty());
    CHECK(ControlOf<SelectControl>(screen.AudioRowForTest("desktop")) != nullptr);  // its menus stay
}

TEST_CASE("ui: Simple mode shows Desktop and Mic tracks; app sources stay hidden and unchanged") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all"), AudioSource("mic", "input"),
                                       AudioSource("only_game.exe", "output_loopback", "only", "game.exe")}, false));
    CHECK_FALSE(screen.AudioPerAppModeForTest());
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop", L"Mic"});
    CHECK(screen.AudioRowForTest("only_game.exe") == nullptr);
    CHECK(screen.StagedAudioFieldForTest("only_game.exe", "target") == "game.exe");
    REQUIRE(screen.AudioRowForTest("mic"));
    CHECK(screen.AudioRowForTest("mic")->title == L"Microphone");
}

TEST_CASE("ui: Audio page: Sources card first, a card per track, Saved audio last; no codec row") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all"),
                                       AudioSource("only_League.exe", "output_loopback", "only", "League.exe", "g1"),
                                       AudioSource("only_Discord.exe", "output_loopback", "only", "Discord.exe", "g1")}, true));
    const auto& children = screen.PageChildrenForTest();
    REQUIRE(children.size() >= 4);
    auto lead = std::dynamic_pointer_cast<CardContainer>(children.front());
    REQUIRE(lead);
    CHECK(lead->title == L"Sources");
    REQUIRE(ControlOf<SwitchControl>(lead->rows[0]));
    CHECK(ControlOf<SwitchControl>(lead->rows[0])->on);  // App tracks
    // Named as the saved file names its tracks (AudioTrackName joined in list order).
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop", L"League + Discord"});
    auto last = std::dynamic_pointer_cast<CardContainer>(children.back());
    REQUIRE(last);
    CHECK(last->title == L"Saved audio");
    std::vector<std::wstring> titles;
    for (const auto& r : last->rows)
        if (auto row = std::dynamic_pointer_cast<SettingRow>(r)) titles.push_back(row->title);
    CHECK(titles == std::vector<std::wstring>{L"Combined track", L"Bitrate"});
}

TEST_CASE("ui: audio_status sets each source's status line in place; an unreported id gets none") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all"),
                                       AudioSource("only_Discord.exe", "output_loopback", "only", "Discord.exe"),
                                       AudioSource("only_Riot.exe", "output_loopback", "only", "Riot.exe")}, true));
    CHECK(screen.AudioRowForTest("only_Discord.exe")->note.empty());  // nothing heard yet
    const size_t built = screen.RebuildCountForTest();

    JsonValue resp = JsonValue::MakeObject();
    JsonValue arr  = JsonValue::MakeArray();
    auto entry = [](const char* id, const char* state) {
        JsonValue e = JsonValue::MakeObject();
        e.Set("id", JsonValue::MakeStr(id));
        e.Set("state", JsonValue::MakeStr(state));
        return e;
    };
    arr.arr.push_back(entry("desktop", "quiet"));
    arr.arr.push_back(entry("only_Discord.exe", "live"));
    arr.arr.push_back(entry("only_Riot.exe", "failed"));
    resp.Set("sources", std::move(arr));
    screen.ApplyAudioStatus(&resp);

    CHECK(screen.AudioRowForTest("desktop")->note == L"Quiet");
    CHECK(screen.AudioRowForTest("only_Discord.exe")->note == L"Live");
    CHECK(screen.AudioRowForTest("only_Discord.exe")->note_tone == SettingRow::Tone::Good);
    CHECK(screen.AudioRowForTest("only_Riot.exe")->note_tone == SettingRow::Tone::Error);
    // A status line that appears needs room under the name the page was laid out without: rebuilt.
    CHECK(screen.RebuildCountForTest() == built + 1);

    // Only the text changes: in place, no rebuild.
    const size_t rebuilds = screen.RebuildCountForTest();
    JsonValue quieter = JsonValue::MakeObject();
    JsonValue arr2 = JsonValue::MakeArray();
    arr2.arr.push_back(entry("desktop", "quiet"));
    arr2.arr.push_back(entry("only_Discord.exe", "quiet"));
    arr2.arr.push_back(entry("only_Riot.exe", "failed"));
    quieter.Set("sources", std::move(arr2));
    screen.ApplyAudioStatus(&quieter);
    CHECK(screen.AudioRowForTest("only_Discord.exe")->note == L"Quiet");
    CHECK(screen.RebuildCountForTest() == rebuilds);

    screen.ApplyAudioStatus(nullptr);  // engine unreachable: the line goes, and its room with it
    CHECK(screen.AudioRowForTest("only_Discord.exe")->note.empty());
    CHECK(screen.RebuildCountForTest() == rebuilds + 1);
}

// Mic's menu lists inputs only (Desktop's outputs only); picking a device stages its id, and
// picking the current default stages "default" so the source keeps following the system default.
TEST_CASE("ui: Mic and Desktop each pick from their own direction's devices") {
    AudioDevicesFakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback"), AudioSource("mic", "input")}, true));

    auto device_of = [&](const char* id) -> std::shared_ptr<SelectControl> {
        auto row = screen.AudioRowForTest(id);
        if (!row) return nullptr;
        std::shared_ptr<SelectControl> last;  // Desktop's first Select is its mode, the device follows
        for (const auto& c : row->controls)
            if (auto sel = std::dynamic_pointer_cast<SelectControl>(c)) last = sel;
        return last;
    };
    auto mic = device_of("mic");
    auto desktop = device_of("desktop");
    REQUIRE(mic);
    REQUIRE(desktop);
    CHECK(mic->options == std::vector<std::wstring>{L"Default: Mic Array", L"Headset Mic"});
    CHECK(mic->selected == 0);
    CHECK(desktop->options == std::vector<std::wstring>{L"Default: Speakers", L"Headphones"});

    mic->on_change(1);
    CHECK(screen.StagedAudioFieldForTest("mic", "device") == "in2");
    CHECK(screen.StagedAudioFieldForTest("desktop", "device") == "default");
    CHECK(screen.AudioSourcesDirtyForTest());

    mic = device_of("mic");  // the staged edit rebuilt the page
    REQUIRE(mic);
    CHECK(mic->selected == 1);
    mic->on_change(0);
    CHECK(screen.StagedAudioFieldForTest("mic", "device") == "default");
}

// The page materializes Desktop when audio.sources is absent; that alone, or an edit undone, is
// not a change (it used to keep the Save bar up for good).
TEST_CASE("ui: Audio with no audio.sources in config: an undone edit leaves staging") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, DefaultConfigJson());
    CHECK_FALSE(screen.HasPendingChangesForTest());
    auto sw = ControlOf<SwitchControl>(screen.AudioRowForTest("desktop"));
    REQUIRE(sw);
    sw->on_change(false);
    CHECK(screen.AudioSourcesDirtyForTest());
    sw = ControlOf<SwitchControl>(screen.AudioRowForTest("desktop"));
    REQUIRE(sw);
    sw->on_change(true);
    CHECK_FALSE(screen.AudioSourcesDirtyForTest());
    CHECK_FALSE(screen.HasPendingChangesForTest());
}

TEST_CASE("ui: a track's more menu mixes it into another and splits it back") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all"), AudioSource("mic", "input")}, false));
    REQUIRE(PickFromMore(host, screen.AudioTrackCardForTest("mic"), L"Mix with Desktop"));
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop + Mic"});
    CHECK(screen.StagedAudioFieldForTest("mic", "group_id") == screen.StagedAudioFieldForTest("desktop", "group_id"));
    REQUIRE(PickFromMore(host, screen.AudioTrackCardForTest("desktop"), L"Split into separate tracks"));
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop", L"Mic"});
}

TEST_CASE("ui: dragging a source by its grip mixes it into the track it's dropped on, or out to its own") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all"),
                                       AudioSource("only_League.exe", "output_loopback", "only", "League.exe", "g1"),
                                       AudioSource("only_Discord.exe", "output_loopback", "only", "Discord.exe", "g1")}, true));
    auto center = [](const D2D1_RECT_F& r) { return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f); };
    auto grip_down = [&](const char* id) {
        auto row = screen.AudioRowForTest(id);
        REQUIRE(row);
        screen.OnMouseDown(center(row->GripRect()));
    };

    // Discord onto Desktop: the slot appears below the tracks, Desktop's card lights up.
    grip_down("only_Discord.exe");
    REQUIRE(screen.DraggingAudioForTest());
    CHECK(screen.AudioRowForTest("only_Discord.exe")->lifted);
    REQUIRE(screen.AudioDropSlotForTest());
    auto desktop_card = screen.AudioTrackCardForTest("desktop");
    REQUIRE(desktop_card);
    screen.OnMouseMove(center(desktop_card->bounds));
    CHECK(desktop_card->drop_target);
    screen.OnMouseUp(center(desktop_card->bounds));
    CHECK_FALSE(screen.DraggingAudioForTest());
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop + Discord", L"League"});
    CHECK(screen.AudioDropSlotForTest() == nullptr);

    // Back out onto the slot: its own track again.
    grip_down("only_Discord.exe");
    auto slot = screen.AudioDropSlotForTest();
    REQUIRE(slot);
    screen.OnMouseMove(center(slot->bounds));
    CHECK(slot->active);
    screen.OnMouseUp(center(slot->bounds));
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop", L"League", L"Discord"});

    // A source alone on its track has nowhere new to go; a drop on nothing changes nothing.
    grip_down("only_Discord.exe");
    CHECK(screen.AudioDropSlotForTest() == nullptr);
    screen.OnMouseUp(D2D1::Point2F(1, 1));
    CHECK(screen.AudioTrackTitlesForTest() == std::vector<std::wstring>{L"Desktop", L"League", L"Discord"});
}

class PlayingAppsHost : public FakeScreenHost {
public:
    void RequestAudioSessionListAsync(std::function<void(std::vector<AudioSessionEntry>)> on_result) override {
        if (!on_result) return;
        AudioSessionEntry chrome, vlc;
        chrome.name = "chrome.exe";
        chrome.display_name = "Google Chrome";
        vlc.name = "vlc.exe";
        vlc.display_name = "VLC";
        on_result({chrome, vlc});
    }
};

// "Own track" leaves the app out of Desktop, one app at a time (process loopback excludes one tree);
// removing that track gives Desktop it back.
TEST_CASE("ui: Playing now: Own track excludes it from Desktop once; removing the track clears it") {
    PlayingAppsHost host;
    SettingsScreen screen(&host);
    OpenAudioPage(screen, AudioConfig({AudioSource("desktop", "output_loopback", "all", "")}, true));
    auto own_of = [&](const char* id) -> std::shared_ptr<ButtonControl> {
        auto row = screen.AudioRowForTest(id);
        return row ? ControlOf<ButtonControl>(row) : nullptr;
    };
    REQUIRE(own_of("live:chrome.exe"));
    own_of("live:chrome.exe")->on_click();
    CHECK(screen.StagedAudioFieldForTest("desktop", "mode") == "all_except");
    CHECK(screen.StagedAudioFieldForTest("desktop", "target") == "chrome.exe");
    CHECK(screen.AudioRowForTest("live:chrome.exe") == nullptr);  // a track now
    REQUIRE(screen.AudioRowForTest("only_chrome.exe"));
    REQUIRE(own_of("live:vlc.exe"));
    CHECK_FALSE(own_of("live:vlc.exe")->enabled);  // Desktop already leaves one out

    // Edit list puts a remove button on Chrome's row; removing it gives Desktop all its audio back.
    REQUIRE(PickFromMore(host, std::dynamic_pointer_cast<CardContainer>(screen.PageChildrenForTest().front()), L"Edit list"));
    auto remove = ControlOf<IconButtonControl>(screen.AudioRowForTest("only_chrome.exe"));
    REQUIRE(remove);
    CHECK(ControlOf<IconButtonControl>(screen.AudioRowForTest("desktop")) == nullptr);  // Desktop stays
    remove->on_click();
    CHECK(screen.StagedAudioFieldForTest("desktop", "mode") == "all");
    CHECK(screen.StagedAudioFieldForTest("desktop", "target").empty());
    CHECK_FALSE(screen.AudioSourcesDirtyForTest());
}

TEST_CASE("ui: staged audio gain is dirty until saved, restored on discard") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);  // populates audio_sources_ui_ via BuildAudioSourceRows
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    const double committed_before = screen.CommittedDesktopGainForTest();
    CHECK_FALSE(screen.AudioSourcesDirtyForTest());

    screen.StageAudioGainForTest(-6.0);
    CHECK(screen.AudioSourcesDirtyForTest());
    CHECK(screen.CommittedDesktopGainForTest() == committed_before);  // staged, not yet committed

    screen.DiscardAudioSourcesForTest();
    CHECK_FALSE(screen.AudioSourcesDirtyForTest());
    CHECK(screen.CommittedDesktopGainForTest() == committed_before);
}

// An edit put back to the saved value by hand drops out of staging, and the
// Save bar goes with the last one.
TEST_CASE("ui: an edit set back to the saved value leaves staging") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    ombra::JsonValue desktop = ombra::JsonValue::MakeObject();
    desktop.Set("id", ombra::JsonValue::MakeStr("desktop"));
    desktop.Set("kind", ombra::JsonValue::MakeStr("output_loopback"));
    desktop.Set("device", ombra::JsonValue::MakeStr("default"));
    desktop.Set("mode", ombra::JsonValue::MakeStr("all"));
    desktop.Set("target", ombra::JsonValue::MakeStr(""));
    desktop.Set("enabled", ombra::JsonValue::MakeBool(true));
    desktop.Set("gain_db", ombra::JsonValue::MakeNum(0.0));
    ombra::JsonValue sources = ombra::JsonValue::MakeArray();
    sources.arr.push_back(desktop);
    config.Set("audio.sources", sources);
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    SUBCASE("field") {
        const auto* saved = config.Find("buffer.seconds");
        REQUIRE(saved);
        const ombra::JsonValue original = *saved;
        const auto* cursor = config.Find("capture.capture_cursor");
        REQUIRE(cursor);
        screen.ApplySettingChangeForTest("buffer.seconds", ombra::JsonValue::MakeNum(original.num + 30));
        screen.ApplySettingChangeForTest("capture.capture_cursor", ombra::JsonValue::MakeBool(!cursor->b));
        CHECK(screen.HasPendingChangesForTest());
        screen.ApplySettingChangeForTest("buffer.seconds", original);
        CHECK(screen.StagedValueForTest("buffer.seconds") == nullptr);
        CHECK(screen.HasPendingChangesForTest());  // the other edit still stands
        screen.ApplySettingChangeForTest("capture.capture_cursor", *cursor);
        CHECK_FALSE(screen.HasPendingChangesForTest());
    }
    SUBCASE("audio") {
        screen.StageAudioGainForTest(-6.0);
        CHECK(screen.AudioSourcesDirtyForTest());
        screen.StageAudioGainForTest(0.0);
        CHECK_FALSE(screen.AudioSourcesDirtyForTest());
        CHECK_FALSE(screen.HasPendingChangesForTest());
    }
}

// BuildAudioSourceRows' materialized-default
// entry (screens.cpp, hit when audio.sources is absent from config, same as DefaultConfigJson()
// here) used to stamp a "track" key into the synthesized source -- the field that caused the
// Code.exe/Discord.exe collision (config.h's AudioSourceSpec comment);
// this confirms the UI no longer writes it, not just that nothing reads it.
TEST_CASE("ui: BuildAudioSourceRows' default source carries no legacy \"track\" key") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);  // populates audio_sources_ui_ via BuildAudioSourceRows
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    CHECK_FALSE(screen.AudioSourceHasKeyForTest(0, "track"));
}

TEST_CASE("ui: ApplySettingChange stages, not commits, until SaveAll") {
    // Settings Save/Discard bar: a field edit used to reach the engine
    // immediately (ApplySettingChange's own RequestSetConfigAsync call). It now only stages into
    // pending_changes_ -- no pipe call, current_config_ untouched -- until SaveAll() commits every
    // staged key as one batch.
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAdvanced);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    const int set_config_calls_before = host.pipe.counts["set_config"];
    screen.ApplySettingChangeForTest("capture.capture_cursor", JsonValue::MakeBool(false));

    CHECK(host.pipe.counts["set_config"] == set_config_calls_before);  // no round trip at edit time
    CHECK(screen.HasPendingChangesForTest());
    {
        const auto* v = screen.CurrentConfigValueForTest("capture.capture_cursor");
        REQUIRE(v != nullptr);
        CHECK(v->b);  // still the old (true) value -- not yet saved
    }

    screen.SaveAllForTest();

    CHECK(host.pipe.counts["set_config"] == set_config_calls_before + 1);  // exactly one batch commit
    CHECK_FALSE(screen.HasPendingChangesForTest());
    const auto* v = screen.CurrentConfigValueForTest("capture.capture_cursor");
    REQUIRE(v != nullptr);
    CHECK(v->type == JsonValue::Type::Bool);
    CHECK_FALSE(v->b);
}

TEST_CASE("ui: Diagnostics' sharing tiles and Detailed logging stage their keys") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAdvanced);
    screen.SetActiveSubTab(1);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto slider = std::dynamic_pointer_cast<LevelTilesControl>(screen.ControlForTest("diagnostics.sharing"));
    REQUIRE(slider);
    CHECK(slider->selected == 1);  // Error reports, the schema default
    slider->focused = true;
    CHECK(slider->OnKeyDown(VK_LEFT));
    const JsonValue* staged = screen.StagedValueForTest("diagnostics.sharing");
    REQUIRE(staged);
    CHECK(staged->str == "off");
    auto sw = std::dynamic_pointer_cast<SwitchControl>(screen.ControlForTest("log.level"));
    REQUIRE(sw);
    CHECK_FALSE(sw->on);
    sw->focused = true;
    sw->OnKeyDown(VK_SPACE);
    staged = screen.StagedValueForTest("log.level");
    REQUIRE(staged);
    CHECK(staged->str == "debug");
}

namespace {
// Holds preview replies until the test flushes them, and answers with its own bitrate, so the
// derived-bitrate sync can be driven through both orderings.
class BitrateSyncHost : public FakeScreenHost {
public:
    std::vector<JsonValue> sent;
    std::vector<std::function<void()>> held;
    void RequestPreviewAsync(const JsonValue& config,
                             std::function<void(std::optional<JsonValue>, std::optional<JsonValue>)> on_result) override {
        sent.push_back(config);
        held.push_back([on_result]() {
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("bitrate_kbps", JsonValue::MakeNum(31337.0));
            if (on_result) on_result(resp, std::nullopt);
        });
    }
    void Flush() {
        auto h = std::move(held);
        held.clear();
        for (auto& f : h) f();
    }
};
}  // namespace

TEST_CASE("ui: a quality preset pick stages at once and stages the engine's derived bitrate on reply") {
    BitrateSyncHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailShortcuts);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    host.sent.clear();
    host.held.clear();
    host.pipe.counts.clear();
    screen.ChoosePresetForTest("encode.quality_preset", "ultra");
    CHECK(host.pipe.counts["preview_config"] == 0);  // nothing blocking went over the pipe
    REQUIRE(host.sent.size() >= 1);
    // The request that carries the sync is the one without a bitrate; FetchPreviewAsync's own
    // request (the whole staged config) may be in `sent` too.
    const bool any_without_bitrate = std::any_of(host.sent.begin(), host.sent.end(), [](const JsonValue& c) {
        const auto* p = c.Find("encode.quality_preset");
        return !c.Find("encode.bitrate_kbps") && p && p->str == "ultra";
    });
    CHECK(any_without_bitrate);

    host.Flush();
    screen.SaveAllForTest();
    const auto* br = screen.CurrentConfigValueForTest("encode.bitrate_kbps");
    REQUIRE(br != nullptr);
    CHECK(br->num == doctest::Approx(31337.0));
    const auto* preset = screen.CurrentConfigValueForTest("encode.quality_preset");
    REQUIRE(preset != nullptr);
    CHECK(preset->str == "ultra");
}

TEST_CASE("ui: a resolution pick under a preset asks for the derived bitrate and confirms as a realloc") {
    BitrateSyncHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    host.sent.clear();
    host.held.clear();
    screen.ChoosePresetForTest("capture.resolution", "1080p");
    const bool asked = std::any_of(host.sent.begin(), host.sent.end(), [](const JsonValue& c) {
        const auto* r = c.Find("capture.resolution");
        return !c.Find("encode.bitrate_kbps") && r && r->str == "1080p";
    });
    CHECK(asked);
    CHECK(screen.PendingWorstApplyForTest() == ApplyKind::Realloc);

    host.Flush();
    screen.SaveAllForTest();
    const auto* br = screen.CurrentConfigValueForTest("encode.bitrate_kbps");
    REQUIRE(br != nullptr);
    CHECK(br->num == doctest::Approx(31337.0));
}

TEST_CASE("ui: a derived bitrate reply is dropped after Discard or a switch to custom") {
    for (const bool via_discard : {true, false}) {
        CAPTURE(via_discard);
        BitrateSyncHost host;
        SettingsScreen screen(&host);
        const auto specs = RealSpecs();
        const auto config = DefaultConfigJson();
        screen.InitSchema(specs, &config);
        screen.SetActiveRail(kRailShortcuts);
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
        const double before = config.Find("encode.bitrate_kbps")->num;

        screen.ChoosePresetForTest("encode.quality_preset", "ultra");
        if (via_discard) screen.DiscardAllForTest();
        else screen.ChoosePresetForTest("encode.quality_preset", "custom");
        host.Flush();
        screen.SaveAllForTest();

        const auto* br = screen.CurrentConfigValueForTest("encode.bitrate_kbps");
        REQUIRE(br != nullptr);
        CHECK(br->num == doctest::Approx(before));
    }
}

TEST_CASE("ui: the memory card's engine baseline subtracts what the rings hold, not their full size") {
    // Rings commit as footage arrives: 20 s into a 100 MB ring, private bytes are 160 MB of which the
    // rings hold 30. Subtracting the full 101 MB left the engine at 59 MB (and at 0 right after a
    // start or a disarm's release).
    class HeldHost : public FakeScreenHost {
    public:
        bool report_held = true;
        void RequestPreviewAsync(const JsonValue&,
                                 std::function<void(std::optional<JsonValue>, std::optional<JsonValue>)> on_result) override {
            constexpr double kMb = 1024.0 * 1024.0;
            JsonValue mem = JsonValue::MakeObject();
            mem.Set("video_pipelines", JsonValue::MakeNum(1));
            mem.Set("audio_sources", JsonValue::MakeNum(1));
            mem.Set("video_capacity_bytes", JsonValue::MakeNum(100.0 * kMb));
            mem.Set("video_overshoot_bytes", JsonValue::MakeNum(0));
            mem.Set("audio_capacity_bytes", JsonValue::MakeNum(1.0 * kMb));
            mem.Set("audio_overshoot_bytes", JsonValue::MakeNum(0));
            if (report_held) {
                mem.Set("video_committed_bytes", JsonValue::MakeNum(28.0 * kMb));
                mem.Set("audio_committed_bytes", JsonValue::MakeNum(2.0 * kMb));
            }
            JsonValue status = JsonValue::MakeObject();
            status.Set("memory", std::move(mem));
            status.Set("rss_private_mb", JsonValue::MakeNum(160));
            if (on_result) on_result(JsonValue::MakeObject(), status);
        }
    };
    constexpr double kMb = 1024.0 * 1024.0;
    HeldHost host;
    SettingsScreen screen(&host);
    const auto config = DefaultConfigJson();
    screen.InitSchema(RealSpecs(), &config);
    screen.FetchPreviewAsync();
    {
        const MemoryCardData& d = screen.MemoryCardDataForTest();
        REQUIRE(d.valid);
        CHECK(d.video_bytes == doctest::Approx(28.0 * kMb));
        CHECK(d.video_max_bytes == doctest::Approx(100.0 * kMb));
        CHECK(d.engine_bytes == doctest::Approx(130.0 * kMb));
        CHECK(d.TotalBytes() == doctest::Approx(160.0 * kMb));
        CHECK(d.MaxBytes() == doctest::Approx(231.0 * kMb));
    }
    // An engine that doesn't report it: the old assumption, full rings.
    host.report_held = false;
    screen.FetchPreviewAsync();
    const MemoryCardData& d = screen.MemoryCardDataForTest();
    CHECK(d.video_bytes == doctest::Approx(100.0 * kMb));
    CHECK(d.engine_bytes == doctest::Approx(59.0 * kMb));
}

TEST_CASE("ui: an older preview reply landing last doesn't overwrite After Save") {
    // Replies to FetchPreviewAsync come back on separate worker threads in any order.
    class ReorderHost : public FakeScreenHost {
    public:
        std::vector<std::function<void()>> held;
        void RequestPreviewAsync(const JsonValue& config,
                                 std::function<void(std::optional<JsonValue>, std::optional<JsonValue>)> on_result) override {
            const auto* br = config.Find("encode.bitrate_kbps");
            const double video = (br && br->type == JsonValue::Type::Number ? br->num : 0.0) * 10000.0;
            held.push_back([on_result, video]() {
                JsonValue resp = JsonValue::MakeObject();
                resp.Set("video_ring_total_bytes", JsonValue::MakeNum(video));
                resp.Set("audio_ring_total_bytes", JsonValue::MakeNum(1024.0 * 1024.0));
                JsonValue mem = JsonValue::MakeObject();
                mem.Set("video_pipelines", JsonValue::MakeNum(1));
                mem.Set("audio_sources", JsonValue::MakeNum(1));
                mem.Set("video_capacity_bytes", JsonValue::MakeNum(100.0 * 1024 * 1024));
                mem.Set("video_overshoot_bytes", JsonValue::MakeNum(0));
                mem.Set("audio_capacity_bytes", JsonValue::MakeNum(1024.0 * 1024.0));
                mem.Set("audio_overshoot_bytes", JsonValue::MakeNum(0));
                JsonValue status = JsonValue::MakeObject();
                status.Set("memory", std::move(mem));
                status.Set("rss_private_mb", JsonValue::MakeNum(300));
                if (on_result) on_result(resp, status);
            });
        }
    };
    ReorderHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    host.held.clear();

    screen.ApplySettingChangeForTest("encode.bitrate_kbps", JsonValue::MakeNum(10000));
    screen.ApplySettingChangeForTest("encode.bitrate_kbps", JsonValue::MakeNum(20000));
    REQUIRE(host.held.size() >= 2);
    for (auto it = host.held.rbegin(); it != host.held.rend(); ++it) (*it)();  // newest first

    const MemoryCardData& d = screen.MemoryCardDataForTest();
    REQUIRE(d.has_projection);
    CHECK(d.proj_video_bytes == doctest::Approx(20000.0 * 10000.0));
}

TEST_CASE("ui: DiscardAll reverts a staged edit without ever calling set_config") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAdvanced);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    const int set_config_calls_before = host.pipe.counts["set_config"];
    screen.ApplySettingChangeForTest("capture.capture_cursor", JsonValue::MakeBool(false));
    REQUIRE(screen.HasPendingChangesForTest());

    screen.DiscardAllForTest();

    CHECK(host.pipe.counts["set_config"] == set_config_calls_before);  // never reached the engine
    CHECK_FALSE(screen.HasPendingChangesForTest());
    const auto* v = screen.CurrentConfigValueForTest("capture.capture_cursor");
    REQUIRE(v != nullptr);
    CHECK(v->b);  // reverted to the original value
}

TEST_CASE("ui: SaveAll shows one Realloc confirm modal for a batch, not one per key") {
    // buffer.seconds is Realloc-kind, capture.capture_cursor is Free -- staging both and saving
    // together must confirm exactly once for the whole batch (SettingsScreen::pending_worst_apply_),
    // not once per Realloc key touched.
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    screen.ApplySettingChangeForTest("capture.capture_cursor", JsonValue::MakeBool(false));  // Free
    screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(90.0));             // Realloc
    CHECK(host.modal_count == 0);  // no modal at stage time either

    screen.SaveAllForTest();

    CHECK(host.modal_count == 1);
    const auto* cursor_v = screen.CurrentConfigValueForTest("capture.capture_cursor");
    const auto* buf_v = screen.CurrentConfigValueForTest("buffer.seconds");
    REQUIRE(cursor_v != nullptr);
    REQUIRE(buf_v != nullptr);
    CHECK_FALSE(cursor_v->b);
    CHECK(buf_v->num == 90.0);
}

TEST_CASE("ui: an older set_config reply can't stomp a newer one to the same key") {
    // The out-of-order hazard the async conversion introduced survives the move to
    // staged batches: two SaveAll() calls for an overlapping key can still have their replies land
    // out of order if the first is still in flight when the user stages another edit and saves
    // again. defer_set_config_replies lets this test hold both requests' replies and flush them out
    // of order to actually exercise set_config_seq_by_key_ (screens.h), not just reason about it.
    FakeScreenHost host;
    host.defer_set_config_replies = true;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    // buffer.seconds defaults to 60 (config.h) -- both requests below use values that differ from
    // the default and from each other, so which one (if either) actually landed is unambiguous.
    screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(90.0));
    screen.SaveAllForTest();  // fires request 1 (my_seq=1), reply deferred
    // A newer edit staged (and saved) while request 1 is still outstanding -- pending_changes_ still
    // holds "buffer.seconds" (SaveAll only erases a key from it once its own reply lands ok), so this
    // just overwrites the staged value before the second SaveAll picks it up.
    screen.ApplySettingChangeForTest("buffer.seconds", JsonValue::MakeNum(150.0));
    screen.SaveAllForTest();  // fires request 2 (my_seq=2), reply deferred
    REQUIRE(host.pending_set_config_replies.size() == 2);

    // Flush the OLDER request's reply first (the out-of-order case) -- it must not apply, because a
    // newer request for the same key was already issued before this reply landed.
    host.pending_set_config_replies[0]();
    {
        const auto* v = screen.CurrentConfigValueForTest("buffer.seconds");
        REQUIRE(v != nullptr);
        CHECK(v->num == 60.0);  // still the default -- the stale 90 must not have been applied
    }

    // Now the newer request's reply lands -- it must apply.
    host.pending_set_config_replies[1]();
    {
        const auto* v = screen.CurrentConfigValueForTest("buffer.seconds");
        REQUIRE(v != nullptr);
        CHECK(v->num == 150.0);
    }
}

TEST_CASE("ui: a newer staged audio edit isn't marked saved by an older Save's reply") {
    // Same out-of-order hazard as the set_config test above, but for SaveAudioSources'
    // audio_sources_dirty_ flag specifically (screens.h's audio_sources_generation_ comment): the
    // old synchronous Save meant no further edit could be staged while it was in flight, so clearing
    // the flag on reply always matched "what just got saved is exactly the current staged state."
    FakeScreenHost host;
    host.defer_set_config_replies = true;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveSubTab(1);  // Audio -- populates audio_sources_ui_ via BuildAudioSourceRows
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    screen.StageAudioGainForTest(-6.0);
    CHECK(screen.AudioSourcesDirtyForTest());
    screen.SaveAudioSourcesForTest();  // request queued, not yet resolved (defer_set_config_replies)
    REQUIRE(host.pending_set_config_replies.size() == 1);

    // A second edit lands while the Save above is still in flight.
    screen.StageAudioGainForTest(-9.0);
    CHECK(screen.AudioSourcesDirtyForTest());

    // The first Save's reply now lands -- it must NOT clear the dirty flag, because a newer edit
    // was staged after it was issued and hasn't been saved yet.
    host.pending_set_config_replies[0]();
    CHECK(screen.AudioSourcesDirtyForTest());
}

TEST_CASE("ui: Home's Buffer toggle goes through RequestAsync and rolls back only on a failed reply") {
    struct ArmHost : FakeScreenHost {
        bool accept = false;
        void RequestAsync(JsonValue req, DWORD, std::function<void(std::optional<JsonValue>)> cb) override {
            const JsonValue* t = req.Find("type");
            REQUIRE(t);
            CHECK(t->str == "set_armed");
            JsonValue resp = JsonValue::MakeObject();
            resp.Set("ok", JsonValue::MakeBool(accept));
            if (cb) cb(resp);
        }
    } host;
    HomeScreen screen(&host);
    screen.ClickBufferToggleForTest(false);
    CHECK(screen.BufferToggleCheckedForTest());  // rejected: back to armed

    host.accept = true;
    screen.ClickBufferToggleForTest(false);
    CHECK_FALSE(screen.BufferToggleCheckedForTest());
    host.accept = false;
    screen.ClickBufferToggleForTest(true);
    CHECK_FALSE(screen.BufferToggleCheckedForTest());  // rejected: back to the last accepted state
}

TEST_CASE("ui: Home's Instant Replay tile says Off while switched off, not what it keeps") {
    FakeScreenHost host;
    HomeScreen screen(&host);
    JsonValue cfg = JsonValue::MakeObject();
    cfg.Set("buffer.seconds", JsonValue::MakeNum(60));
    JsonValue config = JsonValue::MakeObject();
    config.Set("config", std::move(cfg));
    JsonValue status = JsonValue::MakeObject();
    status.Set("enabled", JsonValue::MakeBool(true));
    status.Set("armed", JsonValue::MakeBool(true));
    screen.UpdateState(&status, &config);
    CHECK(screen.ReplayDetailForTest() == L"Last 60 s ready");
    status.Set("enabled", JsonValue::MakeBool(false));
    status.Set("armed", JsonValue::MakeBool(false));
    screen.UpdateState(&status, nullptr);
    CHECK(screen.ReplayDetailForTest() == L"Off");
    CHECK_FALSE(screen.BufferToggleCheckedForTest());
}

TEST_CASE("ui: HomeScreen lays its three tiles on one centered row, stats clear of the toggle") {
    FakeScreenHost host;
    HomeScreen screen(&host);
    screen.UpdateState(nullptr, nullptr);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    CHECK(screen.ActionCardCount() == 3);
    const D2D1_RECT_F a = screen.ReplayCardBoundsForTest();
    const D2D1_RECT_F b = screen.OpenRecordingsCardBoundsForTest();
    CHECK(a.top == doctest::Approx(b.top));
    CHECK(a.bottom == doctest::Approx(b.bottom));
    CHECK(a.right - a.left == doctest::Approx(b.right - b.left));
    CHECK(b.left > a.right);
    CHECK(a.left == doctest::Approx(1280.0f - b.right));  // row centered in the 1280 bounds

    // The live stats line was drawn at a hardcoded bounds_.top + 210dip, which landed inside the
    // tile row and ran through the Buffer On/Off toggle. It belongs below the tiles.
    const D2D1_RECT_F stats = screen.StatsBoundsForTest();
    CHECK(stats.top > a.bottom);
    CHECK(stats.top > screen.HintBarBoundsForTest().bottom - 0.5f);
}

TEST_CASE("ui: replay reports an unconfirmed save without offering an automatic retry") {
    struct SaveHost : FakeScreenHost {
        void RequestAsync(JsonValue req, DWORD timeout, std::function<void(std::optional<JsonValue>)> cb) override {
            REQUIRE(req.Find("type"));
            CHECK(req.Find("type")->str == "save_now");
            CHECK(timeout == 30000);
            REQUIRE(cb);
            cb(std::nullopt);
        }
    } host;
    HomeScreen screen(&host);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    const auto bounds = screen.ReplayCardBoundsForTest();
    const auto point = D2D1::Point2F((bounds.left + bounds.right) / 2, bounds.top + 20);
    screen.OnMouseDown(point);
    screen.OnMouseUp(point);
    CHECK(host.modal_count == 1);
    CHECK(host.last_modal_buttons.confirm == L"OK");
    CHECK(host.last_modal_buttons.cancel.empty());
}

TEST_CASE("ui: a one-button modal maps Esc onto its sole (confirm) action") {
    ModalDialog two;
    two.visible = true;
    two.OnKeyDown(VK_ESCAPE);
    CHECK(two.last_result == ModalDialog::Result::Cancel);

    ModalDialog one;
    one.visible = true;
    one.cancel_text.clear();
    one.OnKeyDown(VK_ESCAPE);
    CHECK(one.last_result == ModalDialog::Result::Confirm);

    // No cancel button exists, so a click must never resolve as Cancel even at the zeroed rect.
    ModalDialog one_click;
    one_click.visible = true;
    one_click.cancel_text.clear();
    one_click.OnMouseMove(D2D1::Point2F(0, 0));
    one_click.OnMouseDown(D2D1::Point2F(0, 0));
    CHECK(one_click.last_result != ModalDialog::Result::Cancel);
}

// --- Attached-panel geometry (pure, device-free) -----------------------------------------------

namespace {
Microsoft::WRL::ComPtr<ID2D1Factory> MakeD2DFactory() {
    Microsoft::WRL::ComPtr<ID2D1Factory> f;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
    return f;
}
bool Filled(ID2D1Geometry* g, float x, float y) {
    BOOL hit = FALSE;
    g->FillContainsPoint(D2D1::Point2F(x, y), nullptr, &hit);
    return hit != FALSE;
}
}  // namespace

TEST_CASE("ui: attached-panel corner shapes match Noctalia's table") {
    auto top = AttachedCornerShapes(PanelEdge::Top);
    CHECK(top.tl == CornerShape::Concave);
    CHECK(top.tr == CornerShape::Concave);
    CHECK(top.br == CornerShape::Convex);
    CHECK(top.bl == CornerShape::Convex);
    auto left = AttachedCornerShapes(PanelEdge::Left);
    CHECK(left.tl == CornerShape::Concave);
    CHECK(left.bl == CornerShape::Concave);
    CHECK(left.tr == CornerShape::Convex);
    CHECK(left.br == CornerShape::Convex);
}

TEST_CASE("ui: attached-panel geometry overhangs along the edge and clamps radii") {
    const D2D1_RECT_F c = D2D1::RectF(100, 0, 400, 200);
    auto g = MakeAttachedGeometry(PanelEdge::Top, c, 12.0f, 12.0f);
    CHECK(g.bg.left == doctest::Approx(88.0f));
    CHECK(g.bg.right == doctest::Approx(412.0f));
    CHECK(g.bg.top == doctest::Approx(0.0f));
    CHECK(g.bg.bottom == doctest::Approx(200.0f));

    auto side = MakeAttachedGeometry(PanelEdge::Left, D2D1::RectF(0, 100, 200, 400), 12.0f, 12.0f);
    CHECK(side.bg.top == doctest::Approx(88.0f));
    CHECK(side.bg.bottom == doctest::Approx(412.0f));
    CHECK(side.bg.left == doctest::Approx(0.0f));

    // A radius bigger than half the panel is clamped instead of producing a self-crossing path.
    auto tiny = MakeAttachedGeometry(PanelEdge::Top, D2D1::RectF(0, 0, 20, 20), 50.0f, 50.0f);
    CHECK(tiny.corner_radius == doctest::Approx(10.0f));
    CHECK(tiny.bulge_radius == doctest::Approx(10.0f));
}

TEST_CASE("ui: attached-panel path is concave on the edge side and convex away from it") {
    auto factory = MakeD2DFactory();
    REQUIRE(factory);
    const float b = 12.0f;
    // Each case: the panel content rect, and probe points relative to it.
    struct Case { PanelEdge edge; D2D1_RECT_F c; };
    const Case cases[] = {
        {PanelEdge::Top,    D2D1::RectF(100, 0, 400, 200)},
        {PanelEdge::Bottom, D2D1::RectF(100, 800, 400, 1000)},
        {PanelEdge::Left,   D2D1::RectF(0, 100, 200, 400)},
        {PanelEdge::Right,  D2D1::RectF(800, 100, 1000, 400)},
    };
    for (const auto& k : cases) {
        CAPTURE(static_cast<int>(k.edge));
        auto g = MakeAttachedGeometry(k.edge, k.c, b, b);
        auto path = BuildAttachedPanelPath(factory.Get(), g);
        REQUIRE(path);
        const float cx = (k.c.left + k.c.right) * 0.5f;
        const float cy = (k.c.top + k.c.bottom) * 0.5f;
        CHECK(Filled(path.Get(), cx, cy));  // the body itself

        // Edge-side corner *region* just outside the content: filled by the fillet's foot near the
        // edge, empty deeper in / further out. Probe the fillet whose foot sits at the content's
        // first corner along the edge.
        float near_edge_x, near_edge_y, in_circle_x, in_circle_y;
        switch (k.edge) {
            case PanelEdge::Top:
                near_edge_x = k.c.left - 0.5f;  near_edge_y = k.c.top + 0.5f;
                in_circle_x = k.c.left - b + 1; in_circle_y = k.c.top + b - 1; break;
            case PanelEdge::Bottom:
                near_edge_x = k.c.left - 0.5f;  near_edge_y = k.c.bottom - 0.5f;
                in_circle_x = k.c.left - b + 1; in_circle_y = k.c.bottom - b + 1; break;
            case PanelEdge::Left:
                near_edge_x = k.c.left + 0.5f;  near_edge_y = k.c.top - 0.5f;
                in_circle_x = k.c.left + b - 1; in_circle_y = k.c.top - b + 1; break;
            default:  // Right
                near_edge_x = k.c.right - 0.5f; near_edge_y = k.c.top - 0.5f;
                in_circle_x = k.c.right - b + 1; in_circle_y = k.c.top - b + 1; break;
        }
        CHECK(Filled(path.Get(), near_edge_x, near_edge_y));      // fillet fills the wedge at the edge
        CHECK_FALSE(Filled(path.Get(), in_circle_x, in_circle_y)); // ...but is carved out (concave)

        // Away-side corner is rounded (convex): the extreme corner point is NOT filled.
        float far_x, far_y;
        switch (k.edge) {
            case PanelEdge::Top:    far_x = k.c.left + 0.5f;  far_y = k.c.bottom - 0.5f; break;
            case PanelEdge::Bottom: far_x = k.c.left + 0.5f;  far_y = k.c.top + 0.5f;    break;
            case PanelEdge::Left:   far_x = k.c.right - 0.5f; far_y = k.c.top + 0.5f;    break;
            default:                far_x = k.c.left + 0.5f;  far_y = k.c.top + 0.5f;    break;
        }
        CHECK_FALSE(Filled(path.Get(), far_x, far_y));
    }
}

TEST_CASE("ui: screen frame is a ring with rounded inner corners") {
    auto factory = MakeD2DFactory();
    REQUIRE(factory);
    const D2D1_RECT_F screen = D2D1::RectF(0, 0, 1000, 600);
    // thickness 0: only the corner spandrels are filled.
    auto corners = BuildScreenFramePath(factory.Get(), screen, 0.0f, 24.0f);
    REQUIRE(corners);
    CHECK(Filled(corners.Get(), 1.0f, 1.0f));
    CHECK(Filled(corners.Get(), 999.0f, 599.0f));
    CHECK_FALSE(Filled(corners.Get(), 500.0f, 300.0f));
    CHECK_FALSE(Filled(corners.Get(), 500.0f, 1.0f));  // straight edges stay open
    // thickness 8: the whole border is filled too.
    auto ring = BuildScreenFramePath(factory.Get(), screen, 8.0f, 24.0f);
    REQUIRE(ring);
    CHECK(Filled(ring.Get(), 500.0f, 3.0f));
    CHECK_FALSE(Filled(ring.Get(), 500.0f, 300.0f));
}

// --- FlexWidget layout (headless: no dc, fixed-size stub leaves) ---------------------------------

namespace {
struct StubTile : Widget {
    float h;
    explicit StubTile(float height) : h(height) {}
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext&) override {
        return D2D1::SizeF(c.BoundedWidth() ? c.max_w : 10.0f, h);
    }
    void Draw(ID2D1DeviceContext*, const Theme&, float) override {}
};
}  // namespace

TEST_CASE("ui: flex column stacks full-width with per-item margins") {
    auto col = FlexWidget::MakeColumn();
    auto a = std::make_shared<StubTile>(20.0f);
    auto b = std::make_shared<StubTile>(30.0f);
    col->Add(a).margin_before_dip = 8.0f;
    col->Add(b).margin_after_dip = 4.0f;
    LayoutContext ctx;
    const auto sz = col->Measure(LayoutConstraints::Available(400.0f), ctx);
    col->Arrange(D2D1::RectF(100, 50, 100 + 400, 50 + sz.height), ctx);
    CHECK(a->bounds.top == doctest::Approx(50.0f + 8.0f));
    CHECK(a->bounds.right - a->bounds.left == doctest::Approx(400.0f));
    CHECK(b->bounds.top == doctest::Approx(a->bounds.bottom));
    CHECK(sz.height == doctest::Approx(8.0f + 20.0f + 30.0f + 4.0f));
}

TEST_CASE("ui: flex row wraps on basis, clamps a full-row basis, and fill_line owns a line") {
    auto row = FlexWidget::MakeRow(16.0f, /*wrap=*/true);
    auto t1 = std::make_shared<StubTile>(40.0f);
    auto t2 = std::make_shared<StubTile>(60.0f);
    auto t3 = std::make_shared<StubTile>(10.0f);
    auto huge = std::make_shared<StubTile>(10.0f);
    row->Add(t1).basis_dip = 260.0f;
    row->Add(t2).basis_dip = 220.0f;  // 260 + 16 + 220 = 496 <= 500: same line
    row->Add(t3).basis_dip = 220.0f;  // wraps
    row->Add(huge).basis_dip = 100000.0f;  // the old "force a full row" trick: clamps to the line
    LayoutContext ctx;
    const auto sz = row->Measure(LayoutConstraints::Available(500.0f), ctx);
    row->Arrange(D2D1::RectF(0, 0, 500, sz.height), ctx);
    CHECK(t1->bounds.top == doctest::Approx(t2->bounds.top));
    CHECK(t3->bounds.top > t1->bounds.top);
    CHECK(huge->bounds.right - huge->bounds.left == doctest::Approx(500.0f));
    CHECK(huge->bounds.top > t3->bounds.top);
    // Line height is the tallest member (stretch), lines are separated by the gap.
    CHECK(t3->bounds.top == doctest::Approx(60.0f + 16.0f));
}

TEST_CASE("ui: flex equal columns share the width and wrap every N") {
    auto grid = FlexWidget::MakeRow(20.0f, /*wrap=*/true);
    grid->columns = 2;
    auto a = std::make_shared<StubTile>(50.0f);
    auto b = std::make_shared<StubTile>(50.0f);
    auto c = std::make_shared<StubTile>(50.0f);
    grid->Add(a);
    grid->Add(b);
    grid->Add(c);
    LayoutContext ctx;
    const auto sz = grid->Measure(LayoutConstraints::Available(420.0f), ctx);
    grid->Arrange(D2D1::RectF(0, 0, 420, sz.height), ctx);
    CHECK(a->bounds.right - a->bounds.left == doctest::Approx(200.0f));
    CHECK(b->bounds.left == doctest::Approx(220.0f));
    CHECK(c->bounds.left == doctest::Approx(0.0f));
    CHECK(c->bounds.top == doctest::Approx(70.0f));
}

TEST_CASE("ui: flex scales DIP inputs by dpi and skips invisible children") {
    auto col = FlexWidget::MakeColumn(10.0f);
    auto a = std::make_shared<StubTile>(20.0f);
    auto hidden = std::make_shared<StubTile>(99.0f);
    hidden->visible = false;
    auto b = std::make_shared<StubTile>(20.0f);
    col->Add(a);
    col->Add(hidden);
    col->Add(b);
    LayoutContext ctx;
    ctx.dpi_scale = 2.0f;
    const auto sz = col->Measure(LayoutConstraints::Available(300.0f), ctx);
    col->Arrange(D2D1::RectF(0, 0, 300, sz.height), ctx);
    CHECK(b->bounds.top == doctest::Approx(20.0f + 10.0f * 2.0f));
}

TEST_CASE("ui: light theme swaps the palette, flips mono to black, and round-trips") {
    Theme t;
    const D2D1_COLOR_F dark_bg = t.color.panel;
    const D2D1_COLOR_F dark_text = t.color.text;
    const float dark_scrim = t.color.scrim.a;
    t.ApplyMode(ThemeMode::Light);
    CHECK(t.color.panel.r > 0.9f);
    CHECK(t.color.text.r < 0.2f);
    CHECK(t.color.accent.r < 0.2f);     // mono accent is near-black in light
    CHECK(t.color.on_accent.r > 0.9f);  // ...so text on it is light
    CHECK(t.color.scrim.a < dark_scrim);
    t.ApplyAccent(AccentId::Blue);
    CHECK(t.color.accent.b > t.color.accent.r);
    t.ApplyMode(ThemeMode::Dark);   // re-applies the current accent for the new mode
    CHECK(t.color.panel.r == doctest::Approx(dark_bg.r));
    CHECK(t.color.text.r == doctest::Approx(dark_text.r));
    CHECK(t.accent_id == AccentId::Blue);

    Theme mono;
    mono.ApplyMode(ThemeMode::Light);
    mono.ApplyAccent(AccentId::Mono);
    CHECK(mono.color.accent.r < 0.2f);
    CHECK(mono.PickedRgb() == 0xe6e8eeu);  // the brand mark keeps the tray's pale ring
    mono.ApplyMode(ThemeMode::Dark);
    CHECK(mono.color.accent.r > 0.8f);    // and back to the pale mono

    CHECK(ThemeModeFromKey("light") == ThemeMode::Light);
    CHECK(ThemeModeFromKey("dark") == ThemeMode::Dark);
    SetSystemThemeLightProbe([] { return true; });
    CHECK(ThemeModeFromKey("system") == ThemeMode::Light);
    SetSystemThemeLightProbe([] { return false; });
    CHECK(ThemeModeFromKey("system") == ThemeMode::Dark);
    SetSystemThemeLightProbe(nullptr);
    CHECK(ScreenFrameColorFromKey("white") == ScreenFrameColor::Contrast);   // pre-light-theme value
    CHECK(ScreenFrameColorFromKey("dark") == ScreenFrameColor::Match);
}

// --- FlexWidget click dispatch (headless: bounds come from Arrange, not Draw) ---------------------

namespace {
struct ClickTile : Widget {
    float h;
    int clicks = 0;
    explicit ClickTile(float height) : h(height) {}
    D2D1_SIZE_F Measure(const LayoutConstraints& c, const LayoutContext&) override {
        return D2D1::SizeF(c.BoundedWidth() ? c.max_w : 10.0f, h);
    }
    void Draw(ID2D1DeviceContext*, const Theme&, float) override {}
    bool OnMouseDown(D2D1_POINT_2F pt) override {
        if (!HitTest(pt)) return false;
        ++clicks;
        return true;
    }
};
D2D1_POINT_2F Center(const D2D1_RECT_F& r) { return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f); }
}  // namespace

TEST_CASE("ui: flex routes a click to exactly the tile under it, in a wrapped grid and at 2x dpi") {
    for (const float dpi : {1.0f, 2.0f}) {
        auto grid = FlexWidget::MakeRow(20.0f, /*wrap=*/true);
        grid->columns = 2;
        std::vector<std::shared_ptr<ClickTile>> tiles;
        for (int i = 0; i < 4; ++i) {
            tiles.push_back(std::make_shared<ClickTile>(50.0f));
            grid->Add(tiles.back());
        }
        LayoutContext ctx;
        ctx.dpi_scale = dpi;
        const float w = 420.0f * dpi;
        const auto sz = grid->Measure(LayoutConstraints::Available(w), ctx);
        grid->Arrange(D2D1::RectF(0, 0, w, sz.height), ctx);

        for (size_t target = 0; target < tiles.size(); ++target) {
            for (auto& t : tiles) t->clicks = 0;
            CHECK(grid->OnMouseDown(Center(tiles[target]->bounds)));
            for (size_t i = 0; i < tiles.size(); ++i) CHECK(tiles[i]->clicks == (i == target ? 1 : 0));
        }
        // The gap between two columns belongs to no tile.
        for (auto& t : tiles) t->clicks = 0;
        const float gap_x = (tiles[0]->bounds.right + tiles[1]->bounds.left) * 0.5f;
        CHECK_FALSE(grid->OnMouseDown(D2D1::Point2F(gap_x, tiles[0]->bounds.top + 5.0f * dpi)));
        for (auto& t : tiles) CHECK(t->clicks == 0);
    }
}

TEST_CASE("ui: flex click reaches a tile in a nested column, and skips an invisible one") {
    auto outer = FlexWidget::MakeColumn(8.0f);
    auto inner = FlexWidget::MakeRow(10.0f);
    auto a = std::make_shared<ClickTile>(30.0f);
    auto hidden = std::make_shared<ClickTile>(30.0f);
    hidden->visible = false;
    auto b = std::make_shared<ClickTile>(30.0f);
    inner->Add(a).basis_dip = 100.0f;
    inner->Add(b).basis_dip = 100.0f;
    outer->Add(hidden);
    outer->Add(inner);
    LayoutContext ctx;
    const auto sz = outer->Measure(LayoutConstraints::Available(300.0f), ctx);
    outer->Arrange(D2D1::RectF(0, 0, 300, sz.height), ctx);
    CHECK(outer->OnMouseDown(Center(b->bounds)));
    CHECK(b->clicks == 1);
    CHECK(a->clicks == 0);
    CHECK(hidden->clicks == 0);
}

TEST_CASE("ui: a row whose controls move under its text keeps its switch at the right edge, on the title's line") {
    auto row = std::make_shared<SettingRow>();
    row->title = L"Desktop";
    row->grip = true;
    auto a = std::make_shared<SelectControl>();
    a->width_dip = 170.0f;
    auto b = std::make_shared<SelectControl>();
    b->width_dip = 210.0f;
    auto sw = std::make_shared<SwitchControl>();
    row->controls = {a, b, sw};
    LayoutContext ctx;
    const float w = 460.0f;
    const auto sz = row->Measure(LayoutConstraints::Available(w), ctx);
    row->Arrange(D2D1::RectF(0, 0, w, sz.height), ctx);
    CHECK(sw->bounds.right == doctest::Approx(w - Sizes{}.card_padding()));
    CHECK(sw->bounds.bottom <= a->bounds.top);
    CHECK(a->bounds.right <= b->bounds.left);
    CHECK(sz.height >= a->bounds.bottom);
}


// --- Draw-then-click (WARP software device: no GPU, nothing on screen) -----------------------------
// Widgets like ModalDialog only learn their hit rects inside Draw(), so a
// click test that never draws passes vacuously against zeroed rects. This renders once into an
// offscreen bitmap on the WARP rasterizer, then clicks.

namespace {
struct HeadlessDraw {
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
    Theme theme;
    bool ok = false;
    UINT width = 1000, height = 700;

    explicit HeadlessDraw(UINT w = 1000, UINT h = 700) : width(w), height(h) {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11Device> d3d;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr))) return;
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(d3d.As(&dxgi))) return;
        ComPtr<ID2D1Factory1> factory;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) return;
        ComPtr<ID2D1Device> dev;
        if (FAILED(factory->CreateDevice(dxgi.Get(), &dev))) return;
        if (FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, props, &target))) return;
        dc->SetTarget(target.Get());
        ComPtr<IDWriteFactory> dw;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) return;
        ok = theme.InitFonts(dw.Get());
    }
    // Anything with Draw(dc, theme, dpi): a widget, or a whole SettingsScreen.
    template <typename T>
    void Draw(T& w) {
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTextRenderingParams(theme.TextRendering());
        w.Draw(dc.Get(), theme, 1.0f);
        REQUIRE(SUCCEEDED(dc->EndDraw()));
    }
    // One drawn pixel, as straight (non-premultiplied) 0..1 RGB.
    D2D1_COLOR_F Pixel(UINT x, UINT y) {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        REQUIRE(SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, props, &cpu)));
        REQUIRE(SUCCEEDED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr)));
        D2D1_MAPPED_RECT m{};
        REQUIRE(SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m)));
        const BYTE* p = m.bits + y * m.pitch + x * 4;
        const float a = p[3] / 255.0f;
        D2D1_COLOR_F c = a > 0 ? D2D1::ColorF(p[2] / 255.0f / a, p[1] / 255.0f / a, p[0] / 255.0f / a, a)
                               : D2D1::ColorF(0, 0, 0, 0);
        cpu->Unmap();
        return c;
    }
};

bool Near(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return std::abs(a.r - b.r) < 0.02f && std::abs(a.g - b.g) < 0.02f && std::abs(a.b - b.b) < 0.02f;
}
}  // namespace

// ui.font: Inter and Geist come from the exe's resources with real
// Regular and SemiBold faces (no synthesized bold), and text in a script they lack, such as a game
// or window name in CJK, is drawn from a system font instead of empty boxes.
namespace {
struct GlyphProbe final : IDWriteTextRenderer {
    int runs = 0, missing = 0;  // missing: glyph index 0 (.notdef, the empty box)
    std::set<std::wstring> faces;
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ++runs;
        for (UINT32 i = 0; i < run->glyphCount; ++i) missing += run->glyphIndices[i] == 0;
        Microsoft::WRL::ComPtr<IDWriteFontFace3> f3;
        if (SUCCEEDED(run->fontFace->QueryInterface(IID_PPV_ARGS(&f3)))) {
            Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
            wchar_t buf[128] = {};
            if (SUCCEEDED(f3->GetFamilyNames(&names)) && names) names->GetString(0, buf, 128);
            faces.insert(buf);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* d) override { *d = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = {1, 0, 0, 1, 0, 0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override { *p = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** o) override { *o = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};
}  // namespace

TEST_CASE("ui: the embedded fonts load with real weights, and scripts they lack fall back") {
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    REQUIRE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dw.GetAddressOf()))));
    for (const auto& [font, family] : {std::pair{UiFont::Inter, L"Inter"}, std::pair{UiFont::Geist, L"Geist"}}) {
        CAPTURE(std::string(family, family + wcslen(family)));
        IDWriteFontCollection* col = EmbeddedFontCollection(dw.Get(), font);
        REQUIRE(col);
        UINT32 index = 0;
        BOOL found = FALSE;
        REQUIRE(SUCCEEDED(col->FindFamilyName(family, &index, &found)));
        REQUIRE(found);
        Microsoft::WRL::ComPtr<IDWriteFontFamily> fam;
        REQUIRE(SUCCEEDED(col->GetFontFamily(index, &fam)));
        for (DWRITE_FONT_WEIGHT w : {DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_WEIGHT_SEMI_BOLD}) {
            Microsoft::WRL::ComPtr<IDWriteFont> f;
            REQUIRE(SUCCEEDED(fam->GetFirstMatchingFont(w, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &f)));
            CHECK(f->GetWeight() == w);
            CHECK(f->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE);
        }
    }
    CHECK(EmbeddedFontCollection(dw.Get(), UiFont::System) == nullptr);

    Theme theme;
    REQUIRE(theme.InitFonts(dw.Get()));
    CHECK(theme.ui_font == UiFont::Inter);  // the default
    wchar_t name[64] = {};
    theme.Font(TypeRole::Body)->GetFontFamilyName(name, 64);
    CHECK(std::wstring(name) == L"Inter");
    CHECK(theme.TextRendering() != nullptr);  // grid-fit off for an unhinted face

    const std::wstring text = L"Valorant 原神 アペックス 한국어";
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    REQUIRE(SUCCEEDED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), theme.Font(TypeRole::Body),
                                           1000, 100, &layout)));
    GlyphProbe probe;
    REQUIRE(SUCCEEDED(layout->Draw(nullptr, &probe, 0, 0)));
    CHECK(probe.missing == 0);
    CHECK(probe.faces.count(L"Inter") == 1);
    CHECK(probe.faces.size() >= 2);  // the CJK came from somewhere else

    theme.SetUiFont(UiFont::System);
    theme.Font(TypeRole::Body)->GetFontFamilyName(name, 64);
    CHECK(std::wstring(name) == L"Segoe UI Variable Text");
    CHECK(theme.TextRendering() == nullptr);  // hinted: DirectWrite's default
    theme.WordmarkFont()->GetFontFamilyName(name, 64);
    CHECK(std::wstring(name) == L"Inter");  // the brand doesn't follow ui.font
}

// The vector mark (README, site, installer) is the raster mark: D2D's SVG renderer draws it within a
// few levels of RenderBrandMark, and the committed docs/img/ombra-mark.svg is BrandMarkSvg's output.
TEST_CASE("ui: the brand mark's SVG matches the raster mark, and the committed file is current") {
    const std::string svg = ombra::BrandMarkSvg(ombra::kBrandMarkMonoRgb);

    std::string committed;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(OMBRA_TEST_FIXTURE_DIR) + L"/../../docs/img/ombra-mark.svg").c_str(), L"rb") == 0 && f) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) committed.append(buf, n);
        std::fclose(f);
    }
    CHECK_MESSAGE(committed == svg, "regenerate it: ombra-icongen --svg docs/img/ombra-mark.svg");

    constexpr UINT kSize = 256;
    HeadlessDraw h(kSize, kSize);
    REQUIRE(h.ok);
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> dc5;
    REQUIRE(SUCCEEDED(h.dc.As(&dc5)));
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(svg.data()), static_cast<UINT>(svg.size())));
    REQUIRE(stream);
    Microsoft::WRL::ComPtr<ID2D1SvgDocument> doc;
    REQUIRE(SUCCEEDED(dc5->CreateSvgDocument(stream.Get(), D2D1::SizeF(kSize, kSize), &doc)));
    h.dc->BeginDraw();
    h.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc5->DrawSvgDocument(doc.Get());
    REQUIRE(SUCCEEDED(h.dc->EndDraw()));

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    REQUIRE(SUCCEEDED(h.dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, props, &cpu)));
    REQUIRE(SUCCEEDED(cpu->CopyFromBitmap(nullptr, h.target.Get(), nullptr)));
    D2D1_MAPPED_RECT m{};
    REQUIRE(SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m)));
    constexpr uint32_t rgb = ombra::kBrandMarkMonoRgb;
    const auto raster = ombra::RenderBrandMark(kSize, ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                                               (rgb & 255) / 255.0f);
    // The disc's own edge is excluded: D2D and the raster anti-alias it differently (up to ~54
    // levels on its outermost pixels). Everything inside, the glow and the core included, must match.
    int max_inner = 0;
    UINT max_x = 0, max_y = 0;
    double sum = 0;
    for (UINT y = 0; y < kSize; ++y)
        for (UINT x = 0; x < kSize; ++x) {
            const float dx = x + 0.5f - kSize / 2.0f, dy = y + 0.5f - kSize / 2.0f;
            const bool rim = std::sqrt(dx * dx + dy * dy) > kSize / 2.0f - 2.0f;
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(m.bits[y * m.pitch + x * 4 + c]) - int(raster[(y * kSize + x) * 4 + c]));
                sum += d;
                if (!rim && d > max_inner) max_inner = d, max_x = x, max_y = y;
            }
        }
    cpu->Unmap();
    const double mean = sum / (kSize * kSize * 4.0);
    MESSAGE("SVG vs raster at 256 px: inside the rim max " << max_inner << " at " << max_x << "," << max_y
                                                           << "; mean over all " << mean);
    CHECK(mean < 1.0);
    CHECK(max_inner <= 24);  // the core's anti-aliased edges; a wrong radius or width moves whole bands
}

// The shadow is blurred once per open, never per frame.
TEST_CASE("ui: a menu blurs its shadow once per open, and it shows under the menu") {
    HeadlessDraw h;
    REQUIRE(h.ok);
    MenuPopup menu;
    menu.options = {L"Native", L"1440p", L"1080p", L"720p"};
    menu.selected = 2;
    menu.Place(D2D1::RectF(100, 100, 300, 128), D2D1::RectF(0, 0, 1000, 700), h.theme, 1.0f);
    const int before = MenuPopup::ShadowRendersForTest();
    h.Draw(menu);
    menu.hovered_index = 1;  // a hover's repaint
    h.Draw(menu);
    CHECK(MenuPopup::ShadowRendersForTest() == before + 1);
    // Below the menu's bottom edge, the cleared (transparent) target now has the shadow's alpha.
    const UINT x = static_cast<UINT>((menu.bounds.left + menu.bounds.right) / 2);
    CHECK(h.Pixel(x, static_cast<UINT>(menu.bounds.bottom) + 4).a > 0.02f);
    CHECK(h.Pixel(x, static_cast<UINT>(menu.bounds.bottom) + 60).a < 0.01f);

    menu.Place(D2D1::RectF(100, 100, 300, 128), D2D1::RectF(0, 0, 1000, 700), h.theme, 1.0f);  // opened again
    h.Draw(menu);
    CHECK(MenuPopup::ShadowRendersForTest() == before + 2);
}


// Moving the rail has to actually re-lay-out the page. Layout() short-circuits RebuildGroupRows
// unless the scroll area's bounds or the dpi changed, and ui.rail_position isn't either of those
// directly -- it works only because every one of the four positions shifts the content card, and
// therefore the scroll area, to a different rect. Pin that, because a position that left the card
// where it was would silently no-op until something unrelated dirtied the page.
TEST_CASE("ui: each rail position puts the page somewhere different") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);

    std::vector<D2D1_RECT_F> cards;
    size_t rebuilds_before = 0;
    for (const RailPosition pos : {RailPosition::Left, RailPosition::Right,
                                   RailPosition::Top, RailPosition::Bottom}) {
        host.GetTheme().rail_position = pos;
        rebuilds_before = screen.RebuildCountForTest();
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
        CHECK(screen.RebuildCountForTest() > rebuilds_before);  // not short-circuited
        cards.push_back(screen.ViewBoundsForTest());
    }
    REQUIRE(cards.size() == 4);
    for (size_t i = 0; i < cards.size(); ++i) {
        for (size_t j = i + 1; j < cards.size(); ++j) {
            const bool differs = cards[i].left != cards[j].left || cards[i].top != cards[j].top ||
                                 cards[i].right != cards[j].right || cards[i].bottom != cards[j].bottom;
            CHECK(differs);
        }
    }
    // Left/right reserve a column, so the card is narrower; top/bottom reserve a strip, so it is shorter.
    CHECK(cards[0].right - cards[0].left == doctest::Approx(cards[1].right - cards[1].left));
    CHECK(cards[2].bottom - cards[2].top == doctest::Approx(cards[3].bottom - cards[3].top));
    CHECK(cards[2].right - cards[2].left > cards[0].right - cards[0].left);
}

// The hero card appends the spec's unit to its segment labels for readability, but the committed
// value still comes from FieldSpec::presets by index. Pin that they haven't been conflated -- the
// day someone reads the label back instead, buffer.seconds starts being written as "60s".
namespace {
// A card on the page by title, searching nested flex rows/columns.
std::shared_ptr<CardContainer> FindContainerIn(const std::shared_ptr<Widget>& w, const wchar_t* title) {
    if (auto c = std::dynamic_pointer_cast<CardContainer>(w); c && c->title == title) return c;
    if (auto f = std::dynamic_pointer_cast<FlexWidget>(w)) {
        for (const auto& it : f->items) {
            if (auto c = FindContainerIn(it.widget, title)) return c;
        }
    }
    return nullptr;
}
std::shared_ptr<CardContainer> PageCard(const SettingsScreen& screen, const wchar_t* title) {
    for (const auto& w : screen.PageChildrenForTest()) {
        if (auto c = FindContainerIn(w, title)) return c;
    }
    return nullptr;
}
enum AppearanceTab { kLook = 0, kLayout = 1, kNotifications = 2 };
}  // namespace

// Appearance, three sub-tabs: a setting that only applies under another
// one shows only while it applies.
TEST_CASE("ui: Appearance shows a setting's dependents only while they apply") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("ui.screen_frame", JsonValue::MakeBool(false));
    config.Set("ui.panel_position", JsonValue::MakeStr("center"));
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.ControlForTest("ui.theme"));
    CHECK(screen.ControlForTest("ui.accent"));

    screen.SetActiveSubTab(kLayout);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(PageCard(screen, L"Panel"));
    CHECK(screen.ControlForTest("ui.screen_frame"));         // the frame card's header switch
    CHECK_FALSE(screen.ControlForTest("ui.screen_frame_radius"));
    CHECK_FALSE(screen.ControlForTest("ui.panel_attach"));   // attaching needs an edge
    screen.ApplySettingChangeForTest("ui.screen_frame", JsonValue::MakeBool(true));
    screen.ApplySettingChangeForTest("ui.panel_position", JsonValue::MakeStr("top"));
    for (const char* key : {"ui.screen_frame_radius", "ui.screen_frame_thickness", "ui.screen_frame_color", "ui.panel_attach"}) {
        CAPTURE(key);
        CHECK(screen.ControlForTest(key));
    }

    screen.SetActiveSubTab(kNotifications);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.ControlForTest("ui.toast_on_save"));
    CHECK(screen.ControlForTest("ui.toast_position"));       // its own corner picture
    CHECK(screen.ControlForTest("ui.toast_style"));
    screen.ApplySettingChangeForTest("ui.toast_enabled", JsonValue::MakeBool(false));
    CHECK(screen.ControlForTest("ui.toast_enabled"));
    CHECK_FALSE(screen.ControlForTest("ui.toast_on_save"));
    CHECK_FALSE(screen.ControlForTest("ui.toast_position"));
    CHECK_FALSE(screen.ControlForTest("ui.toast_style"));
}

// No scroll: each tab fits the narrowest Settings row with every dependent shown and an
// edit staged (Save bar up), at 100% and 150%.
TEST_CASE("ui: every Appearance tab fits without scrolling with everything expanded") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("ui.screen_frame", JsonValue::MakeBool(true));
    config.Set("ui.panel_position", JsonValue::MakeStr("top"));
    config.Set("ui.toast_enabled", JsonValue::MakeBool(true));
    config.Set("ui.accent", JsonValue::MakeStr("#e5484d"));  // near red: the picker's extra note line
    host.theme.ApplyAccentKey("#e5484d");
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    screen.ApplySettingChangeForTest("ui.card_borders", JsonValue::MakeBool(false));
    for (int tab : {kLook, kLayout, kNotifications}) {
        screen.SetActiveSubTab(tab);
        for (const float s : {1.0f, 1.5f}) {
            CAPTURE(tab);
            CAPTURE(s);
            screen.Layout(D2D1::RectF(0, 0, 780.0f * s, 560.0f * s), s);
            CHECK(screen.MaxScrollForTest() <= 0.0f);
        }
    }
    // Look keeps room for Density, not implemented yet: with it switched on, the tab still fits.
    auto with_density = specs;
    for (auto& spec : with_density)
        if (spec.key == "ui.density") spec.implemented = true;
    SettingsScreen dense(&host);
    dense.InitSchema(with_density, &config);
    dense.SetActiveRail(kRailAppearance);
    dense.ApplySettingChangeForTest("ui.card_borders", JsonValue::MakeBool(false));
    for (const float s : {1.0f, 1.5f}) {
        CAPTURE(s);
        dense.Layout(D2D1::RectF(0, 0, 780.0f * s, 560.0f * s), s);
        REQUIRE(dense.ControlForTest("ui.density"));
        CHECK(dense.MaxScrollForTest() <= 0.0f);
    }
    // Look's widest Theme row: your own theme chosen (four chips, then All themes), the near-red
    // note under the accent and the Save bar up.
    SettingsScreen own(&host);
    auto own_config = config;
    ombra::UserTheme mine{"user1", "Somebody's very long theme name", ombra::kUiPalettes[0].colors};
    own_config.Set("ui.themes", ombra::SerializeUserThemes({mine}));
    own_config.Set("ui.theme", JsonValue::MakeStr("user1"));
    own.InitSchema(specs, &own_config);
    own.SetActiveRail(kRailAppearance);
    own.ApplySettingChangeForTest("ui.card_borders", JsonValue::MakeBool(false));
    for (const float s : {1.0f, 1.5f}) {
        CAPTURE(s);
        own.Layout(D2D1::RectF(0, 0, 780.0f * s, 560.0f * s), s);
        auto chips = std::dynamic_pointer_cast<ThemePickerControl>(own.ControlForTest("ui.theme"));
        REQUIRE(chips);
        CHECK(chips->compact);
        CHECK(chips->tiles.size() == 4);
        CHECK(own.MaxScrollForTest() <= 0.0f);
    }
}

// Your themes: New copies the theme in use and chooses it, a rename keeps it chosen, deleting the
// chosen one goes back to a preset, and presets have nothing to rename or delete. All staged.
TEST_CASE("ui: your themes are made, renamed and deleted as staged edits") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("ui.theme", JsonValue::MakeStr("nord"));
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    screen.SetActiveSubTab(kLook);
    screen.OpenThemeGalleryForTest(true);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK_FALSE(screen.ControlForTest("ui.themes"));  // a preset: no colours to change

    screen.NewUserThemeForTest();
    auto mine = screen.StagedUserThemesForTest();
    REQUIRE(mine.size() == 1);
    CHECK(mine[0].id == "user1");
    CHECK(mine[0].name == "My theme 1");
    CHECK(mine[0].colors.cards == ombra::FindUiPalette("nord")->colors.cards);
    CHECK(screen.StagedThemeKeyForTest() == "user1");
    CHECK(host.theme.palette_key() == "user1");  // previewed while staged
    CHECK(screen.HasPendingChangesForTest());
    CHECK(screen.ThemeGalleryOpenForTest());  // the rebuild stays on the theme page
    CHECK(screen.ControlForTest("ui.themes"));

    screen.NewUserThemeForTest();
    CHECK(screen.StagedUserThemesForTest().size() == 2);
    CHECK(screen.StagedUserThemesForTest()[1].name == "My theme 2");
    CHECK(screen.StagedThemeKeyForTest() == "user2");

    screen.RenameUserThemeForTest("user2", "Midnight");
    CHECK(screen.StagedUserThemesForTest()[1].name == "Midnight");
    CHECK(screen.StagedThemeKeyForTest() == "user2");
    screen.RenameUserThemeForTest("user2", "My theme 1");  // taken: refused
    screen.RenameUserThemeForTest("user2", "Nord");        // a preset's: refused
    CHECK(screen.StagedUserThemesForTest()[1].name == "Midnight");
    screen.RenameUserThemeForTest("nord", "Mine");  // presets aren't renamed
    CHECK(screen.StagedUserThemesForTest().size() == 2);

    screen.DeleteUserThemeForTest("user1");  // not the chosen one: the choice stays
    CHECK(screen.StagedThemeKeyForTest() == "user2");
    screen.DeleteUserThemeForTest("user2");  // the chosen one: back to the preset of its kind
    CHECK(screen.StagedUserThemesForTest().empty());
    CHECK(screen.StagedThemeKeyForTest() == "dark");
    CHECK(host.theme.palette_key() == "dark");
    screen.DeleteUserThemeForTest("nord");  // presets aren't deleted
    CHECK(screen.StagedThemeKeyForTest() == "dark");

    // Leaving Appearance closes the theme page; Discard puts everything back.
    screen.SetActiveRail(kRailCapture);
    screen.SetActiveRail(kRailAppearance);
    screen.DiscardAllForTest();
    CHECK(screen.StagedThemeKeyForTest() == "nord");
    CHECK(screen.StagedUserThemesForTest().empty());
    CHECK(host.theme.palette_key() == "nord");
}

// The old Appearance page showed any key it didn't place as a plain row; the tabs place each one by
// hand, so a key added to these groups has to be added to a tab too, or this fails.
TEST_CASE("ui: every Appearance and Notifications setting has a control on one of the tabs") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("ui.screen_frame", JsonValue::MakeBool(true));
    config.Set("ui.panel_position", JsonValue::MakeStr("top"));
    config.Set("ui.toast_enabled", JsonValue::MakeBool(true));
    config.Set("ui.themes", ombra::SerializeUserThemes({{"user1", "Mine", ombra::kUiPalettes[0].colors}}));
    config.Set("ui.theme", JsonValue::MakeStr("user1"));  // its six colours show on the theme page
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    std::set<std::string> found;
    for (int tab : {static_cast<int>(kLook), static_cast<int>(kLayout), static_cast<int>(kNotifications), -1}) {
        if (tab < 0) {
            screen.SetActiveSubTab(kLook);
            screen.OpenThemeGalleryForTest(true);
        } else {
            screen.SetActiveSubTab(tab);
        }
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
        for (const auto& spec : specs)
            if (screen.ControlForTest(spec.key)) found.insert(spec.key);
    }
    for (const auto& spec : specs) {
        if (!spec.implemented || (spec.group != "Appearance" && spec.group != "Notifications")) continue;
        CAPTURE(spec.key);
        CHECK(found.count(spec.key));
    }
}

// The disk buffer says where the file goes and whether it fits; an old file there counts as free,
// since the next start replaces it.
TEST_CASE("ui: the disk buffer location says whether the file fits") {
    DiskLocation loc;
    loc.drive = L"C:";
    loc.is_default = true;
    CHECK(loc.Caption() == L"Default folder · Free space unknown on C:");
    loc.free_bytes = 100.0 * 1024 * 1024 * 1024;
    CHECK(loc.Caption() == L"Default folder · 100.0 GB free on C:");  // no engine: size unknown
    loc.is_default = false;
    loc.file_bytes = 312.0 * 1024 * 1024;
    CHECK(loc.Fits());
    CHECK(loc.Caption() == L"Buffer file 312 MB · 100.0 GB free on C:");
    loc.free_bytes = 200.0 * 1024 * 1024;
    CHECK_FALSE(loc.Fits());
    CHECK(loc.Caption() == L"Needs 312 MB, only 200 MB free on C:");
    loc.reusable_bytes = 312.0 * 1024 * 1024;  // last start's ring.dat
    CHECK(loc.Fits());
}

// The hand cursor comes from the same routing clicks take: a hand over a control, an arrow over a
// card's title or the empty page, and the scroll offset applied on the way down.
TEST_CASE("ui: the cursor is a hand over controls and an arrow elsewhere") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    screen.SetActiveSubTab(2);  // Notifications
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto card = PageCard(screen, L"Notifications");
    auto sw = screen.ControlForTest("ui.toast_enabled");
    REQUIRE(card);
    REQUIRE(sw);
    const D2D1_RECT_F hc = sw->bounds;
    auto mid = [](const D2D1_RECT_F& r) { return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f); };
    CHECK(screen.CursorAt(mid(hc)) == CursorKind::Hand);                                   // the on/off switch
    CHECK(screen.CursorAt(D2D1::Point2F(card->bounds.left + 20, card->bounds.top + 20)) == CursorKind::Arrow);  // card title
    CHECK(screen.CursorAt(D2D1::Point2F(5, 790)) == CursorKind::Arrow);

    // Scrolled: the same control is now higher on screen by the scroll amount. A short viewport,
    // since Appearance doesn't scroll at full height.
    screen.Layout(D2D1::RectF(0, 0, 1280, 300), 1.0f);
    const float sy = 40.0f;
    REQUIRE(screen.MaxScrollForTest() >= sy);
    sw = screen.ControlForTest("ui.toast_on_buffer_toggle");  // lower down: still in view once scrolled
    REQUIRE(sw);
    const D2D1_RECT_F hs = sw->bounds;
    REQUIRE(hs.top - sy > 0.0f);
    screen.ScrollForTest(sy);
    CHECK(screen.CursorAt(D2D1::Point2F(mid(hs).x, mid(hs).y - sy)) == CursorKind::Hand);
}

TEST_CASE("ui: the panel position picker maps edges and middle to their options") {
    PanelPositionControl picker;
    picker.options = {L"center", L"top", L"bottom", L"left", L"right"};
    picker.bounds = D2D1::RectF(0, 0, 160, 90);  // exactly 16:9
    auto at = [&](float x, float y) { return picker.ZoneAt(D2D1::Point2F(x, y)); };
    CHECK(at(80, 45) == 0);
    CHECK(at(80, 5) == 1);
    CHECK(at(80, 85) == 2);
    CHECK(at(5, 45) == 3);
    CHECK(at(155, 45) == 4);
    CHECK(at(200, 45) == -1);
    int picked = -1;
    picker.on_change = [&](int i) { picked = i; };
    CHECK(picker.OnMouseDown(D2D1::Point2F(80, 85)));
    CHECK(picked == 2);
    CHECK(picker.selected == 2);
}

TEST_CASE("ui: Replay length names its lengths without changing the committed value") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto replay = ContainerTitled(screen, L"Replay");
    REQUIRE(replay);
    CHECK(RowTitles(*replay) == std::vector<std::wstring>{ L"Replay length", L"Keep it in" });
    auto seg = std::dynamic_pointer_cast<SegmentedControl>(screen.ControlForTest("buffer.seconds"));
    REQUIRE(seg);
    CHECK(seg->options == std::vector<std::wstring>{ L"30 s", L"1 min", L"2 min", L"5 min" });

    // Pick "2 min"; the staged value must be the number of seconds, not the label.
    seg->on_change(2);
    const ombra::JsonValue* staged = screen.StagedValueForTest("buffer.seconds");
    REQUIRE(staged);
    CHECK(staged->type == ombra::JsonValue::Type::Number);
    CHECK(staged->num == doctest::Approx(120.0));
}

// A value outside the presets used to leave the control on index 0: a 90 s buffer showed "30s" as
// selected, a 165 fps capture showed "30".
TEST_CASE("ui: a choice shows a current value that isn't one of its presets") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("buffer.seconds", JsonValue::MakeNum(90.0));
    config.Set("capture.framerate", JsonValue::MakeNum(165.0));
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto seg = std::dynamic_pointer_cast<SegmentedControl>(screen.ControlForTest("buffer.seconds"));
    REQUIRE(seg);
    REQUIRE(seg->options.size() == 5);
    CHECK(seg->options[4] == L"90 s");
    CHECK(seg->selected == 4);
    auto fps = std::dynamic_pointer_cast<SelectControl>(screen.ControlForTest("capture.framerate"));
    REQUIRE(fps);
    CHECK(fps->options.back() == L"165 fps");
    CHECK(fps->selected == static_cast<int>(fps->options.size()) - 1);

    seg->on_change(0);
    const ombra::JsonValue* staged = screen.StagedValueForTest("buffer.seconds");
    REQUIRE(staged);
    CHECK(staged->num == doctest::Approx(30.0));
}

// The Output page's file-name field (a TemplateFieldControl). Every keystroke stages, and every
// staged edit rebuilds the page, so each check looks the field up again.
std::shared_ptr<TemplateFieldControl> FileNameField(const SettingsScreen& screen) {
    auto card = ContainerTitled(screen, L"Saved clips");
    auto row = card ? RowTitled(*card, L"File name") : nullptr;
    return row && !row->controls.empty() ? std::dynamic_pointer_cast<TemplateFieldControl>(row->controls[0]) : nullptr;
}

void OpenOutputPage(SettingsScreen& screen, JsonValue& config, const char* filename) {
    config.Set("output.filename", JsonValue::MakeStr(filename));
    screen.InitSchema(RealSpecs(), &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(1);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
}

TEST_CASE("ui: the file name stages as typed; refused characters, Enter and Esc") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    OpenOutputPage(screen, config, "clip {time}");

    auto field = FileNameField(screen);
    REQUIRE(field);
    CHECK(field->text == L"clip {time}");
    CHECK(field->Preview().find(L"clip ") == 0);
    CHECK(!screen.IsEditingText());

    // Type at the end, with a character Windows rejects in between: staged on every key.
    field->focused = true;  // a click into the field
    screen.OnKeyDown(VK_END);
    for (wchar_t c : std::wstring(L" x:y")) screen.OnChar(c);
    const JsonValue* staged = screen.StagedValueForTest("output.filename");
    REQUIRE(staged);
    CHECK(staged->str == "clip {time} xy");
    field = FileNameField(screen);
    REQUIRE(field);
    CHECK(field->focused);  // the rebuilt field keeps focus and caret
    CHECK(field->Caret() == field->text.size());
    CHECK(screen.IsEditingText());

    // Esc puts back what the field held when the edit began, and leaves it.
    screen.OnKeyDown(VK_ESCAPE);
    CHECK(screen.StagedValueForTest("output.filename") == nullptr);
    field = FileNameField(screen);
    REQUIRE(field);
    CHECK(field->text == L"clip {time}");
    CHECK(!screen.IsEditingText());

    // Enter keeps what's staged.
    field->focused = true;
    screen.OnKeyDown(VK_HOME);
    for (wchar_t c : std::wstring(L"my ")) screen.OnChar(c);
    screen.OnKeyDown(VK_RETURN);
    staged = screen.StagedValueForTest("output.filename");
    REQUIRE(staged);
    CHECK(staged->str == "my clip {time}");
    CHECK(!screen.IsEditingText());
}

TEST_CASE("ui: a page rebuild mid-edit keeps the file name's focus, caret and what Esc reverts to") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    OpenOutputPage(screen, config, "clip");

    auto before = FileNameField(screen);
    REQUIRE(before);
    before->focused = true;
    screen.OnKeyDown(VK_HOME);
    for (wchar_t c : std::wstring(L"my ")) screen.OnChar(c);

    // Another staged edit rebuilds the page, as an async reply would.
    screen.ApplySettingChangeForTest("output.per_game_folders", JsonValue::MakeBool(false));
    auto after = FileNameField(screen);
    REQUIRE(after);
    CHECK(after != before);
    CHECK(after->focused);
    CHECK(after->text == L"my clip");
    CHECK(after->Caret() == 3);
    screen.OnKeyDown(VK_ESCAPE);
    CHECK(screen.StagedValueForTest("output.filename") == nullptr);
    CHECK(screen.StagedValueForTest("output.per_game_folders"));  // only the field's own edit goes
}

TEST_CASE("ui: a placeholder inserts at the caret; Reset shows off the default and puts it back") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    auto config = DefaultConfigJson();
    OpenOutputPage(screen, config, "{date}");

    auto field = FileNameField(screen);
    REQUIRE(field);
    REQUIRE(field->on_reset);  // off the default
    field->focused = true;
    screen.OnKeyDown(VK_HOME);
    for (wchar_t c : std::wstring(L"Clip ")) screen.OnChar(c);
    field = FileNameField(screen);
    REQUIRE(field);
    field->InsertToken(0);  // "+ Game"
    const JsonValue* staged = screen.StagedValueForTest("output.filename");
    REQUIRE(staged);
    CHECK(staged->str == "Clip {game}{date}");

    field = FileNameField(screen);
    REQUIRE(field);
    field->on_reset();
    staged = screen.StagedValueForTest("output.filename");
    REQUIRE(staged);
    CHECK(staged->str == "{game} {date} {time}{monitor_suffix}");
    field = FileNameField(screen);
    REQUIRE(field);
    CHECK(!field->on_reset);
}

TEST_CASE("ui: every implemented setting has an explicit form") {
    for (const auto& m : ombra::AllSettings()) {
        if (!m.implemented) continue;
        FieldForm form{};
        INFO("key ", m.key);
        CHECK(FieldFormFor(m.key, &form));
    }
}

// A Custom form only says a page draws the key; this checks one does, with the default config.
TEST_CASE("ui: every Custom setting has a control on some Settings page") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    std::function<void(const std::shared_ptr<Widget>&, std::set<const Widget*>&)> walk =
        [&](const std::shared_ptr<Widget>& w, std::set<const Widget*>& out) {
            if (!w) return;
            out.insert(w.get());
            if (auto f = std::dynamic_pointer_cast<FlexWidget>(w)) for (const auto& it : f->items) walk(it.widget, out);
            if (auto c = std::dynamic_pointer_cast<CardContainer>(w)) {
                for (const auto& a : c->actions) walk(a, out);
                for (const auto& r : c->rows) walk(r, out);
            }
            if (auto r = std::dynamic_pointer_cast<SettingRow>(w)) for (const auto& k : r->controls) walk(k, out);
            if (auto d = std::dynamic_pointer_cast<LevelDetail>(w)) walk(d->tiles, out);
        };
    std::set<std::string> drawn;
    for (int rail = 0; rail < kRailCount; ++rail) {
        screen.SetActiveRail(rail);
        const int subtabs = screen.SubTabBarForTest() ? static_cast<int>(screen.SubTabBarForTest()->tabs.size()) : 0;
        for (int sub = 0; sub < (std::max)(1, subtabs); ++sub) {
            if (subtabs) screen.SetActiveSubTab(sub);
            screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
            std::set<const Widget*> on_page;
            for (const auto& w : screen.PageChildrenForTest()) walk(w, on_page);
            for (const auto& spec : specs) {
                auto c = screen.ControlForTest(spec.key);
                if (c && on_page.count(c.get())) drawn.insert(spec.key);
            }
        }
    }
    const std::set<std::string> not_checked = {
        // Drawn as many controls, none registered under the key itself.
        "capture.monitors", "audio.sources", "audio.per_app_mode", "games.list", "encode.adapter",
        // Shown only once another setting allows it, which the default config doesn't.
        "encode.bitrate_kbps", "encode.cq", "encode.amf_av1_unaligned", "buffer.file_path",
        "capture.target_window", "ui.panel_attach", "ui.screen_frame_color", "ui.screen_frame_thickness",
        "ui.screen_frame_radius", "ui.themes",
    };
    for (const auto& spec : specs) {
        if (!spec.implemented || spec.form != FieldForm::Custom) continue;
        INFO("key ", spec.key);
        if (!not_checked.count(spec.key)) CHECK(drawn.count(spec.key));
    }
}

TEST_CASE("ui: the approved removals render nowhere, and duplicates render once") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);

    // Walk every page and collect every row title and labeled-control label.
    std::vector<std::wstring> seen;
    std::function<void(const std::shared_ptr<Widget>&)> walk = [&](const std::shared_ptr<Widget>& w) {
        if (!w) return;
        if (auto f = std::dynamic_pointer_cast<FlexWidget>(w)) for (const auto& it : f->items) walk(it.widget);
        if (auto r = std::dynamic_pointer_cast<SettingRow>(w)) seen.push_back(r->title);
        if (auto c = std::dynamic_pointer_cast<CardContainer>(w)) {
            if (!c->title.empty()) seen.push_back(c->title);
            for (const auto& row : c->rows) walk(row);
        }
    };
    for (int rail = 0; rail < kRailCount; ++rail) {
        screen.SetActiveRail(rail);
        const int subtabs = screen.SubTabBarForTest() ? static_cast<int>(screen.SubTabBarForTest()->tabs.size()) : 0;
        for (int sub = 0; sub < (std::max)(1, subtabs); ++sub) {
            if (subtabs) screen.SetActiveSubTab(sub);
            screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
            for (const auto& w : screen.PageChildrenForTest()) walk(w);
        }
    }
    auto count = [&](const wchar_t* t) { return std::count(seen.begin(), seen.end(), std::wstring(t)); };
    CHECK(count(L"Buffer size override") == 0);
    CHECK(count(L"Log size cap") == 0);
    CHECK(count(L"Overlap policy") == 0);
    CHECK(count(L"Start with Windows") == 1);
    CHECK(count(L"Hotkey method") == 1);
}

// The button's handler runs inside the card's own mouse dispatch; the page switch it asks for
// rebuilds the page (freeing that card), so it must land after dispatch returns, not during it.
TEST_CASE("ui: Video's Change GPU button opens Advanced > Encoder after the click") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 1400), 1.0f);  // tall enough that nothing scrolls

    auto video = ContainerTitled(screen, L"Video");
    REQUIRE(video);
    auto gpu = RowTitled(*video, L"Recording GPU");
    REQUIRE(gpu);
    REQUIRE(gpu->controls.size() == 1);
    auto change = std::dynamic_pointer_cast<ButtonControl>(gpu->controls[0]);
    REQUIRE(change);
    const D2D1_RECT_F b = change->bounds;
    const D2D1_POINT_2F at = D2D1::Point2F((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f - screen.ScrollYForTest());
    REQUIRE(screen.OnMouseDown(at));
    screen.OnMouseUp(at);
    CHECK(screen.RailForTest().selected == kRailAdvanced);
    REQUIRE(screen.SubTabBarForTest());
    CHECK(screen.SubTabBarForTest()->selected == 0);  // Encoder
    CHECK(ContainerTitled(screen, L"Recording GPU"));
}

TEST_CASE("ui: Video rows show what each choice records; Custom adds a bitrate stepper") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("encode.bitrate_kbps", JsonValue::MakeNum(40000));
    screen.InitSchema(specs, &config);
    screen.SeedSourceSizeForSnapshot(2560, 1440);
    screen.SeedDisplayFpsForSnapshot(144);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto video = ContainerTitled(screen, L"Video");
    REQUIRE(video);
    CHECK(RowTitles(*video) ==
          std::vector<std::wstring>{ L"Resolution", L"Framerate", L"Codec", L"Bitrate", L"Recording GPU" });
    auto res = std::dynamic_pointer_cast<SelectControl>(screen.ControlForTest("capture.resolution"));
    REQUIRE(res);
    CHECK(res->options == std::vector<std::wstring>{ L"Native", L"1440p", L"1080p", L"720p" });
    CHECK(RowTitled(*video, L"Resolution")->help == L"Records 2560 × 1440");
    auto fps = std::dynamic_pointer_cast<SelectControl>(screen.ControlForTest("capture.framerate"));
    REQUIRE(fps);
    CHECK(fps->options.front() == L"Match display");
    CHECK(fps->options[1] == L"30 fps");
    CHECK(std::dynamic_pointer_cast<SegmentedControl>(screen.ControlForTest("encode.video_codec")));

    // The preset is the Bitrate menu; each choice names its number, computed the engine's way.
    auto preset = std::dynamic_pointer_cast<SelectControl>(screen.ControlForTest("encode.quality_preset"));
    REQUIRE(preset);
    REQUIRE(preset->options.size() == 5);
    const int high = ombra::QualityPresetToBitrate("high", 2560, 1440, 60, ombra::VideoCodec::AV1);
    CHECK(preset->selected == 2);
    CHECK(preset->options[2] == L"High · " + std::to_wstring(high / 1000) + L" Mbps");
    CHECK(preset->options[4] == L"Custom");
    CHECK_FALSE(screen.ControlForTest("encode.bitrate_kbps"));

    screen.ApplySettingChangeForTest("encode.quality_preset", JsonValue::MakeStr("custom"));
    auto stepper = std::dynamic_pointer_cast<StepperControl>(screen.ControlForTest("encode.bitrate_kbps"));
    REQUIRE(stepper);
    CHECK(stepper->value == doctest::Approx(40.0));
    CHECK(stepper->unit == L"Mbps");
    stepper->focused = true;
    CHECK(stepper->OnKeyDown(VK_RIGHT));
    const JsonValue* staged = screen.StagedValueForTest("encode.bitrate_kbps");
    REQUIRE(staged);
    CHECK(staged->num == doctest::Approx(41000.0));
}

TEST_CASE("ui: a Select opens its menu through the host, and a pick stages through the preset path") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 1400), 1.0f);

    auto res = std::dynamic_pointer_cast<SelectControl>(screen.ControlForTest("capture.resolution"));
    REQUIRE(res);
    const D2D1_RECT_F b = res->bounds;
    const D2D1_POINT_2F at = D2D1::Point2F((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f - screen.ScrollYForTest());
    REQUIRE(screen.OnMouseDown(at));
    screen.OnMouseUp(at);
    CHECK(res->open);
    CHECK(host.dropdown_options == res->options);
    CHECK(host.dropdown_selected == res->selected);

    // Dismissed: nothing staged, the chevron back down.
    REQUIRE(host.dropdown_close);
    host.dropdown_close();
    CHECK_FALSE(res->open);
    CHECK_FALSE(screen.StagedValueForTest("capture.resolution"));

    REQUIRE(screen.OnMouseDown(at));
    screen.OnMouseUp(at);
    REQUIRE(host.dropdown_pick);
    host.dropdown_pick(2);
    const JsonValue* staged = screen.StagedValueForTest("capture.resolution");
    REQUIRE(staged);
    CHECK(staged->str == "1080p");
}

namespace {
// Clicks the Accent row's custom swatch and returns the colour editor the host was handed.
ColorEditorPopup* OpenAccentEditorForTest(SettingsScreen& screen, FakeScreenHost& host) {
    auto cp = std::dynamic_pointer_cast<ColorPickerControl>(screen.ControlForTest("ui.accent"));
    REQUIRE(cp);
    const D2D1_RECT_F r = cp->CustomSwatchRect();
    const D2D1_POINT_2F at = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f - screen.ScrollYForTest());
    REQUIRE(screen.OnMouseDown(at));
    screen.OnMouseUp(at);
    REQUIRE(host.popup);
    auto* editor = dynamic_cast<ColorEditorPopup*>(host.popup.get());
    REQUIRE(editor);
    return editor;
}
std::string StagedAccent(const SettingsScreen& screen) {
    const JsonValue* v = screen.StagedValueForTest("ui.accent");
    return v ? v->str : std::string("(not staged)");
}
}  // namespace

// Your own accent: the custom swatch opens the editor as the host's
// popup; a drag repaints with the colour and stages it once, on release; Enter stages a typed hex;
// Esc puts back what was there before it opened.
TEST_CASE("ui: the accent editor previews a drag, stages once on release, and Esc puts the old accent back") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();  // theme
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    ColorEditorPopup* editor = OpenAccentEditorForTest(screen, host);
    CHECK(StagedAccent(screen) == "#f97316");  // opening it chooses your own colour, as any swatch click does
    CHECK(editor->Color() == 0xf97316);

    // Into the square (it starts at the popup's padded top left), then along it.
    const D2D1_RECT_F b = editor->bounds;
    const D2D1_POINT_2F from = D2D1::Point2F(b.left + 40, b.top + 40), to = D2D1::Point2F(b.left + 120, b.top + 60);
    REQUIRE(editor->OnMouseDown(from));
    editor->OnMouseMove(to);
    const uint32_t dragged = editor->Color();
    CHECK(dragged != 0xf97316);
    CHECK(host.theme.accent_id == AccentId::Custom);
    CHECK(host.theme.accent_rgb == dragged);              // the page repaints with it...
    CHECK(StagedAccent(screen) == "#f97316");             // ...but nothing is staged mid-drag
    editor->OnMouseUp(to);
    char hex[16];
    std::snprintf(hex, sizeof hex, "#%06x", dragged);
    CHECK(StagedAccent(screen) == hex);
    CHECK_FALSE(editor->Closed());  // the page rebuilt under it; the popup is the host's

    // Typing goes to the hex and starts it over; Enter stages it.
    for (wchar_t ch : std::wstring(L"#112233")) editor->OnChar(ch);
    CHECK(host.theme.accent_rgb == 0x112233);
    editor->OnKeyDown(VK_RETURN);
    CHECK(StagedAccent(screen) == "#112233");
    // Half a hex and Enter: says why, stages nothing.
    for (wchar_t ch : std::wstring(L"#12")) editor->OnChar(ch);
    editor->OnKeyDown(VK_RETURN);
    CHECK_FALSE(editor->HexField().error_text.empty());
    CHECK(StagedAccent(screen) == "#112233");

    // Esc: the first drops the half-typed hex, the second closes and puts the theme's back (no edit left).
    editor->OnKeyDown(VK_ESCAPE);
    CHECK_FALSE(editor->Closed());
    editor->OnKeyDown(VK_ESCAPE);
    CHECK(editor->Closed());
    CHECK_FALSE(screen.StagedValueForTest("ui.accent"));
    CHECK(host.theme.accent_id == AccentId::Theme);
}

TEST_CASE("ui: the accent row leads with the theme's own swatch, chosen by default") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    host.theme.ApplyThemeKey("mocha");
    screen.SetActiveRail(kRailAppearance);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto cp = std::dynamic_pointer_cast<ColorPickerControl>(screen.ControlForTest("ui.accent"));
    REQUIRE(cp);
    REQUIRE(cp->presets.size() == static_cast<size_t>(kAccentSwatchCount));
    CHECK(cp->lead == 1);
    CHECK(cp->selected == 0);
    CHECK(cp->presets[0] == 0xcba6f7u);  // Catppuccin's mauve
    REQUIRE(cp->on_change);
    cp->on_change(1);
    CHECK(StagedAccent(screen) == "mono");
    cp = std::dynamic_pointer_cast<ColorPickerControl>(screen.ControlForTest("ui.accent"));
    REQUIRE(cp);
    cp->on_change(0);
    CHECK_FALSE(screen.StagedValueForTest("ui.accent"));  // back to the default: nothing staged
}

// The near-red note is a line under the swatches: the Accent row grows to hold it, so Corners
// starts below it (it was drawn over the note, 2026-10-30), and the page still fits.
TEST_CASE("ui: an accent near the error red makes room for its note") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("ui.accent", JsonValue::MakeStr("#830000"));
    screen.InitSchema(specs, &config);
    host.theme.ApplyAccentKey("#830000");
    screen.SetActiveRail(kRailAppearance);
    screen.Layout(D2D1::RectF(0, 0, 780, 560), 1.0f);
    auto cp = std::dynamic_pointer_cast<ColorPickerControl>(screen.ControlForTest("ui.accent"));
    auto corners = screen.ControlForTest("ui.corners");
    REQUIRE(cp);
    REQUIRE(corners);
    REQUIRE(cp->ShowsNote(host.theme));
    CHECK(cp->bounds.bottom - cp->bounds.top >= cp->HeightDip(host.theme) - 0.5f);
    CHECK(corners->bounds.top >= cp->bounds.bottom);
    CHECK(screen.MaxScrollForTest() <= 0.0f);

    screen.ApplySettingChangeForTest("ui.accent", JsonValue::MakeStr("blue"));  // no note: no extra line
    cp = std::dynamic_pointer_cast<ColorPickerControl>(screen.ControlForTest("ui.accent"));
    REQUIRE(cp);
    CHECK_FALSE(cp->ShowsNote(host.theme));
    CHECK(cp->HeightDip(host.theme) < host.theme.size.control() + 0.5f);
}

TEST_CASE("ui: a chosen custom accent reopens its editor, and a click outside keeps the colour") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("ui.accent", JsonValue::MakeStr("#3366ff"));
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAppearance);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto cp = std::dynamic_pointer_cast<ColorPickerControl>(screen.ControlForTest("ui.accent"));
    REQUIRE(cp);
    CHECK(cp->selected == static_cast<int>(cp->presets.size()));
    ColorEditorPopup* editor = OpenAccentEditorForTest(screen, host);
    CHECK(editor->Color() == 0x3366ff);
    CHECK_FALSE(screen.StagedValueForTest("ui.accent"));  // already chosen: opening stages nothing

    for (wchar_t ch : std::wstring(L"#00aa55")) editor->OnChar(ch);
    editor->OnMouseDown(D2D1::Point2F(editor->bounds.right + 50, editor->bounds.bottom + 50));
    CHECK(editor->Closed());
    CHECK(StagedAccent(screen) == "#00aa55");
}

TEST_CASE("ui: the colour editor blurs its shadow once per open and draws the picked and used colours") {
    HeadlessDraw h;
    REQUIRE(h.ok);
    ColorEditorPopup editor;
    editor.SetColor(0x1e3a8a);  // deep navy: on dark, the fill has to move lighter
    editor.Place(D2D1::RectF(400, 100, 424, 124), D2D1::RectF(0, 0, 1000, 700), h.theme, 1.0f);
    CHECK(editor.bounds.right == doctest::Approx(424));  // right edges aligned with the swatch
    CHECK(editor.bounds.top > 124);
    const int before = Popup::ShadowRendersForTest();
    h.Draw(editor);
    h.Draw(editor);
    CHECK(Popup::ShadowRendersForTest() == before + 1);
    // Bottom-right of the square is black (no brightness), top-left white (no saturation).
    const auto dark = h.Pixel(static_cast<UINT>(editor.bounds.right) - 24, static_cast<UINT>(editor.bounds.top) + 144);
    CHECK(dark.r + dark.g + dark.b < 0.1f);
}

TEST_CASE("ui: Match display names the rate it records at, and nothing before it is known") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    auto config = DefaultConfigJson();
    config.Set("capture.framerate", JsonValue::MakeNum(0));
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailReplay);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto fps = std::dynamic_pointer_cast<SelectControl>(screen.ControlForTest("capture.framerate"));
    REQUIRE(fps);
    CHECK(fps->options[fps->selected] == L"Match display");
    auto video = ContainerTitled(screen, L"Video");
    REQUIRE(video);
    CHECK(RowTitled(*video, L"Framerate")->help.empty());  // unknown display rate: no "0 fps"

    screen.SeedDisplayFpsForSnapshot(144);
    screen.ApplySettingChangeForTest("capture.framerate", JsonValue::MakeNum(0));
    video = ContainerTitled(screen, L"Video");
    REQUIRE(video);
    CHECK(RowTitled(*video, L"Framerate")->help.find(L"Records 144 fps") == 0);
}

TEST_CASE("ui: rate control steps max bitrate under VBR, CQ under constant quality, neither under CBR") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailAdvanced);
    screen.SetActiveSubTab(0);  // Encoder
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto rc_row = [&] {
        auto card = ContainerTitled(screen, L"Encoding");
        return card ? RowTitled(*card, L"Rate control") : nullptr;
    };
    auto row = rc_row();
    REQUIRE(row);
    REQUIRE(row->controls.size() == 2);
    auto max = std::dynamic_pointer_cast<StepperControl>(screen.ControlForTest("encode.max_bitrate_kbps"));
    REQUIRE(max);
    CHECK(row->controls[0] == max);
    CHECK(max->min_label == L"Auto");
    CHECK(max->value == 0);
    max->focused = true;
    max->OnKeyDown(VK_RIGHT);  // Auto -> 1.25x the 40 Mbps default target, not 1 Mbps
    const JsonValue* staged = screen.StagedValueForTest("encode.max_bitrate_kbps");
    REQUIRE(staged);
    CHECK(staged->num == doctest::Approx(50000.0));

    screen.ApplySettingChangeForTest("encode.rate_control", JsonValue::MakeStr("cqp"));
    row = rc_row();
    REQUIRE(row);
    REQUIRE(row->controls.size() == 2);
    CHECK(row->controls[0] == screen.ControlForTest("encode.cq"));

    screen.ApplySettingChangeForTest("encode.rate_control", JsonValue::MakeStr("cbr"));
    row = rc_row();
    REQUIRE(row);
    CHECK(row->controls.size() == 1);
}

// The Recording GPU card: Auto plus one tile per GPU the engine reported; a click stages
// encode.adapter. Neither backend offers B-frames or lookahead: both break one-frame-in-one-out.
TEST_CASE("ui: the Recording GPU card stages encode.adapter; no B-frames or lookahead on either backend") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SeedRecordingGpusForSnapshot();  // RTX (recording, display) + 780M
    screen.SetActiveRail(kRailAdvanced);
    screen.SetActiveSubTab(0);  // Encoder
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto tiles = CardTiles(ContainerTitled(screen, L"Recording GPU"));
    REQUIRE(tiles.size() == 4);
    CHECK(tiles[0]->title == L"Auto");
    CHECK(tiles[0]->selected);
    CHECK(tiles[0]->note == L"Picks RTX 5060 Laptop GPU");
    CHECK(tiles[1]->title == L"RTX 5060 Laptop GPU");
    CHECK(tiles[1]->detail == L"NVENC · drives the display");
    CHECK(tiles[1]->note == L"Recording now");
    CHECK(tiles[1]->chips == std::vector<std::wstring>{L"H.264", L"AV1"});
    CHECK(tiles[2]->title == L"Radeon 780M");
    CHECK(tiles[2]->detail == L"AMF");
    CHECK(tiles[2]->note.empty());
    CHECK(tiles[3]->title == L"CPU");
    CHECK(tiles[3]->chips == std::vector<std::wstring>{L"H.264"});

    auto encoding_titles = [&] {
        auto card = ContainerTitled(screen, L"Encoding");
        return card ? RowTitles(*card) : std::vector<std::wstring>{};
    };
    const std::vector<std::wstring> expected{L"Rate control", L"Preset", L"Keyframes"};
    CHECK(encoding_titles() == expected);

    tiles[2]->on_change(true);
    const JsonValue* staged = screen.StagedValueForTest("encode.adapter");
    REQUIRE(staged);
    CHECK(staged->str == "amd");
    tiles = CardTiles(ContainerTitled(screen, L"Recording GPU"));
    REQUIRE(tiles.size() == 4);
    CHECK(tiles[2]->selected);
    CHECK_FALSE(tiles[0]->selected);
    CHECK(encoding_titles() == expected);
    auto preset = RowTitled(*ContainerTitled(screen, L"Encoding"), L"Preset");
    REQUIRE(preset);
    CHECK(preset->help.rfind(L"On AMD", 0) == 0);
}

// No GPU encoder would open, so Auto records on the CPU: the warning sits on the tiles that caused it.
TEST_CASE("ui: a CPU fallback warns on the Auto and CPU tiles") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SeedRecordingGpusForSnapshot(/*cpu_fallback=*/true);
    screen.SetActiveRail(kRailAdvanced);
    screen.SetActiveSubTab(0);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    auto card = ContainerTitled(screen, L"Recording GPU");
    REQUIRE(card);
    CHECK(card->help.rfind(L"No GPU encoder would start", 0) == 0);
    auto tiles = CardTiles(card);
    REQUIRE(tiles.size() == 4);
    CHECK(tiles[0]->note == L"Fell back to the CPU");
    CHECK(tiles[0]->note_tone == SettingRow::Tone::Warning);
    CHECK(tiles[1]->note.empty());
    CHECK(tiles[3]->note == L"Recording now");
    CHECK(tiles[3]->note_tone == SettingRow::Tone::Warning);
    // AV1 is the default codec; the CPU records H.264, and a row says so.
    auto av1 = RowTitled(*card, L"AV1");
    REQUIRE(av1);
    CHECK(av1->help_tone == SettingRow::Tone::Warning);
}

namespace {

// Shortcuts (and their troubleshooting), laid out, with the default config.
UINT g_test_mods = 0;  // what HotkeysPage's recording sees held

struct HotkeysPage {
    FakeScreenHost host;
    SettingsScreen screen{&host};
    HotkeysPage() {
        g_test_mods = 0;
        SetHeldModsSourceForTest([] { return g_test_mods; });
        const auto specs = RealSpecs();
        const auto config = DefaultConfigJson();
        screen.InitSchema(specs, &config);
        screen.SetActiveRail(kRailShortcuts);
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    }
    ~HotkeysPage() { SetHeldModsSourceForTest(nullptr); }
};

}  // namespace

TEST_CASE("ui: General page has one shortcut row per action and the method in its own card") {
    HotkeysPage p;
    for (const char* key : {"hotkeys.save_replay", "hotkeys.screenshot", "hotkeys.toggle_buffer", "hotkeys.overlay"}) {
        INFO(key);
        auto v = p.screen.ShortcutRowForTest(key);
        REQUIRE(v.row);
        REQUIRE(v.capture);
        CHECK_FALSE(v.capture->spec.empty());
        CHECK(v.capture->spec == v.default_spec);
        CHECK_FALSE(v.reset->visible);  // only off the default
    }
    CHECK(p.screen.ShortcutRowForTest("hotkeys.save_replay").row->help.rfind(L"Saves the last ", 0) == 0);
    // NVIDIA's defaults are ours too; no row says so.
    CHECK(p.screen.ShortcutRowForTest("hotkeys.overlay").row->tag.empty());
    CHECK(p.screen.ShortcutRowForTest("hotkeys.overlay").row->note.empty());
}

// Keys only: modifiers come from GetKeyState, which a test can't hold down, so these use keys the
// rule accepts bare (F13-F24) and refuses bare (letters).
TEST_CASE("ui: recording a shortcut suspends the engine's hotkeys, refuses bad combos, commits a good one") {
    HotkeysPage p;
    auto row = p.screen.ShortcutRowForTest("hotkeys.save_replay").capture;
    REQUIRE(row);
    row->BeginRecording();
    CHECK(p.screen.IsRecordingShortcut());
    REQUIRE(p.host.hotkey_suspends.size() == 1);
    CHECK(p.host.hotkey_suspends.back() == true);

    // A bare letter would fire while typing.
    p.screen.OnShortcutKey('A', true);
    CHECK(p.screen.IsRecordingShortcut());
    CHECK_FALSE(row->Refusal().empty());
    // The row's help line says why.
    CHECK(p.screen.ShortcutRowForTest("hotkeys.save_replay").row->help == row->Refusal());
    CHECK(p.screen.StagedValueForTest("hotkeys.save_replay") == nullptr);

    // Modifier-only presses never commit.
    p.screen.OnShortcutKey(VK_SHIFT, true);
    CHECK(p.screen.IsRecordingShortcut());

    p.screen.OnShortcutKey(VK_F13, true);
    CHECK_FALSE(p.screen.IsRecordingShortcut());
    CHECK(p.host.hotkey_suspends.back() == false);
    const auto* staged = p.screen.StagedValueForTest("hotkeys.save_replay");
    REQUIRE(staged);
    CHECK(staged->str == "F13");

    // A held modifier is part of the combo.
    row->BeginRecording();
    g_test_mods = MOD_CONTROL | MOD_SHIFT;
    p.screen.OnShortcutKey(VK_SHIFT, true);
    CHECK(row->HeldModsForTest() == (MOD_CONTROL | MOD_SHIFT));  // the caps show them while held
    p.screen.OnShortcutKey(VK_F13, true);
    staged = p.screen.StagedValueForTest("hotkeys.save_replay");
    REQUIRE(staged);
    CHECK(staged->str == "Ctrl+Shift+F13");
}

TEST_CASE("ui: a shortcut another action already uses is refused against the staged values") {
    HotkeysPage p;
    p.screen.ApplySettingChangeForTest("hotkeys.screenshot", ombra::JsonValue::MakeStr("F14"));
    p.screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto row = p.screen.ShortcutRowForTest("hotkeys.save_replay").capture;
    REQUIRE(row);
    row->BeginRecording();
    p.screen.OnShortcutKey(VK_F14, true);
    CHECK(p.screen.IsRecordingShortcut());
    CHECK(row->Refusal().find(L"Screenshot") != std::wstring::npos);

    // Esc cancels without staging anything and hands the hotkeys back.
    p.screen.OnShortcutKey(VK_ESCAPE, true);
    CHECK_FALSE(p.screen.IsRecordingShortcut());
    CHECK(p.host.hotkey_suspends.back() == false);
    CHECK(p.screen.StagedValueForTest("hotkeys.save_replay") == nullptr);
}

TEST_CASE("ui: reset to a default another action took records with the reason, in one click") {
    HotkeysPage p;
    p.screen.ApplySettingChangeForTest("hotkeys.save_replay", ombra::JsonValue::MakeStr("F13"));
    p.screen.ApplySettingChangeForTest("hotkeys.screenshot", ombra::JsonValue::MakeStr("Alt+F10"));
    p.screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto v = p.screen.ShortcutRowForTest("hotkeys.save_replay");
    REQUIRE(v.reset);
    REQUIRE(v.reset->visible);
    const D2D1_RECT_F& r = v.reset->bounds;
    p.screen.OnMouseDown(D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f));
    CHECK(p.screen.IsRecordingShortcut());
    CHECK(v.capture->Refusal().find(L"Screenshot") != std::wstring::npos);
    CHECK(v.row->help == v.capture->Refusal());
    CHECK(p.host.hotkey_suspends.back() == true);
}

TEST_CASE("ui: a click outside a recording row (the Save bar) stops recording and resumes hotkeys") {
    HotkeysPage p;
    p.screen.ApplySettingChangeForTest("hotkeys.backend", ombra::JsonValue::MakeStr("poll"));
    p.screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto row = p.screen.ShortcutRowForTest("hotkeys.save_replay").capture;
    REQUIRE(row);
    row->BeginRecording();
    REQUIRE(p.screen.IsRecordingShortcut());
    // Bottom-right corner of the page: the Save/Discard bar, never a shortcut row.
    p.screen.OnMouseDown(D2D1::Point2F(1270.0f, 790.0f));
    CHECK_FALSE(row->Recording());
    CHECK_FALSE(p.screen.IsRecordingShortcut());
    CHECK(p.host.hotkey_suspends.back() == false);
}

TEST_CASE("ui: leaving the General page mid-recording resumes the engine's hotkeys") {
    HotkeysPage p;
    auto row = p.screen.ShortcutRowForTest("hotkeys.overlay").capture;
    REQUIRE(row);
    row->BeginRecording();
    REQUIRE(p.host.hotkey_suspends.back() == true);
    p.screen.SetActiveRail(kRailAppearance);
    p.screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK_FALSE(p.screen.IsRecordingShortcut());
    CHECK(p.host.hotkey_suspends.back() == false);
}


TEST_CASE("ui: modal confirm and cancel buttons are hit-testable after Draw; one-button has no cancel") {
    HeadlessDraw h;
    REQUIRE(h.ok);
    for (const bool one_button : {false, true}) {
        ModalDialog m;
        m.title = L"T";
        m.message = L"Message";
        if (one_button) m.cancel_text.clear();
        m.bounds = D2D1::RectF(0, 0, 1000, 700);
        h.Draw(m);
        bool saw_confirm = false, saw_cancel = false;
        float confirm_x = 0.0f, cancel_x = 0.0f;
        for (float y = 250.0f; y < 450.0f; y += 4.0f) {
            for (float x = 300.0f; x < 700.0f; x += 4.0f) {
                m.visible = true;
                m.last_result = ModalDialog::Result::None;
                const auto pt = D2D1::Point2F(x, y);
                m.OnMouseMove(pt);
                m.OnMouseDown(pt);
                if (m.last_result == ModalDialog::Result::Confirm && !saw_confirm) { saw_confirm = true; confirm_x = x; }
                if (m.last_result == ModalDialog::Result::Cancel && !saw_cancel) { saw_cancel = true; cancel_x = x; }
            }
        }
        CHECK(saw_confirm);
        CHECK(saw_cancel == !one_button);
        if (!one_button) CHECK(cancel_x < confirm_x);  // cancel sits left of confirm
    }
}

// The card sizes to its text: a fixed 400x200 card gave the message 86 dip, which an Auto-Tune
// result (several paragraphs) overflowed into the buttons.
TEST_CASE("ui: a long modal message stays inside the card, above the buttons, at 100% and 150%") {
    HeadlessDraw h(1600, 1000);
    REQUIRE(h.ok);
    DrawProbe probe(h.theme.dwrite.Get());
    g_draw_probe = &probe;
    for (const float s : {1.0f, 1.5f}) {
        CAPTURE(s);
        h.theme.SetFontScale(s);
        ModalDialog m;
        m.title = L"Auto-Tune Complete";
        m.message = L"Measured end-to-end latency p99 14.2 ms.\n\nThe encoder kept up at every preset tried, but "
                    L"the quality preset was limited by GPU load from another application during the run. "
                    L"Close other GPU-heavy programs and run Auto-Tune again for a result that reflects how "
                    L"your games will actually record.\n\nApply encoder preset p5?";
        m.bounds = D2D1::RectF(0, 0, 1600, 1000);
        probe.Clear();
        h.dc->BeginDraw();
        m.Draw(h.dc.Get(), h.theme, s);
        REQUIRE(SUCCEEDED(h.dc->EndDraw()));
        const DrawnText* msg = nullptr;
        const DrawnText* confirm = nullptr;
        for (const auto& t : probe.texts) {
            if (t.text == m.message) msg = &t;
            if (t.text == m.confirm_text) confirm = &t;
        }
        REQUIRE(msg);
        REQUIRE(confirm);
        CHECK(msg->lines >= 8);  // well past the old fixed 86-dip slot
        CHECK_FALSE(msg->overflow_y);
        CHECK_FALSE(msg->overflow_x);
        CHECK(msg->rect.bottom <= confirm->rect.top);
    }
    g_draw_probe = nullptr;
}

// One home per recording setting.
TEST_CASE("ui: the quality preset lives only on Replay & Quality; Auto-Tune rides on the Preset row") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);
    screen.SetActiveRail(kRailShortcuts);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK_FALSE(screen.ControlForTest("encode.quality_preset"));
    CHECK_FALSE(screen.ControlForTest("engine.autotune"));

    screen.SetActiveRail(kRailAdvanced);
    screen.SetActiveSubTab(0);  // Encoder
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    auto encoding = ContainerTitled(screen, L"Encoding");
    REQUIRE(encoding);
    auto preset = RowTitled(*encoding, L"Preset");
    REQUIRE(preset);
    REQUIRE_FALSE(preset->controls.empty());
    auto button = std::dynamic_pointer_cast<ButtonControl>(preset->controls[0]);
    REQUIRE(button);
    CHECK(button == screen.ControlForTest("engine.autotune"));
    button->on_click();
    CHECK(host.modal_count == 1);  // a confirm first: the run closes this panel
}

TEST_CASE("ui: fonts follow the monitor's DPI, and MeasureText reports layout pixels") {
    // Layout is in physical pixels (geometry * dpi_scale on a 96-DPI context). Fonts used to stay at
    // their 100% size, so at 150% the text shrank inside controls that had grown.
    HeadlessDraw h;
    REQUIRE(h.ok);
    const std::wstring text = L"Replay buffer length";
    const D2D1_SIZE_F at100 = h.theme.MeasureText(h.theme.Font(TypeRole::Label), text);
    REQUIRE(at100.width > 0.0f);

    h.theme.SetFontScale(1.5f);
    const D2D1_SIZE_F at150 = h.theme.MeasureText(h.theme.Font(TypeRole::Label), text);
    CHECK(at150.width == doctest::Approx(at100.width * 1.5f).epsilon(0.08));
    CHECK(at150.height == doctest::Approx(at100.height * 1.5f).epsilon(0.08));
    CHECK(h.theme.Font(TypeRole::Caption)->GetFontSize() == doctest::Approx(SpecFor(TypeRole::Caption).size * 1.5f));

    h.theme.SetFontScale(1.0f);
    CHECK(h.theme.MeasureText(h.theme.Font(TypeRole::Label), text).width == doctest::Approx(at100.width));
}


// --- Games page ----------------------------------------------------------------------------------


namespace {

JsonValue GamesReply() {
    JsonValue resp = JsonValue::MakeObject();
    resp.Set("ok", JsonValue::MakeBool(true));
    JsonValue games = JsonValue::MakeArray();
    auto add = [&](const char* name, const char* path, bool game, bool detected) {
        JsonValue g = JsonValue::MakeObject();
        g.Set("name", JsonValue::MakeStr(name));
        g.Set("path", JsonValue::MakeStr(path));
        g.Set("source", JsonValue::MakeStr(detected ? "steam" : "user"));
        g.Set("game", JsonValue::MakeBool(game));  // decisions applied: the page must not trust this
        g.Set("detected", JsonValue::MakeBool(detected));
        games.arr.push_back(std::move(g));
    };
    add("Apex Legends", "E:\\Games\\Steam\\steamapps\\common\\Apex Legends", true, true);
    add("SteamVR", "E:\\Games\\Steam\\steamapps\\common\\SteamVR", false, true);
    add("Discord", "C:\\Users\\Me\\AppData\\Local\\Discord", false, false);
    resp.Set("games", std::move(games));
    JsonValue seen = JsonValue::MakeArray();
    JsonValue a = JsonValue::MakeObject();
    a.Set("name", JsonValue::MakeStr("renderer.exe"));
    a.Set("path", JsonValue::MakeStr("C:\\Tools\\renderer.exe"));
    seen.arr.push_back(std::move(a));
    resp.Set("seen", std::move(seen));
    return resp;
}

// The committed games.list behind GamesReply(): SteamVR marked not a game, Discord added by hand,
// and one bare exe name carried over from the old exclusions list.
JsonValue GamesDecisions() {
    std::vector<ombra::GameEntry> d(3);
    d[0].name = L"SteamVR";
    d[0].path = L"E:\\Games\\Steam\\steamapps\\common\\SteamVR";
    d[0].source = ombra::GameSource::Steam;
    d[0].is_game = false;
    d[1].name = L"Discord";
    d[1].path = L"C:\\Users\\Me\\AppData\\Local\\Discord";
    d[1].is_game = false;
    d[2].name = L"obs64.exe";
    d[2].path = L"obs64.exe";
    d[2].is_game = false;
    return ombra::SerializeGamesList(d);
}

const GameRow* FindGameRow(const std::vector<GameRow>& list, const wchar_t* name) {
    for (const auto& r : list)
        if (r.name == name) return &r;
    return nullptr;
}

const ombra::GameEntry* FindDecision(const std::vector<ombra::GameEntry>& d, const std::wstring& path) {
    for (const auto& e : d)
        if (ombra::GameKey(e.path) == ombra::GameKey(path)) return &e;
    return nullptr;
}

struct GamesPage {
    FakeScreenHost host;
    SettingsScreen screen{&host};
    GamesPage() {
        JsonValue config = DefaultConfigJson();
        config.Set("games.list", GamesDecisions());
        screen.InitSchema(RealSpecs(), &config);
        const JsonValue reply = GamesReply();
        screen.ApplyGamesReply(&reply);
        screen.SetActiveRail(kRailGames);
        screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    }
    const std::vector<GameRow>& List() {
        REQUIRE(screen.GamesListForTest());
        return screen.GameRowsForTest();
    }
};

}  // namespace

TEST_CASE("ui: Games lists detected games, your decisions and undecided apps, games first") {
    GamesPage p;
    const auto& list = p.List();
    // The undecided app leads, then games, then not-games.
    REQUIRE(list.size() == 5);
    CHECK(list[0].decide);
    CHECK(list[0].name == L"renderer.exe");
    CHECK(list[1].name == L"Apex Legends");
    CHECK(list[1].is_game);
    CHECK_FALSE(list[1].removable);  // detected: it would come back on the next scan
    const GameRow* vr = FindGameRow(list, L"SteamVR");
    REQUIRE(vr);
    CHECK_FALSE(vr->is_game);
    const GameRow* discord = FindGameRow(list, L"Discord");
    REQUIRE(discord);
    CHECK(discord->removable);
    CHECK(discord->source == L"Added by you");
    // A migrated bare name still shows, and says it matches that name anywhere.
    const GameRow* bare = FindGameRow(list, L"obs64.exe");
    REQUIRE(bare);
    CHECK_FALSE(bare->warning.empty());
}

// Only the list scrolls. The card's title and the page stay put.
TEST_CASE("ui: the wheel over the Games list scrolls its rows, not the page") {
    GamesPage p;
    p.screen.Layout(D2D1::RectF(0, 0, 1280, 340), 1.0f);  // short enough that five rows don't fit
    auto list = p.screen.GamesListForTest();
    REQUIRE(list);
    REQUIRE(list->rows.size() == 5);
    const float view_h = list->bounds.bottom - list->bounds.top;
    CHECK(view_h < list->ContentHeight());
    CHECK(p.screen.ScrollYForTest() == 0.0f);
    const float first_top = list->rows[0]->bounds.top;

    const D2D1_POINT_2F over = D2D1::Point2F((list->bounds.left + list->bounds.right) / 2, list->bounds.top + 10);
    p.screen.OnMouseMove(over);
    p.screen.OnMouseWheel(-1.0f);
    CHECK(list->scroll_y > 0.0f);
    CHECK(list->rows[0]->bounds.top == doctest::Approx(first_top - list->scroll_y));
    CHECK(p.screen.ScrollYForTest() == 0.0f);

    // The offset survives the rebuild a switch causes.
    const float kept = list->scroll_y;
    auto toggle = p.List()[1].on_toggle;
    toggle(false);
    p.screen.Layout(D2D1::RectF(0, 0, 1280, 420), 1.0f);
    list = p.screen.GamesListForTest();
    REQUIRE(list);
    CHECK(list->scroll_y == doctest::Approx(kept));
}

TEST_CASE("ui: switching a detected game off stages a decision; back on unstages it") {
    GamesPage p;
    const GameRow* apex = FindGameRow(p.List(), L"Apex Legends");
    REQUIRE(apex);
    auto off = apex->on_toggle;
    off(false);
    auto staged = p.screen.StagedGameDecisions();
    const auto* d = FindDecision(staged, L"E:\\Games\\Steam\\steamapps\\common\\Apex Legends");
    REQUIRE(d);
    CHECK_FALSE(d->is_game);
    CHECK(p.screen.HasPendingChangesForTest());
    // The row stays where it was (ordered by the saved answer) and shows the staged one.
    const auto& list = p.List();
    CHECK(list[1].name == L"Apex Legends");
    CHECK_FALSE(list[1].is_game);

    auto on = list[1].on_toggle;
    on(true);
    // A detected game switched back on needs no decision, and nothing is left to save.
    CHECK(FindDecision(p.screen.StagedGameDecisions(), L"E:\\Games\\Steam\\steamapps\\common\\Apex Legends") == nullptr);
    CHECK_FALSE(p.screen.HasPendingChangesForTest());
}

TEST_CASE("ui: switching a detected not-game back on drops its decision") {
    GamesPage p;
    const GameRow* vr = FindGameRow(p.List(), L"SteamVR");
    REQUIRE(vr);
    auto on = vr->on_toggle;
    on(true);
    CHECK(FindDecision(p.screen.StagedGameDecisions(), L"E:\\Games\\Steam\\steamapps\\common\\SteamVR") == nullptr);
    CHECK(p.screen.HasPendingChangesForTest());
}

TEST_CASE("ui: × removes an entry you added; a user entry switched on keeps its decision") {
    GamesPage p;
    const GameRow* discord = FindGameRow(p.List(), L"Discord");
    REQUIRE(discord);
    auto on = discord->on_toggle;
    on(true);
    const auto decisions = p.screen.StagedGameDecisions();
    const auto* d = FindDecision(decisions, L"C:\\Users\\Me\\AppData\\Local\\Discord");
    REQUIRE(d);
    CHECK(d->is_game);

    discord = FindGameRow(p.List(), L"Discord");
    REQUIRE(discord);
    auto remove = discord->on_remove;
    remove();
    CHECK(FindDecision(p.screen.StagedGameDecisions(), L"C:\\Users\\Me\\AppData\\Local\\Discord") == nullptr);
    CHECK(FindGameRow(p.List(), L"Discord") == nullptr);
}

TEST_CASE("ui: answering a looked-like-a-game app stores its exe path and takes it off the list") {
    GamesPage p;
    auto decide = p.List()[0].on_decide;
    REQUIRE(decide);
    decide(false);
    const auto decisions = p.screen.StagedGameDecisions();
    const auto* d = FindDecision(decisions, L"C:\\Tools\\renderer.exe");
    REQUIRE(d);
    CHECK_FALSE(d->is_game);
    CHECK(d->source == ombra::GameSource::User);
    for (const auto& r : p.List()) CHECK_FALSE(r.decide);
}

namespace {
class AddGameHost : public FakeScreenHost {
public:
    std::vector<std::wstring> options;
    std::function<void(int)> pick;
    std::vector<WindowEntry> windows;
    void RequestWindowListAsync(std::function<void(std::vector<WindowEntry>)> on_result) override {
        if (on_result) on_result(windows);
    }
    void ShowDropdown(const D2D1_RECT_F&, const std::vector<std::wstring>& opts, int,
                      std::function<void(int)> on_select, std::function<void()>) override {
        options = opts;
        pick = std::move(on_select);
    }
};

WindowEntry Win(const wchar_t* title, const char* process, uint64_t hwnd, const wchar_t* path) {
    WindowEntry w{title, process, hwnd};
    w.image_path = path;
    return w;
}
}  // namespace

TEST_CASE("ui: Add a game offers each running program by its own full path") {
    AddGameHost host;
    // Two javaw.exe: a launcher's own Java runtime and an IDE on the system JDK. The process list
    // is one entry per exe name, so it could only ever offer one of them.
    host.windows = {
        Win(L"Minecraft 1.21", "javaw.exe", 1, L"C:\\Users\\Me\\AppData\\Roaming\\PrismLauncher\\java\\bin\\javaw.exe"),
        Win(L"Minecraft 1.21 - second window", "javaw.exe", 2,
            L"C:\\Users\\Me\\AppData\\Roaming\\PrismLauncher\\java\\bin\\javaw.exe"),
        Win(L"IntelliJ Launcher", "javaw.exe", 3, L"C:\\Program Files\\Java\\jdk-21\\bin\\javaw.exe"),
        Win(L"Apex Legends", "r5apex.exe", 4, L"E:\\Games\\Steam\\steamapps\\common\\Apex Legends\\r5apex.exe"),
        Win(L"No path", "nopath.exe", 5, L""),
    };
    SettingsScreen screen(&host);
    JsonValue config = DefaultConfigJson();
    config.Set("games.list", GamesDecisions());
    screen.InitSchema(RealSpecs(), &config);
    const JsonValue reply = GamesReply();
    screen.ApplyGamesReply(&reply);
    screen.SetActiveRail(kRailGames);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    screen.OpenAddGamePickerForTest();
    // Browse, then one entry per javaw.exe path (the second window is the same program).
    // Apex is already a game; a window with no path has nothing safe to store.
    REQUIRE(host.options.size() == 3);
    REQUIRE(host.pick);
    int prism = -1, jdk = -1;
    for (int i = 1; i < static_cast<int>(host.options.size()); ++i) {
        if (host.options[i].find(L"PrismLauncher") != std::wstring::npos) prism = i;
        if (host.options[i].find(L"jdk-21") != std::wstring::npos) jdk = i;
    }
    REQUIRE(prism > 0);
    REQUIRE(jdk > 0);

    auto pick = host.pick;
    pick(prism);
    auto decisions = screen.StagedGameDecisions();
    const auto* d = FindDecision(decisions, L"C:\\Users\\Me\\AppData\\Roaming\\PrismLauncher\\java\\bin\\javaw.exe");
    REQUIRE(d);
    CHECK(d->is_game);
    // A launcher's own runtime is fine: no warning on that row.
    auto list = screen.GamesListForTest();
    REQUIRE(list);
    const GameRow* mc = FindGameRow(screen.GameRowsForTest(), d->name.c_str());
    REQUIRE(mc);
    CHECK(mc->warning.empty());

    // The system JDK's javaw.exe would make every Java program a game: that row says so.
    screen.OpenAddGamePickerForTest();
    int jdk2 = -1;
    for (int i = 1; i < static_cast<int>(host.options.size()); ++i)  // no longer a same-name twin: no path
        if (host.options[i].find(L"IntelliJ Launcher") != std::wstring::npos) jdk2 = i;
    REQUIRE(jdk2 > 0);
    pick = host.pick;
    pick(jdk2);
    decisions = screen.StagedGameDecisions();
    const auto* shared = FindDecision(decisions, L"C:\\Program Files\\Java\\jdk-21\\bin\\javaw.exe");
    REQUIRE(shared);
    list = screen.GamesListForTest();
    REQUIRE(list);
    const GameRow* ide = FindGameRow(screen.GameRowsForTest(), shared->name.c_str());
    REQUIRE(ide);
    CHECK_FALSE(ide->warning.empty());
}

TEST_CASE("ui: an app icon loads from an exe, a missing file gives none, a big image is scaled to fit") {
    ombra::ComInit com;
    wchar_t win[MAX_PATH]{};
    REQUIRE(GetWindowsDirectoryW(win, MAX_PATH) > 0);
    const FileIcon exe = LoadFileIcon(std::wstring(win) + L"\\explorer.exe", 28);
    CHECK(exe.w == 28);
    CHECK(exe.h == 28);
    REQUIRE(exe.bgra.size() == 28u * 28u * 4u);
    // An icon has transparent corners and opaque pixels: not an all-black or all-clear square.
    bool clear = false, opaque = false;
    for (size_t i = 3; i < exe.bgra.size(); i += 4) {
        clear = clear || exe.bgra[i] == 0;
        opaque = opaque || exe.bgra[i] == 255;
    }
    CHECK(clear);
    CHECK(opaque);
    CHECK(LoadFileIcon(std::wstring(win) + L"\\no-such-program.exe", 28).bgra.empty());
    CHECK(LoadFileIcon(L"", 28).bgra.empty());
    // Any image WIC reads works too (Steam's cached icons are .jpg); one bigger than asked is
    // scaled down to fit, keeping its shape.
    const std::wstring wallpaper = std::wstring(win) + L"\\Web\\Wallpaper\\Windows\\img0.jpg";
    if (GetFileAttributesW(wallpaper.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const FileIcon jpg = LoadFileIcon(wallpaper, 28);
        CHECK((std::max)(jpg.w, jpg.h) == 28);
        CHECK(jpg.bgra.size() == static_cast<size_t>(jpg.w) * jpg.h * 4);
    }
}

TEST_CASE("ui: Games rows draw the scan's icon or the exe's own; a file without one is asked for once") {
    ombra::ComInit com;
    wchar_t win[MAX_PATH]{};
    REQUIRE(GetWindowsDirectoryW(win, MAX_PATH) > 0);
    const std::wstring explorer = std::wstring(win) + L"\\explorer.exe";

    FakeScreenHost host;
    SettingsScreen screen(&host);
    JsonValue config = DefaultConfigJson();
    config.Set("games.list", GamesDecisions());
    screen.InitSchema(RealSpecs(), &config);
    JsonValue reply = GamesReply();
    const_cast<JsonValue*>(reply.Find("games"))->arr[0].Set("icon", JsonValue::MakeStr(ombra::NarrowUtf8(explorer)));  // Apex
    screen.ApplyGamesReply(&reply);
    screen.SetActiveRail(kRailGames);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);

    // One request for the page: Apex's scanned icon and the undecided app's own exe. A folder
    // entry has nothing to ask for, and neither has a bare migrated name (not a full path).
    REQUIRE(host.file_icon_requests.size() == 1);
    const auto& asked = host.file_icon_requests[0];
    CHECK(asked.size() == 2);
    CHECK(std::find(asked.begin(), asked.end(), explorer) != asked.end());
    CHECK(std::find(asked.begin(), asked.end(), L"C:\\Tools\\renderer.exe") != asked.end());

    REQUIRE(host.pending_file_icons.size() == 1);
    host.pending_file_icons[0]();
    host.pending_file_icons.clear();
    auto list = screen.GamesListForTest();
    REQUIRE(list);
    const GameRow* apex = FindGameRow(screen.GameRowsForTest(), L"Apex Legends");
    REQUIRE(apex);
    REQUIRE(apex->icon);
    CHECK(apex->icon->w == 28);
    CHECK_FALSE(apex->icon->bgra.empty());
    const GameRow* renderer = FindGameRow(screen.GameRowsForTest(), L"renderer.exe");
    REQUIRE(renderer);
    CHECK((!renderer->icon || renderer->icon->bgra.empty()));  // the file isn't there: controller
    const GameRow* discord = FindGameRow(screen.GameRowsForTest(), L"Discord");
    REQUIRE(discord);
    CHECK_FALSE(discord->icon);

    // More rebuilds (a toggle, a scroll) don't ask again, for the failure either.
    screen.ScrollGamesListForTest(0.0f);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(host.file_icon_requests.size() == 1);
}

TEST_CASE("ui: Games icons the page needed while a fetch was out are asked for once it answers") {
    ombra::ComInit com;
    wchar_t win[MAX_PATH]{};
    REQUIRE(GetWindowsDirectoryW(win, MAX_PATH) > 0);
    const std::wstring explorer = std::wstring(win) + L"\\explorer.exe";

    // Only a hand-added exe that isn't there: the first fetch loads nothing.
    std::vector<ombra::GameEntry> d(1);
    d[0].name = L"Gone";
    d[0].path = L"C:\\Nowhere\\gone.exe";
    FakeScreenHost host;
    SettingsScreen screen(&host);
    JsonValue config = DefaultConfigJson();
    config.Set("games.list", ombra::SerializeGamesList(d));
    screen.InitSchema(RealSpecs(), &config);
    screen.SetActiveRail(kRailGames);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    REQUIRE(host.file_icon_requests.size() == 1);

    // The scan's reply lands while that fetch is still out.
    JsonValue reply = GamesReply();
    const_cast<JsonValue*>(reply.Find("games"))->arr[0].Set("icon", JsonValue::MakeStr(ombra::NarrowUtf8(explorer)));
    screen.ApplyGamesReply(&reply);
    screen.ScrollGamesListForTest(0.0f);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(host.file_icon_requests.size() == 1);  // one at a time

    auto flush = host.pending_file_icons[0];
    host.pending_file_icons.clear();
    flush();  // nothing loaded, but rows were skipped: the page rebuilds and asks for them
    REQUIRE(host.file_icon_requests.size() == 2);
    const auto& asked = host.file_icon_requests[1];
    CHECK(std::find(asked.begin(), asked.end(), explorer) != asked.end());
    CHECK(std::find(asked.begin(), asked.end(), L"C:\\Nowhere\\gone.exe") == asked.end());  // cached failure
}

TEST_CASE("ui: the Games page opens with its list and has no group on Capture") {
    FakeScreenHost host;
    SettingsScreen screen(&host);
    const auto config = DefaultConfigJson();
    screen.InitSchema(RealSpecs(), &config);
    screen.SetActiveRail(kRailCapture);
    screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
    CHECK(screen.RowCountForGroup(L"Games") == 0);
    screen.SetActiveRail(kRailGames);
    REQUIRE(screen.GamesListForTest());
}


// --- Settings, drawn and clicked as a whole ------------------------------------------------------
// The screen-level tests above call the staging functions directly. These go through the path a
// real click takes: Layout, Draw (where the rail, sub-tabs, segmented controls and toggles cache
// their hit rects) and SettingsScreen::OnMouseDown/Up at a point inside what was drawn. A widget
// that lays out but never draws, or draws somewhere its hit test doesn't look, fails here.

namespace {
constexpr float kViewW = 1280.0f, kViewH = 800.0f;

struct DrawnSettings {
    HeadlessDraw   h{static_cast<UINT>(kViewW), static_cast<UINT>(kViewH)};
    FakeScreenHost host;
    SettingsScreen screen{&host};
    std::vector<FieldSpec> specs = RealSpecs();

    DrawnSettings() {
        const auto config = DefaultConfigJson();
        screen.InitSchema(specs, &config);
    }
    void Frame() {
        screen.Layout(D2D1::RectF(0, 0, kViewW, kViewH), 1.0f);
        h.Draw(screen);
    }
    void Click(D2D1_POINT_2F pt) {
        screen.OnMouseDown(pt);
        screen.OnMouseUp(pt);
        Frame();
    }
    static D2D1_POINT_2F Center(const D2D1_RECT_F& r) { return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f); }
    // Where `r` (content space, as row bounds are kept) is on screen at the current scroll.
    D2D1_RECT_F OnScreen(const D2D1_RECT_F& r) const {
        const float y = screen.ScrollYForTest();
        return D2D1::RectF(r.left, r.top - y, r.right, r.bottom - y);
    }
    // Scrolls until `key`'s control is fully inside the card above the Save bar; false if it can't be.
    bool BringIntoView(const std::string& key) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto control = screen.ControlForTest(key);
            if (!control || !control->visible || control->bounds.right <= control->bounds.left) return false;
            const D2D1_RECT_F c    = OnScreen(control->bounds);
            const D2D1_RECT_F view = screen.ViewBoundsForTest();
            if (c.top >= view.top && c.bottom <= view.bottom) return true;
            screen.ScrollForTest((std::max)(0.0f, screen.ScrollYForTest() + c.top - (view.top + 60.0f)));
            Frame();
        }
        return false;
    }
};

// Every (rail, sub-tab) page the Settings panel has, by index.
std::vector<std::pair<int, int>> AllPages(SettingsScreen& screen) {
    std::vector<std::pair<int, int>> pages;
    for (int rail = 0; rail < kRailCount; ++rail) {
        screen.SetActiveRail(rail);
        screen.Layout(D2D1::RectF(0, 0, kViewW, kViewH), 1.0f);
        const TabBar* bar = screen.SubTabBarForTest();
        const int tabs = bar ? static_cast<int>(bar->tabs.size()) : 1;
        for (int t = 0; t < (std::max)(tabs, 1); ++t) pages.emplace_back(rail, t);
    }
    return pages;
}
}  // namespace

TEST_CASE("ui: Settings routes a real click to every rail tile and every sub-tab after Draw") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    d.Frame();
    REQUIRE(d.screen.RailForTest().items.size() == kRailCount);
    for (int i = kRailCount - 1; i >= 0; --i) {
        CAPTURE(i);
        d.Click(DrawnSettings::Center(d.screen.RailForTest().ItemRect(i)));
        CHECK(d.screen.RailForTest().selected == i);
        CHECK_FALSE(d.screen.PageChildrenForTest().empty());

        // Each sub-tab, by sweeping across the strip the way a pointer would.
        const TabBar* bar = d.screen.SubTabBarForTest();
        if (!bar) continue;
        const int tabs = static_cast<int>(bar->tabs.size());
        std::set<int> reached{bar->selected};
        const float y = (bar->bounds.top + bar->bounds.bottom) * 0.5f;
        for (float x = bar->bounds.left + 2.0f; x < bar->bounds.right; x += 8.0f) {
            d.Click(D2D1::Point2F(x, y));
            bar = d.screen.SubTabBarForTest();
            REQUIRE(bar);
            reached.insert(bar->selected);
        }
        CHECK(static_cast<int>(reached.size()) == tabs);
        CHECK(d.screen.RailForTest().selected == i);  // a sub-tab click never changes the page
    }
}

TEST_CASE("ui: every on/off row on every page flips under a real click, and the Save bar saves them") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    size_t flipped = 0;
    std::set<std::string> tried;
    for (const auto& [rail, tab] : AllPages(d.screen)) {
        d.screen.SetActiveRail(rail);
        d.screen.SetActiveSubTab(tab);
        d.screen.ScrollForTest(0);
        d.Frame();
        for (const FieldSpec& spec : d.specs) {
            if (spec.kind != FieldKind::Bool || tried.count(spec.key)) continue;
            if (!d.BringIntoView(spec.key)) continue;
            tried.insert(spec.key);
            CAPTURE(spec.key);
            const auto* before_v = d.screen.StagedValueForTest(spec.key);
            const auto* committed = d.screen.CurrentConfigValueForTest(spec.key);
            const bool before = before_v ? before_v->b : (committed && committed->b);
            d.Click(DrawnSettings::Center(d.OnScreen(d.screen.ControlForTest(spec.key)->bounds)));
            const auto* after = d.screen.StagedValueForTest(spec.key);
            const bool now = after ? after->b : (committed && committed->b);
            CHECK(now != before);
            if (now != before) ++flipped;
        }
    }
    MESSAGE("on/off rows clicked: " << tried.size());
    CHECK(tried.size() >= 5);
    CHECK(flipped == tried.size());

    // The bar is drawn over the card; its Save button, clicked, sends the batch once.
    REQUIRE(d.screen.HasPendingChangesForTest());
    const int before = d.host.pipe.counts["set_config"];
    const auto& save = d.screen.SettingsBarForTest().save;
    d.Click(DrawnSettings::Center(save.bounds));
    CHECK(d.host.pipe.counts["set_config"] == before + 1);
    CHECK_FALSE(d.screen.HasPendingChangesForTest());
}

TEST_CASE("ui: a Select opens its menu under a real click and picking from it stages the choice") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    size_t picked = 0;
    std::set<std::string> tried;
    for (const auto& [rail, tab] : AllPages(d.screen)) {
        d.screen.SetActiveRail(rail);
        d.screen.SetActiveSubTab(tab);
        d.screen.ScrollForTest(0);
        d.Frame();
        for (const FieldSpec& spec : d.specs) {
            if (tried.count(spec.key) || !std::dynamic_pointer_cast<SelectControl>(d.screen.ControlForTest(spec.key))) continue;
            if (!d.BringIntoView(spec.key)) continue;
            tried.insert(spec.key);
            CAPTURE(spec.key);
            d.host.dropdown_options.clear();
            d.host.dropdown_pick = nullptr;
            d.Click(DrawnSettings::Center(d.OnScreen(d.screen.ControlForTest(spec.key)->bounds)));
            REQUIRE(d.host.dropdown_pick);
            REQUIRE(d.host.dropdown_options.size() >= 2);
            CHECK(d.screen.StagedValueForTest(spec.key) == nullptr);  // opening stages nothing
            // Another option than the current one, handed back as the real menu does.
            const int other = d.host.dropdown_selected == 0 ? 1 : 0;
            auto pick = d.host.dropdown_pick;
            pick(other);
            if (d.host.dropdown_close) d.host.dropdown_close();
            d.Frame();
            if (d.screen.StagedValueForTest(spec.key)) ++picked;
        }
    }
    MESSAGE("selects clicked: " << tried.size() << ", staged: " << picked);
    REQUIRE(tried.size() >= 1);
    CHECK(picked == tried.size());
    CHECK(d.screen.HasPendingChangesForTest());
}

TEST_CASE("ui: the Save bar's Discard, clicked, drops a staged edit without a set_config") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    std::string key;
    for (const auto& [rail, tab] : AllPages(d.screen)) {
        d.screen.SetActiveRail(rail);
        d.screen.SetActiveSubTab(tab);
        d.screen.ScrollForTest(0);
        d.Frame();
        for (const FieldSpec& spec : d.specs)
            if (spec.kind == FieldKind::Bool && d.BringIntoView(spec.key)) { key = spec.key; break; }
        if (!key.empty()) break;
    }
    REQUIRE_FALSE(key.empty());
    d.Click(DrawnSettings::Center(d.OnScreen(d.screen.ControlForTest(key)->bounds)));
    REQUIRE(d.screen.HasPendingChangesForTest());
    const int before = d.host.pipe.counts["set_config"];
    const auto& discard = d.screen.SettingsBarForTest().discard;
    d.Click(DrawnSettings::Center(discard.bounds));
    CHECK_FALSE(d.screen.HasPendingChangesForTest());
    CHECK(d.screen.StagedValueForTest(key) == nullptr);
    CHECK(d.host.pipe.counts["set_config"] == before);
}

TEST_CASE("ui: a drawn stepper increments, decrements, clamps and saves or discards its value") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    const std::string key = "encode.keyframe_sec";
    d.screen.SetActiveRail(kRailAdvanced);
    d.screen.SetActiveSubTab(0);
    d.Frame();
    REQUIRE(d.BringIntoView(key));
    auto stepper = [&] { return std::dynamic_pointer_cast<StepperControl>(d.screen.ControlForTest(key)); };
    REQUIRE(stepper());
    const double initial = stepper()->value;
    const double min = stepper()->min, max = stepper()->max, step = stepper()->step;
    REQUIRE(initial > min);
    REQUIRE(initial < max);
    REQUIRE(step > 0);
    auto click_side = [&](int direction) {
        REQUIRE(d.BringIntoView(key));
        const auto r = d.OnScreen(stepper()->bounds);
        const float inset = (r.bottom - r.top) * 0.5f;
        d.Click(D2D1::Point2F(direction < 0 ? r.left + inset : r.right - inset, (r.top + r.bottom) * 0.5f));
    };
    const int before = d.host.pipe.counts["set_config"];
    click_side(1);
    CHECK(stepper()->value == initial + step);
    REQUIRE(d.screen.StagedValueForTest(key));
    CHECK(d.screen.StagedValueForTest(key)->num == initial + step);
    CHECK(d.screen.CurrentConfigValueForTest(key)->num == initial);
    CHECK(d.host.pipe.counts["set_config"] == before);
    click_side(-1);
    CHECK(stepper()->value == initial);

    // Click all the way through each endpoint, including two extra presses at the limit.
    const int clicks = static_cast<int>(std::ceil((max - min) / step)) + 2;
    for (int i = 0; i < clicks; ++i) click_side(-1);
    CHECK(stepper()->value == min);
    REQUIRE(d.screen.StagedValueForTest(key));
    CHECK(d.screen.StagedValueForTest(key)->num == min);
    for (int i = 0; i < clicks; ++i) click_side(1);
    CHECK(stepper()->value == max);
    REQUIRE(d.screen.StagedValueForTest(key));
    CHECK(d.screen.StagedValueForTest(key)->num == max);
    CHECK(d.host.pipe.counts["set_config"] == before);

    d.Click(DrawnSettings::Center(d.screen.SettingsBarForTest().save.bounds));
    CHECK(d.host.pipe.counts["set_config"] == before + 1);
    const auto* patch = d.host.pipe.last_set_config.Find("config");
    REQUIRE(patch);
    REQUIRE(patch->Find(key));
    CHECK(patch->Find(key)->num == max);
    CHECK(d.screen.CurrentConfigValueForTest(key)->num == max);
    CHECK_FALSE(d.screen.HasPendingChangesForTest());

    click_side(-1);
    CHECK(stepper()->value == max - step);
    REQUIRE(d.screen.StagedValueForTest(key));
    CHECK(d.screen.StagedValueForTest(key)->num == max - step);
    d.Click(DrawnSettings::Center(d.screen.SettingsBarForTest().discard.bounds));
    CHECK(stepper()->value == max);
    CHECK(d.screen.CurrentConfigValueForTest(key)->num == max);
    CHECK(d.screen.StagedValueForTest(key) == nullptr);
    CHECK_FALSE(d.screen.HasPendingChangesForTest());
    CHECK(d.host.pipe.counts["set_config"] == before + 1);
}

TEST_CASE("ui: a real click into the file name focuses it; a click elsewhere leaves it with the edit kept") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    d.screen.SetActiveRail(kRailReplay);
    d.screen.SetActiveSubTab(1);
    d.Frame();
    REQUIRE(d.BringIntoView("output.filename"));
    auto field = d.screen.ControlForTest("output.filename");
    REQUIRE(field);
    const D2D1_RECT_F f = d.OnScreen(field->bounds);
    d.Click(D2D1::Point2F(f.right - 60.0f, f.top + 8.0f));  // the field line, by the .mp4 suffix
    CHECK(d.screen.IsEditingText());
    for (wchar_t c : std::wstring(L"x")) d.screen.OnChar(c);
    const JsonValue* staged = d.screen.StagedValueForTest("output.filename");
    REQUIRE(staged);
    CHECK(staged->str == "{game} {date} {time}{monitor_suffix}x");

    const D2D1_RECT_F folder = d.OnScreen(d.screen.ControlForTest("output.root")->bounds);
    d.Click(D2D1::Point2F(folder.left - 200.0f, (folder.top + folder.bottom) * 0.5f));  // the Folder row's text
    CHECK(!d.screen.IsEditingText());
    d.screen.OnKeyDown(VK_ESCAPE);  // no edit left to revert
    staged = d.screen.StagedValueForTest("output.filename");
    REQUIRE(staged);
    CHECK(staged->str == "{game} {date} {time}{monitor_suffix}x");
}

TEST_CASE("ui: a segmented row stages the option under a real click, first and last segment") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    // Options and the chosen index, or {0, -1}.
    auto state = [&](const std::string& key) -> std::pair<int, int> {
        auto control = d.screen.ControlForTest(key);
        if (auto* seg = dynamic_cast<SegmentedControl*>(control.get()))
            return {static_cast<int>(seg->options.size()), seg->selected};
        return {0, -1};
    };
    size_t checked = 0;
    for (const auto& [rail, tab] : AllPages(d.screen)) {
        d.screen.SetActiveRail(rail);
        d.screen.SetActiveSubTab(tab);
        d.screen.ScrollForTest(0);
        d.Frame();
        for (const FieldSpec& spec : d.specs) {
            if (!d.BringIntoView(spec.key)) continue;
            const int n = state(spec.key).first;
            if (n < 2) continue;
            CAPTURE(spec.key);
            for (int target : {n - 1, 0}) {
                CAPTURE(target);
                auto control = d.screen.ControlForTest(spec.key);
                REQUIRE(control);
                const D2D1_RECT_F c = d.OnScreen(control->bounds);
                const float w = (c.right - c.left) / static_cast<float>(n);
                d.Click(D2D1::Point2F(c.left + w * (static_cast<float>(target) + 0.5f), (c.top + c.bottom) * 0.5f));
                CHECK(state(spec.key).second == target);
            }
            ++checked;
            d.screen.DiscardAllForTest();
            d.Frame();
        }
    }
    MESSAGE("segmented rows clicked: " << checked);
    CHECK(checked >= 3);
}

// Notifications' own corner picture stages the corner a click lands on.
TEST_CASE("ui: a click on a corner of the notifications picture stages that corner") {
    DrawnSettings d;
    REQUIRE(d.h.ok);
    d.screen.SetActiveRail(kRailAppearance);
    d.screen.SetActiveSubTab(2);
    d.Frame();
    auto corner = d.screen.ControlForTest("ui.toast_position");
    REQUIRE(corner);
    const D2D1_RECT_F b = d.OnScreen(corner->bounds);
    const float w = b.right - b.left;
    d.Click(D2D1::Point2F(b.left + w * 0.15f, b.top + w * 9.0f / 16.0f * 0.15f));  // the top-left spot
    const JsonValue* staged = d.screen.StagedValueForTest("ui.toast_position");
    REQUIRE(staged);
    CHECK(staged->str == "top_left");
}

// The Clips screen lists only the sandbox in an isolated instance (tests, --drive), whatever
// output.root says: a scripted Delete or trim must never reach the user's clips.
TEST_CASE("clips: an isolated instance lists its own clips folder, grouped by game folder") {
    const std::wstring sandbox = ombra::Instance().data_dir + L"\\clips";
    CreateDirectoryW(sandbox.c_str(), nullptr);
    CreateDirectoryW((sandbox + L"\\VALORANT").c_str(), nullptr);
    const ombra::test::H264Fixture& fx = ombra::test::StandardFixture();
    std::vector<ombra::MuxAu> aus;
    for (size_t i = 0; i < 60; ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
    for (const wchar_t* name : {L"\\Desktop 2026-10-19 10-00-00.mp4", L"\\VALORANT\\VALORANT 2026-10-19 11-00-00.mp4"})
        REQUIRE(ombra::MuxWriteMp4(sandbox + name, fx.sps_pps, fx.width, fx.height, fx.fps, aus));

    FakeScreenHost host;
    ClipsScreen clips(&host);
    clips.Open(L"C:\\Users\\Owner\\Videos\\Ombra");  // never listed: this is an isolated instance
    CHECK(clips.Root() == sandbox);
    REQUIRE(clips.ClipCount() == 2);

    // No video device in a fake host: the player view still opens (with its "unavailable" text)
    // and Esc returns to the grid.
    clips.OpenClipForTest(0);
    CHECK(clips.PlayerOpen());
    CHECK(clips.OnKeyDown(VK_ESCAPE));
    CHECK_FALSE(clips.PlayerOpen());

    // A refresh's reply landing with a clip open: the player follows it, or closes if it's gone.
    clips.OpenClipForTest(1);
    const std::wstring open = clips.OpenPathForTest();
    clips.ApplyListForTest({1, 0});
    CHECK(clips.OpenPathForTest() == open);
    clips.ApplyListForTest({1});  // its new index 0 left, so the other file remains
    CHECK_FALSE(clips.PlayerOpen());
    CHECK(clips.ClipCount() == 1);
}

TEST_CASE("clips: frame steps count frames, never round onto the same one") {
    CHECK(FrameAt(0.0, 30.0) == 0);
    CHECK(FrameAt(10.0, 30.0) == 300);                 // exactly on a frame's start
    CHECK(FrameAt(10.0 + 0.5 / 30.0, 30.0) == 300);    // inside it
    CHECK(FrameAt(301.0 / 30.0, 30.0) == 301);         // 1/30 doesn't round below its frame
    for (int64_t k : {0, 1, 59, 60, 299, 300, 1799}) {
        CHECK(FrameAt(FrameSeekTime(k, 60.0), 60.0) == k);
        CHECK(FrameAt(FrameSeekTime(k, 30.0), 30.0) == k);
    }
}

// Every shortcut is dispatched from the one table the shortcuts list draws from.
TEST_CASE("clips: the shortcut table has one action per key") {
    std::set<UINT> keys;
    for (size_t i = 0; i < kClipShortcutCount; ++i) CHECK(keys.insert(kClipShortcuts[i].vk).second);
}

// Replace: the trim takes the original's name and times; the original goes (here, in the sandbox,
// deleted: the native-window sink stands in for the Recycle Bin).
TEST_CASE("clips: the grid sorts without moving the list underneath") {
    const std::wstring sandbox = ombra::Instance().data_dir + L"\\clips";
    CreateDirectoryW(sandbox.c_str(), nullptr);
    const ombra::test::H264Fixture& fx = ombra::test::StandardFixture();
    std::vector<ombra::MuxAu> aus;
    for (size_t i = 0; i < fx.aus.size(); ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
    std::vector<std::wstring> made;
    for (int i = 0; i < 4; ++i) {
        made.push_back(sandbox + L"\\Sort " + std::to_wstring(i) + L".mp4");
        REQUIRE(ombra::MuxWriteMp4(made.back(), fx.sps_pps, fx.width, fx.height, fx.fps, aus));
    }
    FakeScreenHost host;
    ClipsScreen clips(&host);
    clips.Open(L"");
    const int n = static_cast<int>(clips.ClipCount());
    REQUIRE(n >= 4);
    const std::vector<std::wstring> newest = clips.GridNamesForTest(0);  // list order
    REQUIRE(newest.size() == static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        clips.SetClipFactsForTest(i, static_cast<uint64_t>(i) + 1, static_cast<double>(i), i % 2 ? L"apex" : L"Zelda");

    std::vector<std::wstring> reversed(newest.rbegin(), newest.rend());
    CHECK(clips.GridNamesForTest(1) == reversed);  // Oldest
    CHECK(clips.GridNamesForTest(2) == reversed);  // Largest: the facts grow down the list
    CHECK(clips.GridNamesForTest(3) == reversed);  // Longest: index 0's unread duration goes last
    std::vector<std::wstring> by_game;
    for (int want : {1, 0})
        for (int i = 0; i < n; ++i)
            if (i % 2 == want) by_game.push_back(newest[i]);
    CHECK(clips.GridNamesForTest(4) == by_game);  // case-insensitive, newest first inside a game
    CHECK(clips.GridNamesForTest(0) == newest);
    clips.Leave();
    for (const std::wstring& p : made) DeleteFileW(p.c_str());
}

TEST_CASE("clips: Replace original swaps in the trim under the original's name and times") {
    const std::wstring sandbox = ombra::Instance().data_dir + L"\\clips";
    CreateDirectoryW(sandbox.c_str(), nullptr);
    const std::wstring path = sandbox + L"\\Replace 2026-11-06 10-00-00.mp4";
    const ombra::test::H264Fixture& fx = ombra::test::StandardFixture();
    std::vector<ombra::MuxAu> aus;
    for (size_t i = 0; i < fx.aus.size(); ++i) aus.push_back({fx.aus[i].annexb, fx.aus[i].idr, i});
    REQUIRE(ombra::MuxWriteMp4(path, fx.sps_pps, fx.width, fx.height, fx.fps, aus));
    FILETIME old_write{};
    {
        HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        SYSTEMTIME st{2026, 10, 0, 1, 12, 0, 0, 0};
        SystemTimeToFileTime(&st, &old_write);
        SetFileTime(h, nullptr, nullptr, &old_write);
        CloseHandle(h);
    }
    std::vector<std::wstring> blocked;
    SetNativeWindowSink([&](const std::wstring& what) { blocked.push_back(what); });

    FakeScreenHost host;
    ClipsScreen clips(&host);
    clips.Open(L"");
    int index = -1;
    for (int i = 0; i < static_cast<int>(clips.ClipCount()); ++i) {
        clips.OpenClipForTest(i);
        if (std::filesystem::path(clips.OpenPathForTest()).filename() == std::filesystem::path(path).filename()) index = i;
    }
    REQUIRE(index >= 0);
    clips.OpenClipForTest(index);
    const double dur = clips.ClipDurationForTest(index);
    REQUIRE(dur > 2.5);
    clips.SetTrimForTest(1.0, 2.0);
    host.modal_count = 0;
    clips.OpenSaveMenuForTest();
    REQUIRE(host.dropdown_options.size() == 2);
    CHECK(host.dropdown_options[0] == L"Save trimmed");
    CHECK(host.dropdown_options[1] == L"Replace original");
    host.dropdown_pick(1);
    CHECK(host.modal_count == 1);  // confirmed (the fake host confirms every modal)
    SetNativeWindowSink(nullptr);

    CHECK(blocked.size() == 1);
    ombra::ClipInfo info;
    REQUIRE(ombra::ReadClipInfo(path, info));
    CHECK(info.duration_s == doctest::Approx(1.0).epsilon(0.05));
    WIN32_FILE_ATTRIBUTE_DATA a{};
    REQUIRE(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a));
    CHECK(CompareFileTime(&a.ftLastWriteTime, &old_write) == 0);
    // No "(trim)" file left beside it, and the player is back on the (now shorter) clip.
    CHECK(GetFileAttributesW((sandbox + L"\\Replace 2026-11-06 10-00-00 (trim).mp4").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(std::filesystem::path(clips.OpenPathForTest()).filename() == std::filesystem::path(path).filename());
    CHECK(clips.StatusForTest().rfind(L"Replaced the original", 0) == 0);
}

// Not a check: the design-language inventory. Walks every Settings
// page and writes, per setting, its schema shape and the widget that actually renders it, and per
// page, which widget types it is built from. Run on demand:
//   ombra-tests -tc="ui inventory*" --no-skip   -> <test output>\ui-inventory.md
TEST_CASE("ui inventory: every setting's shape and the widget that renders it" * doctest::skip()) {
    FakeScreenHost host;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    REQUIRE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dw.GetAddressOf()))));
    REQUIRE(host.GetTheme().InitFonts(dw.Get()));
    SettingsScreen screen(&host);
    const auto specs = RealSpecs();
    const auto config = DefaultConfigJson();
    screen.InitSchema(specs, &config);

    auto utf8 = [](const std::wstring& w) {
        std::string out(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
        return out;
    };
    auto type_name = [](const Widget& w) {
        std::string n = typeid(w).name();
        for (const char* strip : {"class ", "ombra_ui::"})
            for (size_t at; (at = n.find(strip)) != std::string::npos;) n.erase(at, std::strlen(strip));
        return n;
    };
    // Everything reachable from a page, containers included.
    std::function<void(const std::shared_ptr<Widget>&, std::vector<const Widget*>&)> walk =
        [&](const std::shared_ptr<Widget>& w, std::vector<const Widget*>& out) {
            if (!w) return;
            out.push_back(w.get());
            if (auto f = std::dynamic_pointer_cast<FlexWidget>(w)) for (const auto& it : f->items) walk(it.widget, out);
            if (auto c = std::dynamic_pointer_cast<CardContainer>(w)) {
                for (const auto& a : c->actions) walk(a, out);
                for (const auto& r : c->rows) walk(r, out);
            }
            if (auto r = std::dynamic_pointer_cast<SettingRow>(w)) for (const auto& k : r->controls) walk(k, out);
        };

    struct Page { std::string name; std::map<std::string, int> types; };
    std::vector<Page> pages;
    std::map<std::string, std::vector<std::string>> where;  // key -> "Page: Widget"
    for (int rail = 0; rail < kRailCount; ++rail) {
        screen.SetActiveRail(rail);
        const int subtabs = screen.SubTabBarForTest() ? static_cast<int>(screen.SubTabBarForTest()->tabs.size()) : 0;
        for (int sub = 0; sub < (std::max)(1, subtabs); ++sub) {
            if (subtabs) screen.SetActiveSubTab(sub);
            screen.Layout(D2D1::RectF(0, 0, 1280, 800), 1.0f);
            Page page;
            page.name = utf8(SettingsRailLabel(rail));
            if (const auto* bar = screen.SubTabBarForTest(); bar && sub < static_cast<int>(bar->tabs.size()))
                page.name += " > " + utf8(bar->tabs[sub]);
            std::vector<const Widget*> all;
            for (const auto& w : screen.PageChildrenForTest()) walk(w, all);
            const std::set<const Widget*> on_page(all.begin(), all.end());
            for (const Widget* w : all) ++page.types[type_name(*w)];
            for (const auto& spec : specs) {
                auto c = screen.ControlForTest(spec.key);
                if (c && on_page.count(c.get())) where[spec.key].push_back(page.name + ": " + type_name(*c));
            }
            pages.push_back(std::move(page));
        }
    }

    const std::wstring path = ombra::test::TestOutputPath(L"ui-inventory.md");
    FILE* f = _wfopen(path.c_str(), L"w");
    REQUIRE(f);
    std::fprintf(f, "# Settings inventory (generated by ui_tests.cpp \"ui inventory\")\n\n");
    auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& x : v) out += (out.empty() ? "" : "|") + x;
        return out;
    };
    const char* kinds[] = {"Bool", "Int", "Double", "String", "Enum", "Action"};
    const char* forms[] = {"Custom", "Hidden"};
    std::fprintf(f, "| key | title | kind | choices / range | kFieldForms | rendered as |\n|---|---|---|---|---|---|\n");
    for (const auto& spec : specs) {
        if (!spec.implemented) continue;
        std::string shape = !spec.enum_options.empty() ? join(spec.enum_options) : join(spec.presets);
        if (spec.min_val != 0 || spec.max_val != 0) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s%g..%g", shape.empty() ? "" : " ", spec.min_val, spec.max_val);
            shape += buf;
        }
        if (!spec.unit.empty()) shape += " " + spec.unit;
        FieldForm form = FieldForm::Hidden;
        const bool has_form = FieldFormFor(spec.key, &form);
        std::string at;
        for (const auto& w : where[spec.key]) at += (at.empty() ? "" : "; ") + w;
        if (at.empty()) at = form == FieldForm::Hidden ? "-" : "(page-specific, not registered)";
        std::fprintf(f, "| %s | %s | %s | %s | %s | %s |\n", spec.key.c_str(), spec.display_name.c_str(),
                     kinds[static_cast<int>(spec.kind)], shape.c_str(),
                     has_form ? forms[static_cast<int>(form)] : "?", at.c_str());
    }
    std::fprintf(f, "\n# Widget types per page\n\n");
    for (const auto& p : pages) {
        std::string list;
        for (const auto& [t, n] : p.types) list += (list.empty() ? "" : ", ") + t + " " + std::to_string(n);
        std::fprintf(f, "- **%s**: %s\n", p.name.c_str(), list.c_str());
    }
    std::fclose(f);
    MESSAGE("wrote ", utf8(path));
}


namespace {
void PumpHostMessages(int ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(5);
    }
}
}  // namespace

TEST_CASE("overlay: Esc on a one-button modal acknowledges it") {
    OverlayHost host;
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, nullptr));
    int confirmed = 0, cancelled = 0;
    ModalButtons one;
    one.confirm = L"OK";
    one.cancel = L"";
    host.ShowModal(L"Title", L"Message", false, [&] { ++confirmed; }, [&] { ++cancelled; }, one);
    host.DispatchForDriver(WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(confirmed == 1);
    CHECK(cancelled == 0);

    confirmed = 0;
    host.ShowModal(L"Title", L"Message", false, [&] { ++confirmed; }, [&] { ++cancelled; }, ModalButtons{});
    host.DispatchForDriver(WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(confirmed == 0);
    CHECK(cancelled == 1);
}

TEST_CASE("overlay: a lost graphics device closes the overlay") {
    OverlayHost host;
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, nullptr));
    host.RenderForDriver();
    PumpHostMessages(20);
    REQUIRE(host.HeadlessOpen());
    host.ForceRenderResultForTest(DXGI_ERROR_DEVICE_REMOVED);
    host.RenderForDriver();
    PumpHostMessages(50);
    CHECK_FALSE(host.HeadlessOpen());
}

TEST_CASE("overlay: losing the mouse mid-press ends the press") {
    OverlayHost host;
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, nullptr));
    host.RenderForDriver();
    const LPARAM empty = MAKELPARAM(5, 700);
    host.DispatchForDriver(WM_LBUTTONDOWN, MK_LBUTTON, empty);
    CHECK(host.MousePressedForTest());
    host.DispatchForDriver(WM_CAPTURECHANGED, 0, 0);
    CHECK_FALSE(host.MousePressedForTest());
    host.DispatchForDriver(WM_LBUTTONDOWN, MK_LBUTTON, empty);
    host.DispatchForDriver(WM_LBUTTONUP, 0, empty);
    CHECK_FALSE(host.MousePressedForTest());
}

TEST_CASE("overlay: going Home with the engine stalled neither blocks the UI nor keeps claiming ready") {
    PipeClient client;  // no engine at this instance's pipe: every request ends with no reply
    OverlayHost host;
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, &client));
    host.SwitchScreen(ActiveScreen::Settings);
    const ULONGLONG before = GetTickCount64();
    host.SwitchScreen(ActiveScreen::Home);
    CHECK(GetTickCount64() - before < 500);  // a request here would wait out its 1.5 s connect
    REQUIRE(host.GetHomeScreen());
    for (int i = 0; i < 100 && host.GetHomeScreen()->ReplayDetailForTest() != L"Not responding"; ++i)
        PumpHostMessages(50);
    CHECK(host.GetHomeScreen()->ReplayDetailForTest() == L"Not responding");
}

namespace {
// An engine at this instance's pipe that answers every message `delay_ms` late, or never (< 0).
class SlowEngine {
public:
    explicit SlowEngine(int delay_ms) : delay_ms_(delay_ms) {
        for (int i = 0; i < 4; ++i) threads_.emplace_back([this] { Serve(); });
    }
    // Every instance gone before this returns: a later test expects no engine at this pipe.
    ~SlowEngine() {
        stop_ = true;
        for (auto& t : threads_) {
            while (WaitForSingleObject(t.native_handle(), 10) == WAIT_TIMEOUT) CancelSynchronousIo(t.native_handle());
            t.join();
        }
    }

private:
    void Serve() {
        HANDLE pipe = CreateNamedPipeW(ombra::Instance().pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                       4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        while (!stop_) {  // a client at a time, as long as this lasts: the overlay's status polls keep coming
            ConnectNamedPipe(pipe, nullptr);
            std::string line;
            char c = 0;
            DWORD n = 0;
            while (!stop_ && ReadFile(pipe, &c, 1, &n, nullptr) && n == 1) {
                if (c != '\n') {
                    line += c;
                    continue;
                }
                if (delay_ms_ < 0) continue;
                Sleep(static_cast<DWORD>(delay_ms_));
                const ombra::JsonValue req = ombra::ParseJson(line);
                line.clear();
                ombra::JsonValue resp = ombra::JsonValue::MakeObject();
                if (const auto* id = req.Find("id")) resp.Set("id", *id);
                resp.Set("ok", ombra::JsonValue::MakeBool(true));
                if (const auto* type = req.Find("type"); type && type->str == "get_config") {
                    ombra::JsonValue config = ombra::JsonValue::MakeObject();
                    config.Set("buffer.seconds", ombra::JsonValue::MakeNum(30));
                    resp.Set("config", std::move(config));
                }
                const std::string out = ombra::WriteJsonCompact(resp) + "\n";
                DWORD written = 0;
                WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    int delay_ms_;
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};
}  // namespace

TEST_CASE("overlay: opening on a stalled engine doesn't block, says so, and fills in if it answers") {
    SlowEngine engine(150);  // 4 round trips (hello + 3 requests) at 150 ms: past the grace, then answered
    Sleep(50);
    PipeClient client;
    REQUIRE(client.Connect(1000));
    OverlayHost host;
    const ULONGLONG before = GetTickCount64();
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, &client));
    CHECK(GetTickCount64() - before < 1000);  // three blocking requests took 450 ms+ on a good day, 9 s stalled
    REQUIRE(host.GetHomeScreen());
    CHECK(host.GetHomeScreen()->ReplayDetailForTest() == L"Not responding");
    for (int i = 0; i < 100 && host.GetHomeScreen()->ReplayDetailForTest() == L"Not responding"; ++i)
        PumpHostMessages(50);
    CHECK(host.GetHomeScreen()->ReplayDetailForTest() == L"Last 30 s ready");
}

TEST_CASE("overlay: a healthy engine's answers are in the first frame, the setup's too") {
    SlowEngine engine(0);
    Sleep(50);
    PipeClient client;
    REQUIRE(client.Connect(1000));
    OverlayHost host;
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, &client));
    REQUIRE(host.GetHomeScreen());
    CHECK(host.GetHomeScreen()->ReplayDetailForTest() == L"Last 30 s ready");
    host.SwitchScreen(ActiveScreen::Setup);
    CHECK(host.ActiveScreenForTest() == ActiveScreen::Setup);  // on return, with nothing pumped
}

TEST_CASE("overlay: a setup asked for with the engine silent keeps the UI going and opens when it gives up") {
    SlowEngine engine(-1);
    Sleep(50);
    PipeClient client;
    REQUIRE(client.Connect(1000));
    OverlayHost host;
    REQUIRE(host.StartHeadless(1280, 720, 1.0f, &client));
    const ULONGLONG before = GetTickCount64();
    host.SwitchScreen(ActiveScreen::Setup);
    CHECK(GetTickCount64() - before < 1000);  // was up to 6 s
    CHECK(host.ActiveScreenForTest() == ActiveScreen::Home);
    for (int i = 0; i < 120 && host.ActiveScreenForTest() != ActiveScreen::Setup; ++i) PumpHostMessages(50);
    CHECK(host.ActiveScreenForTest() == ActiveScreen::Setup);
}

TEST_CASE("ui: a sent record says whether it left the PC, and why not in plain words") {
    namespace sr = ombra::sent_records;
    sr::Record r;
    r.kind = sr::Kind::Crash;
    r.summary = "Ombra crashed: access violation";
    r.status = sr::Status::Sent;
    r.result = "HTTP 200";
    SentList::Item it = SentItem(r);
    CHECK(it.title == L"Crash report");
    CHECK(it.status == L"Sent");
    CHECK(it.tone == SettingRow::Tone::Good);
    r.status = sr::Status::Failed;
    r.result = "WinHTTP error 12007";
    CHECK(SentItem(r).status == L"Not sent: no connection");
    CHECK(SentItem(r).tone == SettingRow::Tone::Warning);
    r.result = "HTTP 429";
    CHECK(SentItem(r).status == L"Not sent: too many reports, tried again later");
    r.result = "HTTP 503";
    CHECK(SentItem(r).status == L"Not sent: server error (HTTP 503)");
    r.status = sr::Status::Unconfirmed;
    r.result.clear();
    CHECK(SentItem(r).status == L"Not confirmed");
    r.kind = sr::Kind::DailyUsage;
    r.day = "2026-11-10";
    CHECK(SentItem(r).title == L"Daily usage");
    CHECK(SentItem(r).summary == L"Settings and performance for 2026-11-10");
}

TEST_CASE("ui: a sent record's preview keeps the first lines and never splits a character") {
    bool cut = true;
    CHECK(SentPreview("a\r\nb\r\n", 10, 1000, &cut) == L"a\nb");
    CHECK_FALSE(cut);
    CHECK(SentPreview("1\n2\n3\n4\n", 2, 1000, &cut) == L"1\n2");
    CHECK(cut);
    // A two-byte character; a byte cap of 2 lands inside it.
    CHECK(SentPreview("a\xC3\xA9z", 10, 2, &cut) == L"a");
    CHECK(cut);
}

}  // namespace ombra_ui::test
