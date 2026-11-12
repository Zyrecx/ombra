#include "ombra/config.h"
#include "ombra/brand.h"

#include "ombra/audio_tracks.h"
#include "ombra/games.h"
#include "ombra/hotkey_spec.h"

#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/util.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace ombra {

std::wstring ConfigDirW() { return Instance().config_dir; }

const char* ApplyTierStr(ApplyTier t) {
    switch (t) {
        case ApplyTier::Free:    return "free";
        case ApplyTier::Rebuild: return "rebuild";
        case ApplyTier::Realloc: return "realloc";
    }
    return "?";
}

const char* TierNameStr(Tier t) {
    switch (t) {
        case Tier::Preset:   return "preset";
        case Tier::Common:   return "common";
        case Tier::Advanced: return "advanced";
    }
    return "?";
}

const char* KindName(SettingKind k) {
    switch (k) {
        case SettingKind::Bool:   return "bool";
        case SettingKind::Int:    return "int";
        case SettingKind::Double: return "double";
        case SettingKind::String: return "string";
        case SettingKind::Enum:   return "enum";
        case SettingKind::Raw:    return "raw";
        case SettingKind::Action: return "action";
    }
    return "?";
}

namespace {
struct QualityRow { int w, h, base_bitrate_kbps; };
// 720p and 900p are 1080p's row x the bitrate reaching the same VMAF at that size, each scored
// against its own area-scaled source (scripts/encbench.py --height, NVENC and AMF, H.264 and AV1,
// CSGO/DOTA2/GTAV/Witcher 3, 2026-11-02). Median 0.43 and 0.63, 0.37-0.55 and 0.54-0.68 on the
// clips whose quality still depends on bits (CSGO sits flat near VMAF 97, so its ratios are noise).
// 900p needs its own row: by pixel count it is nearest 720p, whose bitrate would under-serve it.
constexpr QualityRow kQualityRows[] = {
    {1280, 720, 11000},
    {1600, 900, 16000},
    {1920, 1080, 25000},
    {2560, 1440, 40000},
    {3840, 2160, 80000},
};
const QualityRow& NearestQualityRow(int width, int height) {
    const long long target_px = static_cast<long long>(width) * height;
    const QualityRow* best = &kQualityRows[0];
    long long best_diff = -1;
    for (const QualityRow& r : kQualityRows) {
        const long long diff = std::llabs(static_cast<long long>(r.w) * r.h - target_px);
        if (best_diff < 0 || diff < best_diff) { best = &r; best_diff = diff; }
    }
    return *best;
}
// kQualityRows is calibrated at 60fps (capture.framerate's default); numbers in config.h's comment
// on the two functions below. Above 60, sqrt gives 1.41x at 120; equal VMAF took a median 1.45x (1.35-1.53,
// 10th-90th percentile), three natural 120 fps clips on NVENC and AMF, H.264 and AV1.
double FpsScale(int fps) {
    const int clamped = std::clamp(fps, 1, 240);
    return std::pow(static_cast<double>(clamped) / 60.0, clamped < 60 ? 0.7 : 0.5);
}
}  // namespace

namespace {
double CodecScale(VideoCodec codec) { return codec == VideoCodec::AV1 ? kAv1BitrateFactor : 1.0; }
}  // namespace

int QualityPresetToBitrate(const std::string& preset, int width, int height, int fps, VideoCodec codec) {
    if (preset == "custom") return 0;
    const QualityRow& best = NearestQualityRow(width, height);
    double multiplier = 1.0;
    if (preset == "low") multiplier = 0.5;
    else if (preset == "medium") multiplier = 0.75;
    else if (preset == "high") multiplier = 1.0;
    else if (preset == "ultra") multiplier = 1.5;
    return static_cast<int>(std::llround(best.base_bitrate_kbps * multiplier * FpsScale(fps) * CodecScale(codec)));
}

VideoSize ResolveOutputSize(uint32_t src_w, uint32_t src_h, const std::string& resolution) {
    const VideoSize native{src_w & ~1u, src_h & ~1u};
    const uint32_t short_target = resolution == "1440p" ? 1440u
                                : resolution == "1080p" ? 1080u
                                : resolution == "720p"  ? 720u
                                                        : 0u;
    const uint32_t src_short = (std::min)(src_w, src_h);
    const uint32_t src_long  = (std::max)(src_w, src_h);
    if (short_target == 0 || src_short <= short_target) return native;
    // Rounded to the nearest even long side: 2560x1600 -> 1728x1080, 1366x768 at 720p -> 1280x720.
    const uint64_t long_side = (static_cast<uint64_t>(src_long) * short_target + src_short) / (2ull * src_short) * 2ull;
    const uint32_t scaled_long = static_cast<uint32_t>(long_side);
    return src_w >= src_h ? VideoSize{scaled_long, short_target} : VideoSize{short_target, scaled_long};
}

EncodePlan PlanEncode(VideoSize out, VideoCodec requested, uint32_t align_w, uint32_t align_h,
                      const std::string& unaligned_mode) {
    EncodePlan plan{requested, out};
    align_w = (std::max)(align_w, 1u);
    align_h = (std::max)(align_h, 1u);
    if (requested != VideoCodec::AV1 || (out.width % align_w == 0 && out.height % align_h == 0)) return plan;
    if (unaligned_mode == "pad") return plan;
    if (unaligned_mode == "scale") {
        const VideoSize down{out.width / align_w * align_w, out.height / align_h * align_h};
        if (down.width > 0 && down.height > 0) {
            plan.size = down;
            return plan;
        }
    }
    plan.codec = VideoCodec::H264;  // "h264", anything unknown, or too small to scale down
    return plan;
}

VideoSize PredictPaddedSize(VideoSize size, uint32_t align_w, uint32_t align_h) {
    align_w = (std::max)(align_w, 1u);
    align_h = (std::max)(align_h, 1u);
    auto up = [](uint32_t v, uint32_t a) { return (v + a - 1) / a * a; };
    const uint32_t h = align_h == 16 && size.height % 16 == 8 ? size.height + 2 : up(size.height, align_h);
    return {up(size.width, align_w), h};
}

int ResolveFramerate(int64_t configured, int display_hz) {
    if (configured > 0) return static_cast<int>(configured);
    if (display_hz <= 0) return 60;
    // Windows reports fractional NTSC rates rounded down (59.94 -> 59, 119.88 -> 119).
    if ((display_hz + 1) % 30 == 0) ++display_hz;
    return std::clamp(display_hz, 24, 240);
}

std::string BitrateToQualityPreset(int bitrate_kbps, int width, int height, int fps, VideoCodec codec) {
    for (const char* preset : {"low", "medium", "high", "ultra"})
        if (bitrate_kbps == QualityPresetToBitrate(preset, width, height, fps, codec)) return preset;
    return "custom";
}

const RecordingGpuCaps* PickRecordingGpu(const std::vector<RecordingGpuCaps>& gpus, const std::string& adapter) {
    auto find = [&](const char* vendor) -> const RecordingGpuCaps* {
        for (const auto& g : gpus)
            if (g.vendor == vendor) return &g;
        return nullptr;
    };
    if (adapter == "nvidia" || adapter == "amd" || adapter == "cpu")
        if (const auto* g = find(adapter.c_str())) return g;
    for (const auto& g : gpus)
        if (g.drives_display && g.vendor != "cpu") return &g;
    if (const auto* g = find("nvidia")) return g;
    if (const auto* g = find("amd")) return g;
    return find("cpu");
}

VideoCodec RecordedCodec(const std::vector<RecordingGpuCaps>& gpus, const std::string& adapter,
                         const std::string& video_codec, const std::string& amf_av1_unaligned, VideoSize out) {
    if (video_codec != "av1") return VideoCodec::H264;
    if (gpus.empty()) return VideoCodec::AV1;
    const RecordingGpuCaps* gpu = PickRecordingGpu(gpus, adapter);
    if (!gpu || !gpu->av1) return VideoCodec::H264;
    return PlanEncode(out, VideoCodec::AV1, gpu->av1_align_w, gpu->av1_align_h, amf_av1_unaligned).codec;
}

