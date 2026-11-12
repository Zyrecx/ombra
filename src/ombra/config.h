// Config schema.
//
// %APPDATA%\Ombra\config.json, loaded once at startup, with live apply. Defaults
// for every key; the file is written back only if it was absent, so an unrecognized key from a future
// version is never silently deleted by an older binary loading it.
//
// SINGLE SOURCE OF TRUTH: the OMBRA_CONFIG_* macro lists below are expanded twice — once into
// Config's fields (config.h) and once into the kSettings descriptor table (config.cpp). Both come
// from the same macro invocations, so adding a field to Config without metadata is a compile error.
// Add a new setting by adding one row to the matching list, not by touching Config or kSettings
// directly.
//
// SettingMeta.implemented=false means the key is parsed, validated, and rendered in --help, but
// nothing in the engine reads it yet — the behavior it describes is still hardcoded or missing.
// Flip it to true in the same commit that wires the field to real behavior.
#pragma once

#include "ombra/brand.h"
#include "ombra/video_codec.h"

#include "ombra/json.h"
#include "ombra/ui_palette.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ombra {

enum class Tier { Preset, Common, Advanced };
enum class ApplyTier { Free, Rebuild, Realloc };
enum class SettingKind { Bool, Int, Double, String, Enum, Raw, Action };

struct SettingMeta {
    const char* key;           // dotted path, e.g. "buffer.seconds" — also the --help / error label
    SettingKind kind;
    const char* title;
    const char* help;
    const char* tradeoff;      // may be "" when there's no real tradeoff to state
    Tier        tier;
    const char* group;         // section within tier: "Quality", "Replay", "Output", "Hotkeys", etc.
    const char* presets;       // "|" separated list of natural choices: "30|60|120|144"
    const char* unit;          // "s", "Mbps", "kbps", "MB", "fps"
    ApplyTier   apply;         // Free | Rebuild | Realloc
    double      min = 0;       // Int/Double only
    double      max = 0;       // Int/Double only; min==0 && max==0 means "unbounded"
    const char* allowed = "";  // Enum only: "|"-separated, e.g. "off|desktop|game"
    bool        implemented = true;
};

// clang-format off

// field, key, default, tier, group, presets, unit, apply, implemented, title, help, tradeoff
#define OMBRA_CONFIG_BOOLS(X) \
    X(capture_capture_cursor, "capture.capture_cursor", true, Tier::Advanced, "Monitors", \
      "", "", ApplyTier::Free, true, \
      "Capture cursor", "Includes the mouse cursor in the video recording.", \
      "Off can look cleaner for gameplay-only clips.") \
    X(output_per_game_folders, "output.per_game_folders", true, Tier::Advanced, "Output", \
      "", "", ApplyTier::Free, true, \
      "Per-game folders", "Save clips into a subfolder named after the detected game.", \
      "Off puts every clip in one flat folder.") \
    X(engine_autostart, "engine.autostart", false, Tier::Common, "System", \
      "", "", ApplyTier::Free, true, \
      "Start with Windows", "Launch " OMBRA_PRODUCT_NAME " automatically at sign-in.", "") \
    X(ui_toast_enabled, "ui.toast_enabled", true, Tier::Common, "Notifications", \
      "", "", ApplyTier::Free, true, \
      "Notifications", \
      "Show a small on-screen popup for save/buffer events, even while the overlay is closed.", "") \
    X(ui_toast_on_save, "ui.toast_on_save", true, Tier::Common, "Notifications", \
      "", "", ApplyTier::Free, true, \
      "Save confirmations", "Show a popup when a clip finishes saving.", "") \
    X(ui_toast_on_buffer_toggle, "ui.toast_on_buffer_toggle", true, Tier::Common, "Notifications", \
      "", "", ApplyTier::Free, true, \
      "Buffer on/off", "Show a popup when the instant-replay buffer is turned on or off.", "") \
    X(ui_screen_frame, "ui.screen_frame", false, Tier::Common, "Appearance", \
      "", "", ApplyTier::Free, true, \
      "Screen frame", "Round off the screen's corners while the overlay is open, so it reads as one " \
      "contained surface.", \
      "Only drawn while the overlay is open. It is not a permanent window, so it never touches games.") \
    X(ui_memory_card, "ui.memory_card", true, Tier::Common, "Appearance", \
      "", "", ApplyTier::Free, true, \
      "Memory card", "Show a card beside Settings with what the replay buffer costs in RAM, updating " \
      "as you change buffer length, monitors and audio.", \
      "Hidden automatically when the screen is too narrow for it.") \
    X(ui_card_borders, "ui.card_borders", true, Tier::Common, "Appearance", \
      "", "", ApplyTier::Free, true, \
      "Card outlines", "Draw a thin outline around cards and rows. Off leaves only the fill.", "") \
    X(audio_full_mix_track, "audio.full_mix_track", false, Tier::Advanced, "Audio", \
      "", "", ApplyTier::Free, true, \
      "Combined track", \
      "Also save one extra track with every source mixed together.", \
      "Handy for a quick single-track upload; the separate tracks are still saved for editing.") \
    /* Picks which audio.sources entries run (SelectActiveAudioSources); set_config hands the new
       list to the audio target watcher, live. Rendered by the audio.sources panel's Simple/Per-app
       toggle, never as its own row. */ \
    X(audio_per_app_mode, "audio.per_app_mode", false, Tier::Advanced, "Audio", \
      "", "", ApplyTier::Free, true, \
      "Per-app control", \
      "On records each configured app on its own track. Off (Simple) records one Desktop track " \
      "plus the microphone, and per-app entries stay configured but aren't captured.", \
      "") \
    /* update.h: the check reads GitHub's release feed; nothing about the PC goes with it. */ \
    X(updates_check, "updates.check", true, Tier::Advanced, "System", \
      "", "", ApplyTier::Free, true, \
      "Check for updates", "Looks for a new release on GitHub twice a day.", \
      "Only the request itself reaches GitHub: no account, ID or system details.") \
    X(updates_auto_install, "updates.auto_install", true, Tier::Advanced, "System", \
      "", "", ApplyTier::Free, true, \
      "Install updates automatically", \
      "Installs a downloaded update while you're away from the PC and no game is running. " \
      "Off waits for Install now.", \
      "Installing restarts " OMBRA_PRODUCT_NAME ", which empties the replay buffer.") \
    X(engine_telemetry, "engine.telemetry", true, Tier::Advanced, "System", \
      "", "", ApplyTier::Free, true, \
      "Resource tracking", \
      "Records " OMBRA_PRODUCT_NAME "'s own memory, CPU, GPU and frame-pacing numbers every 30 seconds, plus events " \
      "like saves and restarts, to CSV files in %LOCALAPPDATA%\\" OMBRA_INSTALL_ID "\\telemetry. Nothing leaves " \
      "your machine.", \
      "A few MB of disk per day, capped at 50 MB total.")

