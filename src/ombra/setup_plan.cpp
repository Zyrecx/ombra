#include "ombra/setup_plan.h"

#include <algorithm>

namespace ombra {

namespace {

Config PlanConfig(const SetupPlan& plan, int bitrate_kbps) {
    Config cfg;
    cfg.encode_adapter = plan.adapter;
    cfg.capture_resolution = plan.resolution;
    cfg.capture_framerate = plan.framerate;
    cfg.encode_video_codec = plan.codec;
    cfg.encode_quality_preset = plan.preset;
    cfg.encode_bitrate_kbps = bitrate_kbps;
    cfg.encode_rate_control = plan.rate_control;  // constant quality sizes the ring differently
    return cfg;
}

JsonValue Str(const std::string& s) { return JsonValue::MakeStr(s); }

std::string StrField(const JsonValue& obj, const char* key) {
    const JsonValue* v = obj.Find(key);
    return v && v->type == JsonValue::Type::String ? v->str : std::string();
}

}  // namespace

int RecommendedFramerate(int display_hz) {
    return (std::min)(ResolveFramerate(0, display_hz), 60);
}

SetupPlan RecommendSetup(const SetupFacts& facts) {
    SetupPlan plan;
    plan.framerate = RecommendedFramerate(facts.display_hz);
    const RecordingGpuCaps* gpu = PickRecordingGpu(facts.gpus, "auto");
    plan.codec = !gpu || gpu->av1 ? "av1" : "h264";
    return plan;
}

int SetupBitrateKbps(const SetupPlan& plan, const SetupFacts& facts) {
    if (plan.preset == "custom") return plan.custom_kbps;
    return PresetBitrateFor(PlanConfig(plan, 0), facts.width, facts.height, facts.display_hz, facts.gpus);
}

int64_t SetupMemoryBytes(const SetupPlan& plan, const SetupFacts& facts) {
    const RingSizing ring = ComputeRingSizing(PlanConfig(plan, SetupBitrateKbps(plan, facts)), plan.seconds);
    const int tracks = plan.desktop_audio && plan.mic && !plan.one_track ? 2 : plan.desktop_audio || plan.mic ? 1 : 0;
    return (plan.storage == "ram" ? ring.video_capacity : 0) + tracks * ring.audio_capacity;
}

int64_t SetupDiskFileBytes(const SetupPlan& plan, const SetupFacts& facts) {
    const RingSizing ring = ComputeRingSizing(PlanConfig(plan, SetupBitrateKbps(plan, facts)), plan.seconds);
    return ring.video_capacity + ring.video_overshoot;
}

JsonValue SetupAudioSources(const JsonValue* existing, bool desktop, bool mic, bool one_track) {
    JsonValue out = JsonValue::MakeArray();
    bool have_desktop = false, have_mic = false;
    std::string desktop_id = "desktop";
    // The first of each kind is the one Simple mode records (SelectActiveAudioSources).
    if (existing && existing->type == JsonValue::Type::Array) {
        for (const JsonValue& e : existing->arr) {
            if (e.type == JsonValue::Type::Object && StrField(e, "kind") == "output_loopback" && !StrField(e, "id").empty()) {
                desktop_id = StrField(e, "id");
                break;
            }
        }
        for (const JsonValue& e : existing->arr) {
            if (e.type != JsonValue::Type::Object) continue;
            JsonValue entry = e;
            const std::string kind = StrField(e, "kind");
            if (kind == "output_loopback" && !have_desktop) {
                have_desktop = true;
                entry.Set("enabled", JsonValue::MakeBool(desktop));
                entry.Set("group_id", Str(desktop_id));
            } else if (kind == "input" && !have_mic) {
                have_mic = true;
                entry.Set("enabled", JsonValue::MakeBool(mic));
                entry.Set("group_id", Str(one_track ? desktop_id : StrField(e, "id")));
            }
            out.arr.push_back(std::move(entry));
        }
    }
    if (!have_desktop) {
        JsonValue d = JsonValue::MakeObject();
        d.Set("id", Str(desktop_id));
        d.Set("kind", Str("output_loopback"));
        d.Set("name", Str("Desktop"));
        d.Set("enabled", JsonValue::MakeBool(desktop));
        d.Set("group_id", Str(desktop_id));
        out.arr.insert(out.arr.begin(), std::move(d));
    }
    if (!have_mic) {
        JsonValue m = JsonValue::MakeObject();
        m.Set("id", Str("mic"));
        m.Set("kind", Str("input"));
        m.Set("device", Str("default"));
        m.Set("name", Str("Mic"));
        m.Set("enabled", JsonValue::MakeBool(mic));
        m.Set("group_id", Str(one_track ? desktop_id : "mic"));
        out.arr.push_back(std::move(m));
    }
    return out;
}

JsonValue SetupPatch(const SetupPlan& plan, const JsonValue* existing_sources) {
    JsonValue p = JsonValue::MakeObject();
    p.Set("encode.adapter", Str(plan.adapter));
    p.Set("capture.resolution", Str(plan.resolution));
    p.Set("capture.framerate", JsonValue::MakeNum(plan.framerate));
    p.Set("encode.video_codec", Str(plan.codec));
    p.Set("encode.quality_preset", Str(plan.preset));
    if (plan.preset == "custom") p.Set("encode.bitrate_kbps", JsonValue::MakeNum(plan.custom_kbps));
    p.Set("encode.rate_control", Str(plan.rate_control));
    p.Set("buffer.seconds", JsonValue::MakeNum(plan.seconds));
    p.Set("buffer.storage", Str(plan.storage));
    if (plan.storage == "file") p.Set("buffer.file_path", Str(plan.file_path));
    p.Set("capture.mode", Str(plan.mode));
    p.Set("engine.autostart", JsonValue::MakeBool(plan.autostart));
    p.Set("audio.sources", SetupAudioSources(existing_sources, plan.desktop_audio, plan.mic, plan.one_track));
    if (!plan.output_root.empty()) p.Set("output.root", Str(plan.output_root));
    p.Set("output.per_game_folders", JsonValue::MakeBool(plan.per_game_folders));
    p.Set("ui.toast_enabled", JsonValue::MakeBool(plan.toasts));
    p.Set("ui.toast_position", Str(plan.toast_position));
    p.Set("hotkeys.save_replay", Str(plan.hotkey_save));
    p.Set("hotkeys.overlay", Str(plan.hotkey_overlay));
    p.Set("ui.theme", Str(plan.theme));
    p.Set("ui.accent", Str(plan.accent));
    p.Set("ui.corners", Str(plan.corners));
    p.Set("ui.panel_position", Str(plan.panel_position));
    p.Set("ui.screen_frame", JsonValue::MakeBool(plan.screen_frame));
    p.Set("diagnostics.sharing", Str(plan.sharing));
    return p;
}

}  // namespace ombra
