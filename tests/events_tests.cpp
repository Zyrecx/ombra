#include "ombra/events.h"
#include "ombra/json.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace ombra::test {

namespace {

JsonValue Field(const char* key, bool value) {
    JsonValue f = JsonValue::MakeObject();
    f.Set(key, JsonValue::MakeBool(value));
    return f;
}

std::vector<std::string> Drain(Subscription& sub) {
    std::vector<std::string> lines;
    REQUIRE(sub.Take(lines));
    sub.Sent(lines.size());
    return lines;
}

}  // namespace

TEST_CASE("events: nothing is built while nobody listens") {
    EventHub hub;
    int built = 0;
    hub.PublishIfActive("replay", [&](JsonValue&, JsonValue&) { ++built; });
    CHECK(built == 0);

    Subscription quiet(hub, false);  // subscribed to nothing yet
    CHECK(!hub.Active());
    hub.PublishIfActive("replay", [&](JsonValue&, JsonValue&) { ++built; });
    CHECK(built == 0);

    {
        Subscription sub(hub, false);
        sub.Set({"replay"});
        CHECK(hub.Active());
        hub.PublishIfActive("replay", [&](JsonValue&, JsonValue&) { ++built; });
        CHECK(built == 1);
    }
    CHECK(!hub.Active());
}

TEST_CASE("events: a subscription hears only what it asked for, in kEventNames order") {
    EventHub hub;
    Subscription sub(hub, false);
    const std::vector<std::string> taken = sub.Set({"clip_saved", "no_such_event", "replay"});
    CHECK(taken == std::vector<std::string>{"replay", "clip_saved"});

    hub.Publish("buffering", Field("buffering", true));
    hub.Publish("replay", Field("replay", false));
    const std::vector<std::string> lines = Drain(sub);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == R"({"type":"event","event":"replay","replay":false})");

    sub.Set({});
    CHECK(!hub.Active());
    hub.Publish("replay", Field("replay", true));
    CHECK(Drain(sub).empty());
}

TEST_CASE("events: private fields reach only a privileged subscription") {
    EventHub hub;
    Subscription app(hub, false), own(hub, true);
    app.Set({"clip_saved"});
    own.Set({"clip_saved"});
    hub.PublishIfActive("clip_saved", [](JsonValue& f, JsonValue& p) {
        f.Set("seconds", JsonValue::MakeNum(4));
        JsonValue paths = JsonValue::MakeArray();
        paths.arr.push_back(JsonValue::MakeStr("C:\\clips\\a.mp4"));
        p.Set("paths", std::move(paths));
    });
    const std::vector<std::string> a = Drain(app), o = Drain(own);
    REQUIRE(a.size() == 1);
    REQUIRE(o.size() == 1);
    CHECK(a[0].find("paths") == std::string::npos);
    const JsonValue parsed = ParseJson(o[0]);
    const JsonValue* paths = parsed.Find("paths");
    REQUIRE(paths);
    REQUIRE(paths->arr.size() == 1);
    CHECK(paths->arr[0].str == "C:\\clips\\a.mp4");
    CHECK(parsed.Find("seconds"));
}

TEST_CASE("events: a subscription that falls behind is told to close, and the wake is signalled") {
    EventHub hub;
    Subscription sub(hub, false);
    sub.Set({"replay"});
    for (size_t i = 0; i <= kMaxQueuedEvents; ++i) hub.Publish("replay", Field("replay", i % 2 == 0));
    CHECK(WaitForSingleObject(sub.Wake(), 0) == WAIT_OBJECT_0);
    std::vector<std::string> lines;
    CHECK(!sub.Take(lines));
    CHECK(hub.WaitDrained(0));  // a dropped queue holds nothing to wait for
}

TEST_CASE("events: Close stops publishing, and WaitDrained waits for what was taken") {
    EventHub hub;
    Subscription sub(hub, false);
    sub.Set({"exiting"});
    hub.Publish("exiting", JsonValue::MakeObject());
    hub.Close();
    hub.Publish("exiting", JsonValue::MakeObject());
    CHECK(!hub.WaitDrained(0));

    std::vector<std::string> lines;
    REQUIRE(sub.Take(lines));
    CHECK(lines.size() == 1);
    CHECK(!hub.WaitDrained(0));  // taken but not yet written
    sub.Sent(lines.size());
    CHECK(hub.WaitDrained(0));
}

}  // namespace ombra::test