// field, key, default, min, max, tier, group, presets, unit, apply, implemented, title, help, tradeoff
#define OMBRA_CONFIG_INTS(X) \
    X(capture_framerate, "capture.framerate", 60, 0, 240, Tier::Common, "Quality", \
      "0|30|60|120|144", "fps", ApplyTier::Rebuild, true, \
      "Capture framerate", "Frames per second captured and encoded. 0 matches the display's refresh rate.", \
      "Higher is smoother. The quality preset raises the bitrate with it.") \
    X(encode_bitrate_kbps, "encode.bitrate_kbps", 40000, 500, 200000, Tier::Common, "Quality", \
      "", "kbps", ApplyTier::Realloc, true, \
      "Target bitrate", "Target video bitrate in kbps.", \
      "Higher bitrate means better quality and a bigger ring buffer for the same length.") \
    X(encode_max_bitrate_kbps, "encode.max_bitrate_kbps", 0, 0, 200000, Tier::Advanced, "Quality", \
      "", "kbps", ApplyTier::Realloc, true, \
      "Max bitrate", "VBR peak bitrate in kbps. 0 derives it as 1.25x the target bitrate.", "") \
    X(encode_cq, "encode.cq", 20, 0, 51, Tier::Advanced, "Encoder", \
      "18|20|23|28", "", ApplyTier::Rebuild, true, \
      "Constant quality (CQ)", "Quality level used only when Rate control is Constant quality.", \
      "Lower is higher quality and a larger, less predictable file size.") \
    X(encode_keyframe_sec, "encode.keyframe_sec", 2, 1, 10, Tier::Advanced, "Encoder", \
      "1|2|4", "s", ApplyTier::Rebuild, true, \
      "Keyframe interval", "Seconds between forced IDR frames.", \
      "Shorter gives finer save-point granularity for ~10-15% more bitrate at 1s vs 4s.") \
    X(buffer_max_bytes, "buffer.max_bytes", 0, 0, 107374182400LL, Tier::Advanced, "Replay", \
      "", "MB", ApplyTier::Realloc, true, \
      "Buffer size override", "Ring size in bytes. 0 derives it from buffer.seconds and bitrate.", "") \
    X(audio_bitrate_kbps, "audio.bitrate_kbps", 192, 32, 512, Tier::Common, "Audio", \
      "128|192|256|320", "kbps", ApplyTier::Rebuild, true, \
      "Audio bitrate", "AAC bitrate in kbps.", "") \
    X(log_max_bytes, "log.max_bytes", 20 * 1024 * 1024, 1024 * 1024, 500 * 1024 * 1024, \
      Tier::Advanced, "System", "", "MB", ApplyTier::Free, true, \
      "Log size cap", "Total size of the logs directory before the oldest files are pruned.", "") \
    X(ui_screen_frame_radius, "ui.screen_frame_radius", 12, 0, 96, Tier::Common, "Appearance", \
      "12|24|48", "px", ApplyTier::Free, true, \
      "Frame corner radius", "How round the screen frame's inner corners are.", \
      "Only applies while Screen frame is on.") \
    X(ui_screen_frame_thickness, "ui.screen_frame_thickness", 8, 0, 32, Tier::Common, "Appearance", \
      "0|4|8|16", "px", ApplyTier::Free, true, \
      "Frame border", "Width of a solid border along the screen edge. 0 rounds only the corners.", \
      "Only applies while Screen frame is on.")

#define OMBRA_CONFIG_ACTIONS(X) \
    X(engine_autotune, "engine.autotune", "", Tier::Preset, "System", \
      "", "", ApplyTier::Free, true, \
      "Auto-Tune Encoder", "Benchmarks your hardware to recommend the best encoder settings.", "")

// field, key, default, min, max, tier, group, presets, unit, apply, implemented, title, help, tradeoff
#define OMBRA_CONFIG_DOUBLES(X) \
    X(buffer_seconds, "buffer.seconds", 60.0, 1.0, 1800.0, Tier::Preset, "Replay", \
      "30|60|120|300", "s", ApplyTier::Realloc, true, \
      "Replay length", "How far back " OMBRA_PRODUCT_NAME " can rewind when you hit the save hotkey.", \
      "Longer means more memory; the Memory card shows how much.")