uint32_t BitrateForOpenedCodec(uint32_t kbps, std::optional<VideoCodec> derived_for, VideoCodec opened) {
    if (!derived_for || *derived_for == opened) return kbps;
    return static_cast<uint32_t>(std::llround(kbps * CodecScale(opened) / CodecScale(*derived_for)));
}

std::optional<VideoCodec> PresetBitrateCodec(const Config& cfg, int source_width, int source_height,
                                             const std::vector<RecordingGpuCaps>& gpus) {
    if (cfg.encode_quality_preset == "custom") return std::nullopt;
    return RecordedCodec(gpus, cfg.encode_adapter, cfg.encode_video_codec, cfg.encode_amf_av1_unaligned,
                         ResolveOutputSize(static_cast<uint32_t>(source_width), static_cast<uint32_t>(source_height),
                                           cfg.capture_resolution));
}

int PresetBitrateFor(const Config& cfg, int source_width, int source_height, int display_hz,
                     const std::vector<RecordingGpuCaps>& gpus) {
    const VideoSize out = ResolveOutputSize(static_cast<uint32_t>(source_width), static_cast<uint32_t>(source_height),
                                            cfg.capture_resolution);
    const VideoCodec codec = RecordedCodec(gpus, cfg.encode_adapter, cfg.encode_video_codec,
                                           cfg.encode_amf_av1_unaligned, out);
    return QualityPresetToBitrate(cfg.encode_quality_preset, static_cast<int>(out.width), static_cast<int>(out.height),
                                  ResolveFramerate(cfg.capture_framerate, display_hz), codec);
}

bool ParseHexRgb(const std::string& s, uint32_t* rgb) {
    if (s.size() != 7 || s[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = s[i];
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    if (rgb) *rgb = v;
    return true;
}

namespace {

constexpr const char* kUserThemeFields[6] = {"background", "cards", "menus", "lines", "text", "muted"};

uint32_t& UserThemeField(UiPaletteColors& c, int i) {
    uint32_t* f[6] = {&c.background, &c.cards, &c.menus, &c.lines, &c.text, &c.muted};
    return *f[i];
}

std::string AsciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

size_t Utf8Chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

}  // namespace

bool UserThemeNameOk(const std::string& name, const std::vector<UserTheme>& themes, const std::string& self_id,
                     std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    if (name.empty()) return fail("a theme needs a name");
    if (Utf8Chars(name) > kMaxUserThemeName) return fail("a theme name is at most 32 characters");
    if (name.front() == ' ' || name.back() == ' ') return fail("a theme name can't start or end with a space");
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F) return fail("a theme name can't hold control characters");
    const std::string low = AsciiLower(name);
    if (low == "system") return fail("a preset already has that name");
    for (const UiPalette& p : kUiPalettes)
        if (low == AsciiLower(NarrowUtf8(p.name))) return fail("a preset already has that name");
    for (const UserTheme& t : themes)
        if (t.id != self_id && AsciiLower(t.name) == low) return fail("another theme has that name");
    return true;
}

bool ValidateUserThemes(const JsonValue& v, std::string& err) {
    if (v.type != JsonValue::Type::Array) { err = "must be an array"; return false; }
    if (v.arr.size() > kMaxUserThemes) { err = "at most 24 themes"; return false; }
    std::vector<UserTheme> seen;
    for (const JsonValue& e : v.arr) {
        if (e.type != JsonValue::Type::Object) { err = "every entry must be an object"; return false; }
        const JsonValue* id = e.Find("id");
        if (!id || id->type != JsonValue::Type::String || !IsUserThemeId(id->str)) {
            err = "each theme needs an \"id\" like \"user1\"";
            return false;
        }
        if (FindUserTheme(seen, id->str)) { err = "two themes share the id " + id->str; return false; }
        const JsonValue* name = e.Find("name");
        if (!name || name->type != JsonValue::Type::String) { err = "each theme needs a string \"name\""; return false; }
        if (!UserThemeNameOk(name->str, seen, id->str, &err)) { err += ": " + name->str; return false; }
        UserTheme t{id->str, name->str, {}};
        for (int i = 0; i < 6; ++i) {
            const JsonValue* c = e.Find(kUserThemeFields[i]);
            if (!c || c->type != JsonValue::Type::String || !ParseHexRgb(c->str, &UserThemeField(t.colors, i))) {
                err = std::string("each theme needs \"") + kUserThemeFields[i] + "\" as \"#rrggbb\"";
                return false;
            }
        }
        seen.push_back(std::move(t));
    }
    return true;
}

std::vector<UserTheme> ParseUserThemes(const JsonValue& v) {
    std::vector<UserTheme> out;
    if (v.type != JsonValue::Type::Array) return out;
    for (const JsonValue& e : v.arr) {
        if (out.size() >= kMaxUserThemes) break;
        if (e.type != JsonValue::Type::Object) continue;
        const JsonValue* id = e.Find("id");
        const JsonValue* name = e.Find("name");
        if (!id || id->type != JsonValue::Type::String || !IsUserThemeId(id->str) || FindUserTheme(out, id->str)) continue;
        if (!name || name->type != JsonValue::Type::String || !UserThemeNameOk(name->str, out, id->str)) continue;
        UserTheme t{id->str, name->str, kUiPalettes[0].colors};  // a missing colour is Dark's
        for (int i = 0; i < 6; ++i)
            if (const JsonValue* c = e.Find(kUserThemeFields[i]); c && c->type == JsonValue::Type::String)
                ParseHexRgb(c->str, &UserThemeField(t.colors, i));
        out.push_back(std::move(t));
    }
    return out;
}

JsonValue SerializeUserThemes(const std::vector<UserTheme>& themes) {
    JsonValue arr = JsonValue::MakeArray();
    for (const UserTheme& t : themes) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("id", JsonValue::MakeStr(t.id));
        o.Set("name", JsonValue::MakeStr(t.name));
        UiPaletteColors c = t.colors;
        for (int i = 0; i < 6; ++i) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "#%06x", UserThemeField(c, i) & 0xFFFFFF);
            o.Set(kUserThemeFields[i], JsonValue::MakeStr(hex));
        }
        arr.arr.push_back(std::move(o));
    }
    return arr;
}
namespace {

using FlatMap = std::map<std::string, const JsonValue*>;

void Flatten(const JsonValue& v, const std::string& prefix, FlatMap& out) {
    if (v.type == JsonValue::Type::Object) {
        for (const auto& [k, child] : v.obj) {
            Flatten(child, prefix.empty() ? k : prefix + "." + k, out);
        }
    } else {
        out[prefix] = &v;
    }
}

void InsertNested(JsonValue& root, const std::string& dotted_key, JsonValue value) {
    // A hand-made flat document: LoadConfig prefers this spelling, so a nested copy would never be read.
    for (auto& [k, v] : root.obj) {
        if (k == dotted_key) {
            v = std::move(value);
            return;
        }
    }
    JsonValue* cur = &root;
    size_t start = 0;
    while (true) {
        size_t dot = dotted_key.find('.', start);
        const std::string part = dotted_key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (dot == std::string::npos) {
            auto leaf = std::find_if(cur->obj.begin(), cur->obj.end(),
                                      [&](const auto& kv) { return kv.first == part; });
            if (leaf != cur->obj.end()) {
                leaf->second = std::move(value);
            } else {
                cur->obj.emplace_back(part, std::move(value));
            }
            return;
        }
        auto it = std::find_if(cur->obj.begin(), cur->obj.end(),
                                [&](const auto& kv) { return kv.first == part; });
        if (it == cur->obj.end()) {
            cur->obj.emplace_back(part, JsonValue{});
            cur->obj.back().second.type = JsonValue::Type::Object;
            cur = &cur->obj.back().second;
        } else {
            cur = &it->second;
        }
        start = dot + 1;
    }
}

// Keys this build deleted. Old config.json files still carry them (the default config wrote
// every key): validation skips them quietly and SaveConfig drops them from the file.
const std::set<std::string> kRetiredKeys = {
    "capture.adapter",        // capture always runs on the encode adapter (encode_target.h)
    "engine.game_grace_sec",  // Only in games follows whether a game is open, not the foreground
    "capture.backend",        // WGC only: DDA fails on the same content WGC does
    "output.container",       // MP4 only: a save is written once from RAM, so MKV's crash safety bought nothing
    "audio.layout",           // the audio panel's drag-to-group mixes whatever the user groups
    "encode.bframes",         // encoders are one frame in, one frame out (pacer contract)
    "encode.lookahead",       // same
    "encode.tuning",          // removed with an earlier settings layout; presets pick the tuning
    "ui.panel_edge_style",    // superseded by ui.panel_attach
};

// Removes a key written either dotted ("a.b") or nested ({"a": {"b": ...}}), and a parent object
// the removal leaves empty.
void EraseNested(JsonValue& root, const std::string& dotted_key) {
    auto find = [&](const std::string& name) {
        return std::find_if(root.obj.begin(), root.obj.end(),
                            [&](const auto& kv) { return kv.first == name; });
    };
    if (auto it = find(dotted_key); it != root.obj.end()) root.obj.erase(it);
    const size_t dot = dotted_key.find('.');
    if (dot == std::string::npos) return;
    auto it = find(dotted_key.substr(0, dot));
    if (it == root.obj.end() || it->second.type != JsonValue::Type::Object) return;
    EraseNested(it->second, dotted_key.substr(dot + 1));
    if (it->second.obj.empty()) root.obj.erase(it);
}

void WarnIfUnimplemented(const char* key, bool implemented) {
    if (implemented) return;
    // ApplyConfigPatch runs on every set_config round trip, not just at startup, so logging
    // unconditionally here re-warned on every Settings interaction (5x in 2 minutes, observed
    // live). The key set is static for the process, so once per key per process is enough to
    // still catch it in the logs without the spam.
    static std::mutex warned_mutex;
    static std::set<std::string> warned;
    std::lock_guard<std::mutex> lock(warned_mutex);
    if (!warned.insert(key).second) return;
    Log::Debug("config %s is set but not yet wired to behavior", key);
}

void ApplyBool(bool& field, const char* key, bool implemented, const FlatMap& flat,
               std::vector<ConfigIssue>& issues, std::set<std::string>& visited) {
    auto it = flat.find(key);
    if (it == flat.end()) return;
    visited.insert(key);
    const JsonValue& v = *it->second;
    if (v.type != JsonValue::Type::Bool) {
        issues.push_back({key, std::string("expected bool, got ") + JsonTypeName(v.type)});
        return;
    }
    field = v.b;
    WarnIfUnimplemented(key, implemented);
}

void ApplyInt(int64_t& field, const char* key, double lo, double hi, bool implemented,
              const FlatMap& flat, std::vector<ConfigIssue>& issues, std::set<std::string>& visited) {
    auto it = flat.find(key);
    if (it == flat.end()) return;
    visited.insert(key);
    const JsonValue& v = *it->second;
    if (v.type != JsonValue::Type::Number) {
        issues.push_back({key, std::string("expected number, got ") + JsonTypeName(v.type)});
        return;
    }
    if (!(lo == 0 && hi == 0) && (v.num < lo || v.num > hi)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "value %g out of range [%g, %g]", v.num, lo, hi);
        issues.push_back({key, buf});
        return;
    }
    field = static_cast<int64_t>(std::llround(v.num));
    WarnIfUnimplemented(key, implemented);
}

