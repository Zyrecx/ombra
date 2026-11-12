// Config schema, bitrate/preset round-trip, filename/path templating, capture.monitors and
// audio.sources shape transforms, per-app audio target resolution, and Raw-key shape validation.
// Headless, no GPU. Ported from main.cpp's RunSchemaTest (was `ombra.exe schematest`) — same
// cases, doctest assertions instead of printf/++fails.

#include <doctest/doctest.h>

#include "ombra/audio.h"
#include "ombra/audio_tracks.h"
#include "ombra/config.h"
#include "ombra/json.h"
#include "ombra/paths.h"
#include "ombra/reconfig.h"
#include "ombra/util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace ombra::test {

TEST_CASE("schema: AllSettings() is non-empty and internally consistent") {
    const auto& settings = AllSettings();
    CHECK(!settings.empty());

    std::set<std::string> seen_keys;
    bool all_have_title_help = true;
    bool all_presets_valid = true;
    bool all_enums_valid = true;
    bool all_apply_valid = true;
    bool no_duplicate_keys = true;

    for (const auto& m : settings) {
        if (seen_keys.count(m.key)) no_duplicate_keys = false;
        seen_keys.insert(m.key);

        if (!m.title || m.title[0] == '\0' || !m.help || m.help[0] == '\0') all_have_title_help = false;

        if (m.apply != ApplyTier::Free && m.apply != ApplyTier::Rebuild && m.apply != ApplyTier::Realloc)
            all_apply_valid = false;

        if (m.kind == SettingKind::Enum) {
            std::string allowed(m.allowed);
            if (allowed.empty()) all_enums_valid = false;
        }

        if (m.presets && m.presets[0] != '\0') {
            std::string pres(m.presets);
            size_t start = 0;
            while (start <= pres.size()) {
                size_t bar = pres.find('|', start);
                std::string token = pres.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!token.empty()) {
                    if (m.kind == SettingKind::Int) {
                        try {
                            int64_t val = std::stoll(token);
                            if (!(m.min == 0 && m.max == 0) && (val < m.min || val > m.max))
                                all_presets_valid = false;
                        } catch (...) { all_presets_valid = false; }
                    } else if (m.kind == SettingKind::Double) {
                        try {
                            double val = std::stod(token);
                            if (!(m.min == 0 && m.max == 0) && (val < m.min || val > m.max))
                                all_presets_valid = false;
                        } catch (...) { all_presets_valid = false; }
                    } else if (m.kind == SettingKind::Enum) {
                        std::string allowed(m.allowed);
                        bool found = false;
                        size_t astart = 0;
                        while (astart <= allowed.size()) {
                            size_t abar = allowed.find('|', astart);
                            std::string aopt = allowed.substr(astart, abar == std::string::npos ? std::string::npos : abar - astart);
                            if (aopt == token) { found = true; break; }
                            if (abar == std::string::npos) break;
                            astart = abar + 1;
                        }
                        if (!found) all_presets_valid = false;
                    }
                }
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
        }
    }

    CHECK(all_have_title_help);
    CHECK(all_apply_valid);
    CHECK(all_enums_valid);
    CHECK(all_presets_valid);
    CHECK(no_duplicate_keys);
    CHECK(seen_keys.size() == settings.size());
}

TEST_CASE("schema: smaller sizes get their own preset bitrates") {
    CHECK(QualityPresetToBitrate("high", 1280, 720) == 11000);
    CHECK(QualityPresetToBitrate("high", 1600, 900) == 16000);
    CHECK(QualityPresetToBitrate("high", 1920, 1080) == 25000);
    CHECK(QualityPresetToBitrate("high", 1366, 768) == 11000);   // nearest by pixels
    CHECK(QualityPresetToBitrate("high", 1440, 900) == 16000);
    CHECK(QualityPresetToBitrate("high", 1680, 1050) == 25000);
    CHECK(QualityPresetToBitrate("high", 1728, 1080) == 25000);  // 2560x1600 at capture.resolution 1080p
}

TEST_CASE("schema: quality preset <-> bitrate round-trips at 720p, 900p, 1080p, 1440p, and 4K") {
    struct Res { int w, h; };
    const Res resolutions[] = { {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160} };
    const char* presets[] = { "low", "medium", "high", "ultra" };

    for (const auto& res : resolutions) {
        for (const auto* p : presets) {
            INFO("resolution ", res.w, "x", res.h, " preset ", p);
            int br = QualityPresetToBitrate(p, res.w, res.h);
            REQUIRE(br > 0);
            CHECK(BitrateToQualityPreset(br, res.w, res.h) == p);
        }
    }
}

TEST_CASE("schema: arbitrary bitrate maps to \"custom\" preset") {
    CHECK(BitrateToQualityPreset(33333, 1920, 1080) == "custom");
}