// field, key, default, tier, group, presets, unit, apply, implemented, title, help, tradeoff
#define OMBRA_CONFIG_STRINGS(X) \
    X(encode_preset, "encode.preset", "p4", Tier::Advanced, "Encoder", \
      "p1|p2|p3|p4|p5|p6|p7", "", ApplyTier::Rebuild, true, \
      "Encoder preset", "p1 (fastest) through p7 (slowest, best quality). AMD GPUs map these onto " \
      "three steps: p1-p3 speed, p4-p5 balanced, p6-p7 quality.", "") \
    /* buffer.file_path uses a folder browser with a derived filename; capture.target_window uses a
       list_windows-backed picker, screens.cpp. */ \
    X(buffer_file_path, "buffer.file_path", "", Tier::Advanced, "Replay", \
      "", "", ApplyTier::Realloc, true, \
      "Buffer file path", \
      "Path used when buffer.storage is file. Empty derives %LOCALAPPDATA%\\" OMBRA_INSTALL_ID "\\ring.dat.", \
      "Only shown while buffer.storage is set to file.") \
    X(capture_target_window, "capture.target_window", "", Tier::Advanced, "Monitors", \
      "", "", ApplyTier::Rebuild, true, \
      "Target window", "The program to record when Capture target is One window: its exe path, or " \
      "a title or process name substring.", "") \
    X(output_root, "output.root", "%USERPROFILE%\\Videos\\" OMBRA_PRODUCT_NAME, Tier::Common, "Output", \
      "", "", ApplyTier::Free, true, \
      "Output folder", "Where saved clips are written.", "") \
    /* No extension: the container decides it (WithContainerExtension, save.h). An older config's
       trailing .mp4 is still accepted and replaced. */ \
    X(output_filename, "output.filename", "{game} {date} {time}{monitor_suffix}", \
      Tier::Advanced, "Output", \
      "{game} {date} {time}{monitor_suffix}|{date}_{time}_{game}|" OMBRA_PRODUCT_NAME "_{date}_{time}", \
      "", ApplyTier::Free, true, "File name", \
      "Placeholders: {game} {date} {time} {monitor_suffix}.", "") \
    X(hotkeys_save_replay, "hotkeys.save_replay", "Alt+F10", Tier::Preset, "Hotkeys", \
      "", "", ApplyTier::Free, true, \
      "Save replay hotkey", "Saves the last buffer.seconds of footage.", \
      "Alt+F10 is also ShadowPlay's default — sharing it is a known interaction, not a bug.") \
    X(hotkeys_toggle_buffer, "hotkeys.toggle_buffer", "Alt+F9", Tier::Common, "Hotkeys", \
      "", "", ApplyTier::Free, true, \
      "Toggle buffer hotkey", "Pauses or resumes buffering without exiting.", "") \
    X(hotkeys_screenshot, "hotkeys.screenshot", "Alt+F1", Tier::Common, "Hotkeys", \
      "", "", ApplyTier::Free, true, \
      "Screenshot hotkey", "Saves a single frame from the newest buffered access unit.", "") \
    X(hotkeys_overlay, "hotkeys.overlay", "Alt+Z", Tier::Common, "Hotkeys", \
      "", "", ApplyTier::Free, true, \
      "Overlay hotkey", "Opens the in-game overlay.", "")