void ApplyDouble(double& field, const char* key, double lo, double hi, bool implemented,
                  const FlatMap& flat, std::vector<ConfigIssue>& issues, std::set<std::string>& visited) {
    auto it = flat.find(key);
    if (it == flat.end()) return;
    visited.insert(key);
    const JsonValue& v = *it->second;
    if (v.type != JsonValue::Type::Number) {
        issues.push_back({key, std::string("expected number, got ") + JsonTypeName(v.type)});
        return;
    }
    if (!(lo == 0 && hi == 0) && (v.num < lo || v.num > hi)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "value %g out of range [%g, %g]", v.num, lo, hi);
        issues.push_back({key, buf});
        return;
    }
    field = v.num;
    WarnIfUnimplemented(key, implemented);
}

void ApplyString(std::string& field, const char* key, bool implemented, const FlatMap& flat,
                  std::vector<ConfigIssue>& issues, std::set<std::string>& visited) {
    auto it = flat.find(key);
    if (it == flat.end()) return;
    visited.insert(key);
    const JsonValue& v = *it->second;
    if (v.type != JsonValue::Type::String) {
        issues.push_back({key, std::string("expected string, got ") + JsonTypeName(v.type)});
        return;
    }
    field = v.str;
    WarnIfUnimplemented(key, implemented);
}