TEST_CASE("schema: fps scales the derived bitrate (Settings-IA-V2, 2026-09-06)") {
    // Around the table's 60fps calibration -- omitting fps must stay identical to explicit fps=60,
    // and a higher fps must derive a strictly higher bitrate at the same preset.
    const int br_60fps_explicit = QualityPresetToBitrate("high", 2560, 1440, 60);
    const int br_60fps_default  = QualityPresetToBitrate("high", 2560, 1440);
    const int br_144fps         = QualityPresetToBitrate("high", 2560, 1440, 144);
    CHECK(br_60fps_explicit == br_60fps_default);
    CHECK(br_144fps > br_60fps_default);
    CHECK(BitrateToQualityPreset(br_144fps, 2560, 1440, 144) == "high");
    // 30 fps sits above the most any measured clip needed (0.61x), and 59 and 61 meet 60 smoothly.
    const double br_60 = br_60fps_default;
    CHECK(QualityPresetToBitrate("high", 2560, 1440, 30) / br_60 == doctest::Approx(0.6156).epsilon(0.001));
    CHECK(QualityPresetToBitrate("high", 2560, 1440, 59) / br_60 == doctest::Approx(1.0).epsilon(0.02));
    CHECK(QualityPresetToBitrate("high", 2560, 1440, 61) / br_60 == doctest::Approx(1.0).epsilon(0.02));
    CHECK(BitrateToQualityPreset(QualityPresetToBitrate("high", 2560, 1440, 30), 2560, 1440, 30) == "high");
}

TEST_CASE("schema: AV1 presets take kAv1BitrateFactor of H.264's bitrate and read back as the same preset") {
    for (const char* p : {"low", "medium", "high", "ultra"}) {
        INFO("preset ", p);
        const int h264 = QualityPresetToBitrate(p, 1920, 1080, 30);
        const int av1  = QualityPresetToBitrate(p, 1920, 1080, 30, VideoCodec::AV1);
        CHECK(std::abs(av1 - h264 * kAv1BitrateFactor) <= 1.0);  // each side rounds once
        CHECK(BitrateToQualityPreset(av1, 1920, 1080, 30, VideoCodec::AV1) == p);
        CHECK(BitrateToQualityPreset(h264, 1920, 1080, 30, VideoCodec::AV1) == "custom");
    }
}

TEST_CASE("schema: a preset's bitrate moves to the codec a fallback opened; Custom's doesn't") {
    const uint32_t av1  = QualityPresetToBitrate("high", 1920, 1080, 30, VideoCodec::AV1);
    const uint32_t h264 = QualityPresetToBitrate("high", 1920, 1080, 30);
    CHECK(std::abs(int(BitrateForOpenedCodec(av1, VideoCodec::AV1, VideoCodec::H264)) - int(h264)) <= 1);
    CHECK(std::abs(int(BitrateForOpenedCodec(h264, VideoCodec::H264, VideoCodec::AV1)) - int(av1)) <= 1);
    CHECK(BitrateForOpenedCodec(av1, VideoCodec::AV1, VideoCodec::AV1) == av1);
    CHECK(BitrateForOpenedCodec(9000, std::nullopt, VideoCodec::H264) == 9000);

    Config cfg;
    cfg.encode_quality_preset = "high";
    cfg.encode_video_codec    = "av1";
    CHECK(PresetBitrateCodec(cfg, 1920, 1080, {{"nvidia", true}}) == VideoCodec::AV1);
    CHECK(PresetBitrateCodec(cfg, 1920, 1080, {{"cpu", false}}) == VideoCodec::H264);
    cfg.encode_quality_preset = "custom";
    CHECK_FALSE(PresetBitrateCodec(cfg, 1920, 1080, {{"nvidia", true}}).has_value());
}

TEST_CASE("schema: RecordedCodec is what the pipeline records, H.264 wherever AV1 can't be") {
    const std::vector<RecordingGpuCaps> gpus = {{"nvidia", true}, {"amd", true, 64, 16}, {"cpu", false}};
    const VideoSize p1080{1920, 1080}, p1440{2560, 1440};
    CHECK(RecordedCodec(gpus, "auto", "av1", "h264", p1080) == VideoCodec::AV1);        // NVIDIA first
    CHECK(RecordedCodec(gpus, "auto", "h264", "h264", p1080) == VideoCodec::H264);
    CHECK(RecordedCodec(gpus, "cpu", "av1", "h264", p1080) == VideoCodec::H264);        // x264 is H.264 only
    CHECK(RecordedCodec(gpus, "amd", "av1", "h264", p1080) == VideoCodec::H264);        // 1080 is off AMD's grid
    CHECK(RecordedCodec(gpus, "amd", "av1", "pad", p1080) == VideoCodec::AV1);
    CHECK(RecordedCodec(gpus, "amd", "av1", "h264", p1440) == VideoCodec::AV1);         // 1440 is on it
    CHECK(RecordedCodec({{"nvidia", false}}, "auto", "av1", "h264", p1080) == VideoCodec::H264);  // pre-RTX 40
    CHECK(RecordedCodec({}, "auto", "av1", "h264", p1080) == VideoCodec::AV1);         // unprobed: trust it
    CHECK(PickRecordingGpu(gpus, "amd")->vendor == "amd");
    CHECK(PickRecordingGpu({{"amd", true}, {"cpu", false}}, "nvidia")->vendor == "amd");  // missing: auto
    CHECK(PickRecordingGpu({}, "auto") == nullptr);
}