// field, key, default, allowed, tier, group, presets, unit, apply, implemented, title, help, tradeoff
#define OMBRA_CONFIG_ENUMS(X) \
    X(encode_quality_preset, "encode.quality_preset", "high", "low|medium|high|ultra|custom", \
      Tier::Preset, "Quality", "low|medium|high|ultra|custom", "", ApplyTier::Realloc, true, \
      "Quality preset", "Derives video bitrate automatically from display resolution.", "") \
    X(capture_mode, "capture.mode", "desktop", "desktop|game", Tier::Preset, "Monitors", \
      "desktop|game", "", ApplyTier::Free, true, \
      "When to record", "Always records continuously. Only in games records while a game from " \
      "the Games list is open, in the foreground or not. Ignored for One window, which records " \
      "while that program is open. To turn the replay buffer off entirely, use the Instant Replay " \
      "toggle.", "") \
    X(capture_target, "capture.target", "monitor", "monitor|game|window", \
      Tier::Advanced, "Monitors", "monitor|game|window", "", ApplyTier::Rebuild, true, \
      "Capture target", "What goes into the video: whole displays; the game's window only (with " \
      "When = Always, the display while no game is open); or one program's window, recording " \
      "only while it is open.", "") \
    /* A downscale before the encoder that keeps the source's aspect. See
       ResolveOutputSize. */ \
    X(capture_resolution, "capture.resolution", "native", "native|1440p|1080p|720p", Tier::Common, \
      "Quality", "native|1440p|1080p|720p", "", ApplyTier::Rebuild, true, \
      "Resolution", "Records at this height and keeps the picture's shape. Native records what you see.", \
      "Lower makes the buffer and files smaller at the same quality preset. Never scales up.") \
    /* Capture always runs on the encode adapter (encode_target.h), so there is no separate
       capture.adapter. Stored as a vendor, never a LUID: LUIDs change on reboot. */ \
    X(encode_adapter, "encode.adapter", "auto", "auto|nvidia|amd|cpu", Tier::Advanced, "Encoder", \
      "auto|nvidia|amd|cpu", "", ApplyTier::Rebuild, true, \
      "Recording GPU", "Which GPU captures and encodes. Auto uses the NVIDIA GPU while the recorded game renders on it, else the GPU that drives the recorded display, else NVIDIA, then AMD, then the CPU.", \
      "The CPU encoder (x264) works on any PC but uses much more CPU, which can lower game FPS.") \
    /* Replaces encode.codec, which never did anything: the old default config wrote "h264" into\
       every file, so MigrateLegacyConfig drops it rather than let it pin users to H.264. */ \
    X(encode_video_codec, "encode.video_codec", "av1", "av1|h264", Tier::Advanced, "Encoder", \
      "av1|h264", "", ApplyTier::Rebuild, true, \
      "Codec", "AV1 makes sharper clips at the same size; H.264 plays on anything.", \
      "GPUs without AV1 encoding (before RTX 40 / RX 7000) record H.264 instead.")     /* Radeon RX 7000 / 780M code AV1 only at multiples of 64x16 and pad anything else (1080 lines        come out as 1082). The user can pick; pad is the default. AV1 needs 24-45% fewer bits than H.264 on the 780M. PlanEncode applies it. */     X(encode_amf_av1_unaligned, "encode.amf_av1_unaligned", "pad", "h264|pad|scale", Tier::Advanced, "Encoder",       "h264|pad|scale", "", ApplyTier::Rebuild, true,       "AV1 at an unsupported size", "This GPU codes AV1 only at some sizes: record H.264, pad the "       "picture with a few lines, or scale it slightly smaller.", "") \
    X(encode_rate_control, "encode.rate_control", "vbr", "vbr|cbr|cqp", Tier::Advanced, "Encoder", \
      "vbr|cbr|cqp", "", ApplyTier::Rebuild, true, \
      "Rate control", "How the encoder targets bitrate.", "") \
    X(buffer_storage, "buffer.storage", "ram", "ram|file", Tier::Advanced, "Replay", \
      "ram|file", "", ApplyTier::Realloc, true, \
      "Buffer storage", "ram keeps the ring in memory; file memory-maps it on disk.", \
      "Use file for long always-on buffers (20+ minutes). The file is recreated on every start, " \
      "so this trades RAM for disk; it does not keep the buffer across restarts.") \
    X(audio_codec, "audio.codec", "aac", "aac", Tier::Advanced, "Audio", \
      "aac", "", ApplyTier::Rebuild, true, \
      "Audio codec", "Audio codec used for encoding.", "Only aac exists today.") \
    X(save_overlap_policy, "save.overlap_policy", "truncate", "truncate|overlap", Tier::Advanced, \
      "Replay", "truncate|overlap", "", ApplyTier::Free, true, \
      "Overlap policy", \
      "truncate starts a fresh segment on every save; overlap lets quick re-saves share footage.", \
      "") \
    X(hotkeys_backend, "hotkeys.backend", "poll", "auto|register_hotkey|ll_hook|poll", Tier::Advanced, \
      "Hotkeys", "auto|register_hotkey|ll_hook|poll", "", ApplyTier::Free, true, \
      "Hotkey method", "poll reads key state directly (GetAsyncKeyState, same approach OBS uses) " \
      "instead of registering or hooking anything, so it works in games that block the others; " \
      "an app sharing the shortcut still reacts to it. auto uses RegisterHotKey and falls back " \
      "to a low-level keyboard hook if that fails to register.", "") \
    X(ui_theme, "ui.theme", "dark", "dark|light|black|slate|ember|forest|mocha|latte|nord|dracula|gruvbox|rosepine|system|user<n>", \
      Tier::Common, "Appearance", "dark|light|black|slate|ember|forest|mocha|latte|nord|dracula|gruvbox|rosepine|system", \
      "", ApplyTier::Free, true, \
      "UI theme", "Palette for the whole overlay (ombra/ui_palette.h); system follows Windows' app mode with Dark or " \
      "Light, userN one of ui.themes. Mono accent turns black on light palettes.", "") \
    X(ui_accent, "ui.accent", "theme", "theme|mono|violet|cyan|blue|green|#rrggbb", Tier::Common, "Appearance", \
      "theme|mono|violet|cyan|blue|green", "", ApplyTier::Free, true, \
      "Accent color", "Highlight color used across the panel: the theme's own (mono where it has none), a " \
      "preset or any #rrggbb.", "") \
    X(ui_panel_position, "ui.panel_position", "center", "center|top|bottom|left|right", Tier::Common, "Appearance", \
      "center|top|bottom|left|right", "", ApplyTier::Free, true, \
      "Panel position", "Where the overlay panel sits on screen.", \
      "Center floats in the middle. The other four hug that edge of the screen.") \
    X(ui_panel_attach, "ui.panel_attach", "floating", "floating|attached", Tier::Common, "Appearance", \
      "floating|attached", "", ApplyTier::Free, true, \
      "Panel style", "Whether the panel floats as a card or fuses into the screen edge.", \
      "Attached needs a panel position other than center.") \
    X(ui_screen_frame_color, "ui.screen_frame_color", "match", "match|contrast|accent", Tier::Common, "Appearance", \
      "match|contrast|accent", "", ApplyTier::Free, true, \
      "Frame color", "Color of the screen frame. Match fuses with the panel; contrast (white on dark, " \
      "black on light) or accent stands out over a game.", \
      "Only applies while Screen frame is on.") \
    X(ui_rail_position, "ui.rail_position", "left", "left|right|top|bottom", Tier::Common, "Appearance", \
      "left|right|top|bottom", "", ApplyTier::Free, true, \
      "Tab bar position", "Which side of the Settings panel the tab bar sits on.", \
      "Left/right stack the tabs vertically; top/bottom lay them out in a row.") \
    X(ui_corners, "ui.corners", "default", "sharp|default|round", Tier::Common, "Appearance", \
      "sharp|default|round", "", ApplyTier::Free, true, \
      "Corners", "How rounded cards, rows and controls are.", "") \
    X(log_level, "log.level", "info", "info|debug", Tier::Advanced, "System", \
      "info|debug", "", ApplyTier::Free, true, \
      "Detailed logging", "Debug adds detail lines to the log for troubleshooting.", "") \
    /* What leaves the PC: off, error reports (default, opt-out) or error
       reports plus performance and usage. Local logs and telemetry are written at every level. */ \
    X(diagnostics_sharing, "diagnostics.sharing", "errors", "off|errors|usage", Tier::Advanced, "System", \
      "off|errors|usage", "", ApplyTier::Free, true, \
      "Share with the developer", "Crash and error reports, and optionally performance, settings and the games you play.", "") \
    X(ui_font, "ui.font", "inter", "inter|system|geist", Tier::Common, "Appearance", \
      "inter|system|geist", "", ApplyTier::Free, true, \
      "Font", "The typeface of every label. Inter and Geist ship with Ombra; System is Windows' own.", "") \
    X(ui_density, "ui.density", "default", "compact|default|comfortable", Tier::Common, "Appearance", \
      "compact|default|comfortable", "", ApplyTier::Free, true, \
      "Density", "Spacing and control heights. Text size never changes.", "") \
    X(ui_toast_position, "ui.toast_position", "top_right", \
      "top_right|bottom_right|top_left|bottom_left", Tier::Common, "Notifications", \
      "top_right|bottom_right|top_left|bottom_left", "", ApplyTier::Free, true, \
      "Notification position", "Where save/buffer popups appear on screen.", \
      "Toasts slide in from the screen edge nearest their corner.") \
    X(ui_toast_style, "ui.toast_style", "card", "card|compact", Tier::Common, "Notifications", \
      "card|compact", "", ApplyTier::Free, true, \
      "Notification style", "Card: a full-width card. Compact: a pill as wide as its text.", "")