void ApplyEnum(std::string& field, const char* key, const char* allowed, bool implemented,
               const FlatMap& flat, std::vector<ConfigIssue>& issues, std::set<std::string>& visited) {
    auto it = flat.find(key);
    if (it == flat.end()) return;
    visited.insert(key);
    const JsonValue& v = *it->second;
    if (v.type != JsonValue::Type::String) {
        issues.push_back({key, std::string("expected string, got ") + JsonTypeName(v.type)});
        return;
    }
    std::string opts(allowed);
    bool        ok = false;
    size_t      start = 0;
    while (start <= opts.size()) {
        size_t bar = opts.find('|', start);
        std::string opt = opts.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (opt == "#rrggbb" ? ParseHexRgb(v.str, nullptr) : opt == "user<n>" ? IsUserThemeId(v.str) : opt == v.str) { ok = true; break; }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (!ok) {
        issues.push_back({key, "value \"" + v.str + "\" is not one of: " + opts});
        return;
    }
    field = v.str;
    WarnIfUnimplemented(key, implemented);
}

void MarkRaw(const char* key, bool implemented, const FlatMap& flat, std::set<std::string>& visited) {
    if (flat.find(key) == flat.end()) return;
    visited.insert(key);
    WarnIfUnimplemented(key, implemented);
}

// Two shapes here (games.list validates in games.cpp): "capture.monitors" is
// an array of (non-empty) strings; "audio.sources" is an
// array of {id?, kind, device, mode?, target?, enabled?, gain_db?} — superseding the earlier
// {kind, device, gain?, muted?} shape (never shipped with implemented=true, so no
// config.json in the wild depends on the old field names). An absent or empty array still resolves
// to exactly today's single output_loopback/mode=all behavior — that's a runtime default
// (audio.cpp), not something validated here. A "track" key here is a dead leftover (deleted
// config.h's AudioSourceSpec comment) -- not validated or read, silently ignored like
// any other unknown key, so an old config.json with one still loads unchanged.
bool IsStringArray(const JsonValue& v, std::string& err) {
    if (v.type != JsonValue::Type::Array) { err = "must be an array of strings"; return false; }
    for (const JsonValue& e : v.arr) {
        if (e.type != JsonValue::Type::String) { err = "every entry must be a string"; return false; }
    }
    return true;
}

bool IsAudioSourcesArray(const JsonValue& v, std::string& err) {
    if (v.type != JsonValue::Type::Array) { err = "must be an array"; return false; }
    for (const JsonValue& e : v.arr) {
        if (e.type != JsonValue::Type::Object) { err = "every entry must be an object"; return false; }

        const JsonValue* kind = e.Find("kind");
        if (!kind || kind->type != JsonValue::Type::String) {
            err = "each entry needs a string \"kind\""; return false;
        }
        if (kind->str != "output_loopback" && kind->str != "input") {
            err = "\"kind\" must be \"output_loopback\" or \"input\""; return false;
        }
        if (const JsonValue* id = e.Find("id"); id && id->type != JsonValue::Type::String) {
            err = "\"id\", if present, must be a string"; return false;
        }
        if (const JsonValue* device = e.Find("device"); device && device->type != JsonValue::Type::String) {
            err = "\"device\", if present, must be a string"; return false;
        }
        if (const JsonValue* mode = e.Find("mode")) {
            if (mode->type != JsonValue::Type::String) {
                err = "\"mode\", if present, must be a string"; return false;
            }
            if (mode->str != "all" && mode->str != "all_except" && mode->str != "game_only" &&
                mode->str != "only") {
                err = "\"mode\" must be one of: all|all_except|game_only|only"; return false;
            }
            if (kind->str != "output_loopback") {
                err = "\"mode\" only applies to kind=\"output_loopback\""; return false;
            }
        }
        if (const JsonValue* target = e.Find("target"); target && target->type != JsonValue::Type::String) {
            err = "\"target\", if present, must be a string"; return false;
        }
        if (const JsonValue* name = e.Find("name"); name && name->type != JsonValue::Type::String) {
            err = "\"name\", if present, must be a string"; return false;
        }
        if (const JsonValue* enabled = e.Find("enabled"); enabled && enabled->type != JsonValue::Type::Bool) {
            err = "\"enabled\", if present, must be a bool"; return false;
        }
        // Reserved, unread (deferred) — validated now so
        // a later reader doesn't have to also become the first validator of pre-existing configs.
        if (const JsonValue* gain = e.Find("gain_db"); gain && gain->type != JsonValue::Type::Number) {
            err = "\"gain_db\", if present, must be a number"; return false;
        }
        // Arbitrary drag-to-group audio mixing. Sources sharing
        // a group_id are summed into one AAC track before muxing (main.cpp); absent defaults to the
        // source's own `id` in ResolveAudioSources below, so every pre-existing config (one source
        // per group, today's behavior) round-trips unchanged with no migration needed.
        if (const JsonValue* group_id = e.Find("group_id"); group_id && group_id->type != JsonValue::Type::String) {
            err = "\"group_id\", if present, must be a string"; return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::string> RawConfigKeys() {
    std::vector<std::string> keys;
#define X(key, tier, grp, app, impl, title, help, tradeoff) keys.push_back(key);
    OMBRA_CONFIG_RAW(X)
#undef X
    return keys;
}

std::string DescribeNonDefaultSettings(const Config& cfg, const RawConfigMap& raw) {
    constexpr size_t kMaxValue = 160;  // games.list can hold hundreds of entries
    std::string      out;
    auto add = [&](const std::string& key, const JsonValue& v) {
        std::string text = WriteJsonCompact(v);
        if (text.size() > kMaxValue) {
            text = v.type == JsonValue::Type::Array ? "[" + std::to_string(v.arr.size()) + " entries]"
                                                    : text.substr(0, kMaxValue) + "...";
        }
        if (!out.empty()) out += ' ';
        out += key + "=" + text;
    };
    const JsonValue now = SerializeConfigFlat(cfg);
    const JsonValue def = SerializeConfigFlat(Config{});
    for (const auto& [key, value] : now.obj) {
        const JsonValue* d = def.Find(key);
        if (!d || !JsonEquals(*d, value)) add(key, value);
    }
    for (const auto& [key, value] : raw)
        if (value.type != JsonValue::Type::Null) add(key, value);
    return out;
}

bool ValidateRawConfigValue(const std::string& key, const JsonValue& value, std::string& err) {
    if (key == "capture.monitors")
        return IsStringArray(value, err);
    if (key == "audio.sources") return IsAudioSourcesArray(value, err);
    if (key == "games.list") return ValidateGamesList(value, err);
    if (key == "ui.themes") return ValidateUserThemes(value, err);
    err = "unknown raw key";  // should never happen -- RawConfigKeys() is the only source of names
    return false;
}

RingSizing ComputeRingSizing(const Config& cfg, double buffer_seconds) {
    RingSizing r;
    const double bitrate     = static_cast<double>(cfg.encode_bitrate_kbps);
    // Constant quality has no bitrate limit, so its ring is a cap on what a busy scene may hold:
    // twice VBR's. At CQ 20, 1080p60, High, that holds 120 s of AV1 on the hardest clip measured
    // (GTA V, 63 Mbps; H.264 at 105 Mbps gets about 93 s; scripts/encbench.py --cqp, 2026-11-02).
    // Commit follows live footage, so an easy scene never touches the extra. Max bitrate is hidden
    // and unused under CQP: a stale value mustn't size the ring.
    const bool   cqp         = cfg.encode_rate_control == "cqp";
    const double max_bitrate = cqp ? bitrate * 1.25 * 2.0
        : (cfg.encode_max_bitrate_kbps > 0)
        ? static_cast<double>(cfg.encode_max_bitrate_kbps)
        : static_cast<double>(static_cast<uint32_t>(bitrate * 1.25));
    r.video_capacity = (cfg.buffer_max_bytes > 0)
        ? static_cast<int64_t>(cfg.buffer_max_bytes)
        : static_cast<int64_t>(buffer_seconds * max_bitrate * 1000.0 / 8.0 * 1.3);
    // A save takes about a second; 2 s of encoding at max bitrate sits past the pin boundary.
    r.video_overshoot = static_cast<int64_t>(2.0 * max_bitrate * 1000.0 / 8.0);
    // Per audio source: its encoded track plus its PCM staging (AudioTrackHub). Exact while each
    // source is its own track; a group shares one, so grouped configs come out slightly high.
    const auto audio  = AudioTrackHub::SizeFor(buffer_seconds, static_cast<uint32_t>(cfg.audio_bitrate_kbps), 0);
    r.audio_capacity  = static_cast<int64_t>(audio.ring_capacity + AudioTrackHub::StageBytes());
    r.audio_overshoot = static_cast<int64_t>(audio.ring_overshoot);
    return r;
}

ConfigPreview PreviewConfig(const Config& current_cfg, const JsonValue& patch_json,
                            int display_width, int display_height, int display_hz,
                            const std::vector<RecordingGpuCaps>& gpus) {
    ConfigPreview preview;
    FlatMap flat;
    Flatten(patch_json, "", flat);

    Config working = current_cfg;
    ApplyTier max_tier = ApplyTier::Free;

    // A key re-sent at its live value doesn't raise the tier: the same batch arriving twice (a
    // double-clicked Save sends it again before the first reply clears it) rebuilt the pipeline
    // twice. Keys without a flat typed value (audio.sources) still count, and so do values resolved
    // at pipeline build ("match display", adapter "auto"): re-saving one is how a user re-resolves.
    const JsonValue current = SerializeConfigFlat(current_cfg);
    const auto& settings = AllSettings();
    for (const auto& [key, val] : flat) {
        const bool resolves_at_build = (key == "capture.framerate" && val->type == JsonValue::Type::Number && val->num == 0) ||
                                       (key == "encode.adapter" && val->type == JsonValue::Type::String && val->str == "auto");
        if (const JsonValue* cur = current.Find(key); !resolves_at_build && cur && cur->type == val->type &&
            ((cur->type == JsonValue::Type::Bool && cur->b == val->b) ||
             (cur->type == JsonValue::Type::Number && cur->num == val->num) ||
             (cur->type == JsonValue::Type::String && cur->str == val->str))) {
            continue;
        }
        for (const auto& meta : settings) {
            if (key == meta.key) {
                if (meta.apply == ApplyTier::Realloc) {
                    max_tier = ApplyTier::Realloc;
                } else if (meta.apply == ApplyTier::Rebuild && max_tier != ApplyTier::Realloc) {
                    max_tier = ApplyTier::Rebuild;
                }
                break;
            }
        }
    }
    auto it_preset = flat.find("encode.quality_preset");
    auto it_bitrate = flat.find("encode.bitrate_kbps");

    // fps for the preset<->bitrate derivation below: the patch's own new value if this same edit
    // also touches capture.framerate, else current_cfg's already-live value. `working` hasn't been
    // patched yet at this point (ApplyConfigPatch runs after), so it can't be read from `working`
    // here -- same reason it_preset/it_bitrate are read from `flat` instead of `working` above.
    // Both sides resolved, so "match display" (0) compares as the rate it actually records at.
    const int current_fps = ResolveFramerate(current_cfg.capture_framerate, display_hz);
    int fps = current_fps;
    if (auto it_fps = flat.find("capture.framerate");
        it_fps != flat.end() && it_fps->second->type == JsonValue::Type::Number) {
        fps = ResolveFramerate(static_cast<int64_t>(it_fps->second->num), display_hz);
    }

    // The bitrate presets are calibrated by the encoded size, so capture.resolution scales the
    // source size (the caller's display_width/height) the same way the pipeline will.
    const VideoSize current_out = ResolveOutputSize(static_cast<uint32_t>(display_width),
                                                    static_cast<uint32_t>(display_height),
                                                    current_cfg.capture_resolution);
    VideoSize out = current_out;
    if (auto it_res = flat.find("capture.resolution");
        it_res != flat.end() && it_res->second->type == JsonValue::Type::String) {
        out = ResolveOutputSize(static_cast<uint32_t>(display_width), static_cast<uint32_t>(display_height),
                                it_res->second->str);
    }
    const int out_w = static_cast<int>(out.width), out_h = static_cast<int>(out.height);

    // The codec the pipeline will record, before and after: the patch's codec, GPU or AMF-unaligned
    // choice can each turn AV1 into H.264 or back, and the preset bitrate depends on it.
    auto patched_str = [&](const char* key, const std::string& live) {
        auto it = flat.find(key);
        return it != flat.end() && it->second->type == JsonValue::Type::String ? it->second->str : live;
    };
    const VideoCodec current_codec = RecordedCodec(gpus, current_cfg.encode_adapter, current_cfg.encode_video_codec,
                                                   current_cfg.encode_amf_av1_unaligned, current_out);
    const VideoCodec codec = RecordedCodec(gpus, patched_str("encode.adapter", current_cfg.encode_adapter),
                                           patched_str("encode.video_codec", current_cfg.encode_video_codec),
                                           patched_str("encode.amf_av1_unaligned", current_cfg.encode_amf_av1_unaligned),
                                           out);

    // A framerate, resolution or recorded-codec change re-derives the live preset's bitrate too
    // (the preset is fps-, size- and codec-scaled).
    const bool rate_or_size_only = it_preset == flat.end() && it_bitrate == flat.end() &&
                                   (fps != current_fps || !(out == current_out) || codec != current_codec);
    if (rate_or_size_only) {
        const std::string& preset = current_cfg.encode_quality_preset;
        if (preset != "custom") {
            int derived_br = QualityPresetToBitrate(preset, out_w, out_h, fps, codec);
            working.encode_bitrate_kbps = derived_br;
            preview.derived.push_back({"encode.bitrate_kbps", JsonValue::MakeNum(derived_br)});
        }
    } else if (it_preset != flat.end() && it_preset->second->type == JsonValue::Type::String) {
        std::string preset = it_preset->second->str;
        if (preset != "custom" && it_bitrate == flat.end()) {
            int derived_br = QualityPresetToBitrate(preset, out_w, out_h, fps, codec);
            working.encode_bitrate_kbps = derived_br;
            preview.derived.push_back({"encode.bitrate_kbps", JsonValue::MakeNum(derived_br)});
        }
    } else if (it_bitrate != flat.end() && it_bitrate->second->type == JsonValue::Type::Number) {
        int br = static_cast<int>(it_bitrate->second->num);
        if (it_preset == flat.end()) {
            std::string derived_p = BitrateToQualityPreset(br, out_w, out_h, fps, codec);
            working.encode_quality_preset = derived_p;
            preview.derived.push_back({"encode.quality_preset", JsonValue::MakeStr(derived_p)});
        }
    }

    // A derived bitrate resizes the ring like a typed one would, whatever tier set it off.
    for (const auto& [key, val] : preview.derived) {
        if (key == "encode.bitrate_kbps" &&
            static_cast<int64_t>(val.num) != current_cfg.encode_bitrate_kbps) {
            max_tier = ApplyTier::Realloc;
        }
    }

    ApplyConfigPatch(working, patch_json);

    int64_t bitrate = working.encode_bitrate_kbps;
    preview.bitrate_kbps = bitrate;
    preview.sizing = ComputeRingSizing(working, working.buffer_seconds);
    // Whatever key moved the video ring's size has to re-Init it (encode.rate_control to or from CQP).
    const RingSizing current_sizing = ComputeRingSizing(current_cfg, current_cfg.buffer_seconds);
    if (preview.sizing.video_capacity != current_sizing.video_capacity ||
        preview.sizing.video_overshoot != current_sizing.video_overshoot) {
        max_tier = ApplyTier::Realloc;
    }
    preview.apply_tier = max_tier;
    preview.ring_bytes = preview.sizing.video_capacity;
    preview.mb_per_min = (bitrate * 1000.0 / 8.0 * 60.0) / (1024.0 * 1024.0);
    preview.fps = ResolveFramerate(working.capture_framerate, display_hz);
    preview.display_fps = ResolveFramerate(0, display_hz);

    if (preview.apply_tier == ApplyTier::Realloc) {
        preview.warnings.push_back("Applying this setting will reallocate the buffer and discard current replay footage.");
    }

    return preview;
}

std::vector<MonitorSpec> ResolveCaptureMonitors(const JsonValue& raw) {
    std::vector<std::string> devices;
    if (raw.type == JsonValue::Type::Array) {
        for (const JsonValue& entry : raw.arr) {
            if (entry.type == JsonValue::Type::String && !entry.str.empty()) devices.push_back(entry.str);
        }
    }
    if (devices.empty()) devices.push_back("primary");

    std::vector<MonitorSpec> out;
    out.reserve(devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        MonitorSpec spec;
        spec.device = devices[i];

        // "\\.\DISPLAY2" -> "DISPLAY2"; anything else (e.g. a second "primary") is used as-is.
        std::string name = devices[i];
        const std::string prefix = "\\\\.\\";
        if (name.rfind(prefix, 0) == 0) name = name.substr(prefix.size());

        spec.suffix = (i == 0) ? "" : (" - " + name);
        spec.label  = (devices[i] == "primary") ? "Primary" : name;
        out.push_back(std::move(spec));
    }
    return out;
}

std::wstring ExpandPathString(const std::string& input) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), w.data(), len);
    wchar_t buf[MAX_PATH];
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf);
    return w;
}