TEST_CASE("config: a change of recorded codec under a preset re-derives the bitrate and reallocates") {
    const std::vector<RecordingGpuCaps> gpus = {{"nvidia", true}, {"amd", true, 64, 16}, {"cpu", false}};
    Config cfg{};
    cfg.capture_framerate     = 60;
    cfg.encode_quality_preset = "high";
    cfg.encode_bitrate_kbps   = QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1);
    const int h264 = QualityPresetToBitrate("high", 1920, 1080, 60);

    JsonValue to_h264 = JsonValue::MakeObject();
    to_h264.Set("encode.video_codec", JsonValue::MakeStr("h264"));
    const ConfigPreview codec = PreviewConfig(cfg, to_h264, 1920, 1080, 60, gpus);
    CHECK(codec.bitrate_kbps == h264);
    CHECK(codec.apply_tier == ApplyTier::Realloc);

    // The AMD GPU still records AV1 at 1080p under the default unaligned rule (pad): nothing derives.
    JsonValue to_amd = JsonValue::MakeObject();
    to_amd.Set("encode.adapter", JsonValue::MakeStr("amd"));
    CHECK(PreviewConfig(cfg, to_amd, 1920, 1080, 60, gpus).derived.empty());

    // Under the h264 rule it records H.264 at 1080p: same bitrate as asking for it.
    to_amd.Set("encode.amf_av1_unaligned", JsonValue::MakeStr("h264"));
    CHECK(PreviewConfig(cfg, to_amd, 1920, 1080, 60, gpus).bitrate_kbps == h264);

    // Custom keeps the user's number.
    cfg.encode_quality_preset = "custom";
    CHECK(PreviewConfig(cfg, to_h264, 1920, 1080, 60, gpus).derived.empty());
}

TEST_CASE("config: PresetBitrateFor is the preset's bitrate for the source, display rate and GPU") {
    const std::vector<RecordingGpuCaps> gpus = {{"nvidia", true}, {"cpu", false}};
    Config cfg{};
    cfg.encode_quality_preset = "medium";
    cfg.capture_framerate     = 0;  // match display
    cfg.capture_resolution    = "1080p";
    CHECK(PresetBitrateFor(cfg, 2560, 1440, 30, gpus) == QualityPresetToBitrate("medium", 1920, 1080, 30, VideoCodec::AV1));
    cfg.encode_adapter = "cpu";
    CHECK(PresetBitrateFor(cfg, 2560, 1440, 30, gpus) == QualityPresetToBitrate("medium", 1920, 1080, 30));
    cfg.encode_quality_preset = "custom";
    CHECK(PresetBitrateFor(cfg, 2560, 1440, 30, gpus) == 0);
}

TEST_CASE("schema: FormatFilename replaces {game}/{date}/{time} tokens") {
    std::wstring fn1 = FormatFilename("{game} {date} {time}{monitor_suffix}.mp4", L"Apex Legends", L"");
    CHECK(fn1.find(L"Apex Legends") != std::wstring::npos);
    CHECK(fn1.find(L"{game}") == std::wstring::npos);
    CHECK(fn1.find(L"{date}") == std::wstring::npos);
    CHECK(fn1.find(L"{time}") == std::wstring::npos);

    std::wstring fn2 = FormatFilename("{game} {date} {time}{monitor_suffix}.mp4", L"", L" - DISPLAY1");
    CHECK(fn2.find(L"Desktop") != std::wstring::npos);
    CHECK(fn2.find(L" - DISPLAY1") != std::wstring::npos);
}

TEST_CASE("schema: ExpandPathString expands %USERPROFILE%") {
    std::wstring ep = ExpandPathString("%USERPROFILE%\\Videos\\Ombra");
    CHECK(!ep.empty());
    CHECK(ep.find(L"%USERPROFILE%") == std::wstring::npos);
}

// A user who moved Videos: the default follows it, a folder they chose stays, and with no known
// folder the literal default still works.
TEST_CASE("schema: the default output root follows a moved Videos folder") {
    const std::wstring def = L"C:\\Users\\A\\Videos\\Ombra";
    const std::wstring moved = L"D:\\Media\\Videos\\Ombra";
    CHECK(RedirectDefaultRoot(def, def, moved) == moved);
    CHECK(RedirectDefaultRoot(L"c:\\users\\a\\videos\\OMBRA", def, moved) == moved);
    CHECK(RedirectDefaultRoot(L"E:\\Clips", def, moved) == L"E:\\Clips");
    CHECK(RedirectDefaultRoot(def, def, L"") == def);
}