// key, tier, group, apply, implemented, title, help, tradeoff
#define OMBRA_CONFIG_RAW(X) \
    /* Implemented with a per-monitor toggle list (screens.cpp) instead of
       the generic single-row-per-key path -- this row's own tradeoff text below is therefore never
       actually rendered (Capture > Video draws the key itself), kept short anyway for consistency / --help output. ApplyTier is Realloc here for its
       confirm-before-apply UX, not its literal ring-reallocation meaning: there is no live
       pipeline-rebuild path (wmain builds the monitor-pipeline vector once at startup, a
       documented gap), so a change instead triggers a full engine self-restart (EngineSelfRestart,
       main.cpp), which drops the buffer exactly like a real Realloc would. */ \
    X("capture.monitors", Tier::Advanced, "Monitors", ApplyTier::Realloc, true, "Monitors", \
      "Which displays to capture. Each one is saved as its own file.", \
      "Changing this restarts " OMBRA_PRODUCT_NAME ".") \
    /* Applied live: set_config hands the new list to the audio target watcher (main.cpp), which
       opens, closes or reopens only the captures that changed (DiffAudioSources), and the hub
       keeps every track that still exists (AudioTrackHub::SetSources). Nothing is reallocated. */ \
    X("audio.sources", Tier::Advanced, "Audio", ApplyTier::Free, true, "Audio sources", \
      "Audio inputs to mix or track separately.", \
      "") \
    /* [{name, path, source, game}], games.h: the user's decisions only (games they added, "not a
       game" marks, overrides of a detected entry). Launcher-detected games are rescanned, never
       stored. Replaced engine.exclusions/engine.game_overrides/capture.target_programs, which
       MigrateLegacyConfig converts. Free: the engine's GameLibrary takes it live. Rendered by the
       Games page, never as a row. */ \
    X("games.list", Tier::Common, "Games", ApplyTier::Free, true, "Games", \
      "Which programs are games. Installed Riot, Steam and Epic games are found automatically.", "") \
    /* [{id, name, background, cards, menus, lines, text, muted}]: the user's own themes, picked
       by id in ui.theme (a rename keeps the selection). Drawn by the theme gallery, never a row. */ \
    X("ui.themes", Tier::Common, "Appearance", ApplyTier::Free, true, "Your themes", \
      "Themes you made: a name and six colours each.", "")

// clang-format on

struct Config {
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) bool field = def;
    OMBRA_CONFIG_BOOLS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) int64_t field = def;
    OMBRA_CONFIG_INTS(X)
#undef X
#define X(field, key, def, lo, hi, tier, grp, pres, unt, app, impl, title, help, tradeoff) double field = def;
    OMBRA_CONFIG_DOUBLES(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) std::string field = def;
    OMBRA_CONFIG_STRINGS(X)
#undef X
#define X(field, key, def, allowed, tier, grp, pres, unt, app, impl, title, help, tradeoff) std::string field = def;
    OMBRA_CONFIG_ENUMS(X)
#undef X
#define X(field, key, def, tier, grp, pres, unt, app, impl, title, help, tradeoff) std::string field = def;
    OMBRA_CONFIG_ACTIONS(X)
#undef X
};

// "#rrggbb" (either case) -> 0xRRGGBB; rgb may be null to only validate. The one parser for
// ui.accent's custom form. A schema `allowed` option spelled "#rrggbb" accepts any such value.
bool ParseHexRgb(const std::string& s, uint32_t* rgb);

// ui.themes: at most kMaxUserThemes, each with a "userN" id, a name (1-32 characters, unique
// ignoring case, not a preset's) and six "#rrggbb" colours. Parse of an invalid value gives what
// it can read; Serialize writes "#rrggbb".
bool                        ValidateUserThemes(const JsonValue& v, std::string& err);
std::vector<UserTheme>      ParseUserThemes(const JsonValue& v);
JsonValue                   SerializeUserThemes(const std::vector<UserTheme>& themes);
// A name ValidateUserThemes accepts among `themes` (`self_id`: the theme being renamed, skipped).
bool                        UserThemeNameOk(const std::string& name, const std::vector<UserTheme>& themes,
                                            const std::string& self_id, std::string* err = nullptr);