std::wstring ResolveOutputRoot(const std::string& output_root) {
    const std::wstring root = ExpandPathString(output_root.empty() ? "out" : output_root);
    const std::wstring default_root = ExpandPathString(Config{}.output_root);
    if (Instance().isolated) return RedirectDefaultRoot(root, default_root, Instance().data_dir + L"\\clips");
    if (_wcsicmp(root.c_str(), default_root.c_str()) != 0) return root;
    return RedirectDefaultRoot(root, default_root, FallbackClipsDir());
}

std::wstring RedirectDefaultRoot(const std::wstring& root, const std::wstring& default_root,
                                 const std::wstring& replacement) {
    return !replacement.empty() && _wcsicmp(root.c_str(), default_root.c_str()) == 0 ? replacement : root;
}

std::vector<AudioSourceSpec> ResolveAudioSources(const JsonValue& raw) {
    std::vector<AudioSourceSpec> out;
    if (raw.type == JsonValue::Type::Array) {
        for (const JsonValue& entry : raw.arr) {
            if (entry.type != JsonValue::Type::Object) continue;
            AudioSourceSpec spec;
            if (const JsonValue* v = entry.Find("id"); v && v->type == JsonValue::Type::String)
                spec.id = v->str;
            if (const JsonValue* v = entry.Find("kind"); v && v->type == JsonValue::Type::String)
                spec.kind = v->str;
            if (const JsonValue* v = entry.Find("device"); v && v->type == JsonValue::Type::String)
                spec.device = v->str;
            if (const JsonValue* v = entry.Find("mode"); v && v->type == JsonValue::Type::String)
                spec.mode = v->str;
            if (const JsonValue* v = entry.Find("target"); v && v->type == JsonValue::Type::String)
                spec.target = v->str;
            if (const JsonValue* v = entry.Find("name"); v && v->type == JsonValue::Type::String)
                spec.name = v->str;
            if (const JsonValue* v = entry.Find("enabled"); v && v->type == JsonValue::Type::Bool)
                spec.enabled = v->b;
            if (const JsonValue* v = entry.Find("group_id"); v && v->type == JsonValue::Type::String)
                spec.group_id = v->str;
            if (const JsonValue* v = entry.Find("gain_db"); v && v->type == JsonValue::Type::Number)
                spec.gain_db = v->num;
            if (spec.id.empty()) spec.id = "source" + std::to_string(out.size());
            // Ids name a source across live changes (DiffAudioSources, the hub's slots), so a
            // hand-edited duplicate gets a suffix rather than two sources sharing one identity.
            for (int n = 2; std::any_of(out.begin(), out.end(), [&](const AudioSourceSpec& o) { return o.id == spec.id; }); ++n)
                spec.id += "_" + std::to_string(n);
            // Default group_id to this source's own id -- a group of 1 is indistinguishable from
            // "no grouping," which is every config.json written before this feature existed.
            if (spec.group_id.empty()) spec.group_id = spec.id;
            out.push_back(std::move(spec));
        }
    }
    if (out.empty()) {
        AudioSourceSpec spec;  // kind=output_loopback, device="default", mode="all"
        spec.id = "source0";   // today's exact single-loopback default
        spec.group_id = spec.id;
        out.push_back(std::move(spec));
    }
    return out;
}