// The harness is an isolated instance. The default root is the user's real clips folder.
TEST_CASE("schema: ResolveOutputRoot keeps an isolated instance out of the user's clips") {
    const std::wstring home_clips = Instance().data_dir + L"\\clips";
    CHECK(ResolveOutputRoot(Config{}.output_root) == home_clips);
    // However it's spelled: a copied config can hold the expanded path, in any case.
    std::string expanded = NarrowUtf8(ExpandPathString(Config{}.output_root));
    CHECK(ResolveOutputRoot(expanded) == home_clips);
    std::transform(expanded.begin(), expanded.end(), expanded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    CHECK(ResolveOutputRoot(expanded) == home_clips);
    // A root the instance was given stays; so does the bare "out" sentinel.
    CHECK(ResolveOutputRoot("D:\\Clips") == L"D:\\Clips");
    CHECK(ResolveOutputRoot("") == L"out");
}

TEST_CASE("schema: ResolveCaptureMonitors — capture.monitors to pipeline specs") {
    auto specs_empty = ResolveCaptureMonitors(JsonValue{});  // absent key -> Null
    REQUIRE(specs_empty.size() == 1);
    CHECK(specs_empty[0].device == "primary");
    CHECK(specs_empty[0].suffix.empty());
    CHECK(specs_empty[0].label == "Primary");

    JsonValue empty_arr = JsonValue::MakeArray();
    auto specs_empty_arr = ResolveCaptureMonitors(empty_arr);
    REQUIRE(specs_empty_arr.size() == 1);
    CHECK(specs_empty_arr[0].device == "primary");
    CHECK(specs_empty_arr[0].suffix.empty());
    CHECK(specs_empty_arr[0].label == "Primary");

    JsonValue primary_arr = JsonValue::MakeArray();
    primary_arr.arr.push_back(JsonValue::MakeStr("primary"));
    auto specs_primary = ResolveCaptureMonitors(primary_arr);
    REQUIRE(specs_primary.size() == 1);
    CHECK(specs_primary[0].device == "primary");
    CHECK(specs_primary[0].suffix.empty());
    CHECK(specs_primary[0].label == "Primary");

    JsonValue two_arr = JsonValue::MakeArray();
    two_arr.arr.push_back(JsonValue::MakeStr("\\\\.\\DISPLAY1"));
    two_arr.arr.push_back(JsonValue::MakeStr("\\\\.\\DISPLAY2"));
    auto specs_two = ResolveCaptureMonitors(two_arr);
    REQUIRE(specs_two.size() == 2);
    CHECK(specs_two[0].device == "\\\\.\\DISPLAY1");
    CHECK(specs_two[0].suffix.empty());
    CHECK(specs_two[0].label == "DISPLAY1");
    CHECK(specs_two[1].device == "\\\\.\\DISPLAY2");
    CHECK(specs_two[1].suffix == " - DISPLAY2");
    CHECK(specs_two[1].label == "DISPLAY2");
}

TEST_CASE("schema: ResolveAudioSources — audio.sources to source specs") {
    auto specs_empty = ResolveAudioSources(JsonValue{});  // absent key -> Null
    REQUIRE(specs_empty.size() == 1);
    CHECK(specs_empty[0].kind == "output_loopback");
    CHECK(specs_empty[0].device == "default");
    CHECK(specs_empty[0].mode == "all");
    CHECK(specs_empty[0].enabled);

    JsonValue empty_arr = JsonValue::MakeArray();
    auto specs_empty_arr = ResolveAudioSources(empty_arr);
    REQUIRE(specs_empty_arr.size() == 1);
    CHECK(specs_empty_arr[0].kind == "output_loopback");

    JsonValue desktop = JsonValue::MakeObject();
    desktop.Set("id", JsonValue::MakeStr("desktop"));
    desktop.Set("kind", JsonValue::MakeStr("output_loopback"));
    desktop.Set("mode", JsonValue::MakeStr("game_only"));
    JsonValue mic = JsonValue::MakeObject();
    mic.Set("kind", JsonValue::MakeStr("input"));
    mic.Set("enabled", JsonValue::MakeBool(false));
    JsonValue two_sources = JsonValue::MakeArray();
    two_sources.arr.push_back(desktop);
    two_sources.arr.push_back(mic);
    auto specs_two_sources = ResolveAudioSources(two_sources);
    REQUIRE(specs_two_sources.size() == 2);
    CHECK(specs_two_sources[0].id == "desktop");
    CHECK(specs_two_sources[0].mode == "game_only");
    CHECK(specs_two_sources[1].id == "source1");
    CHECK(specs_two_sources[1].kind == "input");
    CHECK(!specs_two_sources[1].enabled);
}

// A clip saved in Simple mode had 9 audio tracks because Simple was
// UI-only state. This uses a real audio.sources shape: Desktop, per-app entries, and the mic
// at index 7 (not index 1).
TEST_CASE("schema: SelectActiveAudioSources — Simple vs Per-app selection, a real config's shape") {
    auto source = [](const char* id, const char* kind, const char* mode, const char* target) {
        JsonValue s = JsonValue::MakeObject();
        s.Set("id", JsonValue::MakeStr(id));
        s.Set("kind", JsonValue::MakeStr(kind));
        s.Set("device", JsonValue::MakeStr("default"));
        if (mode) {
            s.Set("mode", JsonValue::MakeStr(mode));
            s.Set("target", JsonValue::MakeStr(target));
        }
        s.Set("enabled", JsonValue::MakeBool(true));
        return s;
    };
    JsonValue real = JsonValue::MakeArray();
    real.arr.push_back(source("desktop", "output_loopback", "all", ""));
    real.arr.push_back(source("only_Code.exe_2", "output_loopback", "only", "Code.exe"));
    real.arr.push_back(source("only_LeagueClientUxRender.exe", "output_loopback", "only", "LeagueClientUxRender.exe"));
    real.arr.push_back(source("only_Riot Client.exe", "output_loopback", "only", "Riot Client.exe"));
    real.arr.push_back(source("only_Discord.exe", "output_loopback", "only", "Discord.exe"));
    real.arr.push_back(source("only_RiotClientServices.exe", "output_loopback", "only", "RiotClientServices.exe"));
    real.arr.push_back(source("only_brave.exe", "output_loopback", "only", "brave.exe"));
    real.arr.push_back(source("mic", "input", nullptr, nullptr));
    real.arr.push_back(source("only_League of Legends.exe", "output_loopback", "only", "League of Legends.exe"));
    auto configured = ResolveAudioSources(real);

    const auto simple = SelectActiveAudioSources(configured, false);
    REQUIRE(simple.size() == 2);
    CHECK(simple[0].id == "desktop");
    CHECK(simple[0].mode == "all");
    CHECK(simple[1].id == "mic");

    auto pinned_mic = configured;
    pinned_mic[7].device = "headset-id";
    CHECK(SelectActiveAudioSources(pinned_mic, false)[1].device == "headset-id");

    const auto per_app = SelectActiveAudioSources(configured, true);
    REQUIRE(per_app.size() == 9);
    CHECK(per_app[0].id == "desktop");

    auto edited = configured;
    edited[0].mode = "all_except";
    edited[0].target = "Discord.exe";
    const auto simple_excluding = SelectActiveAudioSources(edited, false);
    REQUIRE(!simple_excluding.empty());
    CHECK(simple_excluding[0].mode == "all");
    CHECK(simple_excluding[0].target.empty());

    edited[0].mode = "game_only";
    REQUIRE(!SelectActiveAudioSources(edited, false).empty());
    CHECK(SelectActiveAudioSources(edited, false)[0].mode == "game_only");

    // A switched-off source keeps its place, so switching it never changes the list's shape (and
    // so never restarts the engine); the watcher captures nothing for it.
    edited[7].enabled = false;
    edited[0].enabled = false;
    const auto simple_off = SelectActiveAudioSources(edited, false);
    REQUIRE(simple_off.size() == 2);
    CHECK_FALSE(simple_off[0].enabled);
    CHECK(simple_off[1].id == "mic");
    CHECK_FALSE(simple_off[1].enabled);

    edited[3].enabled = false;
    const auto per_app_off = SelectActiveAudioSources(edited, true);
    REQUIRE(per_app_off.size() == 9);
    CHECK_FALSE(per_app_off[3].enabled);
    CHECK(per_app_off[4].enabled);

    std::vector<AudioSourceSpec> mic_only(1);
    mic_only[0].id = "mic";
    mic_only[0].kind = "input";
    const auto simple_mic_only = SelectActiveAudioSources(mic_only, false);
    REQUIRE(simple_mic_only.size() == 2);
    CHECK(simple_mic_only[0].kind == "output_loopback");
    CHECK(simple_mic_only[0].mode == "all");
    CHECK(simple_mic_only[1].kind == "input");

    CHECK(AudioTrackName(per_app[0]) == "Desktop");
    CHECK(AudioTrackName(per_app[7]) == "Mic");
    CHECK(AudioTrackName(per_app[1]) == "Code");
    CHECK(AudioTrackName(per_app[3]) == "Riot Client");
    CHECK(AudioTrackName(edited[0]) == "Game");

    // The display name saved when an app track was added wins over the exe name (it's what the
    // card shows while the app isn't running, and what the saved file calls the track).
    JsonValue named = JsonValue::MakeArray();
    {
        JsonValue o = JsonValue::MakeObject();
        o.Set("id", JsonValue::MakeStr("only_RiotClientServices.exe"));
        o.Set("kind", JsonValue::MakeStr("output_loopback"));
        o.Set("mode", JsonValue::MakeStr("only"));
        o.Set("target", JsonValue::MakeStr("RiotClientServices.exe"));
        o.Set("name", JsonValue::MakeStr("Riot Client"));
        named.arr.push_back(o);
        std::string err;
        CHECK(ValidateRawConfigValue("audio.sources", named, err));
        o.Set("name", JsonValue::MakeNum(3));
        JsonValue bad = JsonValue::MakeArray();
        bad.arr.push_back(o);
        CHECK_FALSE(ValidateRawConfigValue("audio.sources", bad, err));
    }
    const auto named_specs = ResolveAudioSources(named);
    CHECK(AudioTrackName(named_specs[0]) == "Riot Client");
}

// Audio target watcher (RunEngine). Telemetry 2026-09-13: three mode="only" sources whose
// app wasn't running each recorded the whole desktop mix.
TEST_CASE("schema: WantedTargetCapture and FindTargetRootPid — per-app target resolution") {
    using C = AudioTargetCapture;
    CHECK(WantedTargetCapture("only", 0) == C::kNothing);
    CHECK(WantedTargetCapture("only", 1234) == C::kTargetOnly);
    CHECK(WantedTargetCapture("game_only", 0) == C::kNothing);
    CHECK(WantedTargetCapture("all_except", 0) == C::kAllDesktop);
    CHECK(WantedTargetCapture("all_except", 1234) == C::kAllExceptTarget);

    const std::vector<ProcessEntry> processes = {
        {4, 0, L"System"},
        {100, 50, L"explorer.exe"},
        {210, 200, L"Discord.exe"},  // a child listed before its root
        {200, 100, L"Discord.exe"},  // root: parent is explorer
        {220, 200, L"Discord.exe"},
    };
    CHECK(FindTargetRootPid(processes, L"discord.exe") == 200);
    CHECK(FindTargetRootPid(processes, L"brave.exe") == 0);
}

TEST_CASE("schema: implemented flags read as deliberate, not leftover") {
    const auto& settings = AllSettings();
    auto find_meta = [&](const char* key) -> const SettingMeta* {
        for (const auto& m : settings) if (m.key == std::string(key)) return &m;
        return nullptr;
    };
    auto impl = [&](const char* key) {
        const SettingMeta* m = find_meta(key);
        return m && m->implemented;
    };
    CHECK(impl("buffer.storage"));
    CHECK(impl("buffer.file_path"));
    CHECK(impl("capture.target"));
    CHECK(impl("capture.target_window"));
    {
        const SettingMeta* m = find_meta("capture.monitors");
        REQUIRE(m != nullptr);
        CHECK(m->implemented);
        CHECK(m->apply == ApplyTier::Realloc);
    }
    CHECK(impl("encode.adapter"));
    CHECK(impl("capture.resolution"));
    CHECK(impl("encode.video_codec"));
    {
        const SettingMeta* m = find_meta("audio.sources");
        REQUIRE(m != nullptr);
        CHECK(m->implemented);
        CHECK(m->apply == ApplyTier::Free);  // applied live by the audio target watcher, no confirm
    }
}

TEST_CASE("schema: Raw key shape validation — accept and reject cases") {
    auto keys = RawConfigKeys();
    CHECK(keys.size() == 4);
    CHECK(std::find(keys.begin(), keys.end(), "capture.monitors") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "audio.sources") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "games.list") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "ui.themes") != keys.end());

    std::string err;
    JsonValue good_strs = JsonValue::MakeArray();
    good_strs.arr.push_back(JsonValue::MakeStr("\\\\.\\DISPLAY1"));
    CHECK(ValidateRawConfigValue("capture.monitors", good_strs, err));
    CHECK(!ValidateRawConfigValue("games.list", good_strs, err));  // entries are objects
    CHECK(ValidateRawConfigValue("games.list", ParseJson(R"([{"name":"X","path":"C:\\X","source":"user","game":true}])"), err));

    JsonValue bad_strs = JsonValue::MakeArray();
    bad_strs.arr.push_back(JsonValue::MakeNum(1));
    CHECK(!ValidateRawConfigValue("capture.monitors", bad_strs, err));
    CHECK(!ValidateRawConfigValue("capture.monitors", JsonValue::MakeObject(), err));

    // Source shape: {id?, kind, device?, mode?, target?,
    // enabled?, gain_db?}. Absent/empty resolves to today's single-loopback behavior at
    // the runtime-default level (audio.cpp), not here — this only validates shape.
    JsonValue good_source = JsonValue::MakeObject();
    good_source.Set("id", JsonValue::MakeStr("desktop"));
    good_source.Set("kind", JsonValue::MakeStr("output_loopback"));
    good_source.Set("device", JsonValue::MakeStr("default"));
    good_source.Set("mode", JsonValue::MakeStr("game_only"));
    good_source.Set("target", JsonValue::MakeStr(""));
    good_source.Set("enabled", JsonValue::MakeBool(true));
    JsonValue mic_source = JsonValue::MakeObject();
    mic_source.Set("id", JsonValue::MakeStr("mic"));
    mic_source.Set("kind", JsonValue::MakeStr("input"));
    mic_source.Set("device", JsonValue::MakeStr("default"));
    JsonValue good_sources = JsonValue::MakeArray();
    good_sources.arr.push_back(good_source);
    good_sources.arr.push_back(mic_source);
    CHECK(ValidateRawConfigValue("audio.sources", good_sources, err));

    // "track" (AudioSourceSpec::track, config.h) was deleted -- it was never read
    // anywhere and the UI handed out duplicate values. A
    // wrong-typed value here used to be a hard validation error; now the key isn't looked at
    // at all, so this proves the check is actually gone, not just unreachable.
    JsonValue stray_bad_track = JsonValue::MakeObject();
    stray_bad_track.Set("kind", JsonValue::MakeStr("output_loopback"));
    stray_bad_track.Set("track", JsonValue::MakeStr("four"));
    JsonValue stray_bad_track_arr = JsonValue::MakeArray();
    stray_bad_track_arr.arr.push_back(stray_bad_track);
    CHECK(ValidateRawConfigValue("audio.sources", stray_bad_track_arr, err));

    JsonValue bad_kind = JsonValue::MakeObject();
    bad_kind.Set("kind", JsonValue::MakeStr("loopback"));  // old shape's value, no longer valid
    bad_kind.Set("device", JsonValue::MakeStr("default"));
    JsonValue bad_kind_arr = JsonValue::MakeArray();
    bad_kind_arr.arr.push_back(bad_kind);
    CHECK(!ValidateRawConfigValue("audio.sources", bad_kind_arr, err));

    JsonValue bad_source = JsonValue::MakeObject();
    bad_source.Set("kind", JsonValue::MakeStr("input"));
    bad_source.Set("mode", JsonValue::MakeStr("all"));  // mode only applies to output_loopback
    JsonValue bad_sources = JsonValue::MakeArray();
    bad_sources.arr.push_back(bad_source);
    CHECK(!ValidateRawConfigValue("audio.sources", bad_sources, err));

    JsonValue bad_mode = JsonValue::MakeObject();
    bad_mode.Set("kind", JsonValue::MakeStr("output_loopback"));
    bad_mode.Set("mode", JsonValue::MakeStr("everything"));  // not a valid enum value
    JsonValue bad_mode_arr = JsonValue::MakeArray();
    bad_mode_arr.arr.push_back(bad_mode);
    CHECK(!ValidateRawConfigValue("audio.sources", bad_mode_arr, err));

    JsonValue empty_sources = JsonValue::MakeArray();
    CHECK(ValidateRawConfigValue("audio.sources", empty_sources, err));
}