// String conversions
const char* ApplyTierStr(ApplyTier t);
const char* TierNameStr(Tier t);
const char* KindName(SettingKind k);

// Quality preset <-> bitrate derivation. `fps` scales the resolution-derived base bitrate around 60,
// the table's calibration baseline (capture.framerate's own default), so omitting fps is a no-op.
// Below 60: (fps/60)^0.7, which is 0.62x at 30. Measured 2026-11-03 (scripts/encbench.py --fps 30:
// NVENC and AMF, H.264 and AV1, 6 s each of Xiph's CSGO, DOTA2, GTAV, Witcher 3, each scored against
// its own frame rate): VMAF matching 1080p60 at the low/medium/high bitrates took a median 0.56x and
// at most 0.61x, 0.55-0.61 on GTAV and Witcher 3, whose quality still depends on bits. Other rates
// below 60 follow the curve, unmeasured.
// Above 60: sqrt(fps/60), unmeasured (the clips are 60 fps). It follows the shape of published
// streaming guidance and keeps a 144 fps capture from sharing a 60 fps bitrate.
// `codec` is what the pipeline records (RecordedCodec below): AV1 gets kAv1BitrateFactor of the
// H.264 bitrate, the same quality for fewer bytes.
int QualityPresetToBitrate(const std::string& preset, int width, int height, int fps = 60,
                           VideoCodec codec = VideoCodec::H264);
std::string BitrateToQualityPreset(int bitrate_kbps, int width, int height, int fps = 60,
                                   VideoCodec codec = VideoCodec::H264);

// AV1's preset bitrate as a share of H.264's. Measured 2026-10-31 (scripts/encbench.py: the
// engine's own NVENC/AMF encoders, four lossless 1080p Twitch gameplay clips from Xiph: CSGO,
// DOTA2, GTAV, Witcher 3; VMAF against the source). At every low/medium/high bitrate, AV1 at 0.8x
// scored on average +0.9 VMAF over H.264 at 1x at 60 fps (+0.3 at 30 fps), worst -1.0 / -1.3
// (CSGO, low and medium); Medal's 0.5x and Captail's 0.6x averaged -3.4 and -1.5. Equal-VMAF
// savings (BD-rate) were 22-30% on NVENC at 60 fps, 14-30% at 30, and 24-45% on the 780M's AMF.
// Only 1080p was measured; AV1's lead usually grows with resolution, so 0.8 is conservative above.
inline constexpr double kAv1BitrateFactor = 0.8;

// The size a pipeline encodes at for a capture.resolution value, given the captured size. The preset
// names the short side and the long side follows the source's aspect: at 1080p, 3440x1440 ->
// 2580x1080 and a 1080x1920 window -> itself. Both sides even. "native", an unknown value, or a
// preset at or above the source's short side gives the source size rounded down to even (what the
// capture stage textures already hold).
struct VideoSize {
    uint32_t width  = 0;
    uint32_t height = 0;
    bool operator==(const VideoSize&) const = default;
};
VideoSize ResolveOutputSize(uint32_t src_w, uint32_t src_h, const std::string& resolution);

// What a pipeline opens its encoder with, from the output size above, encode.video_codec and
// encode.amf_av1_unaligned. A GPU whose AV1 codes exactly only at multiples of align_w x align_h
// (the AMF probe's caps; 1x1 elsewhere) gets, at any other size: "h264" H.264 at that size, "pad"
// AV1 at that size (the encoder pads; the mux reads the coded size from the bitstream), "scale"
// AV1 at the size rounded down to the alignment. The UI's Codec card shows the same result.
struct EncodePlan {
    VideoCodec codec = VideoCodec::H264;
    VideoSize  size;
};
EncodePlan PlanEncode(VideoSize out, VideoCodec requested, uint32_t align_w, uint32_t align_h,
                      const std::string& unaligned_mode);
// The size an unaligned AV1 encode comes out at, measured on a 780M (RDNA3, 2026-10-15): width up
// to the alignment, height up to it too, except a height 8 past a multiple of 16 gains 2 lines
// (1080 -> 1082, 184 -> 186). Only for the UI's note; the mux reads the real size from the stream.
VideoSize PredictPaddedSize(VideoSize size, uint32_t align_w, uint32_t align_h);

// One GPU that can record, as the engine's startup probe reports it (get_status "recording_gpus",
// StartupGpuSummary::RecordingGpu). Plain data so config.cpp and the UI share the choice below.
struct RecordingGpuCaps {
    std::string vendor;  // encode.adapter value: "nvidia" | "amd" | "cpu"
    bool        av1 = false;
    uint32_t    av1_align_w = 1, av1_align_h = 1;
    bool        drives_display = false;  // owns the recorded monitor
};
// The GPU encode.adapter resolves to among `gpus`, as PickEncodeAdapter orders it: the named
// vendor if present, else the GPU that drives the display, else NVIDIA, AMD, the CPU. Null when
// the list is empty. Auto's pick with no game running: one on the NVIDIA GPU moves it there.
const RecordingGpuCaps* PickRecordingGpu(const std::vector<RecordingGpuCaps>& gpus, const std::string& adapter);
// The codec a pipeline records for these settings: H.264 on a GPU (or the CPU) without AV1, or
// where PlanEncode falls back; else the requested one. An empty `gpus` (not probed: tests, an older
// engine) trusts the request.
VideoCodec RecordedCodec(const std::vector<RecordingGpuCaps>& gpus, const std::string& adapter,
                         const std::string& video_codec, const std::string& amf_av1_unaligned, VideoSize out);
