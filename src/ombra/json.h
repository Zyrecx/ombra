// A small hand-rolled JSON value + recursive-descent parser + writer — originally built
// inside config.cpp, then extracted here so the pipe protocol (protocol.h,
// pipe.cpp) can share one JSON implementation with config.json loading instead of hand-rolling a
// second one. No library is vendored — same precedent as NVENC setup and the MP4 muxer — hand-roll the hard parts rather than add a dependency.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace ombra {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type                                            type = Type::Null;
    bool                                             b = false;
    double                                           num = 0;
    std::string                                      str;
    std::vector<JsonValue>                           arr;
    std::vector<std::pair<std::string, JsonValue>>   obj;  // insertion order preserved

    static JsonValue MakeBool(bool v)       { JsonValue j; j.type = Type::Bool;   j.b = v;   return j; }
    static JsonValue MakeNum(double v)      { JsonValue j; j.type = Type::Number; j.num = v; return j; }
    static JsonValue MakeStr(std::string v) { JsonValue j; j.type = Type::String; j.str = std::move(v); return j; }
    static JsonValue MakeObject()           { JsonValue j; j.type = Type::Object; return j; }
    static JsonValue MakeArray()            { JsonValue j; j.type = Type::Array;  return j; }

    // Object-only convenience. Find returns nullptr if this isn't an object or the key is absent.
    // Set appends, or overwrites in place if the key already exists (keeps insertion order stable
    // for repeated Set calls on the same key, which the protocol handlers do).
    const JsonValue* Find(const std::string& key) const;
    void              Set(std::string key, JsonValue value);
};

const char* JsonTypeName(JsonValue::Type t);

// Throws std::string (a human-readable message, "offset N" included) on malformed input.
JsonValue ParseJson(const std::string& text);

// Pretty-printed, indented, real newlines between entries — config.json's on-disk form.
void WriteJson(std::ostream& os, const JsonValue& v, int indent = 0);

// Single-line, no whitespace, no embedded newline — required framing for pipe.cpp's
// newline-delimited protocol (a pretty-printed value would be mistaken for multiple messages).
std::string WriteJsonCompact(const JsonValue& v);

// Deep equality; object keys compare regardless of order.
bool JsonEquals(const JsonValue& a, const JsonValue& b);

}  // namespace ombra