std::vector<AudioSourceSpec> SelectActiveAudioSources(const std::vector<AudioSourceSpec>& sources,
                                                      bool per_app) {
    std::vector<AudioSourceSpec> out;
    if (per_app) {
        out = sources;
    } else {
        const AudioSourceSpec* desktop = nullptr;
        const AudioSourceSpec* mic     = nullptr;
        for (const auto& s : sources) {
            if (!desktop && s.kind == "output_loopback") desktop = &s;
            if (!mic && s.kind == "input") mic = &s;
        }
        AudioSourceSpec d;
        if (desktop) {
            d = *desktop;
        } else {
            d.id = "desktop";
            d.group_id = d.id;
        }
        if (d.mode != "game_only") {
            d.mode = "all";
            d.target.clear();
        }
        out.push_back(std::move(d));
        if (mic) out.push_back(*mic);
    }
    if (out.empty()) {
        AudioSourceSpec spec;
        spec.id = "source0";
        spec.group_id = spec.id;
        out.push_back(std::move(spec));
    }
    return out;
}

AudioSourcesDiff DiffAudioSources(const std::vector<AudioSourceSpec>& before,
                                  const std::vector<AudioSourceSpec>& after) {
    AudioSourcesDiff d;
    std::vector<bool> matched(before.size(), false);
    for (const auto& b : after) {
        int found = -1;
        for (size_t i = 0; i < before.size(); ++i) {
            if (!matched[i] && before[i].id == b.id) {
                found = static_cast<int>(i);
                break;
            }
        }
        bool reopen = false;
        if (found >= 0) {
            matched[static_cast<size_t>(found)] = true;
            const auto& a = before[static_cast<size_t>(found)];
            reopen = a.kind != b.kind || a.device != b.device || a.mode != b.mode || a.target != b.target;
        }
        d.old_index.push_back(found);
        d.reopen.push_back(reopen);
    }
    for (size_t i = 0; i < before.size(); ++i)
        if (!matched[i]) d.removed.push_back(i);
    return d;
}

std::string AudioTrackName(const AudioSourceSpec& spec) {
    if (spec.kind == "input") return "Mic";
    if (spec.mode == "game_only") return "Game";
    if (spec.mode == "only" && !spec.name.empty()) return spec.name;
    if (spec.mode == "only" && !spec.target.empty()) {
        std::string name = spec.target;
        if (name.size() > 4) {
            std::string ext = name.substr(name.size() - 4);
            for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (ext == ".exe") name.resize(name.size() - 4);
        }
        return name;
    }
    return "Desktop";
}

namespace {

struct HotkeyField {
    const char*        key;
    std::string Config::* field;
};
constexpr HotkeyField kHotkeyFields[] = {
    {"hotkeys.save_replay", &Config::hotkeys_save_replay},
    {"hotkeys.toggle_buffer", &Config::hotkeys_toggle_buffer},
    {"hotkeys.screenshot", &Config::hotkeys_screenshot},
    {"hotkeys.overlay", &Config::hotkeys_overlay},
};

// A hotkey this patch set is kept only if it's bindable and no other action already uses the same
// combo; otherwise the field goes back to what it was. Stored in canonical spelling, so "alt+f10"
// and "Alt+F10" compare (and save) as one thing.
using HotkeySnapshot = std::array<std::string, std::size(kHotkeyFields)>;

HotkeySnapshot SnapshotHotkeys(const Config& cfg) {
    HotkeySnapshot snap;
    for (size_t i = 0; i < snap.size(); ++i) snap[i] = cfg.*kHotkeyFields[i].field;
    return snap;
}

void ValidateHotkeyFields(Config& cfg, const HotkeySnapshot& before, const FlatMap& flat,
                          std::vector<ConfigIssue>& issues) {
    for (size_t i = 0; i < std::size(kHotkeyFields); ++i) {
        const HotkeyField& hf = kHotkeyFields[i];
        if (!flat.count(hf.key) || cfg.*hf.field == before[i]) continue;
        HotkeyCombo combo;
        const HotkeyProblem problem = ParseHotkeySpec(cfg.*hf.field, combo) ? ValidateHotkeyCombo(combo)
                                                                             : HotkeyProblem::Unparseable;
        if (problem != HotkeyProblem::None) {
            issues.push_back({hf.key, "\"" + cfg.*hf.field + "\": " + HotkeyProblemText(problem)});
            cfg.*hf.field = before[i];
            continue;
        }
        cfg.*hf.field = FormatHotkeySpec(combo);
    }
    // Conflicts after that, against what each field finally holds: a value put back above can be the
    // one a field checked earlier took. Each pass puts one more back, so it ends at `before` at worst.
    for (bool again = true; again;) {
        again = false;
        for (size_t i = 0; i < std::size(kHotkeyFields) && !again; ++i) {
            const HotkeyField& hf = kHotkeyFields[i];
            HotkeyCombo combo;
            if (!flat.count(hf.key) || cfg.*hf.field == before[i] || !ParseHotkeySpec(cfg.*hf.field, combo)) continue;
            for (const auto& other : kHotkeyFields) {
                HotkeyCombo other_combo;
                if (other.field != hf.field && ParseHotkeySpec(cfg.*other.field, other_combo) && other_combo == combo) {
                    issues.push_back({hf.key, "\"" + cfg.*hf.field + "\" is already " + other.key});
                    cfg.*hf.field = before[i];
                    again = true;
                    break;
                }
            }
        }
    }
}

}  // namespace