// A pipeline can open another codec than its preset bitrate was derived for: a fallback after NVIDIA
// is lost or won't open records H.264 at AV1's 0.8x. This moves `kbps` to the opened codec's share;
// Custom (`derived_for` unset) is the user's number and stays.
uint32_t BitrateForOpenedCodec(uint32_t kbps, std::optional<VideoCodec> derived_for, VideoCodec opened);

// The framerate a pipeline actually runs at. capture.framerate 0 means "match the display":
// `display_hz` (DisplayRefreshHz, util.h) with 59/119-style NTSC rates rounded up, clamped to
// 24..240, and 60 if the display's rate couldn't be read. Every engine read of capture.framerate
// goes through this; Config keeps the 0 so SaveConfig never persists the resolved number.
int ResolveFramerate(int64_t configured, int display_hz);

// Bytes each ring takes: `capacity` is the soft eviction budget, `overshoot` the extra physical
// headroom a pin needs (Ring::Init), so one ring commits capacity + overshoot. Shared by the
// engine's allocation and PreviewConfig so the projected and actual memory numbers can't drift.
struct RingSizing {
    int64_t video_capacity  = 0;  // per capture pipeline
    int64_t video_overshoot = 0;
    int64_t audio_capacity  = 0;  // per audio source: encoded track + PCM staging
    int64_t audio_overshoot = 0;
    int64_t VideoBytes() const { return video_capacity + video_overshoot; }
    int64_t AudioBytes() const { return audio_capacity + audio_overshoot; }
};
RingSizing ComputeRingSizing(const Config& cfg, double buffer_seconds);

// What the running engine actually allocated, reported by get_status.
struct MemoryLayout {
    RingSizing sizing;
    size_t     video_pipelines = 0;
    size_t     audio_sources   = 0;
    bool       file_backed     = false;  // buffer.storage=file: video rings are mapped files, not commit
};

struct ConfigPreview {
    ApplyTier                                      apply_tier = ApplyTier::Free;
    int64_t                                        ring_bytes = 0;  // one pipeline's video capacity, no overshoot
    RingSizing                                     sizing;          // per ring incl. overshoot; the UI multiplies by counts
    double                                         mb_per_min = 0.0;
    int64_t                                        bitrate_kbps = 0;  // Surfaced so
                                                                       // the Quality-preset row can
                                                                       // show what it resolves to.
    int                                            fps = 0;          // capture.framerate resolved, patch applied
    int                                            display_fps = 0;  // what "match display" (0) resolves to here
    std::vector<std::pair<std::string, JsonValue>> derived;
    std::vector<std::string>                       warnings;
};

// Generates preview calculations and warnings for candidate patch before applying
// `gpus` decides the recorded codec the preset bitrate is derived for (RecordedCodec).
ConfigPreview PreviewConfig(const Config& current_cfg, const JsonValue& patch_json,
                            int display_width = 1920, int display_height = 1080,
                            int display_hz = 60, const std::vector<RecordingGpuCaps>& gpus = {});

// The bitrate `cfg`'s quality preset derives for this source size, display rate and GPU list; 0
// for "custom". The engine re-derives at startup so a stored value always matches the preset.
int PresetBitrateFor(const Config& cfg, int source_width, int source_height, int display_hz,
                     const std::vector<RecordingGpuCaps>& gpus);
// The codec that bitrate is derived for (EncoderOptions::preset_bitrate_codec); unset for Custom.
std::optional<VideoCodec> PresetBitrateCodec(const Config& cfg, int source_width, int source_height,
                                             const std::vector<RecordingGpuCaps>& gpus);

// One issue per rejected key: unknown key, wrong JSON type, or an out-of-range/disallowed value.
struct ConfigIssue {
    std::string key;
    std::string message;
};

// Every OMBRA_CONFIG_RAW key's dotted name maps to its value exactly as
// read from config.json (Null if absent) — Config itself carries no field for a Raw key
// (SettingKind::Raw keys aren't decomposed), so this is the one place their values
// round-trip through. Table-driven off RawConfigKeys() so a Raw key added later needs a shape case
// in ValidateRawConfigValue(), not a new hand-patched call site wherever this map is read/written.
using RawConfigMap = std::map<std::string, JsonValue>;

std::vector<std::string> RawConfigKeys();
bool ValidateRawConfigValue(const std::string& key, const JsonValue& value, std::string& err);

// "key=value key=value" for every setting that differs from its default, and every Raw key present
// in config.json; long values are shortened ("[N entries]"). The engine's startup header logs it.
std::string DescribeNonDefaultSettings(const Config& cfg, const RawConfigMap& raw);

struct ConfigLoadResult {
    Config                   config;
    std::vector<ConfigIssue> issues;
    bool                     file_existed = false;
    std::wstring             path;
    // wmain resolves pipelines from raw.at("capture.monitors") via ResolveCaptureMonitors() below.
    RawConfigMap             raw;
};

// One entry per monitor pipeline, in config order. An empty or
// absent `raw` (Null, or an empty array) resolves to exactly one entry meaning "whatever monitor
// is primary right now" — today's only behavior. `device` is either the literal "primary" or a
// `\\.\DISPLAYn`-style name as EnumDisplayMonitors/GetMonitorInfo produce. `suffix` is what
// {monitor_suffix} in output.filename expands to: "" for the first entry (matching today's
// single-monitor filenames exactly), " - <name>" for every other entry, derived from the device
// string alone (no OS monitor enumeration here — this must stay callable from schematest with a
// synthetic JsonValue and no real display attached).
// `label` is a second, always-non-empty rendering of the same device string ("Primary" for the
// literal "primary", the bare "DISPLAYn" otherwise) — used to name a file *inside* a per-save
// group folder for an N>1 save (see DoSave's group-folder branch, main.cpp), where `suffix`'s
// "" for the first entry would collide with a directory listing that needs every monitor visibly
// labeled, not just the ones after the first.
struct MonitorSpec {
    std::string device;
    std::string suffix;
    std::string label;
};
std::vector<MonitorSpec> ResolveCaptureMonitors(const JsonValue& raw);

