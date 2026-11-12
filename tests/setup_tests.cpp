#include "ombra/setup_plan.h"

#include <doctest/doctest.h>

using namespace ombra;

namespace {

SetupFacts ThisLaptop() {
    SetupFacts f;
    f.width = 1920;
    f.height = 1080;
    f.display_hz = 120;
    f.gpus = {{"nvidia", true, 1, 1}, {"amd", true, 64, 16}, {"cpu", false, 1, 1}};
    return f;
}

const JsonValue* SourceOfKind(const JsonValue& sources, const char* kind) {
    for (const JsonValue& e : sources.arr)
        if (const JsonValue* k = e.Find("kind"); k && k->str == kind) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("setup: frame rate is the display's, capped at 60") {
    CHECK(RecommendedFramerate(120) == 60);
    CHECK(RecommendedFramerate(240) == 60);
    CHECK(RecommendedFramerate(60) == 60);
    CHECK(RecommendedFramerate(59) == 60);  // NTSC 59.94 reported as 59
    CHECK(RecommendedFramerate(50) == 50);
    CHECK(RecommendedFramerate(0) == 60);   // unknown
}

TEST_CASE("setup: the recommendation is High, 1 minute, mic on, memory") {
    const SetupPlan plan = RecommendSetup(ThisLaptop());
    CHECK(plan.framerate == 60);
    CHECK(plan.preset == "high");
    CHECK(plan.seconds == 60.0);
    CHECK(plan.mic);
    CHECK(plan.desktop_audio);
    CHECK(plan.one_track);
    CHECK(plan.storage == "ram");
    CHECK(plan.rate_control == "vbr");
    CHECK(plan.codec == "av1");
    CHECK(SetupBitrateKbps(plan, ThisLaptop()) == 20000);
}

TEST_CASE("setup: a PC with no AV1 encoder is recommended H.264 at H.264's bitrate") {
    SetupFacts f = ThisLaptop();
    f.gpus = {{"cpu", false, 1, 1}};
    const SetupPlan plan = RecommendSetup(f);
    CHECK(plan.codec == "h264");
    CHECK(SetupBitrateKbps(plan, f) == 25000);
}

TEST_CASE("setup: memory counts the video ring only while it lives in memory") {
    const SetupFacts f = ThisLaptop();
    SetupPlan plan = RecommendSetup(f);
    const int64_t in_memory = SetupMemoryBytes(plan, f);
    CHECK(in_memory > 200 * 1024 * 1024);
    plan.storage = "file";
    const int64_t on_disk = SetupMemoryBytes(plan, f);
    CHECK(on_disk < 32 * 1024 * 1024);
    CHECK(SetupDiskFileBytes(plan, f) > on_disk);
}

TEST_CASE("setup: the memory and disk figures follow constant quality's bigger ring") {
    const SetupFacts f = ThisLaptop();
    SetupPlan plan = RecommendSetup(f);
    const int64_t vbr_memory = SetupMemoryBytes(plan, f);
    const int64_t vbr_disk   = SetupDiskFileBytes(plan, f);
    plan.rate_control = "cqp";
    CHECK(SetupMemoryBytes(plan, f) > vbr_memory);
    CHECK(SetupDiskFileBytes(plan, f) == vbr_disk * 2);
}

TEST_CASE("setup: a custom bitrate overrides the preset") {
    SetupPlan plan = RecommendSetup(ThisLaptop());
    plan.preset = "custom";
    plan.custom_kbps = 12000;
    CHECK(SetupBitrateKbps(plan, ThisLaptop()) == 12000);
    const JsonValue patch = SetupPatch(plan, nullptr);
    CHECK(patch.Find("encode.bitrate_kbps")->num == 12000);
}

TEST_CASE("setup: error reports are the default sharing level and the patch writes the choice") {
    SetupPlan plan = RecommendSetup(ThisLaptop());
    CHECK(plan.sharing == "errors");
    CHECK(SetupPatch(plan, nullptr).Find("diagnostics.sharing")->str == "errors");
    plan.sharing = "off";
    CHECK(SetupPatch(plan, nullptr).Find("diagnostics.sharing")->str == "off");
}

TEST_CASE("setup: a fresh config gets Desktop and Mic in one track, the mic's group the Desktop's") {
    const JsonValue s = SetupAudioSources(nullptr, true, true, true);
    REQUIRE(s.arr.size() == 2);
    const JsonValue* d = SourceOfKind(s, "output_loopback");
    const JsonValue* m = SourceOfKind(s, "input");
    REQUIRE(d);
    REQUIRE(m);
    CHECK(d->Find("group_id")->str == m->Find("group_id")->str);
    CHECK(m->Find("enabled")->b);
}

TEST_CASE("setup: separate tracks give each source its own group") {
    const JsonValue s = SetupAudioSources(nullptr, true, true, false);
    CHECK(SourceOfKind(s, "output_loopback")->Find("group_id")->str != SourceOfKind(s, "input")->Find("group_id")->str);
}

TEST_CASE("setup: an existing app track and the Desktop's own id survive") {
    JsonValue existing = JsonValue::MakeArray();
    const auto entry = [](const char* id, const char* kind) {
        JsonValue e = JsonValue::MakeObject();
        e.Set("id", JsonValue::MakeStr(id));
        e.Set("kind", JsonValue::MakeStr(kind));
        return e;
    };
    existing.arr.push_back(entry("source0", "output_loopback"));
    existing.arr.push_back(entry("discord", "output_loopback"));
    existing.arr.push_back(entry("micA", "input"));
    const JsonValue s = SetupAudioSources(&existing, true, false, true);
    REQUIRE(s.arr.size() == 3);
    CHECK(s.arr[0].Find("group_id")->str == "source0");
    CHECK(s.arr[1].Find("id")->str == "discord");
    CHECK_FALSE(s.arr[1].Find("enabled"));  // not the setup's to change
    CHECK_FALSE(s.arr[2].Find("enabled")->b);
    CHECK(s.arr[2].Find("group_id")->str == "source0");
}

TEST_CASE("setup: the patch is accepted by the engine's own config parser") {
    SetupPlan plan = RecommendSetup(ThisLaptop());
    plan.storage = "file";
    plan.file_path = "E:\\Ombra\\ring.dat";
    plan.output_root = "D:\\Clips";
    const JsonValue patch = SetupPatch(plan, nullptr);
    const JsonValue current = SerializeConfigFlat(Config{});
    for (const auto& [key, value] : patch.obj) {
        INFO(key);
        if (key != "audio.sources") CHECK(current.Find(key.c_str()) != nullptr);  // a raw key: not in the flat form
    }
    const ConfigPreview preview = PreviewConfig(Config{}, patch, 1920, 1080, 120, {});
    CHECK(preview.fps == 60);
    CHECK(preview.bitrate_kbps > 0);
}
