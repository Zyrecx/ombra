// Headless round-trip test for json.h: parse -> write -> parse and assert structural equality, on
// a handful of representative values (nested object, array, an escaped string, and the "//"
// comment stripping config.json's loader depends on). No GPU needed. Ported from
// main.cpp's RunJsonTest (was `ombra.exe jsontest`) — same cases, doctest assertions instead of
// printf/++fails.

#include <doctest/doctest.h>

#include "ombra/json.h"

#include <sstream>
#include <string>

namespace ombra::test {

namespace {

bool JsonEqual(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JsonValue::Type::Null:   return true;
        case JsonValue::Type::Bool:   return a.b == b.b;
        case JsonValue::Type::Number: return a.num == b.num;
        case JsonValue::Type::String: return a.str == b.str;
        case JsonValue::Type::Array: {
            if (a.arr.size() != b.arr.size()) return false;
            for (size_t i = 0; i < a.arr.size(); ++i)
                if (!JsonEqual(a.arr[i], b.arr[i])) return false;
            return true;
        }
        case JsonValue::Type::Object: {
            if (a.obj.size() != b.obj.size()) return false;
            for (size_t i = 0; i < a.obj.size(); ++i)
                if (a.obj[i].first != b.obj[i].first || !JsonEqual(a.obj[i].second, b.obj[i].second))
                    return false;
            return true;
        }
    }
    return false;
}

void CheckRoundtrip(const std::string& text, bool compact) {
    JsonValue parsed;
    REQUIRE_NOTHROW(parsed = ParseJson(text));

    const std::string written = compact ? WriteJsonCompact(parsed)
                                         : [&] { std::ostringstream os; WriteJson(os, parsed, 0);
                                                 return os.str(); }();

    JsonValue reparsed;
    REQUIRE_NOTHROW(reparsed = ParseJson(written));

    CHECK(JsonEqual(parsed, reparsed));

    // The whole point of WriteJsonCompact: pipe.cpp's newline framing breaks if a single message's
    // serialization contains a real newline byte anywhere in it.
    if (compact) CHECK(written.find('\n') == std::string::npos);
}

}  // namespace

TEST_CASE("json: nested object round-trips, pretty") {
    CheckRoundtrip(R"({"a": {"b": 1, "c": [1, 2, 3]}, "d": true, "e": null})", false);
}

TEST_CASE("json: nested object round-trips, compact framing") {
    CheckRoundtrip(R"({"a": {"b": 1, "c": [1, 2, 3]}, "d": true})", true);
}

TEST_CASE("json: escaped string round-trips, pretty") {
    CheckRoundtrip(R"({"s": "line one\nline two \"quoted\" \\backslash\\"})", false);
}

TEST_CASE("json: escaped string round-trips, compact framing") {
    CheckRoundtrip(R"({"s": "line one\nline two \"quoted\""})", true);
}

TEST_CASE("json: array of mixed types round-trips") {
    CheckRoundtrip(R"([1, "two", false, null, {"five": 5}])", false);
}

TEST_CASE("json: \"//\" line comments are stripped, not treated as data") {
    JsonValue v;
    REQUIRE_NOTHROW(v = ParseJson("{\n  // a comment\n  \"a\": 1 // trailing comment\n}\n"));
    REQUIRE(v.type == JsonValue::Type::Object);
    REQUIRE(v.obj.size() == 1);
    CHECK(v.obj[0].first == "a");
    CHECK(v.obj[0].second.num == 1);
}

TEST_CASE("json: nesting past the limit is a parse error, not a stack overflow") {
    const auto nested = [](int depth) { return std::string(depth, '[') + std::string(depth, ']'); };
    CHECK_NOTHROW(ParseJson(nested(64)));
    CHECK_THROWS_AS(ParseJson(nested(65)), std::string);
    CHECK_THROWS_AS(ParseJson(std::string(1000000, '[')), std::string);
    std::string objects;
    for (int i = 0; i < 1000000; ++i) objects += "{\"a\":";
    CHECK_THROWS_AS(ParseJson(objects), std::string);
}

TEST_CASE("json: \\u escapes decode to UTF-8") {
    CHECK(ParseJson("\"Pok\\u00e9mon\"").str == "Pok\xC3\xA9mon");
    CHECK(ParseJson("\"\\u4e16\"").str == "\xE4\xB8\x96");
    CHECK(ParseJson("\"\\ud83d\\ude00\"").str == "\xF0\x9F\x98\x80");
    CHECK(ParseJson("\"a\\u0041\"").str == "aA");
    // Half a pair is the replacement character, and what follows it survives.
    CHECK(ParseJson("\"\\ud83dx\"").str == "\xEF\xBF\xBDx");
    CHECK(ParseJson("\"\\ud83d\\u0041\"").str == "\xEF\xBF\xBD" "A");
    CHECK(ParseJson("\"\\ude00\"").str == "\xEF\xBF\xBD");
    CHECK_THROWS_AS(ParseJson("\"\\u12\""), std::string);
}

TEST_CASE("json: JsonValue::Set overwrites in place, Find returns nullptr for a missing key") {
    JsonValue obj = JsonValue::MakeObject();
    obj.Set("a", JsonValue::MakeNum(1));
    obj.Set("b", JsonValue::MakeStr("x"));
    obj.Set("a", JsonValue::MakeNum(2));  // overwrite, not append

    REQUIRE(obj.obj.size() == 2);
    const JsonValue* a = obj.Find("a");
    REQUIRE(a != nullptr);
    CHECK(a->num == 2);
    CHECK(obj.Find("z") == nullptr);
}

}  // namespace ombra::test