TEST_CASE("schema: ring sizing counts overshoot and scales with buffer.seconds") {
    Config cfg;
    cfg.encode_bitrate_kbps     = 40000;
    cfg.encode_max_bitrate_kbps = 0;  // derived: 1.25x bitrate
    cfg.buffer_max_bytes        = 0;

    const RingSizing s = ComputeRingSizing(cfg, 120.0);
    // 120 s * 50 Mbps max / 8 * 1.3 = 975 MB capacity; 2 s of max bitrate on top.
    CHECK(s.video_capacity == static_cast<int64_t>(120.0 * 50000 * 1000.0 / 8.0 * 1.3));
    CHECK(s.video_overshoot == static_cast<int64_t>(2.0 * 50000 * 1000.0 / 8.0));
    CHECK(s.VideoBytes() == s.video_capacity + s.video_overshoot);
    // Per source: its encoded track (AAC at audio.bitrate_kbps) plus its PCM staging.
    const auto audio = AudioTrackHub::SizeFor(120.0, static_cast<uint32_t>(cfg.audio_bitrate_kbps), 0);
    CHECK(s.audio_capacity == static_cast<int64_t>(audio.ring_capacity + AudioTrackHub::StageBytes()));
    CHECK(s.audio_overshoot == static_cast<int64_t>(audio.ring_overshoot));

    const RingSizing half = ComputeRingSizing(cfg, 60.0);
    CHECK(half.video_capacity * 2 == doctest::Approx(static_cast<double>(s.video_capacity)).epsilon(0.001));
    CHECK(half.video_overshoot == s.video_overshoot);  // overshoot doesn't scale with the buffer

    // buffer.max_bytes overrides the capacity only.
    cfg.buffer_max_bytes = 100LL * 1024 * 1024;
    CHECK(ComputeRingSizing(cfg, 120.0).video_capacity == 100LL * 1024 * 1024);

    // PreviewConfig reports the same numbers, so projected and live can't drift.
    cfg.buffer_max_bytes = 0;
    JsonValue patch = JsonValue::MakeObject();
    JsonValue buf   = JsonValue::MakeObject();
    buf.Set("seconds", JsonValue::MakeNum(120));
    patch.Set("buffer", std::move(buf));
    cfg.buffer_seconds = 30;
    const ConfigPreview prev = PreviewConfig(cfg, patch);
    CHECK(prev.sizing.video_capacity == s.video_capacity);
    CHECK(prev.ring_bytes == s.video_capacity);
}

