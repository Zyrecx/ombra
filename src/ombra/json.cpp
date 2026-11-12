#include "ombra/json.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>

namespace ombra {

const JsonValue* JsonValue::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : obj) {
        if (k == key) return &v;
    }
    return nullptr;
}

void JsonValue::Set(std::string key, JsonValue value) {
    for (auto& [k, v] : obj) {
        if (k == key) { v = std::move(value); return; }
    }
    obj.emplace_back(std::move(key), std::move(value));
}

const char* JsonTypeName(JsonValue::Type t) {
    switch (t) {
        case JsonValue::Type::Null:   return "null";
        case JsonValue::Type::Bool:   return "bool";
        case JsonValue::Type::Number: return "number";
        case JsonValue::Type::String: return "string";
        case JsonValue::Type::Array:  return "array";
        case JsonValue::Type::Object: return "object";
    }
    return "?";
}

namespace {

// Minimal recursive-descent JSON parser. Throws std::string on malformed input — parsing here is
// a startup/message-boundary path, not a hot loop, so a thrown std::string beats threading an
// error code through every Parse* function for what's ~150 lines.
class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    JsonValue Parse() {
        SkipWs();
        JsonValue v = ParseValue();
        SkipWs();
        if (pos_ != s_.size()) Fail("trailing data after JSON value");
        return v;
    }

private:
    const std::string& s_;
    size_t              pos_ = 0;
    int                 depth_ = 0;

    // Recursion is bounded: a stack overflow can't be caught, and one would end the engine from a
    // pipe message or, at every start, from config.json. Nothing Ombra reads nests near this.
    static constexpr int kMaxDepth = 64;
    struct DepthGuard {
        JsonParser& p;
        explicit DepthGuard(JsonParser& parser) : p(parser) {
            if (++p.depth_ > kMaxDepth) p.Fail("nested too deeply");
        }
        ~DepthGuard() { --p.depth_; }
    };

    [[noreturn]] void Fail(const std::string& msg) {
        throw std::string("json: ") + msg + " at offset " + std::to_string(pos_);
    }

    char Peek() { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    char Next()  { return pos_ < s_.size() ? s_[pos_++] : '\0'; }

    void SkipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++pos_; continue; }
            // // line comments — config.json is hand-edited, and JSON's ban on comments is the
            // single most common reason a hand-edited config file fails to parse.
            if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    void Expect(char c) {
        if (Next() != c) Fail(std::string("expected '") + c + "'");
    }

    JsonValue ParseValue() {
        SkipWs();
        switch (Peek()) {
            case '{': return ParseObject();
            case '[': return ParseArray();
            case '"': return JsonValue::MakeStr(ParseString());
            case 't': case 'f': return ParseBool();
            case 'n': return ParseNull();
            default:  return ParseNumber();
        }
    }

    JsonValue ParseObject() {
        const DepthGuard guard(*this);
        JsonValue v; v.type = JsonValue::Type::Object;
        Expect('{');
        SkipWs();
        if (Peek() == '}') { ++pos_; return v; }
        while (true) {
            SkipWs();
            if (Peek() != '"') Fail("expected a quoted key");
            std::string key = ParseString();
            SkipWs();
            Expect(':');
            JsonValue val = ParseValue();
            v.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            char c = Next();
            if (c == '}') break;
            if (c != ',') Fail("expected ',' or '}' in object");
        }
        return v;
    }

    JsonValue ParseArray() {
        const DepthGuard guard(*this);
        JsonValue v; v.type = JsonValue::Type::Array;
        Expect('[');
        SkipWs();
        if (Peek() == ']') { ++pos_; return v; }
        while (true) {
            v.arr.push_back(ParseValue());
            SkipWs();
            char c = Next();
            if (c == ']') break;
            if (c != ',') Fail("expected ',' or ']' in array");
            SkipWs();
        }
        return v;
    }

    uint32_t ParseHex4() {
        if (pos_ + 4 > s_.size()) Fail("truncated \\u escape");
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
            else Fail("invalid \\u escape");
        }
        return cp;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) Fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                if (pos_ >= s_.size()) Fail("unterminated escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        uint32_t cp = ParseHex4();
                        // A surrogate pair is one code point; half of one has no meaning, and
                        // U+FFFD keeps the rest of the document readable.
                        if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(pos_, 2, "\\u") == 0) {
                            const size_t after_high = pos_;
                            pos_ += 2;
                            const uint32_t low = ParseHex4();
                            if (low >= 0xDC00 && low <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            else pos_ = after_high;
                        }
                        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: Fail("invalid escape character");
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    JsonValue ParseBool() {
        if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; return JsonValue::MakeBool(true); }
        if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; return JsonValue::MakeBool(false); }
        Fail("invalid literal");
    }

    JsonValue ParseNull() {
        if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; return JsonValue{}; }
        Fail("invalid literal");
    }

    JsonValue ParseNumber() {
        const size_t start = pos_;
        if (Peek() == '-') ++pos_;
        while (std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
        if (Peek() == '.') { ++pos_; while (std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_; }
        if (Peek() == 'e' || Peek() == 'E') {
            ++pos_;
            if (Peek() == '+' || Peek() == '-') ++pos_;
            while (std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
        }
        if (pos_ == start) Fail("expected a value");
        return JsonValue::MakeNum(std::strtod(s_.c_str() + start, nullptr));
    }
};

void WriteJsonString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') os << '\\' << c;
        else if (c == '\n') os << "\\n";
        else if (c == '\r') os << "\\r";
        else if (c == '\t') os << "\\t";
        else os << c;
    }
    os << '"';
}