ConfigPatchResult ApplyConfigPatch(Config& cfg, const JsonValue& flat_json) {
    ConfigPatchResult result;
    FlatMap flat;
    Flatten(flat_json, "", flat);
    std::set<std::string> visited;
    std::vector<ConfigIssue> issues;
    const HotkeySnapshot hotkeys_before = SnapshotHotkeys(cfg);

#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    ApplyBool(cfg.field, key, impl, flat, issues, visited);
    OMBRA_CONFIG_BOOLS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    ApplyInt(cfg.field, key, lo, hi, impl, flat, issues, visited);
    OMBRA_CONFIG_INTS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    ApplyDouble(cfg.field, key, lo, hi, impl, flat, issues, visited);
    OMBRA_CONFIG_DOUBLES(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    ApplyString(cfg.field, key, impl, flat, issues, visited);
    OMBRA_CONFIG_STRINGS(X)
#undef X
#define X(field, key, def, allowed, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    ApplyEnum(cfg.field, key, allowed, impl, flat, issues, visited);
    OMBRA_CONFIG_ENUMS(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    ApplyString(cfg.field, key, impl, flat, issues, visited);
    OMBRA_CONFIG_ACTIONS(X)
#undef X
#define X(key, tier, grp, app, impl, title, help, tradeoff) MarkRaw(key, impl, flat, visited);
    OMBRA_CONFIG_RAW(X)
#undef X
    ValidateHotkeyFields(cfg, hotkeys_before, flat, issues);

    for (const auto& [key, value] : flat) {
        if (visited.count(key) || kRetiredKeys.count(key)) continue;
        (void)value;
        issues.push_back({key, "unknown key — ignored"});
    }

    std::set<std::string> issue_keys;
    for (const auto& issue : issues) issue_keys.insert(issue.key);
    for (const auto& key : visited) {
        if (!issue_keys.count(key)) result.accepted.push_back(key);
    }
    result.rejected = std::move(issues);
    return result;
}

JsonValue SerializeConfigFlat(const Config& cfg) {
    JsonValue root = JsonValue::MakeObject();
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    root.Set(key, JsonValue::MakeBool(cfg.field));
    OMBRA_CONFIG_BOOLS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    root.Set(key, JsonValue::MakeNum(static_cast<double>(cfg.field)));
    OMBRA_CONFIG_INTS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    root.Set(key, JsonValue::MakeNum(cfg.field));
    OMBRA_CONFIG_DOUBLES(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    root.Set(key, JsonValue::MakeStr(cfg.field));
    OMBRA_CONFIG_STRINGS(X)
#undef X
#define X(field, key, def, allowed, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    root.Set(key, JsonValue::MakeStr(cfg.field));
    OMBRA_CONFIG_ENUMS(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    root.Set(key, JsonValue::MakeStr(cfg.field));
    OMBRA_CONFIG_ACTIONS(X)
#undef X
    return root;
}

namespace {

// Splits a "|"-separated SettingMeta field (m.presets / m.allowed) into a JSON string array.
// Both fields use "" to mean "no list" -- an empty array, not an array holding one empty string.
JsonValue SplitPipeList(const char* s) {
    JsonValue out = JsonValue::MakeArray();
    if (!s || s[0] == '\0') return out;
    std::string str(s);
    size_t start = 0;
    while (start <= str.size()) {
        size_t bar = str.find('|', start);
        size_t end = (bar == std::string::npos) ? str.size() : bar;
        out.arr.push_back(JsonValue::MakeStr(str.substr(start, end - start)));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

}  // namespace

JsonValue SerializeSettingsSchema() {
    JsonValue arr = JsonValue::MakeArray();
    for (const SettingMeta& m : AllSettings()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("key", JsonValue::MakeStr(m.key));
        entry.Set("kind", JsonValue::MakeStr(KindName(m.kind)));
        entry.Set("title", JsonValue::MakeStr(m.title));
        entry.Set("help", JsonValue::MakeStr(m.help));
        entry.Set("tradeoff", JsonValue::MakeStr(m.tradeoff));
        entry.Set("tier", JsonValue::MakeStr(TierNameStr(m.tier)));
        entry.Set("group", JsonValue::MakeStr(m.group));
        entry.Set("presets", SplitPipeList(m.presets));
        entry.Set("unit", JsonValue::MakeStr(m.unit));
        entry.Set("apply", JsonValue::MakeStr(ApplyTierStr(m.apply)));
        if (m.kind == SettingKind::Int || m.kind == SettingKind::Double) {
            entry.Set("min", JsonValue::MakeNum(m.min));
            entry.Set("max", JsonValue::MakeNum(m.max));
        }
        if (m.kind == SettingKind::Enum) {
            entry.Set("enum_options", SplitPipeList(m.allowed));
        }
        entry.Set("implemented", JsonValue::MakeBool(m.implemented));
        arr.arr.push_back(std::move(entry));
    }
    JsonValue root = JsonValue::MakeObject();
    root.Set("settings", std::move(arr));
    return root;
}

const std::vector<SettingMeta>& AllSettings() {
    static const std::vector<SettingMeta> table = [] {
        std::vector<SettingMeta> v;
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::Bool, title, help, tradeoff, tier, grp, pres, unt, app, 0, 0, "", impl});
        OMBRA_CONFIG_BOOLS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::Int, title, help, tradeoff, tier, grp, pres, unt, app, \
                                 static_cast<double>(lo), static_cast<double>(hi), "", impl});
        OMBRA_CONFIG_INTS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::Double, title, help, tradeoff, tier, grp, pres, unt, app, lo, hi, "", impl});
        OMBRA_CONFIG_DOUBLES(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::String, title, help, tradeoff, tier, grp, pres, unt, app, 0, 0, "", impl});
        OMBRA_CONFIG_STRINGS(X)
#undef X
#define X(field, key, def, allowed, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::Enum, title, help, tradeoff, tier, grp, pres, unt, app, 0, 0, allowed, impl});
        OMBRA_CONFIG_ENUMS(X)
#undef X
#define X(key, tier, grp, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::Raw, title, help, tradeoff, tier, grp, "", "", app, 0, 0, "", impl});
        OMBRA_CONFIG_RAW(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
        v.push_back(SettingMeta{key, SettingKind::Action, title, help, tradeoff, tier, grp, pres, unt, app, 0, 0, "", impl});
        OMBRA_CONFIG_ACTIONS(X)
#undef X
        return v;
    }();
    return table;
}

namespace {

void MergeConfigInto(JsonValue& root, const Config& cfg) {
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    InsertNested(root, key, JsonValue::MakeBool(cfg.field));
    OMBRA_CONFIG_BOOLS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    InsertNested(root, key, JsonValue::MakeNum(static_cast<double>(cfg.field)));
    OMBRA_CONFIG_INTS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    InsertNested(root, key, JsonValue::MakeNum(cfg.field));
    OMBRA_CONFIG_DOUBLES(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    InsertNested(root, key, JsonValue::MakeStr(cfg.field));
    OMBRA_CONFIG_STRINGS(X)
#undef X
#define X(field, key, def, allowed, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    InsertNested(root, key, JsonValue::MakeStr(cfg.field));
    OMBRA_CONFIG_ENUMS(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) \
    InsertNested(root, key, JsonValue::MakeStr(cfg.field));
    OMBRA_CONFIG_ACTIONS(X)
#undef X
}

JsonValue SerializeConfig(const Config& cfg) {
    JsonValue root; root.type = JsonValue::Type::Object;
    MergeConfigInto(root, cfg);
    return root;
}

// Atomic config writes prevent settings loss:
// every prior writer opened config.json directly with std::ofstream(..., trunc), so a crash, kill,
// or power loss between the open and the final flush left a truncated or half-written file on
// disk — LoadConfig's own ParseJson failure path then falls back to "built-in defaults for this
// run," silently discarding every setting. Fix: write to a sibling .tmp file in the
// same directory (same volume, so the rename below is atomic, not a copy), flush and close it, then
// MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) it over the real path — the
// standard "never observe a torn write" pattern. config.json is now either the old contents or the
// complete new ones, never a partial write, no matter when the process dies.
bool WriteConfigFileAtomic(const std::wstring& path, const JsonValue& root) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "// " OMBRA_PRODUCT_NAME " config — see `" OMBRA_ENGINE_EXE " --help` for every key. Written once on first run;\n"
             "// unrecognized keys (e.g. from a newer version) are left alone, never deleted.\n";
        WriteJson(f, root, 0);
        f << "\n";
        // Closed by hand: a full disk can fail only the last flush, which a destructor would swallow
        // and the rename below would then put a truncated file in place of the good one.
        f.close();
        if (!f) {
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());  // best-effort; a stray .tmp left behind is harmless either way
        return false;
    }
    return true;
}

bool WriteConfigFile(const std::wstring& path, const Config& cfg) {
    return WriteConfigFileAtomic(path, SerializeConfig(cfg));
}

// A key as config.json holds it: nested ({"engine": {"exclusions": ...}}) or, in a hand-made flat
// document, dotted at the top level.
JsonValue* FindConfigKey(JsonValue& root, const std::string& dotted) {
    for (auto& [k, v] : root.obj)
        if (k == dotted) return &v;
    const size_t dot = dotted.find('.');
    if (dot == std::string::npos) return nullptr;
    for (auto& [k, v] : root.obj) {
        if (k != dotted.substr(0, dot) || v.type != JsonValue::Type::Object) continue;
        for (auto& [k2, v2] : v.obj)
            if (k2 == dotted.substr(dot + 1)) return &v2;
    }
    return nullptr;
}