TEST_CASE("config: constant quality doubles the video ring's cap, and switching to it reallocates") {
    Config cfg;
    cfg.encode_bitrate_kbps = 20000;
    cfg.buffer_seconds      = 120;
    const RingSizing vbr = ComputeRingSizing(cfg, 120.0);

    cfg.encode_rate_control = "cqp";
    const RingSizing cqp = ComputeRingSizing(cfg, 120.0);
    CHECK(cqp.video_capacity == static_cast<int64_t>(120.0 * 50000 * 1000.0 / 8.0 * 1.3));
    CHECK(cqp.video_overshoot == vbr.video_overshoot * 2);
    CHECK(cqp.audio_capacity == vbr.audio_capacity);

    // Max bitrate is hidden and unused under CQP: a stale value doesn't size the ring.
    cfg.encode_max_bitrate_kbps = 5000;
    CHECK(ComputeRingSizing(cfg, 120.0).video_capacity == cqp.video_capacity);
    cfg.encode_max_bitrate_kbps = 0;

    // buffer.max_bytes still wins.
    cfg.buffer_max_bytes = 100LL * 1024 * 1024;
    CHECK(ComputeRingSizing(cfg, 120.0).video_capacity == 100LL * 1024 * 1024);
    cfg.buffer_max_bytes = 0;

    const auto rate_patch = [](const char* rc) {
        JsonValue patch = JsonValue::MakeObject();
        JsonValue enc   = JsonValue::MakeObject();
        enc.Set("rate_control", JsonValue::MakeStr(rc));
        patch.Set("encode", std::move(enc));
        return patch;
    };
    // Out of CQP and back in move the ring, so they re-Init it; VBR <-> CBR doesn't.
    CHECK(PreviewConfig(cfg, rate_patch("vbr")).apply_tier == ApplyTier::Realloc);
    cfg.encode_rate_control = "vbr";
    const ConfigPreview to_cqp = PreviewConfig(cfg, rate_patch("cqp"));
    CHECK(to_cqp.apply_tier == ApplyTier::Realloc);
    CHECK(to_cqp.sizing.video_capacity == cqp.video_capacity);
    CHECK(PreviewConfig(cfg, rate_patch("cbr")).apply_tier == ApplyTier::Rebuild);
}

