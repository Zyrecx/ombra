#pragma once

// What the first-run setup recommends and writes. Pure, so it can be
// tested without a window: facts about the PC go in, one config patch comes out.

#include "ombra/config.h"
#include "ombra/json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ombra {

struct SetupFacts {
    int width = 1920, height = 1080;  // the monitor being recorded
    int display_hz = 60;
    std::vector<RecordingGpuCaps> gpus;
};

struct SetupPlan {
    // Recommendations: RecommendSetup fills these from the facts.
    std::string adapter = "auto";
    std::string resolution = "native";
    int framerate = 60;
    std::string codec = "av1";
    std::string preset = "high";
    int custom_kbps = 0;  // set: preset is "custom" and this is the bitrate
    std::string rate_control = "vbr";
    double seconds = 60.0;
    std::string mode = "desktop";  // capture.mode: desktop = always, game = only in games
    bool autostart = true;
    bool desktop_audio = true, mic = true, one_track = true;
    std::string storage = "ram";
    std::string file_path;  // buffer.file_path when storage is "file"
    // The person's own choices: the setup starts from what the config holds and recommends nothing.
    std::string output_root;
    bool per_game_folders = true;
    bool toasts = true;
    std::string toast_position = "top_right";
    std::string hotkey_save = "Alt+F10", hotkey_overlay = "Alt+Z";
    std::string theme = "dark", accent = "theme", corners = "default", panel_position = "center";
    bool screen_frame = false;
    std::string sharing = "errors";  // diagnostics.sharing: off | errors | usage (opt-out default)
};

// The display's rate, capped at 60 (above 60 is unmeasured).
int RecommendedFramerate(int display_hz);
SetupPlan RecommendSetup(const SetupFacts& facts);
// The bitrate the plan records at: its preset for this monitor and codec, or custom_kbps.
int SetupBitrateKbps(const SetupPlan& plan, const SetupFacts& facts);
// What the plan's rings grow to: audio in memory always, video in memory only with storage "ram".
int64_t SetupMemoryBytes(const SetupPlan& plan, const SetupFacts& facts);
// The video ring's file, for storage "file".
int64_t SetupDiskFileBytes(const SetupPlan& plan, const SetupFacts& facts);

// audio.sources with Desktop and Mic set as asked; every other entry (an app's track) is kept.
// One track shares the Desktop's group_id; apart, each source is its own group.
JsonValue SetupAudioSources(const JsonValue* existing, bool desktop, bool mic, bool one_track);
// The set_config patch: flat dotted keys.
JsonValue SetupPatch(const SetupPlan& plan, const JsonValue* existing_sources);

}  // namespace ombra