// Shapes "audio.sources" (already shape-validated by
// ValidateRawConfigValue -- this does not re-validate) into one entry per audio source. An empty or
// absent `raw` resolves to exactly one entry -- kind=output_loopback, device="default",
// mode="all" -- today's only behavior, byte-for-byte, so every existing config.json keeps working
// untouched. `id` defaults to "source0", "source1", ... when absent (used for logging and as a
// fallback track name). A `track` field used to live here (a per-source mux track number); deleted
// nothing ever read it, group_id is what actually drives track grouping (see below),
// and it was decorative enough that the UI handed out duplicate values. A stray "track"
// key in an old config.json is simply ignored, same as any other unknown key.
struct AudioSourceSpec {
    std::string id;
    std::string kind = "output_loopback";  // "output_loopback" | "input"
    std::string device = "default";
    std::string mode = "all";              // output_loopback only: all|all_except|game_only|only
    std::string target;
    std::string name;  // mode "only": the app's display name when it was added ("" = derive from target)
    bool        enabled = true;
    // Sources sharing a group_id are mixed into one AAC track
    // (main.cpp's save path sums their aligned PCM before encoding). Defaults to
    // this source's own `id` when absent in the raw config, so a group of 1 (today's only shape) is
    // indistinguishable from "no grouping" -- ResolveAudioSources below is where that default is
    // actually applied, this field just carries the resolved value.
    std::string group_id;
    // Recording-only gain applied when mixing this source's PCM into its group's track at save
    // time (main.cpp) -- never touches the system/session volume. Was reserved-but-unread before
    // the drag-to-group audio panel; 0.0 = unity gain, so every pre-existing config
    // (field absent) keeps today's exact output.
    double gain_db = 0.0;
};
std::vector<AudioSourceSpec> ResolveAudioSources(const JsonValue& raw);

std::wstring ExpandPathString(const std::string& input);
// output.root as a folder. An isolated instance (OMBRA_HOME) on the default root saves inside its
// home, where its Clips screen looks: the default is the user's real clips folder. Otherwise the
// default follows the real Videos folder (FallbackClipsDir), which the user may have moved.
std::wstring ResolveOutputRoot(const std::string& output_root);
// `replacement` if `root` is the default root (any case) and there is a replacement, else `root`.
std::wstring RedirectDefaultRoot(const std::wstring& root, const std::wstring& default_root,
                                 const std::wstring& replacement);

// Which configured sources the engine runs, in hub order. Per-app (`per_app`): every source.
// Simple: exactly the first output_loopback source forced to mode "all" (game_only kept), plus the
// first input on the default device. Disabled sources stay in the list with enabled=false (the
// audio target watcher captures nothing for them), so switching one on or off never changes the
// list's shape. Per-app entries stay in config either way.
std::vector<AudioSourceSpec> SelectActiveAudioSources(const std::vector<AudioSourceSpec>& sources,
                                                      bool per_app);

// How the audio target watcher gets from the running list `before` to `after` (both
// SelectActiveAudioSources' output), matching sources by id. Per entry of `after`: the index of
// the same source in `before` (-1: added), and whether its capture must be reopened (kind, device,
// mode or target changed; gain_db, group_id and enabled never reopen one). `removed` lists the
// indices of `before` with no match.
struct AudioSourcesDiff {
    std::vector<int>    old_index;
    std::vector<bool>   reopen;
    std::vector<size_t> removed;
};
AudioSourcesDiff DiffAudioSources(const std::vector<AudioSourceSpec>& before,
                                  const std::vector<AudioSourceSpec>& after);

// Track name written into a saved file: "Mic", "Desktop", "Game", or for mode "only" the app's saved
// display name, else its exe name without ".exe".
std::string AudioTrackName(const AudioSourceSpec& spec);

std::wstring ConfigDirW();

ConfigLoadResult LoadConfig();

// Rewrites what an older build wrote into what this one reads, in place: engine.exclusions and
// engine.game_overrides become games.list decisions (not a game / a game) and are removed along
// with capture.target_programs; capture.target window_with_fallback becomes window. True if `root`
// changed; LoadConfig then writes the file back once.
bool MigrateLegacyConfig(JsonValue& root);

const std::vector<SettingMeta>& AllSettings();

void RenderConfigHelp(std::FILE* out);

struct ConfigPatchResult {
    std::vector<std::string> accepted;   // keys found, valid, and applied to cfg
    std::vector<ConfigIssue> rejected;   // keys found but invalid
};

ConfigPatchResult ApplyConfigPatch(Config& cfg, const JsonValue& flat_json);

JsonValue SerializeConfigFlat(const Config& cfg);

JsonValue SerializeSettingsSchema();

bool SaveConfig(const Config& cfg);

// Writes Raw-kind key values straight into config.json, bypassing the
// Config struct entirely (it has no field for them) — the fix for the documented bug where
// set_config on capture.monitors/audio.sources/engine.exclusions reported "accepted" but the new
// value was never actually persisted (ApplyConfigPatch's MarkRaw only marks the key visited; there
// was nowhere for the value itself to go). Same read-existing-JSON/merge/write shape as SaveConfig,
// so a raw value survives alongside whatever SaveConfig itself last wrote for the known fields.
// Only entries already validated by ValidateRawConfigValue() should be passed here.
bool SaveRawConfigValues(const RawConfigMap& values);

}  // namespace ombra