TEST_CASE("config: preview derives a bitrate from the preset only when the patch carries no bitrate") {
    // The UI's quality-preset sync depends on this rule: it must send the staged edits alone. It
    // used to send the whole config, bitrate included, so the "derived" bitrate was the old one.
    Config cfg{};
    cfg.capture_framerate = 60;
    cfg.encode_quality_preset = "medium";
    cfg.encode_bitrate_kbps = QualityPresetToBitrate("medium", 1920, 1080, 60, VideoCodec::AV1);
    const int high = QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1);
    REQUIRE(high != cfg.encode_bitrate_kbps);

    JsonValue preset_only = JsonValue::MakeObject();
    preset_only.Set("encode.quality_preset", JsonValue::MakeStr("high"));
    CHECK(PreviewConfig(cfg, preset_only, 1920, 1080).bitrate_kbps == high);

    JsonValue whole = SerializeConfigFlat(cfg);
    whole.Set("encode.quality_preset", JsonValue::MakeStr("high"));
    CHECK(PreviewConfig(cfg, whole, 1920, 1080).bitrate_kbps == cfg.encode_bitrate_kbps);
}

TEST_CASE("config: a framerate change under a preset re-derives the bitrate and reallocates") {
    Config cfg{};
    cfg.capture_framerate = 60;
    cfg.encode_quality_preset = "high";
    cfg.encode_bitrate_kbps = QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1);
    const int at_120 = QualityPresetToBitrate("high", 1920, 1080, 120, VideoCodec::AV1);
    REQUIRE(at_120 != cfg.encode_bitrate_kbps);

    JsonValue fps = JsonValue::MakeObject();
    fps.Set("capture.framerate", JsonValue::MakeNum(120));
    const ConfigPreview prev = PreviewConfig(cfg, fps, 1920, 1080);
    CHECK(prev.bitrate_kbps == at_120);
    CHECK(prev.apply_tier == ApplyTier::Realloc);  // framerate alone is Rebuild; the ring grows

    cfg.encode_quality_preset = "custom";
    const ConfigPreview custom = PreviewConfig(cfg, fps, 1920, 1080);
    CHECK(custom.bitrate_kbps == cfg.encode_bitrate_kbps);
    CHECK(custom.apply_tier == ApplyTier::Rebuild);
}