void WriteJsonNumber(std::ostream& os, double num) {
    // Whole-valued doubles (every Int field, and most Double defaults) print without a trailing
    // ".0" clutter — cosmetic for config.json, which is meant to be hand-edited, and harmless for
    // the pipe protocol either way.
    if (num == static_cast<int64_t>(num)) os << static_cast<int64_t>(num);
    else os << num;
}

}  // namespace

JsonValue ParseJson(const std::string& text) {
    return JsonParser(text).Parse();
}

void WriteJson(std::ostream& os, const JsonValue& v, int indent) {
    auto pad = [&](int n) { for (int i = 0; i < n; ++i) os << "  "; };
    switch (v.type) {
        case JsonValue::Type::Null:   os << "null"; return;
        case JsonValue::Type::Bool:   os << (v.b ? "true" : "false"); return;
        case JsonValue::Type::Number: WriteJsonNumber(os, v.num); return;
        case JsonValue::Type::String: WriteJsonString(os, v.str); return;
        case JsonValue::Type::Array: {
            if (v.arr.empty()) { os << "[]"; return; }
            os << "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                pad(indent + 1);
                WriteJson(os, v.arr[i], indent + 1);
                if (i + 1 < v.arr.size()) os << ",";
                os << "\n";
            }
            pad(indent);
            os << "]";
            return;
        }
        case JsonValue::Type::Object: {
            if (v.obj.empty()) { os << "{}"; return; }
            os << "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                pad(indent + 1);
                os << '"' << v.obj[i].first << "\": ";
                WriteJson(os, v.obj[i].second, indent + 1);
                if (i + 1 < v.obj.size()) os << ",";
                os << "\n";
            }
            pad(indent);
            os << "}";
            return;
        }
    }
}

namespace {

// No indentation, no real newlines anywhere in the output — pipe.cpp's newline-delimited framing
// depends on that (a pretty-printed value's internal '\n's would be mistaken for message
// boundaries). Kept as a separate function rather than an indent==-1 sentinel on WriteJson: the
// two have different enough shapes (no pad(), no trailing newline-per-entry) that sharing the
// body would need more branches than writing it twice plainly.
void WriteJsonCompactTo(std::ostream& os, const JsonValue& v) {
    switch (v.type) {
        case JsonValue::Type::Null:   os << "null"; return;
        case JsonValue::Type::Bool:   os << (v.b ? "true" : "false"); return;
        case JsonValue::Type::Number: WriteJsonNumber(os, v.num); return;
        case JsonValue::Type::String: WriteJsonString(os, v.str); return;
        case JsonValue::Type::Array: {
            os << "[";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) os << ",";
                WriteJsonCompactTo(os, v.arr[i]);
            }
            os << "]";
            return;
        }
        case JsonValue::Type::Object: {
            os << "{";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) os << ",";
                os << '"' << v.obj[i].first << "\":";
                WriteJsonCompactTo(os, v.obj[i].second);
            }
            os << "}";
            return;
        }
    }
}

}  // namespace

std::string WriteJsonCompact(const JsonValue& v) {
    std::ostringstream os;
    WriteJsonCompactTo(os, v);
    return os.str();
}

bool JsonEquals(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JsonValue::Type::Null:   return true;
        case JsonValue::Type::Bool:   return a.b == b.b;
        case JsonValue::Type::Number: return a.num == b.num;
        case JsonValue::Type::String: return a.str == b.str;
        case JsonValue::Type::Array:
            if (a.arr.size() != b.arr.size()) return false;
            for (size_t i = 0; i < a.arr.size(); ++i)
                if (!JsonEquals(a.arr[i], b.arr[i])) return false;
            return true;
        case JsonValue::Type::Object:
            if (a.obj.size() != b.obj.size()) return false;
            for (const auto& [key, val] : a.obj) {
                const JsonValue* other = b.Find(key);
                if (!other || !JsonEquals(val, *other)) return false;
            }
            return true;
    }
    return false;
}

}  // namespace ombra