bool EraseConfigKey(JsonValue& root, const std::string& dotted) {
    auto erase_in = [](JsonValue& obj, const std::string& key) {
        const auto it = std::find_if(obj.obj.begin(), obj.obj.end(), [&](const auto& kv) { return kv.first == key; });
        if (it == obj.obj.end()) return false;
        obj.obj.erase(it);
        return true;
    };
    bool erased = erase_in(root, dotted);
    const size_t dot = dotted.find('.');
    if (dot == std::string::npos) return erased;
    for (auto& [k, v] : root.obj)
        if (k == dotted.substr(0, dot) && v.type == JsonValue::Type::Object) erased = erase_in(v, dotted.substr(dot + 1)) || erased;
    return erased;
}

}  // namespace

bool MigrateLegacyConfig(JsonValue& root) {
    if (root.type != JsonValue::Type::Object) return false;
    bool changed = false;

    const JsonValue* excl = FindConfigKey(root, "engine.exclusions");
    const JsonValue* over = FindConfigKey(root, "engine.game_overrides");
    if (excl || over) {
        const JsonValue* list_json = FindConfigKey(root, "games.list");
        std::vector<GameEntry> list = list_json ? ParseGamesList(*list_json) : std::vector<GameEntry>{};
        const std::vector<GameEntry> migrated =
            MigrateLegacyGameLists(excl ? *excl : JsonValue{}, over ? *over : JsonValue{}, list);
        if (!migrated.empty()) {
            list.insert(list.end(), migrated.begin(), migrated.end());
            InsertNested(root, "games.list", SerializeGamesList(list));
            Log::Info("config  migrated %zu engine.exclusions/engine.game_overrides entries into games.list",
                      migrated.size());
        }
    }
    for (const char* gone : {"engine.exclusions", "engine.game_overrides", "capture.target_programs", "encode.codec"})
        changed = EraseConfigKey(root, gone) || changed;

    if (JsonValue* target = FindConfigKey(root, "capture.target");
        target && target->type == JsonValue::Type::String && target->str == "window_with_fallback") {
        target->str = "window";
        changed     = true;
    }
    return changed;
}

ConfigLoadResult LoadConfig() {
    ConfigLoadResult result;
    // Every Raw key exists in `raw` on every return, Null when absent: the engine reads them with
    // .at(), so the early returns below (first run, unparsable file) used to abort it on start.
    for (const std::string& key : RawConfigKeys()) result.raw[key] = JsonValue{};

    const std::wstring dir = ConfigDirW();
    if (dir.empty()) {
        result.issues.push_back({"", "could not resolve %APPDATA% — using built-in defaults"});
        return result;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    result.path = dir + L"\\config.json";

    std::ifstream in(result.path, std::ios::binary);
    if (!in) {
        result.file_existed = false;
        if (!WriteConfigFile(result.path, result.config)) {
            result.issues.push_back({"", "could not write default config.json — using built-in "
                                          "defaults for this run"});
        }
        return result;
    }
    result.file_existed = true;

    std::stringstream ss;
    ss << in.rdbuf();
    in.close();  // the migration below may replace the file, which an open handle blocks
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    JsonValue root;
    try {
        root = ParseJson(text);
    } catch (const std::string& err) {
        result.issues.push_back({"", err + " — using built-in defaults for this run"});
        return result;
    }
    if (root.type != JsonValue::Type::Object) {
        result.issues.push_back({"", "config.json root must be an object — using built-in "
                                      "defaults for this run"});
        return result;
    }

    if (MigrateLegacyConfig(root) && !WriteConfigFileAtomic(result.path, root))
        result.issues.push_back({"", "could not write the migrated config.json -- migrating again next start"});

    ConfigPatchResult patch = ApplyConfigPatch(result.config, root);
    result.issues.insert(result.issues.end(), patch.rejected.begin(), patch.rejected.end());

    // Every Raw key may arrive nested ({"capture": {"monitors": [...]}})
    // rather than dotted at the top level — config.json is written nested (MergeConfigInto/
    // InsertNested), so a real on-disk file always takes the nested branch; the dotted Find() only
    // matches a flat hand-crafted document (e.g. a unit test's synthetic root).
    for (const std::string& key : RawConfigKeys()) {
        if (const JsonValue* flat = root.Find(key)) {
            result.raw[key] = *flat;
            continue;
        }
        const size_t dot = key.find('.');
        if (dot == std::string::npos) continue;
        if (const JsonValue* group = root.Find(key.substr(0, dot))) {
            if (const JsonValue* nested = group->Find(key.substr(dot + 1))) result.raw[key] = *nested;
        }
    }

    return result;
}

bool SaveConfig(const Config& cfg) {
    const std::wstring dir = ConfigDirW();
    if (dir.empty()) return false;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\config.json";

    JsonValue root;
    root.type = JsonValue::Type::Object;
    {
        // Scoped so the handle closes before WriteConfigFileAtomic's MoveFileExW below --
        // otherwise the rename tries to replace a path this same process still has open for read,
        // fails (silently, since every caller of SaveConfig drops its bool return), and config.json
        // never actually changes. Confirmed live: 2026-09-17's "audio.per_app_mode toggle reverts
        // after restart" report traced to exactly this on a config.json that already existed.
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }
            try {
                JsonValue parsed = ParseJson(text);
                if (parsed.type == JsonValue::Type::Object) root = std::move(parsed);
            } catch (const std::string&) {
            }
        }
    }

    for (const std::string& key : kRetiredKeys) EraseNested(root, key);
    MergeConfigInto(root, cfg);
    return WriteConfigFileAtomic(path, root);
}

bool SaveRawConfigValues(const RawConfigMap& values) {
    if (values.empty()) return true;
    const std::wstring dir = ConfigDirW();
    if (dir.empty()) return false;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\config.json";

    // Same read-existing-JSON/merge/write shape as SaveConfig — a raw value must survive whatever
    // SaveConfig itself last wrote for the known fields, and vice versa.
    JsonValue root;
    root.type = JsonValue::Type::Object;
    {
        // Scoped so the handle closes before WriteConfigFileAtomic's rename -- see SaveConfig's
        // comment above, same bug, same fix.
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }
            try {
                JsonValue parsed = ParseJson(text);
                if (parsed.type == JsonValue::Type::Object) root = std::move(parsed);
            } catch (const std::string&) {
            }
        }
    }

    for (const auto& [key, value] : values) InsertNested(root, key, value);

    return WriteConfigFileAtomic(path, root);
}

void RenderConfigHelp(std::FILE* out) {
    const char* tier_name[] = {"preset", "common", "advanced"};
    const char* apply_name[] = {"free", "rebuild", "realloc"};
    std::fprintf(out, "" OMBRA_PRODUCT_NAME " config keys (%%APPDATA%%\\" OMBRA_INSTALL_ID "\\config.json):\n\n");
    for (const SettingMeta& m : AllSettings()) {
        std::fprintf(out, "  %-28s [%s, group: %s, apply: %s%s]\n", m.key,
                     tier_name[static_cast<int>(m.tier)], m.group,
                     apply_name[static_cast<int>(m.apply)],
                     m.implemented ? "" : ", not yet wired to behavior");
        std::fprintf(out, "      %s\n", m.title);
        std::fprintf(out, "      %s\n", m.help);
        if (m.presets[0] != '\0') std::fprintf(out, "      Presets: %s\n", m.presets);
        if (m.unit[0] != '\0') std::fprintf(out, "      Unit: %s\n", m.unit);
        if (m.tradeoff[0] != '\0') std::fprintf(out, "      Tradeoff: %s\n", m.tradeoff);
        if (m.kind == SettingKind::Int || m.kind == SettingKind::Double) {
            if (!(m.min == 0 && m.max == 0))
                std::fprintf(out, "      Range: [%g, %g]\n", m.min, m.max);
        } else if (m.kind == SettingKind::Enum) {
            std::fprintf(out, "      Allowed: %s\n", m.allowed);
        }
        std::fprintf(out, "\n");
    }
}

}  // namespace ombra