TEST_CASE("config: framerate 0 matches the display, rounding NTSC rates and clamping") {
    CHECK(ResolveFramerate(144, 60) == 144);   // an explicit rate ignores the display
    CHECK(ResolveFramerate(0, 165) == 165);
    CHECK(ResolveFramerate(0, 59) == 60);
    CHECK(ResolveFramerate(0, 119) == 120);
    CHECK(ResolveFramerate(0, 360) == 240);
    CHECK(ResolveFramerate(0, 0) == 60);       // unreadable display
}

// The real primary display: the exact path rate rounded, never more than 1 Hz off DEVMODE's
// truncated whole-Hz figure (143.98 Hz reads 144 here, 143 there).
TEST_CASE("util: DisplayRefreshHz reads the primary display's rate to the nearest Hz") {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    REQUIRE(GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi));
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    REQUIRE(EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm));
    const int hz = DisplayRefreshHz("primary");
    CAPTURE(hz);
    CAPTURE(dm.dmDisplayFrequency);
    CHECK(hz > 1);
    CHECK(hz >= static_cast<int>(dm.dmDisplayFrequency));
    CHECK(hz <= static_cast<int>(dm.dmDisplayFrequency) + 1);
    CHECK(DisplayRefreshHz(NarrowUtf8(mi.szDevice)) == hz);
}

TEST_CASE("config: switching to match-display re-derives the preset's bitrate at the display's rate") {
    Config cfg{};
    cfg.capture_framerate = 60;
    cfg.encode_quality_preset = "high";
    cfg.encode_bitrate_kbps = QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1);

    JsonValue match = JsonValue::MakeObject();
    match.Set("capture.framerate", JsonValue::MakeNum(0));
    CHECK(PreviewConfig(cfg, match, 1920, 1080, 165).bitrate_kbps ==
          QualityPresetToBitrate("high", 1920, 1080, 165, VideoCodec::AV1));
    // On a 60 Hz display nothing actually changes, so nothing is re-derived.
    CHECK(PreviewConfig(cfg, match, 1920, 1080, 60).derived.empty());
}

TEST_CASE("config: preview reports the rate match-display resolves to") {
    Config cfg{};
    cfg.capture_framerate = 60;
    JsonValue match = JsonValue::MakeObject();
    match.Set("capture.framerate", JsonValue::MakeNum(0));
    const ConfigPreview p = PreviewConfig(cfg, match, 1920, 1080, 119);  // 119.88 Hz reads as 119
    CHECK(p.fps == 120);
    CHECK(p.display_fps == 120);
    // A fixed rate stays fixed; display_fps still answers "what would match display be".
    const ConfigPreview fixed = PreviewConfig(cfg, JsonValue::MakeObject(), 1920, 1080, 165);
    CHECK(fixed.fps == 60);
    CHECK(fixed.display_fps == 165);
}

TEST_CASE("config: a batch re-sent at the live values doesn't rebuild or reallocate") {
    Config cfg{};
    cfg.encode_adapter = "auto";
    cfg.buffer_seconds = 120;

    JsonValue batch = JsonValue::MakeObject();
    batch.Set("encode.adapter", JsonValue::MakeStr("amd"));
    batch.Set("buffer.seconds", JsonValue::MakeNum(60));
    CHECK(PreviewConfig(cfg, batch, 1920, 1080).apply_tier == ApplyTier::Realloc);

    // The first Save committed it; the double-click's copy arrives against that config.
    cfg.encode_adapter = "amd";
    cfg.buffer_seconds = 60;
    CHECK(PreviewConfig(cfg, batch, 1920, 1080).apply_tier == ApplyTier::Free);

    JsonValue adapter_back = JsonValue::MakeObject();
    adapter_back.Set("encode.adapter", JsonValue::MakeStr("auto"));
    CHECK(PreviewConfig(cfg, adapter_back, 1920, 1080).apply_tier == ApplyTier::Rebuild);

    // Values resolved at build still rebuild when re-sent: that's how a user re-resolves them.
    cfg.encode_adapter    = "auto";
    cfg.capture_framerate = 0;
    CHECK(PreviewConfig(cfg, adapter_back, 1920, 1080).apply_tier == ApplyTier::Rebuild);
    JsonValue match = JsonValue::MakeObject();
    match.Set("capture.framerate", JsonValue::MakeNum(0));
    CHECK(PreviewConfig(cfg, match, 1920, 1080, 60).apply_tier == ApplyTier::Rebuild);
}

}  // namespace ombra::test
