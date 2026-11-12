#include "ombra-ui/screens.h"
#include "ombra/brand.h"
#include "ombra-ui/ui/disk_location.h"
#include "ombra-ui/ui/paint.h"
#include "ombra/log.h"
#include "ombra/config.h"
#include "ombra/crash.h"
#include "ombra/crash_upload.h"
#include "ombra/util.h"
#include "ombra/diagnostics.h"
#include "ombra/version.h"
#include "ombra/hotkey_spec.h"
#include "ombra/paths.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <set>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    const int req = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring out(static_cast<size_t>(req), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), req);
    return out;
}

std::function<void(const std::wstring&)> g_native_window_sink;

// Vista+ folder picker, owned by the overlay so it stacks above the topmost overlay window (the
// old unowned SHBrowseForFolderW opened behind it). Returns "" on cancel/failure. COM is already
// initialised on the UI thread (OverlayHost ctor).
std::wstring PickFolder(HWND owner, const std::wstring& initial) {
    if (NativeWindowBlocked(L"folder picker")) return L"";
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!initial.empty()) {
        ComPtr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start.Get());
        }
    }
    if (FAILED(dlg->Show(owner))) return L"";
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return L"";
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return L"";
    std::wstring out = raw;
    CoTaskMemFree(raw);
    return out;
}

// AudioSourceSpec::gain_db's range on the Audio page's stepper: enough to quiet a loud app or lift a
// quiet one; past +12 the group's fixed headroom scale (audio_tracks.h) would clip.
constexpr double kGainDbMin = -24.0;
constexpr double kGainDbMax = 12.0;

// The source an empty audio.sources means (ResolveAudioSources, config.cpp), as the Audio page
// materializes it. StageAudioSources compares against it so an edit undone leaves staging.
ombra::JsonValue DefaultDesktopSource() {
    ombra::JsonValue d = ombra::JsonValue::MakeObject();
    d.Set("id", ombra::JsonValue::MakeStr("desktop"));
    d.Set("kind", ombra::JsonValue::MakeStr("output_loopback"));
    d.Set("device", ombra::JsonValue::MakeStr("default"));
    d.Set("mode", ombra::JsonValue::MakeStr("all"));
    d.Set("target", ombra::JsonValue::MakeStr(""));
    d.Set("enabled", ombra::JsonValue::MakeBool(true));
    return d;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    const int req = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (req <= 0) return "";
    std::string out(static_cast<size_t>(req), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), req, nullptr, nullptr);
    return out;
}


ApplyKind ApplyKindFromStr(const std::string& s) {
    if (s == "rebuild") return ApplyKind::Rebuild;
    if (s == "realloc") return ApplyKind::Realloc;
    return ApplyKind::Free;
}

// The Settings pages in rail order (SettingsRail), each with its sub-tabs. Every page but Capture >
// Audio has a hand-built builder; Audio takes its schema group's settings as rows.
struct SettingsPage {
    IconType icon;
    const wchar_t* label;
    std::vector<const wchar_t*> tabs;
};
const SettingsPage kSettingsPages[kRailCount] = {
    {IconType::Camera,   L"Capture",    {L"Video", L"Audio"}},
    {IconType::Replay,   L"Replay",     {L"Replay & Quality", L"Output"}},
    {IconType::Gamepad2, L"Games",      {}},
    {IconType::Palette,  L"Appearance", {L"Look", L"Layout", L"Notifications"}},
    {IconType::Keyboard, L"Shortcuts",  {}},
    {IconType::Sliders,  L"Advanced",   {L"Encoder", L"Diagnostics"}},
};

// Between the Settings panel and the memory card beside it.
constexpr float kPanelGapDip = 20.0f;
// The page's right edge keeps this free for the scroll bar; the sub-tabs and Save bar end with it.
constexpr float kScrollbarRoomDip = 14.0f;

// What an option is called on screen, where its config value isn't fit to show. Presentation
// only: the committed value is always the schema's own string, and every option not listed here
// shows its raw value (most are already words: "mp4", "dark", "128").
struct OptionLabel { const char* key; const char* value; const wchar_t* label; };
const OptionLabel kOptionLabels[] = {
    { "capture.mode",   "desktop",              L"Always" },
    { "capture.mode",   "game",                 L"Only in games" },
    { "capture.target", "monitor",              L"Displays" },
    { "capture.target", "game",                 L"The game" },
    { "capture.target", "window",               L"One window" },
    { "ui.theme",           "dark",     L"Dark" },
    { "ui.theme",           "light",    L"Light" },
    { "ui.theme",           "system",   L"System" },
    { "ui.theme",           "black",     L"True black" },
    { "ui.theme",           "slate",     L"Slate" },
    { "ui.theme",           "ember",     L"Ember" },
    { "ui.theme",           "forest",    L"Forest" },
    { "ui.theme",           "mocha",     L"Catppuccin" },
    { "ui.theme",           "latte",     L"Latte" },
    { "ui.theme",           "nord",      L"Nord" },
    { "ui.theme",           "dracula",   L"Dracula" },
    { "ui.theme",           "gruvbox",   L"Gruvbox" },
    { "ui.theme",           "rosepine",  L"Ros\x00e9 Pine" },
    { "ui.corners",         "sharp",    L"Sharp" },
    { "ui.corners",         "default",  L"Default" },
    { "ui.corners",         "round",    L"Round" },
    { "ui.font",            "inter",    L"Inter" },
    { "ui.font",            "system",   L"System" },
    { "ui.font",            "geist",    L"Geist" },
    { "ui.panel_attach",    "floating", L"Floating" },
    { "ui.panel_attach",    "attached", L"Attached" },
    { "ui.panel_position",  "center",   L"Center" },
    { "ui.panel_position",  "top",      L"Top" },
    { "ui.panel_position",  "bottom",   L"Bottom" },
    { "ui.panel_position",  "left",     L"Left" },
    { "ui.panel_position",  "right",    L"Right" },
    { "ui.rail_position",   "left",     L"Left" },
    { "ui.rail_position",   "right",    L"Right" },
    { "ui.rail_position",   "top",      L"Top" },
    { "ui.rail_position",   "bottom",   L"Bottom" },
    { "ui.screen_frame_color", "match",    L"Match" },
    { "ui.screen_frame_color", "contrast", L"Contrast" },
    { "ui.screen_frame_color", "accent",   L"Accent" },
    { "ui.toast_position",  "top_right",    L"Top right" },
    { "ui.toast_position",  "bottom_right", L"Bottom right" },
    { "ui.toast_position",  "top_left",     L"Top left" },
    { "ui.toast_position",  "bottom_left",  L"Bottom left" },
    { "ui.toast_style",     "card",         L"Card" },
    { "ui.toast_style",     "compact",      L"Compact" },
    { "ui.density",         "compact",      L"Compact" },
    { "ui.density",         "default",      L"Default" },
    { "ui.density",         "comfortable",  L"Comfortable" },
    { "capture.framerate",     "0",        L"Match display" },
    { "capture.resolution",    "native",   L"Native" },
    { "encode.quality_preset", "low",      L"Low" },
    { "encode.quality_preset", "medium",   L"Medium" },
    { "encode.quality_preset", "high",     L"High" },
    { "encode.quality_preset", "ultra",    L"Ultra" },
    { "encode.quality_preset", "custom",   L"Custom" },
    { "encode.video_codec",    "av1",      L"AV1" },
    { "encode.video_codec",    "h264",     L"H.264" },
    { "encode.amf_av1_unaligned", "h264",  L"H.264" },
    { "encode.amf_av1_unaligned", "pad",   L"Pad" },
    { "encode.amf_av1_unaligned", "scale", L"Scale" },
    { "encode.rate_control",   "vbr",      L"Variable (VBR)" },
    { "encode.rate_control",   "cbr",      L"Constant (CBR)" },
    { "encode.rate_control",   "cqp",      L"Constant quality" },
    { "encode.preset",         "p1",       L"P1 - fastest" },
    { "encode.preset",         "p2",       L"P2" },
    { "encode.preset",         "p3",       L"P3" },
    { "encode.preset",         "p4",       L"P4 - balanced" },
    { "encode.preset",         "p5",       L"P5" },
    { "encode.preset",         "p6",       L"P6" },
    { "encode.preset",         "p7",       L"P7 - best quality" },
    { "save.overlap_policy",   "truncate", L"Fresh each save" },
    { "save.overlap_policy",   "overlap",  L"Allow overlap" },
    { "buffer.storage",        "ram",      L"Memory" },
    { "buffer.storage",        "file",     L"Disk file" },
    { "hotkeys.backend",       "auto",             L"Automatic" },
    { "hotkeys.backend",       "register_hotkey",  L"RegisterHotKey" },
    { "hotkeys.backend",       "ll_hook",          L"Keyboard hook" },
    { "hotkeys.backend",       "poll",             L"Polling" },
};

std::wstring OptionLabelFor(const std::string& key, const std::string& value) {
    for (const auto& o : kOptionLabels) {
        if (key == o.key && value == o.value) return o.label;
    }
    return Utf8ToWide(value);
}

// Every implemented setting, by key: Custom is drawn by its page's builder (screens.h: Build*Page),
// Hidden lives in config.json only. A new key must be added here (the ui test "every implemented
// setting has a form" fails first).
struct FieldFormEntry { const char* key; FieldForm form; };
const FieldFormEntry kFieldForms[] = {
    // Old General group (now Shortcuts; engine.autostart is on Advanced > System)
    { "encode.quality_preset",     FieldForm::Custom },
    { "engine.autotune",           FieldForm::Custom },
    { "engine.autostart",          FieldForm::Custom },
    { "ui.toast_enabled",          FieldForm::Custom },
    { "hotkeys.save_replay",       FieldForm::Custom },    // KeyCaptureControl rows (BuildShortcutsPage)
    { "hotkeys.toggle_buffer",     FieldForm::Custom },
    { "hotkeys.screenshot",        FieldForm::Custom },
    { "hotkeys.overlay",           FieldForm::Custom },
    { "hotkeys.backend",           FieldForm::Custom },    // Troubleshooting card (BuildShortcutsPage)
    // Monitors & Audio
    { "capture.mode",              FieldForm::Custom },
    { "capture.target",            FieldForm::Custom },
    { "capture.target_window",     FieldForm::Custom },
    { "capture.monitors",          FieldForm::Custom },
    { "audio.sources",             FieldForm::Custom },
    { "audio.per_app_mode",        FieldForm::Custom },
    { "audio.bitrate_kbps",        FieldForm::Custom },
    { "audio.full_mix_track",      FieldForm::Custom },
    { "audio.codec",               FieldForm::Hidden },    // one option until a second codec exists
    // Replay & Output
    { "buffer.seconds",            FieldForm::Custom },
    { "capture.resolution",        FieldForm::Custom },
    { "capture.framerate",         FieldForm::Custom },
    { "encode.bitrate_kbps",       FieldForm::Custom },
    { "buffer.storage",            FieldForm::Custom },
    { "buffer.file_path",          FieldForm::Custom },
    { "output.root",               FieldForm::Custom },
    { "output.per_game_folders",   FieldForm::Custom },
    { "output.filename",           FieldForm::Custom },
    // Appearance & Notifications
    { "ui.theme",                  FieldForm::Custom },
    { "ui.accent",                 FieldForm::Custom },
    { "ui.themes",                 FieldForm::Custom },
    { "ui.corners",                FieldForm::Custom },
    { "ui.font",                   FieldForm::Custom },
    { "ui.density",                FieldForm::Custom },
    { "ui.card_borders",           FieldForm::Custom },
    { "ui.panel_position",         FieldForm::Custom },     // PanelPositionControl (Appearance > Layout)
    { "ui.panel_attach",           FieldForm::Custom },
    { "ui.rail_position",          FieldForm::Custom },
    { "ui.memory_card",            FieldForm::Custom },
    { "ui.screen_frame",           FieldForm::Custom },
    { "ui.screen_frame_radius",    FieldForm::Custom },
    { "ui.screen_frame_thickness", FieldForm::Custom },
    { "ui.screen_frame_color",     FieldForm::Custom },
    { "ui.toast_on_save",          FieldForm::Custom },
    { "ui.toast_on_buffer_toggle", FieldForm::Custom },
    { "ui.toast_position",         FieldForm::Custom },     // a corner is spatial: a 2x2 picture
    { "ui.toast_style",            FieldForm::Custom },
    // Advanced
    { "games.list",                FieldForm::Custom },
    { "capture.capture_cursor",    FieldForm::Custom },
    { "encode.rate_control",       FieldForm::Custom },
    { "encode.max_bitrate_kbps",   FieldForm::Custom },
    { "encode.cq",                 FieldForm::Custom },
    { "encode.preset",             FieldForm::Custom },  // seven options
    { "encode.keyframe_sec",       FieldForm::Custom },
    { "encode.adapter",            FieldForm::Custom },    // TileControls (BuildAdvancedEncoderPage)
    { "encode.video_codec",        FieldForm::Custom }, // Codec card's header (BuildAdvancedEncoderPage)
    // Inside the Codec card, only when the staged GPU's AV1 can't code the recorded size exactly.
    { "encode.amf_av1_unaligned",  FieldForm::Custom },
    { "engine.telemetry",          FieldForm::Custom },
    { "updates.check",             FieldForm::Custom },    // Updates card (BuildAdvancedSystemPage)
    { "updates.auto_install",      FieldForm::Custom },
    { "diagnostics.sharing",       FieldForm::Custom },    // a LevelDetail (BuildAdvancedSystemPage)
    { "log.level",                 FieldForm::Custom },    // Detailed logging: info | debug as a switch
    // Removals edit config.json only.
    { "buffer.max_bytes",          FieldForm::Hidden },    // the age cap made it useless or harmful
    { "log.max_bytes",             FieldForm::Hidden },
    { "save.overlap_policy",       FieldForm::Hidden },
};

}  // namespace

void SetNativeWindowSink(std::function<void(const std::wstring&)> sink) { g_native_window_sink = std::move(sink); }

bool NativeWindowBlocked(const std::wstring& what) {
    if (!g_native_window_sink) return false;
    g_native_window_sink(what);
    return true;
}

bool FieldFormFor(const std::string& key, FieldForm* form) {
    for (const auto& e : kFieldForms) {
        if (key == e.key) { *form = e.form; return true; }
    }
    return false;
}

namespace {


std::wstring FormatShortPath(const std::wstring& full_path) {
    if (full_path.empty()) return L"Videos\\" OMBRA_PRODUCT_NAME;

    std::wstring p = full_path;
    for (auto& c : p) {
        if (c == L'/') c = L'\\';
    }
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }

    wchar_t user_profile[MAX_PATH] = {0};
    if (ExpandEnvironmentStringsW(L"%USERPROFILE%", user_profile, MAX_PATH) && user_profile[0] != L'\0') {
        std::wstring up = user_profile;
        for (auto& c : up) {
            if (c == L'/') c = L'\\';
        }
        while (up.size() > 1 && (up.back() == L'\\' || up.back() == L'/')) {
            up.pop_back();
        }
        if (_wcsnicmp(p.c_str(), up.c_str(), up.size()) == 0) {
            if (p.size() > up.size() && p[up.size()] == L'\\') {
                std::wstring rel = p.substr(up.size() + 1);
                if (!rel.empty() && rel.size() <= 20) {
                    return rel;
                }
            }
        }
    }

    const size_t last_slash = p.find_last_of(L'\\');
    if (last_slash == std::wstring::npos) {
        return p;
    }

    const size_t prev_slash = p.find_last_of(L'\\', last_slash > 0 ? last_slash - 1 : 0);
    if (prev_slash != std::wstring::npos) {
        std::wstring two_seg = p.substr(prev_slash + 1);
        if (two_seg.size() <= 18) {
            return two_seg;
        }
    }

    std::wstring last_seg = p.substr(last_slash + 1);
    return last_seg.empty() ? p : last_seg;
}

// Shared by BuildAudioSourceRows' "Add app" tile and BuildExclusionRows' "Add process" row -- both
// build a ShowDropdown option list from the same process_list_cache_ (screens.h), just with
// slightly different label rules (Add app prefers display_name; Add process, mirroring its
// pre-existing behavior, always shows the raw exe name). `prefer_display_name` picks between them.
void ProcessListToDropdownOptions(const std::vector<ProcessEntry>& processes, bool prefer_display_name,
                                   std::vector<std::wstring>* options_w,
                                   std::vector<std::string>* options_utf8) {
    options_w->clear();
    options_utf8->clear();
    options_w->reserve(processes.size());
    options_utf8->reserve(processes.size());
    for (const auto& p : processes) {
        if (p.name.empty()) continue;
        options_utf8->push_back(p.name);
        const bool use_display = prefer_display_name && !p.display_name.empty();
        options_w->push_back(Utf8ToWide(use_display ? p.display_name : p.name));
    }
}

}  // namespace

std::vector<FieldSpec> ParseSchema(const ombra::JsonValue& schema_obj) {
    std::vector<FieldSpec> out;
    const ombra::JsonValue* settings = schema_obj.Find("settings");
    if (!settings || settings->type != ombra::JsonValue::Type::Array) return out;

    for (const auto& item : settings->arr) {
        if (item.type != ombra::JsonValue::Type::Object) continue;
        FieldSpec spec;

        if (const auto* v = item.Find("key"); v && v->type == ombra::JsonValue::Type::String)
            spec.key = v->str;
        if (const auto* v = item.Find("title"); v && v->type == ombra::JsonValue::Type::String)
            spec.display_name = v->str;
        if (const auto* v = item.Find("help"); v && v->type == ombra::JsonValue::Type::String)
            spec.help = v->str;
        if (const auto* v = item.Find("tradeoff"); v && v->type == ombra::JsonValue::Type::String)
            spec.tradeoff = v->str;
        if (const auto* v = item.Find("tier"); v && v->type == ombra::JsonValue::Type::String)
            spec.tier = v->str;
        if (const auto* v = item.Find("group"); v && v->type == ombra::JsonValue::Type::String)
            spec.group = v->str;
        if (const auto* v = item.Find("unit"); v && v->type == ombra::JsonValue::Type::String)
            spec.unit = v->str;
        if (const auto* v = item.Find("apply"); v && v->type == ombra::JsonValue::Type::String)
            spec.apply = ApplyKindFromStr(v->str);
        if (const auto* v = item.Find("implemented"); v && v->type == ombra::JsonValue::Type::Bool)
            spec.implemented = v->b;

        if (const auto* v = item.Find("kind"); v && v->type == ombra::JsonValue::Type::String) {
            if (v->str == "bool") spec.kind = FieldKind::Bool;
            else if (v->str == "int") spec.kind = FieldKind::Int;
            else if (v->str == "double") spec.kind = FieldKind::Double;
            else if (v->str == "enum") spec.kind = FieldKind::Enum;
            else if (v->str == "action") spec.kind = FieldKind::Action;
            else spec.kind = FieldKind::String;
        }

        if (!FieldFormFor(spec.key, &spec.form)) spec.form = FieldForm::Hidden;

        if (const auto* v = item.Find("min"); v && v->type == ombra::JsonValue::Type::Number)
            spec.min_val = v->num;
        if (const auto* v = item.Find("max"); v && v->type == ombra::JsonValue::Type::Number)
            spec.max_val = v->num;
        if (const auto* v = item.Find("step"); v && v->type == ombra::JsonValue::Type::Number)
            spec.step_val = v->num;

        if (const auto* v = item.Find("enum_options"); v && v->type == ombra::JsonValue::Type::Array) {
            for (const auto& opt : v->arr) {
                if (opt.type == ombra::JsonValue::Type::String) spec.enum_options.push_back(opt.str);
            }
        }

        if (const auto* v = item.Find("presets"); v && v->type == ombra::JsonValue::Type::Array) {
            for (const auto& p : v->arr) {
                if (p.type == ombra::JsonValue::Type::String) spec.presets.push_back(p.str);
                else if (p.type == ombra::JsonValue::Type::Number) spec.presets.push_back(std::to_string(static_cast<int>(p.num)));
            }
        }

        if (const auto* v = item.Find("hardware_bound"); v && v->type == ombra::JsonValue::Type::Bool)
            spec.hardware_bound = v->b;

        out.push_back(std::move(spec));
    }
    return out;
}

// =================================================================================================
// HomeScreen -- 3 tiles (Instant Replay with its on/off switch, Record, Clips), the save hint and a
// live stats line. Record is inert until manual record mode exists; its placeholder fills the row.
// Screenshot has no tile; its hotkey keeps working unchanged (main.cpp).
// =================================================================================================

HomeScreen::HomeScreen(ScreenHost* host) : host_(host) {
    replay_card_.title = L"Instant Replay";
    replay_card_.detail = L"—";  // real value arrives with the first preview_config reply
    replay_card_.icon = IconType::Replay;
    replay_card_.primary = true;
    replay_card_.on_click = [this]() {
        if (!host_) return;
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("save_now"));
        host_->RequestAsync(std::move(req), 30000, [this](std::optional<ombra::JsonValue> response) {
            if (!response && host_) {
                host_->ShowModal(L"Replay Save Unconfirmed",
                    L"The engine did not confirm this save. It may still finish. Check Clips before saving again.",
                    false, nullptr, nullptr, ModalButtons{L"OK", L""});
            }
        });  // the shared engine save toast reports progress and completion
    };

    // The switch is the engine's set_armed (the user's on/off), not capture.mode: "desktop vs game"
    // and "on vs off" are different things.
    replay_card_.foot_label = L"Replay buffer";
    replay_card_.toggle.on = true;  // desktop-equivalent default: always armed (matches supervisor)
    replay_card_.toggle.on_change = [this](bool checked) {
        if (!host_) return;
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("set_armed"));
        req.Set("armed", ombra::JsonValue::MakeBool(checked));
        // The switch already shows `checked`; only the newest click's reply may settle it, or a
        // slow reply to an earlier click would flip it back under a later one.
        const uint64_t gen = ++set_armed_generation_;
        host_->RequestAsync(std::move(req), 3000, [this, gen, checked](std::optional<ombra::JsonValue> resp) {
            if (gen != set_armed_generation_) return;
            const auto* v = (resp && resp->type == ombra::JsonValue::Type::Object) ? resp->Find("ok") : nullptr;
            if (v && v->type == ombra::JsonValue::Type::Bool && v->b) {
                is_armed_ = checked;
            } else {
                replay_card_.toggle.on = is_armed_;  // roll back a rejected/failed toggle
            }
            RefreshReplayDetail();
            if (host_) host_->RequestRepaint();
        });
    };

    output_folder_str_ = ombra::ResolveOutputRoot(ombra::Config{}.output_root);

    // Record: a placeholder with no engine support yet. Disabled, so it takes no input at all.
    record_card_.title = L"Record";
    record_card_.detail = L"Coming soon";
    record_card_.icon = IconType::Record;
    record_card_.enabled = false;
    record_card_.reserve_foot = true;  // titles in line with Instant Replay's

    // Clips: the library, preview and trim (ClipsScreen). Its own "Open folder" reaches Explorer.
    clips_card_.title = L"Clips";
    clips_card_.detail = FormatShortPath(output_folder_str_);
    clips_card_.icon = IconType::Film;
    clips_card_.reserve_foot = true;
    clips_card_.on_click = [this]() {
        if (host_) host_->SwitchScreen(ActiveScreen::Clips);
    };
}

void HomeScreen::RefreshReplayDetail() {
    // Off says so: the state line used to keep "ACTIVE" with the switch off.
    if (engine_unavailable_) replay_card_.detail = L"Not responding";
    else if (!is_armed_) replay_card_.detail = L"Off";
    else if (!waiting_text_.empty()) replay_card_.detail = waiting_text_;
    else replay_card_.detail = active_text_;  // empty until a config says how long: never a stale line
    const bool keeping = !engine_unavailable_ && is_armed_ && waiting_text_.empty() && !active_text_.empty();
    replay_card_.live_line = keeping ? RecordingSubject() : std::wstring{};
}

// What the engine is keeping right now, in a few words. Follows the running config, not staged edits.
std::wstring HomeScreen::RecordingSubject() const {
    if (status_target_ == "window") {
        const bool exe = window_name_.size() > 4 && _wcsicmp(window_name_.c_str() + window_name_.size() - 4, L".exe") == 0;
        return exe ? window_name_.substr(0, window_name_.size() - 4) : window_name_;
    }
    if (status_target_ == "game" && !running_game_.empty()) return running_game_;
    if (status_target_ != "game" && status_target_ != "monitor") return {};
    if (display_count_ > 1) return std::to_wstring(display_count_) + L" displays";
    return display_label_.empty() ? L"Your display" : display_label_;
}

void HomeScreen::MarkEngineUnavailable() {
    engine_unavailable_ = true;
    live_stats_str_.clear();
    RefreshReplayDetail();
}

void HomeScreen::UpdateState(const ombra::JsonValue* status_resp, const ombra::JsonValue* config_resp) {
    if (status_resp) {
        engine_unavailable_ = false;
        const auto* armed   = status_resp->Find("armed");
        const auto* enabled = status_resp->Find("enabled");
        const bool  have_armed = armed && armed->type == ombra::JsonValue::Type::Bool;
        // `enabled` is the switch; an older engine only has `armed`.
        if (enabled && enabled->type == ombra::JsonValue::Type::Bool) is_armed_ = enabled->b;
        else if (have_armed) is_armed_ = armed->b;
        replay_card_.toggle.on = is_armed_;
        const auto* status_target = status_resp->Find("target");
        status_target_ = status_target && status_target->type == ombra::JsonValue::Type::String ? status_target->str : std::string();
        const auto* game = status_resp->Find("current_game");  // null while no game runs
        running_game_ = game && game->type == ombra::JsonValue::Type::String ? Utf8ToWide(game->str) : std::wstring();
        waiting_text_.clear();
        if (is_armed_ && have_armed && !armed->b) {
            const auto* target = status_resp->Find("target");
            const bool  window = target && target->type == ombra::JsonValue::Type::String && target->str == "window";
            waiting_text_ = window ? L"Waiting for the window" : L"Waiting for a game";
        }
        if (const auto* v = status_resp->Find("buffer_fill_pct"); v && v->type == ombra::JsonValue::Type::Number) {
            wchar_t buf[256];
            double private_mb = 0.0;
            if (const auto* r = status_resp->Find("rss_private_mb"); r && r->type == ombra::JsonValue::Type::Number) private_mb = r->num;

            // The engine names the backend it is recording with; the topology says whether frames
            // cross adapters to get there.
            std::wstring enc_info = L"None";
            if (const auto* b = status_resp->Find("encoder_backend"); b && b->type == ombra::JsonValue::Type::String)
                enc_info = b->str == "x264" ? L"CPU (x264)" : Utf8ToWide(b->str);
            if (const auto* topo = status_resp->Find("gpu_topology"); topo && topo->type == ombra::JsonValue::Type::String) {
                if (topo->str == "T5") enc_info = L"None";
                else if (topo->str == "T2" || topo->str == "T3") enc_info += L" (cross-GPU)";
            }
            swprintf(buf, std::size(buf), L"Buffer: %.0f%%  •  RAM: %.0f MB  •  Encoder: %ls", v->num, private_mb, enc_info.c_str());
            live_stats_str_ = buf;
        }
    }

    if (config_resp) {
        if (const auto* cfg = config_resp->Find("config"); cfg && cfg->type == ombra::JsonValue::Type::Object) {
            if (const auto* v = cfg->Find("buffer.seconds"); v && v->type == ombra::JsonValue::Type::Number) {
                const std::wstring secs = std::to_wstring(static_cast<int>(v->num));
                active_text_ = L"Last " + secs + L" s ready";
                hint_suffix_ = L"to save the last " + secs + L" seconds";
            }
            if (const auto* v = cfg->Find("hotkeys.save_replay"); v && v->type == ombra::JsonValue::Type::String) {
                hint_keys_ = Utf8ToWide(v->str);
            }
            if (const auto* v = cfg->Find("hotkeys.toggle_buffer"); v && v->type == ombra::JsonValue::Type::String) {
                replay_card_.foot_keys = Utf8ToWide(v->str);
            }
            if (const auto* v = cfg->Find("output.root"); v && v->type == ombra::JsonValue::Type::String) {
                output_folder_str_ = ombra::ResolveOutputRoot(v->str);
                clips_card_.detail = FormatShortPath(output_folder_str_);
            }
            if (const auto* v = cfg->Find("capture.target_window"); v && v->type == ombra::JsonValue::Type::String) {
                window_name_ = Utf8ToWide(v->str);
            }
            // Several displays are just counted. One is named, which takes the monitor list: an
            // empty capture.monitors means the primary (ResolveCaptureMonitors).
            std::string device = "primary";
            display_count_ = 1;
            if (const auto* v = cfg->Find("capture.monitors"); v && v->type == ombra::JsonValue::Type::Array && !v->arr.empty()) {
                display_count_ = static_cast<int>(v->arr.size());
                if (v->arr[0].type == ombra::JsonValue::Type::String) device = v->arr[0].str;
            }
            const uint64_t gen = ++display_label_generation_;
            if (display_count_ == 1 && host_) {
                host_->RequestMonitorListAsync([this, gen, device](std::vector<MonitorEntry> monitors) {
                    if (gen != display_label_generation_) return;
                    for (const auto& m : monitors) {
                        if (m.device != device && !(device == "primary" && m.primary)) continue;
                        display_label_ = Utf8ToWide(m.label.empty() ? m.device : m.label);
                        RefreshReplayDetail();
                        if (host_) host_->RequestRepaint();
                        return;
                    }
                });
            }
        }
    }
    RefreshReplayDetail();
}

void HomeScreen::Layout(const D2D1_RECT_F& bounds, float dpi_scale) {
    bounds_ = bounds;
    const float s = dpi_scale;
    const Sizes z = host_ ? host_->GetTheme().size : Sizes{};

    const D2D1_RECT_F& area = bounds;
    const float gap = z.space_lg() * s;
    const float hint_h = z.control_xs() * s, stats_h = LineHeight(TypeRole::Caption) * s;
    const float below = z.space_lg() * s + hint_h + z.space_xs() * s + stats_h + 2 * z.space_sm() * s;
    const float avail_w = area.right - area.left;
    const float avail_h = area.bottom - area.top;
    // Tiles fill the content area rather than a fixed size. They read as slightly smaller boxes
    // than a square, a little narrower than tall.
    constexpr float kTileShrink = 0.92f;
    constexpr float kTileAspect = 0.90f;  // width / height
    const float tile = (std::min)((avail_w - gap * 2.0f) / 3.0f, avail_h - below);
    const float tile_h = tile * kTileShrink;
    const float tile_w = tile_h * kTileAspect;
    const float total_w = tile_w * 3.0f + gap * 2.0f;
    const float x = area.left + (avail_w - total_w) * 0.5f;
    const float top = area.top;

    replay_card_.bounds = D2D1::RectF(x, top, x + tile_w, top + tile_h);
    record_card_.bounds = D2D1::RectF(x + tile_w + gap, top, x + tile_w * 2 + gap, top + tile_h);
    clips_card_.bounds  = D2D1::RectF(x + tile_w * 2 + gap * 2, top, x + total_w, top + tile_h);

    // Both span exactly the tile row, so they centre under the tiles rather than under the panel.
    // They sit on a plate space_sm round them, a tile gap below the tiles.
    const float hint_top = top + tile_h + z.space_lg() * s + z.space_sm() * s;
    hint_bounds_  = D2D1::RectF(x, hint_top, x + total_w, hint_top + hint_h);
    stats_bounds_ = D2D1::RectF(x, hint_bounds_.bottom + z.space_xs() * s, x + total_w,
                                hint_bounds_.bottom + z.space_xs() * s + stats_h);
}

void HomeScreen::Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) {
    const float s = dpi_scale;
    const paint::Paint p{dc, theme, s};
    const ColorRoles& c = theme.color;
    replay_card_.Draw(dc, theme, s);
    record_card_.Draw(dc, theme, s);
    clips_card_.Draw(dc, theme, s);

    // "Press [Alt] [F10] to save the last 60 seconds", centred under the tiles.
    const std::vector<std::wstring> keys = paint::SplitKeys(hint_keys_.empty() ? std::wstring(L"—") : hint_keys_);
    const std::wstring press = L"Press";
    const float sp = theme.size.space_sm() * s, kh = hint_bounds_.bottom - hint_bounds_.top;
    const float pw = p.TextW(press, TypeRole::Body), sw = p.TextW(hint_suffix_, TypeRole::Body);
    const float kw = p.Keys(keys, 0, 0, kh, false);
    const float hint_w = pw + sp + kw + sp + sw;
    {
        // On the bare scrim a light theme's dark text vanishes over a bright game: the lines sit on
        // a raised plate as wide as the longer of them.
        const float sw_stats = live_stats_str_.empty() ? 0.0f : p.TextW(live_stats_str_, TypeRole::Caption);
        const float half = ((std::min)((std::max)(hint_w, sw_stats), hint_bounds_.right - hint_bounds_.left) * 0.5f) +
                           theme.size.space_md() * s;
        const float cx = (hint_bounds_.left + hint_bounds_.right) * 0.5f, v = theme.size.space_sm() * s;
        const float bottom = (live_stats_str_.empty() ? hint_bounds_.bottom : stats_bounds_.bottom) + v;
        const auto plate = D2D1::RectF(cx - half, hint_bounds_.top - v, cx + half, bottom);
        p.Fill(plate, theme.size.radius_card() * s, c.raised);
        p.CardEdge(plate, theme.size.radius_card() * s);
    }
    float hx = (hint_bounds_.left + hint_bounds_.right - hint_w) * 0.5f;
    p.Text(press, TypeRole::Body, D2D1::RectF(hx, hint_bounds_.top, hx + pw + 1, hint_bounds_.bottom), c.text);
    hx += pw + sp;
    p.Keys(keys, hx, paint::Cy(hint_bounds_), kh);
    hx += kw + sp;
    p.Text(hint_suffix_, TypeRole::Body, D2D1::RectF(hx, hint_bounds_.top, hx + sw + 1, hint_bounds_.bottom), c.text);

    if (!live_stats_str_.empty())
        p.Text(live_stats_str_, TypeRole::Caption, stats_bounds_, c.text2, {TextAlign::Center, false, true});
}

bool HomeScreen::OnMouseMove(D2D1_POINT_2F pt) {
    bool rep = replay_card_.OnMouseMove(pt);
    rep |= record_card_.OnMouseMove(pt);
    rep |= clips_card_.OnMouseMove(pt);
    return rep;
}

CursorKind HomeScreen::CursorAt(D2D1_POINT_2F pt) const {
    for (const Control* w : {static_cast<const Control*>(&replay_card_), static_cast<const Control*>(&record_card_),
                             static_cast<const Control*>(&clips_card_)}) {
        if (w->CursorAt(pt) != CursorKind::Arrow) return w->CursorAt(pt);
    }
    return CursorKind::Arrow;
}

bool HomeScreen::OnMouseDown(D2D1_POINT_2F pt) {
    // Every tile hears it, so a press elsewhere clears the others' focus.
    bool took = replay_card_.OnMouseDown(pt);
    took |= record_card_.OnMouseDown(pt);  // disabled: never takes it
    took |= clips_card_.OnMouseDown(pt);
    return took;
}

bool HomeScreen::OnMouseUp(D2D1_POINT_2F pt) {
    replay_card_.OnMouseUp(pt);
    record_card_.OnMouseUp(pt);
    clips_card_.OnMouseUp(pt);
    return false;
}

bool HomeScreen::OnKeyDown(UINT vk) {
    if (vk == VK_ESCAPE) {
        if (host_) host_->CloseOverlay();
        return true;
    }
    // Esc only: Home has no Tab order, so a tile is focused only by a click, and Space or Enter
    // after one would save again or reopen Clips unasked.
    return false;
}

// =================================================================================================
// SettingsScreen -- fully schema-driven off ombra::AllSettings() via the pipe's get_schema
// payload (ParseSchema above). No setting row is a literal in this file.
// =================================================================================================

const wchar_t* SettingsRailLabel(int rail) {
    return rail >= 0 && rail < kRailCount ? kSettingsPages[rail].label : L"";
}

int SettingsRailByName(const std::wstring& prefix) {
    if (prefix.empty()) return -1;
    for (int i = 0; i < kRailCount; ++i)
        if (_wcsnicmp(prefix.c_str(), kSettingsPages[i].label, prefix.size()) == 0) return i;
    return -1;
}

SettingsScreen::SettingsScreen(ScreenHost* host) : host_(host) {
    for (const SettingsPage& page : kSettingsPages) rail_.items.push_back({page.icon, page.label});
    rail_.on_change = [this](int i) {
        ShowPage(i, subtab_[static_cast<size_t>(i)]);
        if (host_) host_->RequestRepaint();
    };
    tabs_.tabs = kSettingsPages[0].tabs.empty() ? std::vector<std::wstring>{} : std::vector<std::wstring>(
        kSettingsPages[0].tabs.begin(), kSettingsPages[0].tabs.end());
    tabs_.on_change = [this](int i) {
        ShowPage(rail_.selected, i);
        if (host_) host_->RequestRepaint();
    };
    save_bar_.discard.on_click = [this]() { DiscardAll(); };
    save_bar_.save.on_click = [this]() { SaveAll(); };

}

void SettingsScreen::ShowPage(int rail, int subtab) {
    if (rail < 0 || rail >= kRailCount) return;
    const auto& tabs = kSettingsPages[rail].tabs;
    if (rail != rail_.selected || subtab != subtab_[static_cast<size_t>(rail)]) theme_gallery_open_ = false;
    rail_.selected = rail;
    subtab_[static_cast<size_t>(rail)] = tabs.empty() ? 0 : std::clamp(subtab, 0, static_cast<int>(tabs.size()) - 1);
    tabs_.tabs.assign(tabs.begin(), tabs.end());
    tabs_.selected = subtab_[static_cast<size_t>(rail)];
    tabs_.hover_index = -1;
    scroll_area_.scroll_y = 0.0f;
    sent_loaded_ = false;  // Diagnostics reads what was sent again
    RebuildGroupRows(current_dpi_scale_);  // Layout() rebuilds again if the sub-tab row came or went
}

void SettingsScreen::ApplyPreviewReplies(const ombra::JsonValue* resp, const ombra::JsonValue* status_resp) {
    auto num = [](const ombra::JsonValue* obj, const char* key, double& out) {
        const auto* v = obj ? obj->Find(key) : nullptr;
        if (!v || v->type != ombra::JsonValue::Type::Number) return false;
        out = v->num;
        return true;
    };
    const bool resp_ok = resp && resp->type == ombra::JsonValue::Type::Object;
    if (resp_ok) {
        double v = 0.0;
        if (num(resp, "mb_per_minute", v)) preview_mb_per_min_ = std::to_wstring(static_cast<int>(v)) + L" MB/min";
        if (num(resp, "bitrate_kbps", v)) {
            preview_bitrate_mbps_ = L"~" + std::to_wstring(static_cast<int>(std::llround(v / 1000.0))) + L" Mbps";
        }
        if (num(resp, "display_fps", v)) preview_display_fps_ = static_cast<int>(v);
        if (num(resp, "source_width", v)) preview_source_w_ = static_cast<uint32_t>(v);
        if (num(resp, "source_height", v)) preview_source_h_ = static_cast<uint32_t>(v);
        // Per-ring bytes (capacity + overshoot) for the *edited* config: the most a config takes.
        // Ring memory is committed as footage arrives and freed a buffer length after a disarm, so
        // the engine holds less than this until its buffer has filled once.
        double video = 0.0, audio = 0.0;
        if (num(resp, "video_ring_total_bytes", video) && num(resp, "audio_ring_total_bytes", audio)) {
            preview_memory_.video_ring_bytes = video;
            preview_memory_.audio_ring_bytes = audio;
            preview_memory_.valid            = true;
        }
    }
    // What the engine actually allocated (get_status.memory) and holds (rss_private_mb). Offline,
    // the counts stay at the defaults: one pipeline, one audio source.
    if (status_resp && status_resp->type == ombra::JsonValue::Type::Object) {
        double v = 0.0;
        if (const auto* m = status_resp->Find("memory"); m && m->type == ombra::JsonValue::Type::Object) {
            double vp = 0.0, vo = 0.0, ao = 0.0, ac = 0.0, vc = 0.0;
            if (num(m, "video_pipelines", v)) preview_memory_.video_pipelines = (std::max)(1.0, v);
            if (num(m, "audio_sources", v)) preview_memory_.audio_sources = v;
            if (const auto* f = m->Find("file_backed"); f && f->type == ombra::JsonValue::Type::Bool) {
                preview_memory_.file_backed = f->b;
            }
            if (num(m, "video_capacity_bytes", vc) && num(m, "video_overshoot_bytes", vo) &&
                num(m, "audio_capacity_bytes", ac) && num(m, "audio_overshoot_bytes", ao) &&
                num(status_resp, "rss_private_mb", vp)) {
                preview_memory_.max_video_bytes = (vc + vo) * preview_memory_.video_pipelines;
                preview_memory_.max_audio_bytes = (ac + ao) * preview_memory_.audio_sources;
                // Rings commit as footage arrives, so what they hold comes from the engine. An engine
                // that doesn't report it (older build) gets the old assumption: full rings.
                double held_v = 0.0, held_a = 0.0;
                preview_memory_.held_video_bytes = num(m, "video_committed_bytes", held_v)
                    ? held_v : (preview_memory_.file_backed ? 0.0 : preview_memory_.max_video_bytes);
                preview_memory_.held_audio_bytes = num(m, "audio_committed_bytes", held_a)
                    ? held_a : preview_memory_.max_audio_bytes;
                const double held = (preview_memory_.file_backed ? 0.0 : preview_memory_.held_video_bytes) +
                                    preview_memory_.held_audio_bytes;
                preview_memory_.engine_bytes = (std::max)(0.0, vp * 1024.0 * 1024.0 - held);
                preview_memory_.engine_valid = true;
            }
        }
        if (num(status_resp, "sys_mem_total_mb", v)) preview_memory_.sys_total_bytes = v * 1024.0 * 1024.0;
        if (num(status_resp, "sys_mem_load_pct", v)) preview_memory_.sys_load_pct = v;
        preview_memory_.vram_bytes = num(status_resp, "gpu_dedicated_mb", v) ? v * 1024.0 * 1024.0 : -1.0;
        ApplyRecordingGpus(*status_resp);
        ApplyHotkeyStatus(*status_resp);
        ApplyRunStatus(*status_resp);
    }
}

void SettingsScreen::ApplyRunStatus(const ombra::JsonValue& status) {
    auto str = [&](const char* key) {
        const auto* v = status.Find(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string();
    };
    running_game_ = Utf8ToWide(str("current_game"));
    run_target_ = str("target");
    run_mode_ = str("mode");
    const auto* on = status.Find("enabled");
    run_enabled_ = on && on->type == ombra::JsonValue::Type::Bool && on->b;
}

void SettingsScreen::SeedRunStatusForSnapshot(const ombra::JsonValue& status) {
    ApplyRunStatus(status);
    RebuildGroupRows(current_dpi_scale_);
}

void SettingsScreen::ApplyRecordingGpus(const ombra::JsonValue& status) {
    const auto* list = status.Find("recording_gpus");
    if (!list || list->type != ombra::JsonValue::Type::Array) return;  // an older engine: keep what we had
    auto str = [](const ombra::JsonValue& obj, const char* key) {
        const auto* v = obj.Find(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string();
    };
    auto flag = [](const ombra::JsonValue& obj, const char* key) {
        const auto* v = obj.Find(key);
        return v && v->type == ombra::JsonValue::Type::Bool && v->b;
    };
    auto align = [](const ombra::JsonValue& obj, const char* key) {
        const auto* v = obj.Find(key);
        return (v && v->type == ombra::JsonValue::Type::Number && v->num >= 1.0) ? static_cast<uint32_t>(v->num) : 1u;
    };
    recording_gpus_.clear();
    for (const auto& g : list->arr) {
        if (g.type != ombra::JsonValue::Type::Object) continue;
        recording_gpus_.push_back({str(g, "vendor"), str(g, "backend"), str(g, "name"), flag(g, "hevc"), flag(g, "av1"),
                                   align(g, "av1_align_w"), align(g, "av1_align_h"), flag(g, "drives_display")});
    }
    recording_gpu_now_ = str(status, "encoder_adapter");
    recording_on_cpu_  = str(status, "encoder_backend") == "x264";
    display_owner_gpu_ = str(status, "gpu_display_owner");
    codec_now_         = str(status, "encoder_codec");
}

std::vector<ombra::RecordingGpuCaps> SettingsScreen::RecordingGpuCapsNow() const {
    std::vector<ombra::RecordingGpuCaps> caps;
    for (const auto& g : recording_gpus_) caps.push_back({g.vendor, g.av1, g.av1_align_w, g.av1_align_h, g.drives_display});
    return caps;
}

const SettingsScreen::RecordingGpu* SettingsScreen::StagedRecordingGpu() const {
    std::string pref = "auto";
    if (const auto* v = EffectiveConfigValue("encode.adapter"); v && v->type == ombra::JsonValue::Type::String) pref = v->str;
    const auto caps = RecordingGpuCapsNow();  // same order as recording_gpus_
    const ombra::RecordingGpuCaps* picked = ombra::PickRecordingGpu(caps, pref);
    return picked ? &recording_gpus_[static_cast<size_t>(picked - caps.data())] : nullptr;
}

void SettingsScreen::SeedRecordingGpusForSnapshot(bool cpu_fallback, bool panel, bool game_on_rtx) {
    recording_gpus_ = {{"nvidia", "NVENC", "NVIDIA GeForce RTX 5060 Laptop GPU", true, true, 1, 1, !panel},
                       {"amd", "AMF", "AMD Radeon 780M Graphics", true, true, 64, 16, panel},
                       {"cpu", "x264", "CPU (x264)", false, false}};
    const char* owner  = panel ? "AMD Radeon 780M Graphics" : "NVIDIA GeForce RTX 5060 Laptop GPU";
    recording_gpu_now_ = game_on_rtx ? "NVIDIA GeForce RTX 5060 Laptop GPU" : owner;
    display_owner_gpu_ = owner;
    codec_now_         = "av1";
    recording_on_cpu_  = cpu_fallback;
    if (cpu_fallback) {
        recording_gpu_now_ = "AMD Radeon 780M Graphics";  // the adapter capture runs on
        codec_now_         = "h264";
    }
}

void SettingsScreen::ProjectedRingCounts(double& pipelines, double& sources) const {
    pipelines = preview_memory_.video_pipelines;
    sources   = preview_memory_.audio_sources;
    // capture.monitors is a Raw key; the Monitors page stages it in pending_changes_.
    if (const auto* m = EffectiveConfigValue("capture.monitors"); m && m->type == ombra::JsonValue::Type::Array) {
        pipelines = static_cast<double>(ombra::ResolveCaptureMonitors(*m).size());
    }
    if (audio_sources_dirty_) {
        ombra::JsonValue arr = ombra::JsonValue::MakeArray();
        for (const auto& s : audio_sources_ui_) arr.arr.push_back(s);
        sources = static_cast<double>(ombra::SelectActiveAudioSources(ombra::ResolveAudioSources(arr), audio_per_app_mode_).size());
    }
}

void SettingsScreen::UpdateMemoryCard() {
    const auto& pm = preview_memory_;
    MemoryCardData& d = memory_card_.data;
    d.valid = pm.engine_valid;
    if (!d.valid) return;
    d.file_backed      = pm.file_backed;
    d.video_bytes      = pm.held_video_bytes;
    d.audio_bytes      = pm.held_audio_bytes;
    d.video_max_bytes  = pm.max_video_bytes;
    d.audio_max_bytes  = pm.max_audio_bytes;
    d.engine_bytes     = pm.engine_bytes;
    d.sys_total_bytes  = pm.sys_total_bytes;
    d.sys_load_pct     = pm.sys_load_pct;
    d.vram_bytes       = pm.vram_bytes;

    auto whole = [](double v) { return std::to_wstring(static_cast<int>(std::llround(v))); };
    // Committed seconds: the causes sit next to what the engine holds now, not the staged edit.
    const auto* secs = current_config_.Find("buffer.seconds");
    const std::wstring secs_text = (secs && secs->type == ombra::JsonValue::Type::Number)
        ? whole(secs->num) + L"\x00a0s" : L"";
    double pipes = 1.0, srcs = 1.0;
    ProjectedRingCounts(pipes, srcs);
    auto plural = [&](double n, const wchar_t* one, const wchar_t* many) {
        return whole(n) + L"\x00a0" + (static_cast<int>(n) == 1 ? one : many);
    };
    // The causes describe the *committed* numbers next to them, so use the engine's own counts.
    d.video_cause = secs_text + L" · " + plural(pm.video_pipelines, L"monitor", L"monitors");
    d.audio_cause = secs_text + L" · " + plural(pm.audio_sources, L"source", L"sources");
    // Every source's ring is the same size, so one number covers them all.
    if (pm.audio_sources > 1.0) {
        d.audio_cause += L" ×\x00a0" + whole(pm.max_audio_bytes / pm.audio_sources / (1024.0 * 1024.0)) + L"\x00a0MB";
    }

    // Staged edits: swap in the edited config's per-ring sizes and counts.
    d.has_projection = false;
    if (pm.valid && HasPendingChanges()) {
        d.proj_video_bytes = pm.video_ring_bytes * pipes;
        d.proj_audio_bytes = pm.audio_ring_bytes * srcs;
        d.proj_file_backed = StagedRingIsFile();
        d.has_projection = std::abs(d.ProjectedBytes() - d.MaxBytes()) >= 1024.0 * 1024.0;
    }
}

float SettingsScreen::MemoryCardExtraWidth(float dpi_scale) {
    return kPanelGapDip * dpi_scale + MemoryCard::Width(dpi_scale);
}

void SettingsScreen::FetchPreviewAsync() {
    if (!host_) return;
    // The staged config, not just the committed one: the preview (and the memory card's "After
    // Save" line) answers "what would this cost", which is the question while edits are pending.
    ombra::JsonValue effective = current_config_;
    for (const auto& [key, val] : pending_changes_) effective.Set(key, val);
    // Each request runs on its own worker thread and pipe connection, so replies can land out of
    // order. Only the newest request's reply describes the staged config: a quality-preset pick
    // sends one preview with the old bitrate and another once the derived bitrate is staged, and
    // the first landing last left "After Save" showing the old bitrate's cost.
    const uint64_t gen = ++preview_generation_;
    host_->RequestPreviewAsync(
        effective,
        [this, gen](std::optional<ombra::JsonValue> resp, std::optional<ombra::JsonValue> status_resp) {
            if (gen != preview_generation_) return;
            ApplyPreviewReplies(resp ? &*resp : nullptr, status_resp ? &*status_resp : nullptr);
            RebuildGroupRows(current_dpi_scale_);
            if (host_) host_->RequestRepaint();
        });
}


void SettingsScreen::InitSchema(const std::vector<FieldSpec>& specs, const ombra::JsonValue* config_obj) {
    // InitSchema runs once per overlay open (OverlayHost::Show),
    // so this is the "close/reopen" boundary the thumbnail/session cache should
    // clear on -- a stale bitmap from a previous session never survives past it.
    monitor_list_fetched_ = false;
    sent_loaded_ = false;
    running_game_icon_source_.clear();  // as below: a worker's reply dies with the window
    running_game_icon_.reset();
    // The window (hwnd) an in-flight RequestMonitorListAsync/RequestMonitorThumbnailAsync worker is
    // watching is destroyed on hide -- its IsWindow(hwnd) check then fails and
    // it returns without ever posting a result, so the pending flag/set below would otherwise get
    // stuck true forever across a close/reopen with a fetch still in flight. Reset here, same
    // close/reopen boundary as the caches themselves.
    monitor_list_pending_ = false;
    monitor_thumb_cache_.clear();
    monitor_thumb_pending_.clear();
    monitor_thumb_fresh_.clear();
    window_list_fetched_ = false;
    window_list_pending_ = false;
    window_list_.clear();
    window_thumb_cache_.clear();
    window_thumb_pending_.clear();
    window_thumb_fresh_.clear();
    windows_show_all_ = false;
    audio_sessions_fetched_ = false;
    audio_sessions_pending_ = false;  // same close/reopen reset as monitor_list_pending_ above
    audio_sessions_cache_.clear();
    audio_status_.clear();
    audio_status_pending_ = false;
    process_list_fetched_ = false;
    process_list_pending_ = false;    // same close/reopen reset as monitor_list_pending_ above
    process_list_cache_.clear();
    process_display_names_.clear();
    audio_device_list_fetched_ = false;
    audio_device_list_pending_ = false;  // same close/reopen reset as monitor_list_pending_ above
    audio_device_list_cache_.clear();
    games_fetched_ = false;
    games_pending_ = false;  // same close/reopen reset as monitor_list_pending_ above
    games_failed_ = false;
    ++games_generation_;
    games_detected_.clear();
    games_seen_.clear();
    games_list_scroll_ = 0.0f;
    games_edit_list_ = false;
    // A fetch from the last open never answers (its window is gone). Loaded icons stay; failures
    // are tried again, in case the file has turned up since.
    game_icons_pending_ = false;
    game_icons_skipped_ = false;
    std::erase_if(game_icons_, [](const auto& kv) { return !kv.second || kv.second->bgra.empty(); });
    // A staged-but-unsaved audio edit (audio_sources_dirty_) doesn't survive a close/reopen either
    // -- same boundary as the caches above. BuildAudioSourceRows will reload audio_sources_ui_ from
    // config_obj below since the flag is clear.
    audio_sources_dirty_ = false;
    // Same close/reopen boundary for every other staged-but-unsaved field edit.
    pending_changes_.clear();
    pending_worst_apply_ = ApplyKind::Free;
    ++bitrate_sync_generation_;

    specs_ = specs;
    if (config_obj && config_obj->type == ombra::JsonValue::Type::Object) {
        current_config_ = *config_obj;
    } else {
        current_config_ = ombra::JsonValue::MakeObject();
    }

    // The preview readouts are fetched by OverlayHost::Show once its window exists
    // (FetchPreviewAsync); until then they keep the spec's own tradeoff text.

    RebuildGroupRows(current_dpi_scale_);
}

void SettingsScreen::SetActiveRail(int index) {
    if (index < 0 || index >= kRailCount || index == rail_.selected) return;
    ShowPage(index, subtab_[static_cast<size_t>(index)]);
}

void SettingsScreen::SetActiveSubTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.tabs.size()) || index == ActiveSubTab()) return;
    ShowPage(rail_.selected, index);
}

void SettingsScreen::SeedMonitorsForSnapshot(int count, bool layout, bool thumbs) {
    monitor_rows_.clear();
    monitor_thumb_cache_.clear();
    // `layout`: a laptop panel left of a primary external, then a portrait screen raised on its
    // right -- negative coordinates and mixed shapes.
    struct Place { const wchar_t* label; int left, top, w, h; };
    const Place places[] = {{L"Primary", 0, 0, 2560, 1440}, {L"DISPLAY2", -1920, 360, 1920, 1080}, {L"DISPLAY3", 2560, -240, 1080, 1920}};
    for (int i = 0; i < count; ++i) {
        MonitorRow mr;
        mr.device = "\\\\.\\DISPLAY" + std::to_string(i + 1);
        mr.label = L"Display " + std::to_wstring(i + 1);
        mr.width = 2560;
        mr.height = 1440;
        if (layout && i < 3) {
            mr.label = places[i].label;
            mr.left = places[i].left;
            mr.top = places[i].top;
            mr.width = places[i].w;
            mr.height = places[i].h;
            mr.positioned = true;
        }
        mr.resolution = std::to_wstring(mr.width) + L" × " + std::to_wstring(mr.height);
        mr.primary = i == 0;
        mr.checked = (i == 0);
        monitor_rows_.push_back(mr);
        monitor_thumb_fresh_.insert(mr.device);
        if (!thumbs) continue;

        MonitorThumbCache t;
        t.w = mr.width / 8;
        t.h = mr.height / 8;
        t.bgra.resize(static_cast<size_t>(t.w) * t.h * 4);
        for (UINT y = 0; y < t.h; ++y) {
            for (UINT x = 0; x < t.w; ++x) {
                uint8_t* px = &t.bgra[(static_cast<size_t>(y) * t.w + x) * 4];
                // Light/dark bands so an icon that's invisible on either shows up.
                const bool bright = ((x / 40) + i) % 2 == 0;
                px[0] = static_cast<uint8_t>(bright ? 230 : 40 + (y * 180 / t.h) / 3);
                px[1] = static_cast<uint8_t>(bright ? 230 : 60 + (x * 320 / t.w) / 4);
                px[2] = static_cast<uint8_t>(bright ? 235 : 90);
                px[3] = 255;
            }
        }
        monitor_thumb_cache_.emplace(mr.device, std::move(t));
    }
    monitor_list_fetched_ = true;
    RebuildGroupRows(current_dpi_scale_);
}

void SettingsScreen::SeedMemoryForSnapshot(bool with_projection) {
    constexpr double kMb = 1024.0 * 1024.0;
    auto& m = preview_memory_;
    m.engine_valid = true;
    m.video_pipelines = 1.0;
    m.audio_sources = 2.0;
    // 70 s into a 120 s buffer: the rings hold a bit over half of what they will.
    m.max_video_bytes = 591.0 * kMb;
    m.max_audio_bytes = 55.0 * kMb;
    m.held_video_bytes = 342.0 * kMb;
    m.held_audio_bytes = 33.0 * kMb;
    m.engine_bytes = 177.0 * kMb;
    m.sys_total_bytes = 15.3 * 1024.0 * kMb;
    m.sys_load_pct = 54.0;
    m.vram_bytes = 128.0 * kMb;
    m.valid = true;
    m.video_ring_bytes = 591.0 * kMb;
    m.audio_ring_bytes = 27.6 * kMb;
    m.file_backed = false;
    preview_mb_per_min_ = L"288 MB/min";
    current_config_.Set("buffer.seconds", ombra::JsonValue::MakeNum(120));
    pending_changes_.clear();
    if (with_projection) {
        pending_changes_["buffer.seconds"] = ombra::JsonValue::MakeNum(300);
        m.video_ring_bytes = 1470.0 * kMb;
        m.audio_ring_bytes = 69.0 * kMb;
    }
    RebuildGroupRows(current_dpi_scale_);
}

void SettingsScreen::SeedAudioForSnapshot(float scroll_y, bool per_app) {
    using ombra::JsonValue;
    auto source = [](const char* id, const char* kind, const char* mode, const char* target, const char* group) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("id", JsonValue::MakeStr(id));
        o.Set("kind", JsonValue::MakeStr(kind));
        o.Set("device", JsonValue::MakeStr("default"));
        if (mode) o.Set("mode", JsonValue::MakeStr(mode));
        if (target) o.Set("target", JsonValue::MakeStr(target));
        if (group) o.Set("group_id", JsonValue::MakeStr(group));
        return o;
    };
    JsonValue sources = JsonValue::MakeArray();
    sources.arr.push_back(source("desktop", "output_loopback", "all", nullptr, nullptr));
    sources.arr.push_back(source("mic", "input", nullptr, nullptr, nullptr));
    sources.arr.back().Set("enabled", JsonValue::MakeBool(false));  // one card switched off
    sources.arr.push_back(source("app:league", "output_loopback", "only", "LeagueClient.exe", "grp1"));
    sources.arr.push_back(source("app:discord", "output_loopback", "only", "Discord.exe", "grp1"));
    sources.arr.push_back(source("app:spotify", "output_loopback", "only", "Spotify.exe", nullptr));
    UpdateConfigValue("audio.sources", sources);
    UpdateConfigValue("audio.per_app_mode", JsonValue::MakeBool(per_app));
    // Two apps playing that aren't tracks yet, plus one that is (it must not be listed twice).
    audio_sessions_fetched_ = true;
    audio_sessions_cache_ = {"chrome.exe", "Spotify.exe", "vlc.exe"};
    audio_status_ = {{"desktop", AudioSourceStatus::Live}, {"mic", AudioSourceStatus::Off},
                     {"app:league", AudioSourceStatus::Waiting}, {"app:discord", AudioSourceStatus::Live},
                     {"app:spotify", AudioSourceStatus::Failed}};
    process_display_names_["chrome.exe"] = "Google Chrome";
    process_display_names_["vlc.exe"] = "VLC media player";
    audio_device_list_fetched_ = true;
    audio_device_list_cache_ = {{"out1", "Speakers (Realtek(R) Audio)", true, false},
                                {"out2", "Headphones (USB-C Audio)", false, false},
                                {"in1", "Microphone Array (AMD Audio Device)", true, true},
                                {"in2", "Headset Microphone (USB-C Audio)", false, true}};
    RebuildGroupRows(current_dpi_scale_);
    scroll_area_.scroll_y = scroll_y;
}

void SettingsScreen::UpdateConfigValue(const std::string& key, const ombra::JsonValue& val) {
    current_config_.Set(key, val);
}

size_t SettingsScreen::TotalWiredControls() const {
    size_t count = 0;
    for (const auto& g : groups_) count += g.row_count;
    return count;
}

size_t SettingsScreen::RowCountForGroup(const std::wstring& group_name) const {
    for (const auto& g : groups_) {
        if (g.name == group_name) return g.row_count;
    }
    return 0;
}

size_t SettingsScreen::GroupHeadingCount() const {
    return groups_.size();
}

void SettingsScreen::StageAudioGainForTest(double gain_db) {
    if (audio_sources_ui_.empty()) return;
    audio_sources_ui_[0].Set("gain_db", ombra::JsonValue::MakeNum(gain_db));
    CommitAudioSourcesNoRealloc();
}

double SettingsScreen::CommittedDesktopGainForTest() const {
    const auto* arr = current_config_.Find("audio.sources");
    if (!arr || arr->type != ombra::JsonValue::Type::Array || arr->arr.empty()) return 0.0;
    const auto* v = arr->arr[0].Find("gain_db");
    return (v && v->type == ombra::JsonValue::Type::Number) ? v->num : 0.0;
}

void SettingsScreen::RebuildGroupRows(float dpi_scale) {
    // A row-panel builder below (BuildDisplaysCard/BuildAudioSourceRows/BuildExclusionRows) can fire
    // an async host request whose callback calls back in here on receipt. With a host that resolves
    // synchronously that would otherwise recurse while this call still holds a live reference into
    // groups_ (about to be cleared). Defer instead of recursing: the outer call notices the request
    // after it finishes and loops once more, so this function never runs inside itself.
    if (rebuilding_group_rows_) {
        rebuild_requested_while_rebuilding_ = true;
        deferred_rebuild_dpi_scale_ = dpi_scale;
        return;
    }
    rebuilding_group_rows_ = true;
    ++rebuild_body_depth_for_test_;
    if (auto f = template_field_.lock(); f && f->focused) {
        if (!template_edit_) template_edit_ = TemplateEdit{f->text};
        template_edit_->caret = f->Caret();
    } else {
        template_edit_.reset();
    }
    template_field_.reset();  // BuildOutputPage sets it again
    max_rebuild_body_depth_for_test_ = (std::max)(max_rebuild_body_depth_for_test_, rebuild_body_depth_for_test_);

    ++rebuild_count_for_test_;
    current_dpi_scale_ = dpi_scale;
    audio_page_built_ = false;  // BuildAudioSourceRows sets it again if the Audio page is built
    system_page_built_ = false;
    rows_dirty_ = false;
    last_rebuild_bounds_ = scroll_area_.bounds;
    last_rebuild_dpi_scale_ = dpi_scale;

    groups_.clear();
    games_list_.reset();
    sent_list_.reset();
    game_rows_.clear();
    recording_restored_ = false;
    scroll_area_.children.clear();

    const int sub = ActiveSubTab();
    switch (rail_.selected) {
        case kRailCapture:    sub == 0 ? BuildCaptureVideoPage() : BuildAudioPage(); break;
        case kRailReplay:     sub == 0 ? BuildReplayQualityPage() : BuildOutputPage(); break;
        case kRailGames:      BuildGamesPage(); break;
        case kRailAppearance:
            sub == 0 ? BuildAppearanceLookPage() : sub == 1 ? BuildAppearanceLayoutPage() : BuildAppearanceNotificationsPage();
            break;
        case kRailShortcuts:  BuildShortcutsPage(); break;
        case kRailAdvanced:   sub == 0 ? BuildAdvancedEncoderPage() : BuildAdvancedSystemPage(); break;
        default: break;
    }

    // The page is one Column of section headers and the sections' items; Measure/Arrange place them.
    // scroll_area_.children is rebuilt from the top-level items in draw/scroll order.
    //
    // Bounds are *absolute* device coordinates (ScrollAreaWidget::Draw only subtracts scroll_y at
    // draw time -- it never translates by the scroll area's own origin), and Arrange() takes an
    // absolute rect, so nothing here needs to know about scrolling.
    scroll_area_.scrollbar_room_dip = kScrollbarRoomDip;
    const float area_w = (scroll_area_.bounds.right - scroll_area_.bounds.left) - kScrollbarRoomDip * dpi_scale;

    auto page = FlexWidget::MakeColumn();
    // Every page builds one section; its cards carry their own titles (CardContainer::title).
    for (auto& g : groups_) {
        for (auto& item : g.items) {
            page->Add(item.widget).margin_after_dip = item.margin_after_dip;
        }
    }

    LayoutContext ctx;
    ctx.dpi_scale = dpi_scale;
    ctx.theme = host_ ? &host_->GetTheme() : nullptr;
    float page_h = page->Measure(LayoutConstraints::Available(area_w), ctx).height;
    // The Games list scrolls inside its own box: shrink it until the page fits the view, down to a
    // few rows (below that the page scrolls as well). It grows back when the view does (the Save
    // bar leaving) since every rebuild starts from its full height.
    if (auto list = games_list_.lock(); list && dpi_scale > 0.0f) {
        const float view_h = scroll_area_.bounds.bottom - scroll_area_.bounds.top;
        const float full_dip = list->ContentHeight() / dpi_scale;
        const float min_dip = list->LeadHeight(3) / dpi_scale;
        const float want_dip = full_dip - (page_h - view_h) / dpi_scale;
        if (want_dip < full_dip) {
            list->viewport_h_dip = (std::max)(min_dip, want_dip);
            page_h = page->Measure(LayoutConstraints::Available(area_w), ctx).height;
        }
    }
    page->Arrange(D2D1::RectF(scroll_area_.bounds.left, scroll_area_.bounds.top,
                              scroll_area_.bounds.left + area_w, scroll_area_.bounds.top + page_h), ctx);
    for (const auto& it : page->items) scroll_area_.children.push_back(it.widget);

    scroll_area_.content_height = page_h;
    // A rebuild can shrink the page under the current scroll (Per-app -> Simple, a target that
    // drops the tiles); without this the view sits past the end, scrollbar below the card.
    const float view_h = scroll_area_.bounds.bottom - scroll_area_.bounds.top;
    scroll_area_.scroll_y = std::clamp(scroll_area_.scroll_y, 0.0f, (std::max)(0.0f, page_h - view_h));
    --rebuild_body_depth_for_test_;
    if (template_field_.expired()) template_edit_.reset();
    // A shortcut was recording and the page it was on is gone (rail or sub-tab switch): release the
    // engine's hotkeys now rather than when the lease lapses.
    if (!recording_key_.empty() && !recording_restored_) {
        if (auto old = recording_row_.lock()) old->CancelRecording();  // clears recording_key_ via its callback
        recording_row_.reset();
        if (!recording_key_.empty()) {
            recording_key_.clear();
            if (host_) host_->SetHotkeysSuspended(false);
        }
    }

    // The meters run only while the Audio page shows them.
    if (!audio_page_built_) meters_.clear();
    SyncMeters();

    rebuilding_group_rows_ = false;
    if (rebuild_requested_while_rebuilding_) {
        rebuild_requested_while_rebuilding_ = false;
        RebuildGroupRows(deferred_rebuild_dpi_scale_);
    }
}

bool SettingsScreen::OnMeterTick() {
    if (meters_.empty() || !level_reader_) return false;
    const ULONGLONG now = GetTickCount64();
    const float dt = meter_last_ms_ ? static_cast<float>(now - meter_last_ms_) / 1000.0f : 0.0f;
    meter_last_ms_ = now;
    std::vector<LevelTarget> targets;
    targets.reserve(meters_.size());
    for (const auto& b : meters_) targets.push_back(b.target);
    std::vector<float> peaks;
    level_reader_->Read(targets, &peaks);
    bool changed = false;
    for (size_t i = 0; i < meters_.size() && i < peaks.size(); ++i) {
        auto m = meters_[i].meter.lock();
        if (!m) continue;
        // Rises at once, falls at OBS's rate.
        float& shown = meter_db_.try_emplace(meters_[i].target.id, -INFINITY).first->second;
        shown = (std::max)(PeakToDb(peaks[i]) + meters_[i].gain_db, shown - kMeterDecayDbPerSec * dt);
        const int before = m->LitBlocks();
        m->db = shown;
        changed = changed || m->LitBlocks() != before;
    }
    return changed;
}

void SettingsScreen::SyncMeters() {
    if (!meters_.empty() && !level_reader_) level_reader_ = MakeLevelReader();
    const bool meters_on = !meters_.empty() && level_reader_;  // no reader (tests, --drive): nothing to tick
    if (!meters_on) {
        level_reader_.reset();
        meter_last_ms_ = 0;
    }
    if (meters_on != meter_timer_on_ && host_) {
        meter_timer_on_ = meters_on;
        host_->SetMeterTimer(meters_on);
    }
    if (meters_on) OnMeterTick();
}

void SettingsScreen::StopMeters() {
    if (meter_timer_on_ && host_) host_->SetMeterTimer(false);
    meter_timer_on_ = false;
    level_reader_.reset();
    meter_last_ms_ = 0;
    meter_db_.clear();
}

std::wstring SettingsScreen::RingUseText() const {
    if (!preview_memory_.valid || preview_mb_per_min_.empty()) return L"";
    auto mb = [](double bytes) { return std::to_wstring(static_cast<int>(bytes / (1024 * 1024))) + L" MB"; };
    const auto& m = preview_memory_;
    double pipes = 1.0, srcs = 1.0;
    ProjectedRingCounts(pipes, srcs);
    const double video = m.video_ring_bytes * pipes, audio = m.audio_ring_bytes * srcs;
    // The most the rings take. RAM is committed as footage arrives, so it's reached one buffer
    // length after a start; the disk file is sized up front.
    return StagedRingIsFile()
        ? L"Reserves " + mb(video) + L" on disk, uses up to " + mb(audio) + L" RAM (" + preview_mb_per_min_ + L")"
        : L"Uses up to " + mb(video + audio) + L" RAM once full (video " + mb(video) + L", audio " + mb(audio) +
              L"; " + preview_mb_per_min_ + L")";
}

SettingsScreen::Choices SettingsScreen::ChoicesFor(const FieldSpec& spec) {
    // A value that isn't one of the presets gets its own trailing choice. Without it nothing
    // matched and the control kept index 0, showing "30" as selected.
    Choices out;
    out.values = !spec.presets.empty() ? spec.presets : spec.enum_options;
    std::string cur;
    if (const auto* v = EffectiveConfigValue(spec.key)) {
        if (v->type == ombra::JsonValue::Type::String) cur = v->str;
        else if (v->type == ombra::JsonValue::Type::Number) cur = std::to_string(static_cast<int>(v->num));
    }
    auto it = std::find(out.values.begin(), out.values.end(), cur);
    if (!cur.empty() && it == out.values.end()) it = out.values.insert(out.values.end(), cur);
    out.selected = it == out.values.end() ? -1 : static_cast<int>(it - out.values.begin());
    return out;
}

std::shared_ptr<SelectControl> SettingsScreen::MakeSelect(const FieldSpec& spec, const ChoiceLabel& label) {
    const Choices choices = ChoicesFor(spec);
    auto sel = std::make_shared<SelectControl>();
    sel->id = spec.key;
    for (const auto& v : choices.values) sel->options.push_back(label(v));
    sel->selected = choices.selected;
    sel->on_change = [this, spec, values = choices.values](int i) {
        if (i >= 0 && i < static_cast<int>(values.size())) ChoosePreset(spec, values[i]);
    };
    // The menu goes through the host, above the page; the field's chevron points up until it closes.
    std::weak_ptr<SelectControl> weak = sel;
    sel->on_open = [this, weak](SelectControl& s) {
        if (!host_) {
            s.open = false;
            return;
        }
        D2D1_RECT_F at = s.bounds;
        at.top -= scroll_area_.scroll_y;
        at.bottom -= scroll_area_.scroll_y;
        host_->ShowDropdown(at, s.options, s.selected, s.on_change, [this, weak] {
            if (auto self = weak.lock()) self->open = false;  // a pick rebuilds the page; Esc doesn't
            if (host_) host_->RequestRepaint();
        });
    };
    controls_by_key_[spec.key] = sel;
    return sel;
}

std::shared_ptr<SegmentedControl> SettingsScreen::MakeSegmented(const FieldSpec& spec, const ChoiceLabel& label) {
    const Choices choices = ChoicesFor(spec);
    auto seg = std::make_shared<SegmentedControl>();
    seg->id = spec.key;
    for (const auto& v : choices.values) seg->options.push_back(label(v));
    seg->selected = choices.selected;
    seg->on_change = [this, spec, values = choices.values](int i) {
        if (i >= 0 && i < static_cast<int>(values.size())) ChoosePreset(spec, values[i]);
    };
    controls_by_key_[spec.key] = seg;
    return seg;
}

void SettingsScreen::ChoosePreset(const FieldSpec& spec, const std::string& choice) {
    if (spec.kind == FieldKind::Int || spec.kind == FieldKind::Double)
        ApplySettingChange(spec, ombra::JsonValue::MakeNum(std::stod(choice)));
    else
        ApplySettingChange(spec, ombra::JsonValue::MakeStr(choice));
    SyncDerivedBitrateAsync(spec);
}

void SettingsScreen::SyncDerivedBitrateAsync(const FieldSpec& spec) {
    if (spec.key != "encode.quality_preset" && spec.key != "capture.framerate" &&
        spec.key != "capture.resolution") return;
    const uint64_t gen = ++bitrate_sync_generation_;  // before the early outs: picking "custom" cancels
    if (!host_) return;
    const auto* preset_v = EffectiveConfigValue("encode.quality_preset");
    if (!preset_v || preset_v->type != ombra::JsonValue::Type::String || preset_v->str == "custom") return;

    // The staged edits only, never the whole config and never a bitrate: preview_config patches
    // onto the engine's live config, and it only derives a bitrate from the preset when the patch
    // doesn't carry one. The old synchronous version sent current_config_ (bitrate included), so
    // the engine skipped the derivation and the "synced" bitrate was always the old one.
    ombra::JsonValue patch = ombra::JsonValue::MakeObject();
    for (const auto& [key, val] : pending_changes_) {
        if (key != "encode.bitrate_kbps") patch.Set(key, val);
    }
    patch.Set("encode.quality_preset", *preset_v);

    host_->RequestPreviewAsync(patch, [this, gen](std::optional<ombra::JsonValue> resp,
                                                  std::optional<ombra::JsonValue>) {
        if (gen != bitrate_sync_generation_) return;
        const ombra::JsonValue* br = resp ? resp->Find("bitrate_kbps") : nullptr;
        if (!br || br->type != ombra::JsonValue::Type::Number || br->num <= 0) return;
        auto it = std::find_if(specs_.begin(), specs_.end(),
                               [](const FieldSpec& s) { return s.key == "encode.bitrate_kbps"; });
        if (it == specs_.end()) return;
        ApplySettingChange(*it, ombra::JsonValue::MakeNum(br->num));
    });
}

const FieldSpec* SettingsScreen::FindSpec(const std::string& key) const {
    auto it = std::find_if(specs_.begin(), specs_.end(), [&](const FieldSpec& s) { return s.key == key; });
    return (it != specs_.end() && it->implemented) ? &*it : nullptr;
}

// ui.accent: five presets and your own colour, whose swatch opens the editor. The editor is the
// host's popup, so a commit that rebuilds this page doesn't close it.
std::shared_ptr<ColorPickerControl> SettingsScreen::BuildAccentPicker() {
    auto cp = std::make_shared<ColorPickerControl>();
    SetAccentSwatches(*cp, host_ ? host_->GetTheme() : Theme{});
    std::string key = "theme";
    if (const auto* v = EffectiveConfigValue("ui.accent"); v && v->type == ombra::JsonValue::Type::String) key = v->str;
    cp->selected = AccentSwatchIndex(AccentIdFromKey(key));
    ombra::ParseHexRgb(key, &cp->custom);
    cp->on_change = [this](int i) {
        if (const FieldSpec* spec = FindSpec("ui.accent"); spec && i >= 0 && i < kAccentSwatchCount)
            ApplySettingChange(*spec, ombra::JsonValue::MakeStr(AccentIdToKey(kAccentSwatches[i])));
    };
    cp->on_edit_custom = [this, custom = cp->custom](const D2D1_RECT_F& swatch) {
        D2D1_RECT_F at = swatch;
        at.top -= scroll_area_.scroll_y;
        at.bottom -= scroll_area_.scroll_y;
        OpenAccentEditor(at, custom);
    };
    controls_by_key_["ui.accent"] = cp;
    return cp;
}

void SettingsScreen::OpenAccentEditor(const D2D1_RECT_F& anchor, uint32_t custom) {
    const FieldSpec* found = FindSpec("ui.accent");
    if (!host_ || !found) return;
    const FieldSpec spec = *found;  // by value: a commit rebuilds the page and its specs' rows
    const auto* v = EffectiveConfigValue("ui.accent");
    const std::string original = v && v->type == ombra::JsonValue::Type::String ? v->str : "theme";
    auto hex = [](uint32_t rgb) {
        char b[16];
        std::snprintf(b, sizeof b, "#%06x", rgb);
        return std::string(b);
    };
    auto editor = std::make_unique<ColorEditorPopup>();
    editor->SetColor(custom);
    // Dragging only repaints with the colour (no staging, no rebuild); letting go stages it once.
    editor->on_preview = [this, hex](uint32_t rgb) {
        if (!host_) return;
        host_->GetTheme().ApplyAccentKey(hex(rgb));
        host_->RequestRepaint();
    };
    editor->on_commit = [this, spec, hex](uint32_t rgb) { ApplySettingChange(spec, ombra::JsonValue::MakeStr(hex(rgb))); };
    editor->on_cancel = [this, spec, original] { ApplySettingChange(spec, ombra::JsonValue::MakeStr(original)); };
    // Opening it chooses your own colour, as clicking any swatch chooses that one.
    if (AccentIdFromKey(original) != AccentId::Custom) ApplySettingChange(spec, ombra::JsonValue::MakeStr(hex(custom)));
    host_->ShowPopup(std::move(editor), anchor);
}

namespace {
std::string HexKey(uint32_t rgb) {
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", rgb);
    return b;
}
}  // namespace

std::string SettingsScreen::StagedThemeKey() const {
    const auto* v = EffectiveConfigValue("ui.theme");
    return v && v->type == ombra::JsonValue::Type::String ? v->str : "dark";
}

std::vector<ombra::UserTheme> SettingsScreen::StagedUserThemes() const {
    const auto* v = EffectiveConfigValue("ui.themes");
    return v ? ombra::ParseUserThemes(*v) : std::vector<ombra::UserTheme>{};
}

void SettingsScreen::StageThemes(const std::vector<ombra::UserTheme>& themes, const std::string& select) {
    const FieldSpec* list = FindSpec("ui.themes");
    const FieldSpec* key = FindSpec("ui.theme");
    if (!list || !key) return;
    // Both first, by value: the first change rebuilds the page and the control that called this.
    std::vector<std::pair<FieldSpec, ombra::JsonValue>> changes;
    changes.emplace_back(*list, ombra::SerializeUserThemes(themes));
    if (select != StagedThemeKey()) changes.emplace_back(*key, ombra::JsonValue::MakeStr(select));
    ApplySettingChanges(changes);
}

// The colours `key` shows now: a preset's, a user theme's, or System's resolved mode.
ombra::UiPaletteColors SettingsScreen::ThemeColorsFor(const std::string& key) const {
    for (const ombra::UiPalette& p : ombra::kUiPalettes)
        if (key == p.key) return p.colors;
    const std::vector<ombra::UserTheme> mine = StagedUserThemes();
    if (const ombra::UserTheme* u = ombra::FindUserTheme(mine, key)) return u->colors;
    const bool light_now = host_ && host_->GetTheme().mode == ThemeMode::Light;
    return ombra::kUiPalettes[light_now && key == "system" ? 1 : 0].colors;
}

// ui.theme as tiles. Compact (Look): the chosen theme, then System, Dark and Light, as small chips.
// Presets (the theme page): every palette, then System. Mine: your themes, then a New tile.
std::shared_ptr<ThemePickerControl> SettingsScreen::BuildThemePicker(ThemeTiles which) {
    if (!FindSpec("ui.theme")) return nullptr;
    using Kind = ThemePickerControl::Kind;
    auto tp = std::make_shared<ThemePickerControl>();
    tp->id = "ui.theme";
    tp->compact = which == ThemeTiles::Compact;
    const std::string current = StagedThemeKey();
    const std::vector<ombra::UserTheme> mine = StagedUserThemes();
    std::vector<std::string> keys;
    auto add_key = [&](const std::string& key) {
        if (key == "system") {
            tp->tiles.push_back({Kind::System, L"System", {}});
        } else if (const ombra::UserTheme* u = ombra::FindUserTheme(mine, key)) {
            tp->tiles.push_back({Kind::User, ombra::WideFromUtf8(u->name), u->colors});
        } else {
            for (const ombra::UiPalette& p : ombra::kUiPalettes)
                if (key == p.key) tp->tiles.push_back({Kind::Palette, p.name, p.colors});
        }
        if (tp->tiles.size() > keys.size()) keys.push_back(key);
    };
    if (which == ThemeTiles::Compact) {
        if (current != "system" && current != "dark" && current != "light") add_key(current);
        for (const char* k : {"system", "dark", "light"}) add_key(k);
    } else if (which == ThemeTiles::Presets) {
        for (const ombra::UiPalette& p : ombra::kUiPalettes) add_key(p.key);
        add_key("system");
    } else {
        for (const ombra::UserTheme& u : mine) add_key(u.id);
        if (mine.size() < ombra::kMaxUserThemes) {
            tp->tiles.push_back({Kind::New, L"New", {}});
            keys.push_back({});
        }
    }
    for (size_t i = 0; i < keys.size(); ++i)
        if (!keys[i].empty() && keys[i] == current) tp->selected = static_cast<int>(i);
    tp->on_change = [this, keys](int i) {
        if (i < 0 || i >= static_cast<int>(keys.size())) return;
        if (keys[i].empty()) {
            NewUserTheme();
            return;
        }
        const std::string key = keys[i];  // by value: the change rebuilds the page and this lambda
        if (const FieldSpec* found = FindSpec("ui.theme")) ApplySettingChange(*found, ombra::JsonValue::MakeStr(key));
    };
    tp->on_edit_field = [this](int field, const D2D1_RECT_F& at) {
        D2D1_RECT_F r = at;
        r.top -= scroll_area_.scroll_y;
        r.bottom -= scroll_area_.scroll_y;
        OpenThemeFieldEditor(field, r);
    };
    if (tp->selected >= 0 || !controls_by_key_.count("ui.theme")) controls_by_key_["ui.theme"] = tp;
    if (tp->ShowsFields()) controls_by_key_["ui.themes"] = tp;
    return tp;
}

// A copy of the theme in use, named "My theme N", chosen.
void SettingsScreen::NewUserTheme() {
    std::vector<ombra::UserTheme> mine = StagedUserThemes();
    if (mine.size() >= ombra::kMaxUserThemes) return;
    ombra::UserTheme u;
    u.id = ombra::NextUserThemeId(mine);
    u.colors = ThemeColorsFor(StagedThemeKey());
    for (int n = 1;; ++n) {
        u.name = "My theme " + std::to_string(n);
        if (ombra::UserThemeNameOk(u.name, mine, {})) break;
    }
    mine.push_back(u);
    StageThemes(mine, u.id);
}

void SettingsScreen::DeleteUserTheme(const std::string& id) {
    std::vector<ombra::UserTheme> mine = StagedUserThemes();
    const ombra::UserTheme* u = ombra::FindUserTheme(mine, id);
    if (!u) return;
    // The chosen theme goes back to the preset of its kind.
    const std::string fallback = ombra::UiBackgroundIsLight(u->colors.background) ? "light" : "dark";
    const std::string current = StagedThemeKey();
    const std::string select = current == id ? fallback : current;
    std::erase_if(mine, [&](const ombra::UserTheme& t) { return t.id == id; });
    StageThemes(mine, select);
}

void SettingsScreen::OpenThemeNamePrompt(const std::string& id, const D2D1_RECT_F& anchor) {
    if (!host_) return;
    const std::vector<ombra::UserTheme> mine = StagedUserThemes();
    const ombra::UserTheme* u = ombra::FindUserTheme(mine, id);
    if (!u) return;
    auto prompt = std::make_unique<TextPromptPopup>();
    prompt->caption = L"Name";
    prompt->Field().max_length = static_cast<int>(ombra::kMaxUserThemeName);
    prompt->SetText(ombra::WideFromUtf8(u->name));
    prompt->validate = [this, id](const std::wstring& text) -> std::wstring {
        std::string err;
        if (ombra::UserThemeNameOk(ombra::NarrowUtf8(text), StagedUserThemes(), id, &err)) return {};
        return ombra::WideFromUtf8(err);
    };
    prompt->on_commit = [this, id](const std::wstring& text) { RenameUserTheme(id, ombra::NarrowUtf8(text)); };
    host_->ShowPopup(std::move(prompt), anchor);
}

void SettingsScreen::RenameUserTheme(const std::string& id, const std::string& name) {
    std::vector<ombra::UserTheme> themes = StagedUserThemes();
    if (!ombra::FindUserTheme(themes, id) || !ombra::UserThemeNameOk(name, themes, id)) return;
    for (ombra::UserTheme& t : themes)
        if (t.id == id) t.name = name;
    StageThemes(themes, StagedThemeKey());
}

void SettingsScreen::OpenThemeFieldEditor(int field, const D2D1_RECT_F& anchor) {
    if (field < 0 || field >= ThemePickerControl::kFields || !host_) return;
    const std::string id = StagedThemeKey();
    const std::vector<ombra::UserTheme> mine = StagedUserThemes();
    const ombra::UserTheme* u = ombra::FindUserTheme(mine, id);
    if (!u) return;
    ombra::UiPaletteColors colors = u->colors;
    const uint32_t original = ThemePickerControl::FieldOf(colors, field);
    auto editor = std::make_unique<ColorEditorPopup>();
    editor->accent_foot = false;
    editor->SetColor(original);
    // Dragging only repaints with the colour (no staging, no rebuild); letting go stages it once.
    editor->on_preview = [this, id, field](uint32_t c) {
        if (!host_) return;
        Theme& t = host_->GetTheme();
        for (ombra::UserTheme& ut : t.user_themes)
            if (ut.id == id) ThemePickerControl::FieldOf(ut.colors, field) = c;
        t.ApplyThemeKey(id);
        host_->RequestRepaint();
    };
    auto stage = [this, id, field](uint32_t c) {
        std::vector<ombra::UserTheme> themes = StagedUserThemes();
        for (ombra::UserTheme& t : themes)
            if (t.id == id) ThemePickerControl::FieldOf(t.colors, field) = c;
        StageThemes(themes, StagedThemeKey());
    };
    editor->on_commit = stage;
    editor->on_cancel = [stage, original] { stage(original); };
    host_->ShowPopup(std::move(editor), anchor);
}

std::shared_ptr<SwitchControl> SettingsScreen::MakeSwitch(const FieldSpec& spec) {
    auto sw = std::make_shared<SwitchControl>();
    sw->id = spec.key;
    const auto* v = EffectiveConfigValue(spec.key);
    sw->on = v && v->type == ombra::JsonValue::Type::Bool && v->b;
    sw->on_change = [this, spec](bool on) { ApplySettingChange(spec, ombra::JsonValue::MakeBool(on)); };
    controls_by_key_[spec.key] = sw;
    return sw;
}

// Appearance is three sub-tabs, Look | Layout | Notifications: with the
// rows one page scrolled 144 dip at defaults, and each tab fits with every dependent shown.
namespace {
std::shared_ptr<SettingRow> AppearanceRow(const wchar_t* title, std::wstring help, std::shared_ptr<Widget> control) {
    if (!control) return nullptr;
    auto row = std::make_shared<SettingRow>();
    row->title = title;
    row->help = std::move(help);
    row->controls = {std::move(control)};
    return row;
}
void AddRow(CardContainer& card, std::shared_ptr<SettingRow> row) {
    if (row) card.rows.push_back(std::move(row));
}
}  // namespace

// Your own theme's text against its cards: a warning under the row, never a block on Save.
static std::wstring FaintTextNote(const ombra::UiPaletteColors& cc) {
    const double text = ombra::ContrastRatio(cc.text, cc.cards), muted = ombra::ContrastRatio(cc.muted, cc.cards);
    const bool text_low = text < 4.5;
    if (!text_low && muted >= 4.5) return {};
    wchar_t b[128];
    std::swprintf(b, 128, L"%ls is hard to read on Cards (%.1f:1, aim for 4.5:1).", text_low ? L"Text" : L"Muted text",
                  text_low ? text : muted);
    return b;
}

void SettingsScreen::BuildAppearanceLookPage() {
    if (theme_gallery_open_) {
        BuildThemeGalleryPage();
        return;
    }
    GroupSection sec;
    sec.name = L"Look";
    auto seg = [&](const char* key) -> std::shared_ptr<Widget> {
        const FieldSpec* s = FindSpec(key);
        return s ? MakeSegmented(*s, [k = s->key](const std::string& v) { return OptionLabelFor(k, v); }) : nullptr;
    };
    auto card = std::make_shared<CardContainer>();
    card->title = L"Look";
    if (auto chips = BuildThemePicker(ThemeTiles::Compact)) {
        // The chips carry no names: the help names the chosen one.
        const int sel = chips->selected;
        const std::wstring name = sel >= 0 && sel < static_cast<int>(chips->tiles.size()) ? chips->tiles[sel].name : L"";
        auto more = std::make_shared<ButtonControl>();
        more->label = L"All themes";
        more->in_row = true;
        more->on_click = [this] { OpenThemeGallery(true); };
        auto row = AppearanceRow(L"Theme", name == L"System" ? L"System: follows Windows' light or dark mode." : name, chips);
        row->help_one_line = true;
        row->controls.push_back(more);
        card->rows.push_back(row);
    }
    if (FindSpec("ui.accent")) {
        // A tall control (the near-red note is a line under the swatches): text keeps to the top.
        auto row = AppearanceRow(L"Accent", L"Selected controls, the Save button and focus rings.", BuildAccentPicker());
        row->align_top = true;
        card->rows.push_back(row);
    }
    AddRow(*card, AppearanceRow(L"Font", L"Inter and Geist ship with Ombra; System is Windows' own.", seg("ui.font")));
    AddRow(*card, AppearanceRow(L"Corners", L"How round every card, button and menu is.", seg("ui.corners")));
    AddRow(*card, AppearanceRow(L"Density", L"How much space rows and controls get.", seg("ui.density")));
    if (const FieldSpec* s = FindSpec("ui.card_borders"))
        AddRow(*card, AppearanceRow(L"Outlines", L"A thin line around each card.", MakeSwitch(*s)));
    sec.AddRow(card);
    groups_.push_back(std::move(sec));
}

void SettingsScreen::OpenThemeGallery(bool open) {
    theme_gallery_open_ = open;
    scroll_area_.scroll_y = 0;
    RebuildGroupRows(current_dpi_scale_);
    if (host_) host_->RequestRepaint();
}

// Every theme: the presets, then yours, which you can rename, delete and recolour. Edits stage like
// any other setting; Back returns to Look with them still staged.
void SettingsScreen::BuildThemeGalleryPage() {
    GroupSection sec;
    sec.name = L"Themes";
    auto presets = std::make_shared<CardContainer>();
    presets->title = L"Themes";
    auto back = std::make_shared<ButtonControl>();
    back->label = L"Back";
    back->icon = IconType::Back;
    back->on_click = [this] { OpenThemeGallery(false); };
    presets->actions = {back};
    if (auto tiles = BuildThemePicker(ThemeTiles::Presets)) {
        auto row = AppearanceRow(L"Presets", L"System follows Windows' light or dark mode.", tiles);
        row->controls_below = true;
        presets->rows.push_back(row);
    }
    sec.AddRow(presets);

    auto yours = std::make_shared<CardContainer>();
    yours->title = L"Your themes";
    if (auto tiles = BuildThemePicker(ThemeTiles::Mine)) {
        const bool own = tiles->ShowsFields();
        const std::string id = StagedThemeKey();
        if (own) {
            auto rename = std::make_shared<ButtonControl>();
            rename->label = L"Rename";
            std::weak_ptr<ButtonControl> weak = rename;
            rename->on_click = [this, id, weak] {
                auto b = weak.lock();
                if (!b) return;
                D2D1_RECT_F at = b->bounds;
                at.top -= scroll_area_.scroll_y;
                at.bottom -= scroll_area_.scroll_y;
                OpenThemeNamePrompt(id, at);
            };
            auto del = std::make_shared<ButtonControl>();
            del->label = L"Delete";
            del->kind = ButtonControl::Kind::Danger;
            del->on_click = [this, id] { DeleteUserTheme(id); };
            yours->actions = {rename, del};
        }
        auto row = AppearanceRow(L"Make your own",
                                 tiles->tiles.size() <= 1 ? L"New starts from the theme you're using."
                                 : own                    ? L"Click a colour to change it."
                                                          : L"Pick one to change its colours, or New to copy the theme you're using.",
                                 tiles);
        row->controls_below = true;
        if (own && tiles->selected >= 0) {
            row->note = FaintTextNote(tiles->tiles[tiles->selected].colors);
            row->note_tone = SettingRow::Tone::Warning;
        }
        yours->rows.push_back(row);
    }
    sec.AddRow(yours);
    groups_.push_back(std::move(sec));
}

void SettingsScreen::BuildAppearanceLayoutPage() {
    GroupSection sec;
    sec.name = L"Layout";
    auto is_on = [&](const char* key) {
        const auto* v = EffectiveConfigValue(key);
        return v && v->type == ombra::JsonValue::Type::Bool && v->b;
    };
    auto str_of = [&](const char* key, const char* fallback) -> std::string {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string(fallback);
    };
    auto label = [](const std::string& key) -> ChoiceLabel {
        return [key](const std::string& v) { return OptionLabelFor(key, v); };
    };
    const std::string position = str_of("ui.panel_position", "center");
    const bool frame_on = is_on("ui.screen_frame");

    // --- Panel: the screen picture places it; beside it, what the picture can't pick.
    auto panel = std::make_shared<CardContainer>();
    panel->title = L"Panel";
    const float gap = host_ ? host_->GetTheme().size.space_lg() : Sizes{}.space_lg();  // picture to rows
    std::shared_ptr<PanelPositionControl> picker;
    if (const FieldSpec* spec = FindSpec("ui.panel_position")) {
        picker = std::make_shared<PanelPositionControl>();
        picker->id = spec->key;
        picker->tooltip = L"Click an edge or the middle to place the panel";
        // The rows beside it keep room for a title and a four-way segmented control on one line
        // (narrower, each control drops under its text and the tab scrolls); the picture gives way.
        // Density widens those controls, so the room scales with it.
        const float rows_dip = host_ ? host_->GetTheme().size.S(440.0f) : 440.0f;
        const float dpi = current_dpi_scale_ > 0.0f ? current_dpi_scale_ : 1.0f;
        const float page_w = (scroll_area_.bounds.right - scroll_area_.bounds.left) / dpi - kScrollbarRoomDip;
        const float pad = host_ ? host_->GetTheme().size.card_padding() : 16.0f;
        picker->max_width_dip = std::clamp(page_w - 2 * pad - gap - rows_dip, 180.0f, 280.0f);
        for (const auto& opt : spec->enum_options) {
            picker->options.push_back(Utf8ToWide(opt));
            if (opt == position) picker->selected = static_cast<int>(picker->options.size()) - 1;
        }
        picker->attached = str_of("ui.panel_attach", "floating") == "attached";
        picker->rail_position = Utf8ToWide(str_of("ui.rail_position", "left"));
        picker->screen_frame = frame_on;
        const FieldSpec s = *spec;
        picker->on_change = [this, s](int idx) {
            if (idx >= 0 && idx < static_cast<int>(s.enum_options.size()))
                ApplySettingChange(s, ombra::JsonValue::MakeStr(s.enum_options[idx]));
        };
        controls_by_key_[spec->key] = picker;
    }
    // The rows beside the picture: a column with the card's dividers, not a card inside a card.
    auto beside = FlexWidget::MakeColumn();
    auto add_beside = [&](std::shared_ptr<SettingRow> row) {
        if (!row) return;
        row->divider_above = !beside->items.empty();
        beside->Add(row);
    };
    if (position != "center") {  // attaching needs an edge
        if (const FieldSpec* s = FindSpec("ui.panel_attach"))
            add_beside(AppearanceRow(L"Style", L"Attached fuses into the edge.", MakeSegmented(*s, label(s->key))));
    }
    if (const FieldSpec* s = FindSpec("ui.rail_position"))
        add_beside(AppearanceRow(L"Tabs", L"Which side the page list sits on.", MakeSegmented(*s, label(s->key))));
    if (const FieldSpec* s = FindSpec("ui.memory_card"))
        add_beside(AppearanceRow(L"Memory card", L"Memory use beside Settings.", MakeSwitch(*s)));
    auto hero = FlexWidget::MakeRow(gap);
    hero->align = FlexAlign::Start;
    if (picker) hero->Add(picker);
    FlexItem& rest = hero->Add(beside);
    rest.basis_dip = 0.0f;  // the rest of the line, not the rows' unbounded width
    rest.grow = 1.0f;
    panel->rows.push_back(hero);

    // --- Screen frame: its three options are one row of menus, so the tab fits with it on.
    auto frame = std::make_shared<CardContainer>();
    frame->title = L"Screen frame";
    if (const FieldSpec* s = FindSpec("ui.screen_frame")) frame->actions.push_back(MakeSwitch(*s));
    if (frame_on) {
        auto prefixed = [](const wchar_t* prefix) -> ChoiceLabel {
            return [prefix](const std::string& v) { return prefix + Utf8ToWide(v); };
        };
        auto row = std::make_shared<SettingRow>();
        row->title = L"Shape";
        row->help = L"Its corners, border and colour.";  // one line, or the tab scrolls at the narrowest panel
        if (const FieldSpec* s = FindSpec("ui.screen_frame_radius")) row->controls.push_back(MakeSelect(*s, prefixed(L"Radius ")));
        if (const FieldSpec* s = FindSpec("ui.screen_frame_thickness")) row->controls.push_back(MakeSelect(*s, prefixed(L"Border ")));
        if (const FieldSpec* s = FindSpec("ui.screen_frame_color")) row->controls.push_back(MakeSelect(*s, label(s->key)));
        // Short menus ("Radius 48", "Contrast"): at 200 dip each the three drop under the text at 1600 x 1000.
        for (auto& c : row->controls) static_cast<SelectControl&>(*c).width_dip = 128.0f;
        if (!row->controls.empty()) frame->rows.push_back(row);
    } else {
        frame->help = L"Rounds the screen's corners while the overlay is open, so it reads as one surface.";
    }
    sec.AddRow(panel);
    sec.AddRow(frame);
    groups_.push_back(std::move(sec));
}

void SettingsScreen::BuildAppearanceNotificationsPage() {
    GroupSection sec;
    sec.name = L"Notifications";
    const auto* enabled = EffectiveConfigValue("ui.toast_enabled");
    const bool toasts_on = enabled && enabled->type == ombra::JsonValue::Type::Bool && enabled->b;

    auto card = std::make_shared<CardContainer>();
    card->title = L"Notifications";
    if (const FieldSpec* s = FindSpec("ui.toast_enabled")) card->actions.push_back(MakeSwitch(*s));
    if (toasts_on) {
        if (const FieldSpec* s = FindSpec("ui.toast_on_save"))
            AddRow(*card, AppearanceRow(L"Clip saved", L"A popup when a save finishes.", MakeSwitch(*s)));
        if (const FieldSpec* s = FindSpec("ui.toast_on_buffer_toggle"))
            AddRow(*card, AppearanceRow(L"Buffer on/off", L"A popup when recording starts or stops.", MakeSwitch(*s)));
        if (const FieldSpec* s = FindSpec("ui.toast_style"))
            AddRow(*card, AppearanceRow(L"Style", L"A card, or a pill as wide as its text.",
                                        MakeSegmented(*s, [k = s->key](const std::string& v) { return OptionLabelFor(k, v); })));
        // Its own corner picture: the Layout tab's screen only places the panel now.
        if (const FieldSpec* spec = FindSpec("ui.toast_position")) {
            auto corner = std::make_shared<PositionPickerControl>();
            corner->id = spec->key;
            const auto* v = EffectiveConfigValue(spec->key);
            const std::string at = v && v->type == ombra::JsonValue::Type::String ? v->str : "top_right";
            for (const auto& opt : spec->enum_options) {
                const bool right = opt.find("right") != std::string::npos, bottom = opt.find("bottom") != std::string::npos;
                corner->spots.push_back({right ? 0.85f : 0.15f, bottom ? 0.85f : 0.15f});
                corner->names.push_back(OptionLabelFor(spec->key, opt));
                if (opt == at) corner->selected = static_cast<int>(corner->spots.size()) - 1;
            }
            const FieldSpec s = *spec;
            corner->on_change = [this, s](int i) {
                if (i >= 0 && i < static_cast<int>(s.enum_options.size()))
                    ApplySettingChange(s, ombra::JsonValue::MakeStr(s.enum_options[i]));
            };
            controls_by_key_[spec->key] = corner;
            auto row = AppearanceRow(L"Corner", L"Where on the screen they appear.", corner);
            row->align_top = true;
            card->rows.push_back(row);
        }
    } else {
        card->help = L"No popups. Saves and the buffer work the same; you just aren't told on screen.";
    }
    sec.AddRow(card);
    groups_.push_back(std::move(sec));
}

namespace {
const FieldSpec* FindSpecIn(const std::vector<FieldSpec>& specs, const std::string& key) {
    auto it = std::find_if(specs.begin(), specs.end(), [&](const FieldSpec& s) { return s.key == key; });
    return (it != specs.end() && it->implemented) ? &*it : nullptr;
}
// "NVIDIA GeForce RTX 5060 Laptop GPU" -> "RTX 5060 Laptop GPU", "AMD Radeon 780M Graphics" ->
// "Radeon 780M": the vendor is already on the tile's encoder line.
std::wstring ShortGpuName(const std::string& name) {
    std::wstring w = Utf8ToWide(name);
    for (const wchar_t* prefix : {L"NVIDIA ", L"GeForce ", L"AMD "}) {
        const size_t n = wcslen(prefix);
        if (w.compare(0, n, prefix) == 0) w.erase(0, n);
    }
    const std::wstring suffix = L" Graphics";
    if (w.size() > suffix.size() && w.compare(w.size() - suffix.size(), suffix.size(), suffix) == 0)
        w.erase(w.size() - suffix.size());
    return w;
}

}  // namespace

#ifdef OMBRA_REPLAY_PAGE_EXPERIMENT
// Build-time page replacement; no runtime switch or copied screen.
#include OMBRA_REPLAY_PAGE_EXPERIMENT
#else
// Replay & Quality: every
// setting that shapes the recorded video has this one home. Replay says how far back and where it's
// kept; each Video row says what its choice records. The recording GPU stays on Advanced > Encoder;
// the last Video row names the one in use and goes there.
void SettingsScreen::BuildReplayQualityPage() {
    GroupSection sec;
    sec.name = L"Replay";
    auto str_of = [&](const char* key, const char* fallback) -> std::string {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string(fallback);
    };
    auto num_of = [&](const char* key) -> double {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::Number) ? v->num : 0.0;
    };
    auto mbps = [](int kbps) { return std::to_wstring(static_cast<int>(std::lround(kbps / 1000.0))) + L" Mbps"; };
    auto row_of = [](const wchar_t* title, std::wstring help, std::vector<std::shared_ptr<Widget>> controls) {
        auto row = std::make_shared<SettingRow>();
        row->title = title;
        row->help = std::move(help);
        row->controls = std::move(controls);
        return row;
    };
    auto plain = [](const std::string& key) {
        return ChoiceLabel([key](const std::string& v) { return OptionLabelFor(key, v); });
    };

    // What the staged settings record. The bitrate follows the engine's derivation
    // (config.cpp: the preset scaled by the output size before any AV1 alignment, and the fps).
    const std::string res    = str_of("capture.resolution", "native");
    const std::string codec  = str_of("encode.video_codec", "av1");
    const std::string preset = str_of("encode.quality_preset", "high");
    const bool have_source   = preview_source_w_ > 0 && preview_source_h_ > 0;
    const ombra::VideoSize scaled =
        have_source ? ombra::ResolveOutputSize(preview_source_w_, preview_source_h_, res) : ombra::VideoSize{};
    ombra::VideoSize coded = scaled;
    if (const RecordingGpu* gpu = StagedRecordingGpu(); have_source && gpu && gpu->av1 && codec == "av1")
        coded = ombra::PlanEncode(scaled, ombra::VideoCodec::AV1, gpu->av1_align_w, gpu->av1_align_h,
                                  str_of("encode.amf_av1_unaligned", "pad")).size;
    const int fps_setting = static_cast<int>(num_of("capture.framerate"));
    // The engine's reading of "match display" (NTSC rounding, clamping; 60 while the rate is unknown).
    const int fps = ombra::ResolveFramerate(fps_setting, preview_display_fps_);
    // The codec the engine will record (H.264 where the GPU or the AMF size rule says so): the
    // preset bitrate depends on it, as config.cpp derives it.
    const ombra::VideoCodec recorded =
        ombra::RecordedCodec(RecordingGpuCapsNow(), str_of("encode.adapter", "auto"), codec,
                             str_of("encode.amf_av1_unaligned", "pad"), scaled);
    auto preset_kbps = [&](const std::string& p) {
        return have_source ? ombra::QualityPresetToBitrate(p.c_str(), static_cast<int>(scaled.width),
                                                           static_cast<int>(scaled.height), fps, recorded)
                           : 0;
    };
    const int kbps = preset == "custom" || !have_source ? static_cast<int>(num_of("encode.bitrate_kbps"))
                                                        : preset_kbps(preset);

    // --- Replay: how far back, and where it's kept.
    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Replay";
        const bool file = StagedRingIsFile();
        if (const FieldSpec* spec = FindSpecIn(specs_, "buffer.seconds")) {
            // What the length costs: the engine's projection when there is one.
            std::wstring use = RingUseText();
            if (use.empty()) {
                const int secs = static_cast<int>(num_of("buffer.seconds"));
                use = std::to_wstring(secs) + L" s of video is about " + std::to_wstring(secs * kbps / 8000) +
                      (file ? L" MB on disk, plus audio" : L" MB of RAM once full, plus audio");
            }
            auto length = MakeSegmented(*spec, [](const std::string& v) {
                const int s = std::atoi(v.c_str());
                return s >= 60 && s % 60 == 0 ? std::to_wstring(s / 60) + L" min" : std::to_wstring(s) + L" s";
            });
            card->rows.push_back(row_of(L"Replay length", use + L".", {length}));
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "buffer.storage")) {
            card->rows.push_back(row_of(L"Keep it in",
                                        file ? L"The file is recreated on every start; it doesn't survive a restart."
                                             : L"Memory is fastest; a disk file trades RAM for disk space.",
                                        {MakeSegmented(*spec, plain(spec->key))}));
        }
        if (file) {
            // The folder, and whether the buffer file fits on its drive.
            std::function<void()> browse;
            if (const auto loc = RingFileLocation(&browse)) {
                auto path = std::make_shared<PathControl>();
                path->id = "buffer.file_path";
                path->path = loc->folder;
                path->on_browse = std::move(browse);
                path->width_dip = 280.0f;
                auto folder = row_of(L"Folder", loc->Caption(), {path});
                if (!loc->Fits()) folder->help_tone = SettingRow::Tone::Error;
                controls_by_key_["buffer.file_path"] = path;
                card->rows.push_back(folder);
            }
        }
        sec.AddRow(card);
    }

    // --- Video: each row says what its choice records.
    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Video";
        constexpr float kSelectDip = 184.0f;  // one width, so the Selects line up

        if (const FieldSpec* spec = FindSpecIn(specs_, "capture.resolution")) {
            std::wstring help;
            if (have_source) {
                const bool smaller = scaled.width != (preview_source_w_ & ~1u) || scaled.height != (preview_source_h_ & ~1u);
                help = L"Records " + std::to_wstring(coded.width) + L" × " + std::to_wstring(coded.height) +
                       (smaller ? L", scaled down" : L"");
            }
            auto sel = MakeSelect(*spec, plain(spec->key));
            sel->width_dip = kSelectDip;
            card->rows.push_back(row_of(L"Resolution", help, {sel}));
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "capture.framerate")) {
            // "Match display" names no number; the help says what it records at.
            std::wstring help;
            if (fps_setting <= 0 && preview_display_fps_ > 0)
                help = L"Records " + std::to_wstring(fps) + L" fps, your display's rate";
            else if (preview_display_fps_ > 0)
                help = L"Your display runs at " + std::to_wstring(preview_display_fps_) + L" Hz";
            auto sel = MakeSelect(*spec, [key = spec->key](const std::string& v) {
                return v == "0" ? OptionLabelFor(key, v) : Utf8ToWide(v) + L" fps";
            });
            sel->width_dip = kSelectDip;
            card->rows.push_back(row_of(L"Framerate", help, {sel}));
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "encode.video_codec")) {
            // Same bitrate either way (the preset has no codec input), so AV1 buys sharpness, not size.
            auto row = row_of(L"Codec", codec == "h264" ? L"Plays in every app" : L"Sharper at the same bitrate",
                              {MakeSegmented(*spec, plain(spec->key))});
            // AV1 the staged encoder can't record: say so here; Advanced > Encoder explains why.
            if (const RecordingGpu* gpu = StagedRecordingGpu();
                codec == "av1" && (recording_on_cpu_ || (gpu && (gpu->vendor == "cpu" || !gpu->av1)))) {
                row->help = L"This GPU can't record AV1, so it falls back to H.264";
                row->help_tone = SettingRow::Tone::Warning;
            }
            card->rows.push_back(row);
        }
        // Bitrate is what the quality preset sets, so the preset is its menu, each choice with its
        // number. Custom adds a stepper for the number itself.
        if (const FieldSpec* spec = FindSpecIn(specs_, "encode.quality_preset")) {
            auto sel = MakeSelect(*spec, [&](const std::string& v) {
                const int k = v == "custom" ? 0 : preset_kbps(v);
                return k > 0 ? OptionLabelFor(spec->key, v) + L" · " + mbps(k) : OptionLabelFor(spec->key, v);
            });
            sel->width_dip = kSelectDip;
            std::vector<std::shared_ptr<Widget>> controls{sel};
            std::wstring help = L"More is sharper, and makes the replay bigger";
            if (const FieldSpec* br = FindSpecIn(specs_, "encode.bitrate_kbps"); br && preset == "custom") {
                // Stored in kbps, stepped in whole Mbps.
                auto stepper = std::make_shared<StepperControl>();
                stepper->id = br->key;
                stepper->min = (std::max)(1.0, std::ceil(br->min_val / 1000.0));
                stepper->max = br->max_val / 1000.0;
                stepper->value = std::clamp(std::round(num_of("encode.bitrate_kbps") / 1000.0), stepper->min, stepper->max);
                stepper->unit = L"Mbps";
                const FieldSpec br_spec = *br;
                stepper->on_change = [this, br_spec](double v) {
                    ApplySettingChange(br_spec, ombra::JsonValue::MakeNum(v * 1000.0));
                };
                controls_by_key_[br->key] = stepper;
                controls.insert(controls.begin(), stepper);
                help = L"Your own number, in 1 Mbps steps";
            }
            card->rows.push_back(row_of(L"Bitrate", help + L".", std::move(controls)));
        }
        {
            std::wstring where;
            const RecordingGpu* gpu = StagedRecordingGpu();
            if (recording_on_cpu_ || (gpu && gpu->vendor == "cpu")) {
                where = L"The CPU (x264)";
            } else if (gpu) {
                where = ShortGpuName(gpu->name) + L" (" + Utf8ToWide(gpu->backend) + L")";
                if (!display_owner_gpu_.empty())
                    where += gpu->name == display_owner_gpu_ ? L", which drives this display"
                                                             : L", not the GPU driving this display";
            } else {
                where = L"Chosen on Advanced > Encoder";
            }
            auto change = std::make_shared<ButtonControl>();
            change->label = L"Change";
            change->in_row = true;
            change->on_click = [this]() { QueueNavigate(kRailAdvanced, 0); };
            card->rows.push_back(row_of(L"Recording GPU", where, {change}));
        }
        sec.AddRow(card);
    }
    groups_.push_back(std::move(sec));
}
#endif

// buffer.file_path as a place on a drive: the folder (the default one when the key is empty), and
// whether the staged buffer file fits there. The key holds the file's full path; the folder picker
// works in folders, so the file name is stripped for display and appended on Browse.
std::optional<DiskLocation> SettingsScreen::RingFileLocation(std::function<void()>* on_browse) {
    const FieldSpec* spec = FindSpecIn(specs_, "buffer.file_path");
    if (!spec) return std::nullopt;
    static constexpr wchar_t kRingFile[] = L"\\ring.dat";
    std::wstring file;
    if (const auto* v = EffectiveConfigValue(spec->key); v && v->type == ombra::JsonValue::Type::String && !v->str.empty()) {
        wchar_t expanded[MAX_PATH];
        const std::wstring raw = Utf8ToWide(v->str);
        file = ExpandEnvironmentStringsW(raw.c_str(), expanded, MAX_PATH) ? expanded : raw;
    }
    DiskLocation loc;
    loc.is_default = file.empty();
    if (file.empty()) file = ombra::Instance().data_dir + kRingFile;  // what the engine derives (reconfig.cpp)
    const size_t slash = file.find_last_of(L"\\/");
    loc.folder = slash == std::wstring::npos ? file : file.substr(0, slash);

    // Free space on the volume the folder will be on: ask the nearest folder that exists.
    std::wstring probe = loc.folder;
    ULARGE_INTEGER avail{};
    while (!probe.empty() && !GetDiskFreeSpaceExW(probe.c_str(), &avail, nullptr, nullptr)) {
        const size_t up = probe.find_last_of(L"\\/");
        probe = up == std::wstring::npos ? L"" : probe.substr(0, up);
    }
    if (!probe.empty()) {
        loc.free_bytes = static_cast<double>(avail.QuadPart);
        wchar_t volume[MAX_PATH];
        if (GetVolumePathNameW(probe.c_str(), volume, MAX_PATH)) {
            loc.drive = volume;
            while (!loc.drive.empty() && (loc.drive.back() == L'\\' || loc.drive.back() == L'/')) loc.drive.pop_back();
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA existing{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &existing))
        loc.reusable_bytes = static_cast<double>((static_cast<uint64_t>(existing.nFileSizeHigh) << 32) | existing.nFileSizeLow);
    // The preview answers for the staged config (this row only shows with file storage staged); the
    // file is the video rings, sized up front. preview_memory_.file_backed is the committed engine's.
    if (preview_memory_.valid) {
        double pipes = 1.0, srcs = 1.0;
        ProjectedRingCounts(pipes, srcs);
        loc.file_bytes = preview_memory_.video_ring_bytes * pipes;
    }

    const FieldSpec s = *spec;
    if (on_browse) *on_browse = [this, s, folder = loc.folder]() {
        std::wstring picked = PickFolder(host_ ? host_->OwnerWindow() : nullptr, folder);
        if (picked.empty()) return;
        ApplySettingChange(s, ombra::JsonValue::MakeStr(WideToUtf8(picked + kRingFile)));
        RebuildGroupRows(current_dpi_scale_);
        if (host_) host_->RequestRepaint();
    };
    return loc;
}

// Output: where clips go, and what they're called. One card of rows, like Replay & Quality's.
void SettingsScreen::BuildOutputPage() {
    GroupSection sec;
    sec.name = L"Output";
    auto str_of = [&](const char* key) -> std::wstring {
        const auto* v = EffectiveConfigValue(key);
        return v && v->type == ombra::JsonValue::Type::String ? Utf8ToWide(v->str) : std::wstring();
    };
    auto row_of = [](const wchar_t* title, std::wstring help, std::vector<std::shared_ptr<Widget>> controls) {
        auto row = std::make_shared<SettingRow>();
        row->title = title;
        row->help = std::move(help);
        row->controls = std::move(controls);
        return row;
    };
    const bool per_game = [&] {
        const auto* v = EffectiveConfigValue("output.per_game_folders");
        return !v || v->type != ombra::JsonValue::Type::Bool || v->b;
    }();

    auto card = std::make_shared<CardContainer>();
    card->title = L"Saved clips";
    {
        std::wstring root;
        if (const FieldSpec* spec = FindSpecIn(specs_, "output.root")) {
            root = ombra::ResolveOutputRoot(WideToUtf8(str_of("output.root")));
            auto path =std::make_shared<PathControl>();
            path->id = spec->key;
            path->path = root;
            path->width_dip = 280.0f;
            const FieldSpec s = *spec;
            path->on_browse = [this, s, root]() {
                std::wstring picked = PickFolder(host_ ? host_->OwnerWindow() : nullptr, root);
                if (!picked.empty()) ApplySettingChange(s, ombra::JsonValue::MakeStr(WideToUtf8(picked)));
            };
            controls_by_key_[spec->key] = path;
            card->rows.push_back(row_of(L"Folder", L"Where a save puts its clip.", {path}));
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "output.per_game_folders")) {
            auto sw = std::make_shared<SwitchControl>();
            sw->id = spec->key;
            sw->on = per_game;
            const FieldSpec s = *spec;
            sw->on_change = [this, s](bool on) { ApplySettingChange(s, ombra::JsonValue::MakeBool(on)); };
            controls_by_key_[spec->key] = sw;
            card->rows.push_back(row_of(L"A folder per game",
                                        per_game ? L"A Valorant clip goes in Valorant, the desktop's in the folder itself."
                                                 : L"Every clip goes in the folder itself.",
                                        {sw}));
        }
    }

    // File name: placeholders as pills, staged as typed.
    if (const FieldSpec* spec = FindSpecIn(specs_, "output.filename")) {
        static constexpr char kDefault[] = "{game} {date} {time}{monitor_suffix}";  // config.h, save.cpp

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t date[16], time[16];
        swprintf_s(date, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);  // as FormatFilename (reconfig.cpp)
        swprintf_s(time, L"%02d-%02d-%02d", st.wHour, st.wMinute, st.wSecond);

        auto field = std::make_shared<TemplateFieldControl>();
        field->id = spec->key;
        field->tokens = {{L"{game}", L"Game", L"Valorant"}, {L"{date}", L"Date", date},
                         {L"{time}", L"Time", time}, {L"{monitor_suffix}", L"Monitor", L""}};
        field->text = str_of(spec->key.c_str());
        field->fallback = Utf8ToWide(kDefault);  // the engine names an empty pattern this way
        // Characters Windows rejects in a file name never get typed (the engine replaces them too).
        field->accept_char = [](wchar_t c) { return !wcschr(L"<>:\"/\\|?*", c); };
        field->width_dip = 420.0f;
        if (template_edit_) {
            field->focused = true;
            field->SetCaret(template_edit_->caret);
        } else {
            field->SetCaret(field->text.size());
        }
        const FieldSpec s = *spec;
        const std::wstring built = field->text;
        field->on_change = [this, s, built](const std::wstring& text) {
            if (!template_edit_) template_edit_ = TemplateEdit{built};
            ApplySettingChange(s, ombra::JsonValue::MakeStr(WideToUtf8(text)));
        };
        template_field_ = field;
        controls_by_key_[spec->key] = field;

        // Back to the shipped pattern, off the default only (as a shortcut's reset).
        if (WideToUtf8(field->text) != kDefault) {
            field->on_reset = [this, s]() {
                template_edit_.reset();
                ApplySettingChange(s, ombra::JsonValue::MakeStr(kDefault));
            };
        }

        // {monitor_suffix} is " - <name>" for every monitor but the first (config.cpp).
        auto row = row_of(L"File name", L"Monitor adds the monitor's name when you record several, except on the first one.", {field});
        row->align_top = true;
        card->rows.push_back(row);
    }
    sec.AddRow(card);
    groups_.push_back(std::move(sec));
}

// Leaves the file-name field: Enter and a click elsewhere keep what's staged, Esc puts back what
// the field held when the edit began.
void SettingsScreen::EndTemplateEdit(bool revert) {
    auto f = template_field_.lock();
    if (!f) return;
    f->focused = f->focus_visible = false;
    f->selected_chip = -1;
    const std::optional<TemplateEdit> edit = std::exchange(template_edit_, std::nullopt);
    if (revert && edit && edit->original != f->text) {
        if (const FieldSpec* spec = FindSpecIn(specs_, f->id))
            ApplySettingChange(*spec, ombra::JsonValue::MakeStr(WideToUtf8(edit->original)));
    }
}

// --- Games ---------------------------------------------------------------------------------------

namespace {

std::wstring GameSourceLabel(ombra::GameSource s) {
    switch (s) {
        case ombra::GameSource::Riot:  return L"Riot";
        case ombra::GameSource::Steam: return L"Steam";
        case ombra::GameSource::Epic:  return L"Epic";
        case ombra::GameSource::Gog:   return L"GOG";
        case ombra::GameSource::Ubisoft: return L"Ubisoft";
        case ombra::GameSource::User:  break;
    }
    return L"Added by you";
}

std::wstring LowerFileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring f = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (auto& c : f) c = static_cast<wchar_t>(towlower(c));
    return f;
}

// A system-wide install of a runtime (Program Files, JAVA_HOME, the per-user Python install,
// WindowsApps), as opposed to a launcher's own bundled copy like Prism's java folder.
bool IsSharedRuntimeLocation(const std::wstring& path) {
    const std::wstring key = ombra::GameKey(path);
    auto under = [&](const wchar_t* env, const wchar_t* suffix) {
        wchar_t buf[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(env, buf, MAX_PATH) || buf[0] == L'\0') return false;
        const std::wstring root = ombra::GameKey(std::wstring(buf) + suffix);
        return !root.empty() && key.size() > root.size() && key.compare(0, root.size(), root) == 0 &&
               key[root.size()] == L'\\';
    };
    return under(L"ProgramFiles", L"") || under(L"ProgramFiles(x86)", L"") || under(L"ProgramW6432", L"") ||
           under(L"JAVA_HOME", L"") || under(L"LOCALAPPDATA", L"\\Programs\\Python") ||
           key.find(L"\\windowsapps\\") != std::wstring::npos;
}

// What's worth saying about an entry's path instead of the path itself. A runtime host (javaw.exe,
// python.exe) runs other programs, so a shared install -- a system JDK rather than the launcher's
// own copy -- would make all of them games; a launcher's bundled copy is fine and gets no warning.
// A bare exe name (carried over from the old lists) matches that name anywhere on the machine.
std::wstring GamePathWarning(const ombra::GameEntry& g) {
    if (g.path.find_first_of(L"\\/") == std::wstring::npos)
        return L"Matches any " + g.path + L", wherever it runs from.";
    const std::wstring file = LowerFileName(g.path);
    for (const wchar_t* host : {L"javaw.exe", L"java.exe", L"python.exe", L"pythonw.exe"}) {
        if (file == host && IsSharedRuntimeLocation(g.path))
            return L"A shared " + file + L": every program it runs counts as this game.";
    }
    return L"";
}

bool SameDecisions(const std::vector<ombra::GameEntry>& a, const std::vector<ombra::GameEntry>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
        const auto key = ombra::GameKey(x.path);
        const bool found = std::any_of(b.begin(), b.end(), [&](const ombra::GameEntry& y) {
            return ombra::GameKey(y.path) == key && y.is_game == x.is_game && y.name == x.name && y.source == x.source;
        });
        if (!found) return false;
    }
    return true;
}

}  // namespace

std::vector<ombra::GameEntry> SettingsScreen::StagedGameDecisions() const {
    const ombra::JsonValue* v = EffectiveConfigValue("games.list");
    return v ? ombra::ParseGamesList(*v) : std::vector<ombra::GameEntry>{};
}

void SettingsScreen::StageGameDecisions(const std::vector<ombra::GameEntry>& decisions) {
    const FieldSpec* spec = FindSpecIn(specs_, "games.list");
    if (!spec) return;
    // Back to what's saved: unstage rather than keep an edit that changes nothing, so the Save bar
    // goes away when a switch is flipped and flipped back.
    const ombra::JsonValue* committed = current_config_.Find("games.list");
    const auto saved = committed ? ombra::ParseGamesList(*committed) : std::vector<ombra::GameEntry>{};
    if (SameDecisions(decisions, saved) && pending_changes_.count("games.list")) {
        pending_changes_.erase("games.list");
        RebuildGroupRows(current_dpi_scale_);
        if (host_) host_->RequestRepaint();
        return;
    }
    ApplySettingChange(*spec, ombra::SerializeGamesList(decisions));
}

void SettingsScreen::StageGameDecision(const ombra::GameEntry& entry, bool is_game) {
    auto decisions = StagedGameDecisions();
    const std::wstring key = ombra::GameKey(entry.path);
    decisions.erase(std::remove_if(decisions.begin(), decisions.end(),
                                   [&](const ombra::GameEntry& d) { return ombra::GameKey(d.path) == key; }),
                    decisions.end());
    if (!(entry.detected && is_game)) {
        ombra::GameEntry d = entry;
        d.is_game = is_game;
        d.detected = false;
        decisions.push_back(std::move(d));
    }
    StageGameDecisions(decisions);
}

void SettingsScreen::StageGameRemoval(const std::wstring& path) {
    auto decisions = StagedGameDecisions();
    const std::wstring key = ombra::GameKey(path);
    decisions.erase(std::remove_if(decisions.begin(), decisions.end(),
                                   [&](const ombra::GameEntry& d) { return ombra::GameKey(d.path) == key; }),
                    decisions.end());
    StageGameDecisions(decisions);
}

void SettingsScreen::ApplyGamesReply(const ombra::JsonValue* resp) {
    games_pending_ = false;
    games_fetched_ = true;
    const auto* ok = resp ? resp->Find("ok") : nullptr;
    games_failed_ = !(ok && ok->type == ombra::JsonValue::Type::Bool && ok->b);
    if (games_failed_) return;  // keep whatever an earlier reply gave
    auto str = [](const ombra::JsonValue& o, const char* k) {
        const auto* v = o.Find(k);
        return (v && v->type == ombra::JsonValue::Type::String) ? Utf8ToWide(v->str) : std::wstring();
    };
    games_detected_.clear();
    if (const auto* arr = resp->Find("games"); arr && arr->type == ombra::JsonValue::Type::Array) {
        for (const auto& g : arr->arr) {
            const auto* det = g.Find("detected");
            // The user's own entries come from games.list; only the scan's results are kept here.
            if (!det || det->type != ombra::JsonValue::Type::Bool || !det->b) continue;
            ombra::GameEntry e;
            e.name = str(g, "name");
            e.path = str(g, "path");
            const auto* src = g.Find("source");
            if (src && src->type == ombra::JsonValue::Type::String) ombra::ParseGameSource(src->str, &e.source);
            e.is_game = true;  // a scan only ever reports games; the reply's "game" has decisions applied
            e.detected = true;
            e.icon = str(g, "icon");
            if (!e.path.empty()) games_detected_.push_back(std::move(e));
        }
    }
    games_seen_.clear();
    if (const auto* arr = resp->Find("seen"); arr && arr->type == ombra::JsonValue::Type::Array) {
        for (const auto& a : arr->arr) {
            SeenGame sg{str(a, "name"), str(a, "path")};
            if (!sg.path.empty()) games_seen_.push_back(std::move(sg));
        }
    }
}

void SettingsScreen::FetchGamesAsync(bool rescan) {
    if (!host_ || games_pending_) return;
    games_pending_ = true;
    const uint64_t gen = ++games_generation_;
    ombra::JsonValue req = ombra::JsonValue::MakeObject();
    req.Set("type", ombra::JsonValue::MakeStr("list_games"));
    if (rescan) req.Set("rescan", ombra::JsonValue::MakeBool(true));
    host_->RequestAsync(std::move(req), 3000, [this, gen](std::optional<ombra::JsonValue> resp) {
        if (gen != games_generation_) return;  // an overlay reopen or a newer request superseded it
        ApplyGamesReply(resp ? &*resp : nullptr);
        RebuildGroupRows(current_dpi_scale_);
        if (host_) host_->RequestRepaint();
    });
}

void SettingsScreen::SeedGamesForSnapshot() {
    ++games_generation_;  // drop the real (engine-less) fetch's reply
    games_pending_ = false;
    ombra::JsonValue resp = ombra::JsonValue::MakeObject();
    resp.Set("ok", ombra::JsonValue::MakeBool(true));
    ombra::JsonValue games = ombra::JsonValue::MakeArray();
    auto add = [&](const char* name, const char* path, const char* source, const char* icon = nullptr) {
        ombra::JsonValue g = ombra::JsonValue::MakeObject();
        g.Set("name", ombra::JsonValue::MakeStr(name));
        g.Set("path", ombra::JsonValue::MakeStr(path));
        g.Set("source", ombra::JsonValue::MakeStr(source));
        if (icon) g.Set("icon", ombra::JsonValue::MakeStr(icon));
        g.Set("game", ombra::JsonValue::MakeBool(true));
        g.Set("detected", ombra::JsonValue::MakeBool(true));
        games.arr.push_back(std::move(g));
    };
    add("League of Legends", "E:\\Games\\Riot Games\\League of Legends", "riot", "C:\\Windows\\explorer.exe");
    add("VALORANT", "E:\\Games\\Riot Games\\VALORANT\\live", "riot", "C:\\Windows\\System32\\Taskmgr.exe");
    add("Counter-Strike 2", "E:\\Games\\Steam\\steamapps\\common\\Counter-Strike Global Offensive", "steam",
        "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg");
    add("Apex Legends", "E:\\Games\\Steam\\steamapps\\common\\Apex Legends", "steam", "C:\\Windows\\regedit.exe");
    add("Baldur's Gate 3", "E:\\Games\\Steam\\steamapps\\common\\Baldurs Gate 3", "steam");
    add("Deep Rock Galactic", "E:\\Games\\Steam\\steamapps\\common\\Deep Rock Galactic", "steam");
    add("Elden Ring", "E:\\Games\\Steam\\steamapps\\common\\ELDEN RING", "steam");
    add("Hades II", "E:\\Games\\Steam\\steamapps\\common\\Hades II", "steam");
    add("Helldivers 2", "E:\\Games\\Steam\\steamapps\\common\\Helldivers 2", "steam");
    add("Lethal Company", "E:\\Games\\Steam\\steamapps\\common\\Lethal Company", "steam");
    add("Rocket League", "E:\\Games\\Steam\\steamapps\\common\\rocketleague", "steam");
    add("SteamVR", "E:\\Games\\Steam\\steamapps\\common\\SteamVR", "steam", "C:\\Windows\\System32\\cmd.exe");
    add("Wallpaper Engine", "E:\\Games\\Steam\\steamapps\\common\\wallpaper_engine", "steam");
    add("Fortnite", "C:\\Program Files\\Epic Games\\Fortnite", "epic");
    resp.Set("games", std::move(games));
    ombra::JsonValue seen = ombra::JsonValue::MakeArray();
    for (const auto& [name, path] : {std::pair{"renderer.exe", "C:\\Tools\\Renderer\\renderer.exe"},
                                     std::pair{"Blender", "C:\\Program Files\\Blender Foundation\\Blender 4.2\\blender.exe"}}) {
        ombra::JsonValue a = ombra::JsonValue::MakeObject();
        a.Set("name", ombra::JsonValue::MakeStr(name));
        a.Set("path", ombra::JsonValue::MakeStr(path));
        seen.arr.push_back(std::move(a));
    }
    resp.Set("seen", std::move(seen));
    ApplyGamesReply(&resp);

    // The user's decisions, committed: two not-games among the detected, Discord and OBS by hand,
    // and Java launchers through their own javaw.exe.
    std::vector<ombra::GameEntry> decisions;
    auto decide = [&](const wchar_t* name, const wchar_t* path, ombra::GameSource src, bool is_game) {
        ombra::GameEntry e;
        e.name = name;
        e.path = path;
        e.source = src;
        e.is_game = is_game;
        decisions.push_back(std::move(e));
    };
    decide(L"SteamVR", L"E:\\Games\\Steam\\steamapps\\common\\SteamVR", ombra::GameSource::Steam, false);
    decide(L"Wallpaper Engine", L"E:\\Games\\Steam\\steamapps\\common\\wallpaper_engine", ombra::GameSource::Steam, false);
    decide(L"Discord", L"C:\\Users\\Owner\\AppData\\Local\\Discord", ombra::GameSource::User, false);
    decide(L"OBS Studio", L"C:\\Program Files\\obs-studio", ombra::GameSource::User, false);
    decide(L"Minecraft", L"C:\\Users\\Owner\\AppData\\Roaming\\PrismLauncher\\java\\java-runtime-delta\\bin\\javaw.exe",
           ombra::GameSource::User, true);
    current_config_.Set("games.list", ombra::SerializeGamesList(decisions));
    // Icons from files every Windows install has (an .exe, a .jpg), loaded here and now: a snapshot
    // never pumps the async fetch's reply. Rows without one keep the controller.
    std::vector<std::wstring> missing;
    for (const auto& g : games_detected_) GameIcon(g.icon, missing);
    const UINT px = static_cast<UINT>(Sizes{}.app_icon() * (std::max)(1.0f, current_dpi_scale_) + 0.5f);
    for (const auto& p : missing) game_icons_[p] = std::make_shared<const FileIcon>(LoadFileIcon(p, px));
    RebuildGroupRows(current_dpi_scale_);
}

// The loaded icon for `source`, or null -- then `source` goes on `missing` unless it was asked for
// already (a failure is cached like a success).
std::shared_ptr<const FileIcon> SettingsScreen::GameIcon(const std::wstring& source,
                                                         std::vector<std::wstring>& missing) const {
    if (source.empty()) return nullptr;
    if (auto it = game_icons_.find(source); it != game_icons_.end()) return it->second;
    if (std::find(missing.begin(), missing.end(), source) == missing.end()) missing.push_back(source);
    return nullptr;
}

// Built from list_windows, not list_processes: the process list is one entry per exe *name*, so with
// two javaw.exe running (a game and an IDE's JVM) it offered one path, whichever came first.
// Every window carries its own image path; one choice per distinct path.
void SettingsScreen::OpenAddGamePicker(const D2D1_RECT_F& anchor) {
    if (!host_) return;
    PrefetchProcessNames();  // the game's name rather than "javaw" where the engine can name it
    host_->ShowDropdown(anchor, {L"Loading…"}, -1, [](int) {});
    host_->RequestWindowListAsync([this, anchor](std::vector<WindowEntry> windows) {
        if (!host_) return;
        // Same list the One window grid shows: refresh its cache too. A newer request supersedes an
        // older one's reply, so a grid fetch in flight would otherwise stay pending until reopen.
        window_list_ = windows;
        window_list_fetched_ = true;
        window_list_pending_ = false;
        const auto merged = ombra::MergeGames(games_detected_, StagedGameDecisions());
        struct Choice { std::wstring label, name, path, file; };
        std::vector<Choice> choices;
        std::set<std::wstring> seen_keys;
        for (const auto& w : windows) {
            if (w.image_path.empty()) continue;  // no full path, nothing safe to store
            if (!seen_keys.insert(ombra::GameKey(w.image_path)).second) continue;
            if (ombra::ClassifyImage(merged, w.image_path) != ombra::GameVerdict::Unknown) continue;  // answered
            std::wstring name;
            if (auto it = process_display_names_.find(w.process); it != process_display_names_.end() && !it->second.empty())
                name = Utf8ToWide(it->second);
            if (name.empty()) name = w.title.empty() ? Utf8ToWide(w.process) : w.title;
            const std::wstring file = Utf8ToWide(w.process);
            choices.push_back({name + L"  (" + file + L")", name, w.image_path, file});
        }
        // Two programs with the same exe name say where each lives.
        for (auto& c : choices) {
            const bool twin = std::count_if(choices.begin(), choices.end(), [&](const Choice& o) {
                                  return _wcsicmp(o.file.c_str(), c.file.c_str()) == 0;
                              }) > 1;
            if (twin) c.label = c.name + L"  (" + c.path + L")";
        }
        std::sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) {
            return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
        });
        std::vector<std::wstring> options{L"Browse for a folder\u2026"};
        for (const auto& c : choices) options.push_back(c.label);
        host_->ShowDropdown(anchor, options, -1, [this, choices](int i) {
            ombra::GameEntry e;
            e.source = ombra::GameSource::User;
            if (i == 0) {
                const std::wstring folder = PickFolder(host_ ? host_->OwnerWindow() : nullptr, L"");
                if (folder.empty()) return;
                e.path = folder;
                e.name = folder.substr(folder.find_last_of(L"\\/") + 1);
                if (e.name.empty()) e.name = folder;
            } else if (i > 0 && static_cast<size_t>(i) <= choices.size()) {
                e.path = choices[i - 1].path;
                e.name = choices[i - 1].name;
            } else {
                return;
            }
            StageGameDecision(e, true);
        });
    });
}

// A game's icon: what the scan found, else the program itself when the entry is one exe's full path
// (a bare migrated name like "obs64.exe" names no file).
static std::wstring GameIconSource(const std::wstring& icon, const std::wstring& path) {
    if (!icon.empty()) return icon;
    const bool exe = path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".exe") == 0;
    return exe && path.find(L'\\') != std::wstring::npos ? path : std::wstring{};
}

// Games: only the card's rows scroll; its title, help and ⋯ stay put, as does the Save bar.
void SettingsScreen::BuildGamesPage() {
    if (!games_fetched_ && !games_pending_) FetchGamesAsync(false);

    GroupSection sec;
    sec.name = L"Games";

    const auto decisions = StagedGameDecisions();
    auto merged = ombra::MergeGames(games_detected_, decisions);

    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Games";
        card->help = games_failed_
            ? L"" OMBRA_PRODUCT_NAME "'s engine didn't answer, so installed games aren't listed. Only the ones you added are."
            : L"Riot, Steam, Epic, GOG and Ubisoft Connect games are found on their own. Switch one off and it never counts as a game.";

        // Games first, then the rest, alphabetical within each -- by the saved answer, not the
        // staged one, so a row doesn't jump out from under the pointer when its switch is flipped.
        const ombra::JsonValue* committed = current_config_.Find("games.list");
        const auto saved = ombra::MergeGames(games_detected_, committed ? ombra::ParseGamesList(*committed)
                                                                        : std::vector<ombra::GameEntry>{});
        auto saved_game = [&](const ombra::GameEntry& g) {
            const std::wstring key = ombra::GameKey(g.path);
            for (const auto& s : saved)
                if (ombra::GameKey(s.path) == key) return s.is_game;
            return true;  // added since the last save: with the games
        };
        std::stable_sort(merged.begin(), merged.end(), [&](const ombra::GameEntry& a, const ombra::GameEntry& b) {
            const bool ga = saved_game(a), gb = saved_game(b);
            if (ga != gb) return ga;
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        auto list = std::make_shared<RowList>();
        std::vector<std::wstring> missing_icons;
        // Apps Game mode armed for on its own that the list doesn't answer for yet, first. One
        // answered here leaves at once; the engine drops it from `seen` once the answer is saved.
        for (const auto& a : games_seen_) {
            if (ombra::ClassifyImage(merged, a.path) != ombra::GameVerdict::Unknown) continue;
            GameRow r;
            r.name = a.name;
            r.source = L"Looked like a game";
            r.detail = a.path;
            r.decide = true;
            r.icon = GameIcon(GameIconSource({}, a.path), missing_icons);
            ombra::GameEntry e;
            e.name = a.name;
            e.path = a.path;
            e.source = ombra::GameSource::User;
            r.on_decide = [this, e](bool is_game) { StageGameDecision(e, is_game); };
            game_rows_.push_back(std::move(r));
        }
        for (const auto& g : merged) {
            GameRow r;
            r.name = g.name;
            r.source = GameSourceLabel(g.detected ? g.source : ombra::GameSource::User);
            r.detail = g.path;
            r.warning = GamePathWarning(g);
            r.is_game = g.is_game;
            r.removable = !g.detected;
            r.icon = GameIcon(GameIconSource(g.icon, g.path), missing_icons);
            r.on_toggle = [this, g](bool is_game) { StageGameDecision(g, is_game); };
            if (r.removable) r.on_remove = [this, path = g.path]() { StageGameRemoval(path); };
            game_rows_.push_back(std::move(r));
        }
        list->empty_text = games_pending_ && !games_fetched_
            ? L"Looking for installed games\u2026"
            : L"No games yet. Riot, Steam, Epic, GOG and Ubisoft Connect games show up here on their own; add any other from ⋯.";
        if (!missing_icons.empty() && game_icons_pending_) game_icons_skipped_ = true;
        if (!missing_icons.empty() && !game_icons_pending_ && host_) {
            game_icons_pending_ = true;
            const UINT px = static_cast<UINT>(Sizes{}.app_icon() * current_dpi_scale_ + 0.5f);
            host_->RequestFileIconsAsync(std::move(missing_icons), px, [this](std::vector<FileIcon> icons) {
                game_icons_pending_ = false;
                bool any = false;
                for (auto& icon : icons) {
                    any = any || !icon.bgra.empty();
                    std::wstring key = icon.path;
                    game_icons_[std::move(key)] = std::make_shared<const FileIcon>(std::move(icon));
                }
                if (any || game_icons_skipped_) {
                    game_icons_skipped_ = false;
                    RebuildGroupRows(current_dpi_scale_);
                    if (host_) host_->RequestRepaint();
                }
            });
        }
        // The rows themselves (GameRow is what tests read).
        const bool any_removable = std::any_of(game_rows_.begin(), game_rows_.end(), [](const GameRow& r) { return r.removable; });
        if (games_edit_list_ && !any_removable) games_edit_list_ = false;
        for (const GameRow& g : game_rows_) {
            auto row = std::make_shared<SettingRow>();
            row->title = g.name;
            row->help = g.source + L" · " + g.detail;
            row->help_one_line = true;  // paths run long; every row keeps one height
            row->icon = g.icon;
            row->glyph = IconType::Gamepad2;
            row->faded = !g.decide && !g.is_game;
            row->attention = g.decide;
            if (!g.warning.empty()) {
                row->note = g.warning;
                row->note_tone = SettingRow::Tone::Warning;
            }
            if (g.decide) {
                for (const bool yes : {true, false}) {
                    auto b = std::make_shared<ButtonControl>();
                    b->label = yes ? L"It's a game" : L"Not a game";
                    b->in_row = true;
                    b->on_click = [f = g.on_decide, yes] { if (f) f(yes); };
                    row->controls.push_back(b);
                }
            } else {
                if (games_edit_list_ && g.removable) {
                    // Before the switch, so every switch keeps the right edge.
                    auto remove = std::make_shared<IconButtonControl>();
                    remove->glyph = IconButtonControl::Glyph::Remove;
                    remove->tooltip = L"Remove";
                    remove->on_click = [f = g.on_remove] { if (f) f(); };
                    row->controls.push_back(remove);
                }
                auto sw = std::make_shared<SwitchControl>();
                sw->on = g.is_game;
                sw->on_change = [f = g.on_toggle](bool on) { if (f) f(on); };
                row->controls.push_back(sw);
            }
            list->rows.push_back(row);
        }
        list->scroll_y = games_list_scroll_;
        list->on_scroll = [this](float y) { games_list_scroll_ = y; };
        games_list_ = list;
        card->rows = {list};

        // ⋯: Add game, Rescan, Edit list; Done while editing.
        if (games_edit_list_) {
            auto done = std::make_shared<ButtonControl>();
            done->label = L"Done";
            done->on_click = [this] {
                games_edit_list_ = false;
                RebuildGroupRows(current_dpi_scale_);
                if (host_) host_->RequestRepaint();
            };
            card->actions = {done};
        } else {
            auto more = std::make_shared<IconButtonControl>();
            more->glyph = IconButtonControl::Glyph::More;
            IconButtonControl* raw = more.get();
            more->on_click = [this, raw, any_removable] {
                if (!host_) return;
                D2D1_RECT_F at = raw->bounds;
                at.top -= scroll_area_.scroll_y;
                at.bottom -= scroll_area_.scroll_y;
                std::vector<std::wstring> labels{L"Add game", L"Rescan launchers"};
                if (any_removable) labels.push_back(L"Edit list");
                host_->ShowDropdown(at, labels, -1, [this, at](int i) {
                    if (i == 0) {
                        OpenAddGamePicker(at);
                    } else if (i == 1) {
                        std::erase_if(game_icons_, [](const auto& kv) { return !kv.second || kv.second->bgra.empty(); });
                        FetchGamesAsync(true);
                    } else if (i == 2) {
                        games_edit_list_ = true;
                        RebuildGroupRows(current_dpi_scale_);
                        if (host_) host_->RequestRepaint();
                    }
                });
            };
            card->actions = {more};
        }
        sec.AddRow(card);
    }
    groups_.push_back(std::move(sec));
}

// Advanced -> Encoder: which GPU records, as a tile per encoder, then
// how it encodes. Each tile says what it codes and what it's doing now.
void SettingsScreen::BuildAdvancedEncoderPage() {
    GroupSection sec;
    sec.name = L"Encoder";
    auto str_of = [&](const char* key, const char* fallback) -> std::string {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string(fallback);
    };
    auto num_of = [&](const char* key) -> double {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::Number) ? v->num : 0.0;
    };
    auto row_of = [](const wchar_t* title, std::wstring help, std::vector<std::shared_ptr<Widget>> controls) {
        auto row = std::make_shared<SettingRow>();
        row->title = title;
        row->help = std::move(help);
        row->controls = std::move(controls);
        return row;
    };
    // A number stepper staging `key`, `div` stored units to one shown (kbps -> Mbps).
    auto stepper_for = [&](const FieldSpec& spec, const wchar_t* unit, double div = 1.0) {
        auto st = std::make_shared<StepperControl>();
        st->id = spec.key;
        st->min = spec.min_val / div;
        st->max = spec.max_val / div;
        st->value = std::clamp(std::round(num_of(spec.key.c_str()) / div), st->min, st->max);
        st->unit = unit;
        st->on_change = [this, spec, div](double v) { ApplySettingChange(spec, ombra::JsonValue::MakeNum(v * div)); };
        controls_by_key_[spec.key] = st;
        return st;
    };
    const RecordingGpu* staged_gpu = StagedRecordingGpu();
    const bool          amf        = staged_gpu && staged_gpu->backend == "AMF";

    // --- Recording GPU: Auto, then one tile per encoder the engine reported.
    if (const FieldSpec* adapter_spec = FindSpecIn(specs_, "encode.adapter")) {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Recording GPU";
        const std::string pref = str_of("encode.adapter", "auto");
        const FieldSpec spec_copy = *adapter_spec;
        auto grid = std::make_shared<TileGrid>();
        grid->columns = 4;
        auto add_tile = [&](std::shared_ptr<TileControl> tile, const std::string& value) {
            tile->id = "encode.adapter:" + value;
            tile->selected = pref == value;
            tile->on_change = [this, spec_copy, value](bool) {
                ApplySettingChange(spec_copy, ombra::JsonValue::MakeStr(value));
            };
            grid->tiles.push_back(tile);
        };

        auto automatic = std::make_shared<TileControl>();
        automatic->title = L"Auto";
        automatic->detail = L"Display's GPU; NVIDIA in games";
        {
            const auto caps = RecordingGpuCapsNow();  // same order as recording_gpus_
            if (const ombra::RecordingGpuCaps* picked = ombra::PickRecordingGpu(caps, "auto")) {
                const RecordingGpu& g = recording_gpus_[static_cast<size_t>(picked - caps.data())];
                // Recording elsewhere under auto (a game on the NVIDIA GPU, or the display GPU's
                // encoder refused): name what records, not what auto would pick with no game.
                const ombra::JsonValue* saved = current_config_.Find("encode.adapter");
                const bool saved_auto = !saved || saved->type != ombra::JsonValue::Type::String || saved->str == "auto";
                const bool elsewhere  = pref == "auto" && saved_auto && !recording_on_cpu_ &&
                                       !recording_gpu_now_.empty() && recording_gpu_now_ != g.name;
                automatic->note = L"Picks " + (elsewhere      ? ShortGpuName(recording_gpu_now_)
                                               : g.vendor == "cpu" ? std::wstring(L"the CPU")
                                                                   : ShortGpuName(g.name));
            }
        }
        if (recording_on_cpu_ && pref == "auto") {
            automatic->note = L"Fell back to the CPU";
            automatic->note_tone = SettingRow::Tone::Warning;
        }
        add_tile(automatic, "auto");

        for (const auto& g : recording_gpus_) {
            auto tile = std::make_shared<TileControl>();
            tile->chips = {L"H.264"};
            if (g.vendor == "cpu") {
                // Not a GPU: no "drives the display", and its cost is on the tile itself.
                tile->title = L"CPU";
                tile->detail = L"x264 · heavy on the CPU";
                if (recording_on_cpu_) {
                    tile->note = L"Recording now";
                    tile->note_tone = SettingRow::Tone::Warning;
                }
                add_tile(tile, g.vendor);
                continue;
            }
            tile->title = ShortGpuName(g.name);
            tile->detail = Utf8ToWide(g.backend) + (g.drives_display ? L" · drives the display" : L"");
            if (g.av1) tile->chips.push_back(L"AV1");
            if (!recording_on_cpu_ && g.name == recording_gpu_now_) {
                tile->note = L"Recording now";
                tile->note_tone = SettingRow::Tone::Good;
            }
            add_tile(tile, g.vendor);
        }
        card->rows.push_back(grid);

        if (recording_gpus_.empty()) {
            card->help = L"Waiting for the engine to report its GPUs.";
        } else if (recording_on_cpu_ && pref != "cpu") {
            card->help = L"No GPU encoder would start, so " OMBRA_PRODUCT_NAME " records on the CPU. That takes several cores and can "
                         L"lower your game's FPS; updating the GPU driver usually fixes it.";
        } else if (pref == "cpu") {
            card->help = L"The CPU encoder works on any PC but takes several cores while recording, which can lower "
                         L"your game's FPS. Pick a GPU if one works.";
        } else {
            card->help = L"Auto records on the GPU that drives the display, and on NVIDIA while a game runs there: it "
                         L"encodes better and is awake anyway. Switching rebuilds the pipeline; a clip can't span the switch.";
        }

        // How the staged GPU handles the codec picked on Replay & Quality (its one home): a row only
        // when there's a caveat. The engine falls back to H.264 on a GPU without AV1.
        const bool av1 = str_of("encode.video_codec", "av1") != "h264";
        // A Radeon RX 7000 / 780M codes AV1 only at multiples of 64x16; at any other recorded size
        // the user picks what happens (encode.amf_av1_unaligned), and the help names the result.
        const ombra::VideoSize wanted = preview_source_w_ > 0 && preview_source_h_ > 0
                                            ? ombra::ResolveOutputSize(preview_source_w_, preview_source_h_,
                                                                       str_of("capture.resolution", "native"))
                                            : ombra::VideoSize{};
        const bool unaligned = av1 && staged_gpu && staged_gpu->av1 && wanted.width > 0 &&
                               (wanted.width % staged_gpu->av1_align_w || wanted.height % staged_gpu->av1_align_h);
        auto size_text = [](ombra::VideoSize sz) { return std::to_wstring(sz.width) + L"×" + std::to_wstring(sz.height); };
        if (av1 && ((staged_gpu && staged_gpu->vendor == "cpu") || recording_on_cpu_)) {
            auto row = row_of(L"AV1", L"The CPU encoder records H.264: AV1 on the CPU is too slow to record live.", {});
            row->help_tone = SettingRow::Tone::Warning;
            card->rows.push_back(row);
        } else if (av1 && staged_gpu && !staged_gpu->av1) {
            auto row = row_of(L"AV1", L"This GPU can't encode AV1, so it records H.264.", {});
            row->help_tone = SettingRow::Tone::Warning;
            card->rows.push_back(row);
        } else if (const FieldSpec* mode_spec = FindSpecIn(specs_, "encode.amf_av1_unaligned"); unaligned && mode_spec) {
            const std::string mode = str_of("encode.amf_av1_unaligned", "pad");
            const ombra::EncodePlan plan = ombra::PlanEncode(wanted, ombra::VideoCodec::AV1, staged_gpu->av1_align_w,
                                                             staged_gpu->av1_align_h, mode);
            std::wstring help = L"This GPU codes AV1 exactly only at multiples of " + std::to_wstring(staged_gpu->av1_align_w) +
                                L"×" + std::to_wstring(staged_gpu->av1_align_h) + L". ";
            if (plan.codec == ombra::VideoCodec::H264) {
                help += L"At " + size_text(wanted) + L" it records H.264.";
            } else if (mode == "pad") {
                const ombra::VideoSize padded = ombra::PredictPaddedSize(wanted, staged_gpu->av1_align_w, staged_gpu->av1_align_h);
                // The padding decodes black (gpu_encode_tests, 780M).
                std::wstring edges;
                if (padded.height > wanted.height)
                    edges = std::to_wstring(padded.height - wanted.height) + L" black lines along the bottom";
                if (padded.width > wanted.width)
                    edges += (edges.empty() ? L"" : L" and ") + std::to_wstring(padded.width - wanted.width) +
                             L" black columns on the right";
                help += L"Pad records AV1 at " + size_text(padded) + L", with " + edges + L".";
            } else {
                help += L"Scale records AV1 at " + size_text(plan.size) + L", squeezed slightly from " + size_text(wanted) + L".";
            }
            card->rows.push_back(row_of((L"AV1 at " + size_text(wanted)).c_str(), help,
                                        {MakeSegmented(*mode_spec, [key = mode_spec->key](const std::string& v) {
                                            return OptionLabelFor(key, v);
                                        })}));
        } else if (av1 && codec_now_ == "h264" && staged_gpu && staged_gpu->name == recording_gpu_now_) {
            auto row = row_of(L"AV1", L"Recording H.264 right now: this GPU didn't open AV1. The log says why.", {});
            row->help_tone = SettingRow::Tone::Warning;
            card->rows.push_back(row);
        }
        sec.AddRow(card);
    }

    // --- Encoding: rate control with the one number that belongs to it, then preset and keyframes.
    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Encoding";
        if (const FieldSpec* spec = FindSpecIn(specs_, "encode.rate_control")) {
            const std::string rc = str_of(spec->key.c_str(), "vbr");
            auto seg = MakeSegmented(*spec, [](const std::string& v) {
                return v == "cqp" ? std::wstring(L"Quality") : v == "cbr" ? std::wstring(L"CBR") : std::wstring(L"VBR");
            });
            std::vector<std::shared_ptr<Widget>> controls{seg};
            std::wstring help;
            if (rc == "cqp") {
                if (const FieldSpec* cq = FindSpecIn(specs_, "encode.cq")) {
                    auto st = stepper_for(*cq, L"CQ");
                    controls.insert(controls.begin(), st);
                }
                help = L"Holds a quality level instead of a bitrate. Lower CQ looks better and makes bigger files, so "
                       L"how many seconds fit in the buffer varies.";
            } else if (rc == "cbr") {
                help = L"Bitrate stays at the target all the time: predictable size, a little softer in busy scenes.";
            } else {
                if (const FieldSpec* mx = FindSpecIn(specs_, "encode.max_bitrate_kbps")) {
                    // 0 means 1.25x the target bitrate (config.h); + from Auto starts at that number.
                    auto st = stepper_for(*mx, L"Mbps", 1000.0);
                    st->min_label = L"Auto";
                    st->first_step_above_min = (std::max)(1.0, std::round(num_of("encode.bitrate_kbps") * 1.25 / 1000.0));
                    controls.insert(controls.begin(), st);
                }
                help = L"Bitrate follows the scene and peaks at the number beside it. Auto is 1.25× the target.";
            }
            card->rows.push_back(row_of(L"Rate control", help, std::move(controls)));
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "encode.preset")) {
            auto sel = MakeSelect(*spec, [key = spec->key](const std::string& v) { return OptionLabelFor(key, v); });
            sel->width_dip = 184.0f;
            std::vector<std::shared_ptr<Widget>> controls{sel};
            // Auto-Tune measures this encoder and can only recommend a faster preset, so it sits
            // beside the preset it changes.
            if (const FieldSpec* tune = FindSpecIn(specs_, "engine.autotune"); tune && tune->implemented) {
                auto btn = std::make_shared<ButtonControl>();
                btn->id = tune->key;
                btn->label = L"Auto-Tune";
                btn->in_row = true;
                btn->on_click = [this, t = *tune]() { ConfirmAutotune(t); };
                controls_by_key_[tune->key] = btn;
                controls.insert(controls.begin(), btn);
            }
            card->rows.push_back(row_of(L"Preset",
                                        amf ? L"On AMD it has three steps: P1-P3 speed, P4-P5 balanced, P6-P7 quality."
                                            : L"Slower looks better and costs more GPU time.",
                                        std::move(controls)));
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "encode.keyframe_sec")) {
            card->rows.push_back(row_of(L"Keyframes", L"Shorter gives finer save points for 10-15% more bitrate.",
                                        {stepper_for(*spec, L"s")}));
        }
        sec.AddRow(card);
    }
    groups_.push_back(std::move(sec));
}

// Auto-Tune runs on confirm, not on Save: it isn't a value, it's a one-shot benchmark.
void SettingsScreen::ConfirmAutotune(const FieldSpec& spec) {
    if (!host_) return;
    host_->ShowModal(Utf8ToWide(spec.display_name),
        L"This closes this panel and draws a small window on the desktop for a few "
        L"seconds to benchmark latency. Make sure no games are currently fullscreen "
        L"in the foreground. Results appear here the next time you open Settings.",
        true, [this, spec]() {
        // The modal text above promises this panel closes -- it never actually did.
        // capturebench.cpp's own SetForegroundWindow() call needs the overlay to
        // relinquish the Windows foreground-lock first (a freshly spawned, unrelated
        // process generally can't just steal focus from a window that's still open
        // and topmost); without this, the benchmark silently aborted on its very
        // first frame (aborted_focus, frames=0)
        // with nothing visible on screen at all. CloseOverlay() also restores
        // prev_foreground_ (Hide(), overlay_window.cpp), which is what actually frees
        // the foreground slot capturebench's window needs.
        if (host_) host_->CloseOverlay();
        ApplySettingChange(spec, ombra::JsonValue::MakeStr("run"));
    });
}

// Advanced -> Diagnostics: what Ombra records about itself, and where to find it.
void SettingsScreen::BuildAdvancedSystemPage() {
    GroupSection sec;
    sec.name = L"Diagnostics";
    auto row_of = [](const wchar_t* title, std::wstring help, std::vector<std::shared_ptr<Widget>> controls) {
        auto row = std::make_shared<SettingRow>();
        row->title = title;
        row->help = std::move(help);
        row->controls = std::move(controls);
        return row;
    };
    auto switch_for = [this](const FieldSpec& spec, bool fallback) {
        auto sw = std::make_shared<SwitchControl>();
        sw->id = spec.key;
        const auto* v = EffectiveConfigValue(spec.key);
        sw->on = (v && v->type == ombra::JsonValue::Type::Bool) ? v->b : fallback;  // config.h's default
        sw->on_change = [this, s = spec](bool on) { ApplySettingChange(s, ombra::JsonValue::MakeBool(on)); };
        controls_by_key_[spec.key] = sw;
        return sw;
    };

    // Set once, usually by the first-run setup.
    if (const FieldSpec* spec = FindSpecIn(specs_, "engine.autostart"); spec && spec->implemented) {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Startup";
        card->rows.push_back(row_of(L"Start with Windows",
                                    L"" OMBRA_PRODUCT_NAME " starts in the background when you sign in, so the replay buffer is ready before your first game.",
                                    {switch_for(*spec, false)}));
        sec.AddRow(card);
    }

    // Updates: what this copy is, what the engine last found, and the two switches.
    {
        system_page_built_ = true;
        if (update_.state.empty() && !update_seeded_) RequestUpdateStatus();
        auto card = std::make_shared<CardContainer>();
        card->title = L"Updates";
        const auto* auto_v = EffectiveConfigValue("updates.auto_install");
        const bool auto_install = !(auto_v && auto_v->type == ombra::JsonValue::Type::Bool) || auto_v->b;
        bool is_error = false;
        const std::wstring line = UpdateStatusLine(update_, auto_install, static_cast<int64_t>(std::time(nullptr)), &is_error);
        std::vector<std::shared_ptr<Widget>> buttons;
        const auto button = [&](const wchar_t* label, std::function<void()> click, bool enabled = true) {
            auto b = std::make_shared<ButtonControl>();
            b->label = label;
            b->in_row = true;
            b->enabled = enabled;
            b->on_click = std::move(click);
            buttons.push_back(b);
        };
        const std::string& st = update_.state;
        const bool busy = st == "checking" || st == "downloading" || st == "installing";
        if (st == "ready") {
            button(L"Install now", [this]() { SendUpdateCommand("update_install"); });
        } else if (st == "available" && !update_.page_url.empty()) {
            button(L"Release notes", [this, url = Utf8ToWide(update_.page_url)]() {
                if (host_) host_->CloseOverlay();  // the browser takes the foreground
                if (!NativeWindowBlocked(L"the browser at " + url))
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            });
        }
        if (st != "ready" && st != "off")
            button(busy ? L"Checking\u2026" : L"Check now", [this]() { SendUpdateCommand("update_check"); },
                   !busy && !st.empty() && st != "unreachable");
        const std::wstring version = update_.current.empty() ? std::wstring(L"" OMBRA_PRODUCT_NAME)
                                                             : L"" OMBRA_PRODUCT_NAME " " + Utf8ToWide(update_.current);
        auto status = row_of(version.c_str(), is_error ? L"" : line, std::move(buttons));
        if (is_error) {
            status->note = line;
            status->note_tone = SettingRow::Tone::Error;
        }
        card->rows.push_back(status);
        if (const FieldSpec* spec = FindSpecIn(specs_, "updates.check")) {
            card->rows.push_back(row_of(L"Check for updates",
                                        L"Looks for a new release on GitHub twice a day.",
                                        {switch_for(*spec, true)}));
        }
        // Scoop and portable copies are updated outside Ombra, so there is nothing to install.
        if (const FieldSpec* spec = FindSpecIn(specs_, "updates.auto_install");
            spec && (update_.kind.empty() || update_.kind == "installer")) {
            card->rows.push_back(row_of(L"Install automatically",
                                        L"While you're away from the PC and no game is running. Installing restarts "
                                        L"" OMBRA_PRODUCT_NAME ", which empties the replay buffer.",
                                        {switch_for(*spec, true)}));
        }
        sec.AddRow(card);
    }

    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Setup";
        auto again = std::make_shared<ButtonControl>();
        again->label = L"Run setup again";
        again->in_row = true;
        again->on_click = [this]() { if (host_) host_->SwitchScreen(ActiveScreen::Setup); };
        card->rows.push_back(row_of(L"First-run setup", L"Go through the recommendations for this PC again.", {again}));
        sec.AddRow(card);
    }

    // What leaves the PC: three level tiles, the chosen one's words under them.
    if (const FieldSpec* spec = FindSpecIn(specs_, "diagnostics.sharing")) {
        auto share = std::make_shared<CardContainer>();
        share->title = L"Share with the developer";
        auto detail = std::make_shared<LevelDetail>();
        detail->stops = SharingStops();
        detail->tiles->id = spec->key;
        detail->tiles->options = SharingLabels();
        const auto* v = EffectiveConfigValue(spec->key);
        detail->tiles->selected = SharingIndex(v && v->type == ombra::JsonValue::Type::String ? v->str : "");
        detail->tiles->on_change = [this, s = *spec](int i) {
            ApplySettingChange(s, ombra::JsonValue::MakeStr(kSharingKeys[i]));
            if (host_) host_->RequestRepaint();
        };
        controls_by_key_[spec->key] = detail->tiles;
        share->rows.push_back(detail);
        share->rows.push_back(BuildSentList());
        sec.AddRow(share);
    }

    auto card = std::make_shared<CardContainer>();
    card->title = L"Troubleshooting";
    {
        const bool sending = send_state_ == SendState::Busy;
        std::vector<std::shared_ptr<Widget>> buttons;
        // A build without a key can't send; an isolated instance (tests, --drive) shows it anyway
        // and its child answers "no key" unless a local test key is set.
        if (*ombra::SentryDsn() || ombra::Instance().isolated) {
            auto send = std::make_shared<ButtonControl>();
            if (send_state_ == SendState::Sent) {
                send->id = "bug_report";
                send->label = L"Open GitHub issue";
                send->on_click = [this]() { OpenBugReport(); };
            } else {
                send->id = "send_logs";
                send->label = sending ? L"Sending…" : L"Send to developer";
                send->enabled = !sending && !logs_busy_ && send_state_ != SendState::StillSending;
                send->on_click = [this]() { SendLogs(); };
            }
            send->in_row = true;
            buttons.push_back(send);
        }
        auto get = std::make_shared<ButtonControl>();
        get->id = "get_logs";
        get->label = logs_busy_ ? L"Saving…" : L"Save zip";
        get->in_row = true;
        get->enabled = !logs_busy_ && !sending;
        get->on_click = [this]() { ExportLogs(); };
        buttons.push_back(get);
        auto row = row_of(L"Logs for a bug report",
                          L"3 days of logs and tracking files, crash reports, settings and system info, user name and "
                          L"paths removed.",
                          buttons);
        const std::wstring ref = Utf8ToWide(send_id_.substr(0, 8));
        switch (send_state_) {
            case SendState::Sent:
                row->note = L"Sent as " + ref + L". Open an issue to say what went wrong.";
                row->note_tone = SettingRow::Tone::Good;
                break;
            case SendState::StillSending:
                row->note = L"Still sending " + ref + L". It shows under What was sent when done.";
                break;
            case SendState::Failed:
                row->note = L"Couldn't send. Save the zip for a GitHub issue instead.";
                row->note_tone = SettingRow::Tone::Error;
                break;
            case SendState::NoKey:
                row->note = L"This copy can't send. Save the zip for a GitHub issue.";
                row->note_tone = SettingRow::Tone::Error;
                break;
            case SendState::Unreadable:
                row->note = L"Couldn't collect logs. Save the zip for a GitHub issue.";
                row->note_tone = SettingRow::Tone::Error;
                break;
            case SendState::TooMany:
                row->note = L"Sent 5 times today. Save the zip for a GitHub issue.";
                row->note_tone = SettingRow::Tone::Error;
                break;
            default: break;
        }
        if (!logs_status_.empty()) {
            row->note = logs_status_;
            row->note_tone = SettingRow::Tone::Error;
        }
        card->rows.push_back(row);
    }
    if (const FieldSpec* spec = FindSpecIn(specs_, "log.level")) {
        auto sw = std::make_shared<SwitchControl>();
        sw->id = spec->key;
        const auto* v = EffectiveConfigValue(spec->key);
        sw->on = v && v->type == ombra::JsonValue::Type::String && v->str == "debug";
        sw->on_change = [this, s = *spec](bool on) { ApplySettingChange(s, ombra::JsonValue::MakeStr(on ? "debug" : "info")); };
        controls_by_key_[spec->key] = sw;
        card->rows.push_back(row_of(L"Detailed logging",
                                    L"For a problem that's hard to pin down. Leave it off unless asked.",
                                    {sw}));
    }
    if (const FieldSpec* spec = FindSpecIn(specs_, "engine.telemetry")) {
        card->rows.push_back(row_of(L"Resource tracking",
                                    L"" OMBRA_PRODUCT_NAME "'s memory, CPU, GPU and frame pacing every 30 s, kept on this PC (up to 50 MB).",
                                    {switch_for(*spec, true)}));
    }
    auto open_logs = std::make_shared<ButtonControl>();
    open_logs->label = L"Open folder";
    open_logs->in_row = true;
    open_logs->on_click = [this]() {
        // Explorer takes the foreground; close the overlay first so it doesn't sit on top of it.
        const std::wstring dir = ombra::Instance().data_dir;
        if (host_) host_->CloseOverlay();
        if (!NativeWindowBlocked(L"Explorer at " + dir)) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    card->rows.push_back(row_of(L"Logs and tracking files", L"Unscrubbed, to read yourself.",
                                {open_logs}));
    sec.AddRow(card);
    groups_.push_back(std::move(sec));
}

void SettingsScreen::ExportLogs() {
    if (logs_busy_ || !host_) return;
    logs_busy_ = true;
    logs_status_.clear();
    RebuildGroupRows(current_dpi_scale_);
    host_->RequestRepaint();
    host_->RunAsync([this, post = host_->UiPoster()] {
        const std::wstring path = ombra::DefaultBundlePath();
        ombra::BundleResult r;
        if (path.empty()) r.error = "no Downloads folder";
        else r = ombra::WriteDiagnosticsBundle(path);
        if (r.ok) ombra::Log::Info("diagnostics bundle  %zu files, %.1f MB written (overlay)", r.files, r.bytes / (1024.0 * 1024.0));
        else ombra::Log::Error("diagnostics bundle  not written: %s", r.error.c_str());
        post([this, path, r] {
            logs_busy_ = false;
            logs_status_ = r.ok ? L"" : L"Couldn't save the logs: " + Utf8ToWide(r.error);
            if (r.ok) {
                // Explorer takes the foreground; the overlay goes first, as Open folder does.
                if (host_) host_->CloseOverlay();
                if (!NativeWindowBlocked(L"Explorer selecting " + path)) {
                    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
                        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                        ILFree(pidl);
                    }
                }
                return;
            }
            RebuildGroupRows(current_dpi_scale_);
            if (host_) host_->RequestRepaint();
        });
    });
}

void SettingsScreen::SendLogs() {
    if (send_state_ == SendState::Busy || logs_busy_ || !host_) return;
    send_state_ = SendState::Busy;
    send_id_ = ombra::crash::NewReportId();
    logs_status_.clear();
    RebuildGroupRows(current_dpi_scale_);
    host_->RequestRepaint();
    host_->RunAsync([this, id = send_id_, post = host_->UiPoster()] {
        const std::wstring zip = ombra::Instance().data_dir + L"\\" + ombra::crash::kSendLogsZipPrefix +
                                 Utf8ToWide(id.substr(0, 8)) + L".zip";
        SendState result = SendState::Unreadable;
        const ombra::BundleResult b = ombra::WriteDiagnosticsBundle(zip);
        if (!b.ok) {
            ombra::Log::Error("send logs  bundle not written: %s", b.error.c_str());
            DeleteFileW(zip.c_str());
        } else {
            wchar_t self[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring exe(self, n);
            exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"" OMBRA_ENGINE_EXE;
            std::wstring cmd = L"\"" + exe + L"\" send-logs \"" + zip + L"\" " + Utf8ToWide(id);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            // Not a child of this process's lifetime: it posts and deletes the zip if the overlay closes.
            if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                nullptr, nullptr, &si, &pi)) {
                ombra::Log::Error("send logs  could not start %ls (%lu)", exe.c_str(), GetLastError());
                DeleteFileW(zip.c_str());
                result = SendState::Failed;
            } else {
                CloseHandle(pi.hThread);
                // The child's own post gives up well within this (crash_upload.cpp timeouts).
                if (WaitForSingleObject(pi.hProcess, 10 * 60 * 1000) != WAIT_OBJECT_0) {
                    result = SendState::StillSending;
                } else {
                    DWORD code = 1;
                    GetExitCodeProcess(pi.hProcess, &code);
                    using R = ombra::crash_upload::SendLogsResult;
                    switch (static_cast<R>(code)) {
                        case R::Sent:       result = SendState::Sent; break;
                        case R::NoDsn:      result = SendState::NoKey; break;
                        case R::Unreadable: result = SendState::Unreadable; break;
                        case R::TooMany:    result = SendState::TooMany; break;
                        default:            result = SendState::Failed; break;
                    }
                }
                CloseHandle(pi.hProcess);
            }
        }
        post([this, id, result] {
            if (id != send_id_) return;
            send_state_ = result;
            ReloadSentRecords();
            RebuildGroupRows(current_dpi_scale_);
            if (host_) host_->RequestRepaint();
        });
    });
}

void SettingsScreen::OpenBugReport() {
    if (!host_) return;
    const std::wstring url = Utf8ToWide(ombra::BugReportUrl(ombra::MachineInfo(), send_id_));
    // The browser takes the foreground; the overlay goes first, as Open folder does.
    host_->CloseOverlay();
    if (!NativeWindowBlocked(L"the browser at " + url)) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int SharingIndex(const std::string& key) {
    for (int i = 0; i < 3; ++i)
        if (key == kSharingKeys[i]) return i;
    return 1;
}

std::vector<std::wstring> SharingLabels() { return {L"Off", L"Error reports", L"+ Usage"}; }

std::vector<LevelDetail::Stop> SharingStops() {
    // Short on purpose. The Error reports line must keep the dump caveat
    // because a minidump can't be scrubbed.
    return {
        {L"Nothing is sent.", L"Logs stay on this PC for a bug report you send yourself.", L"Nothing leaves this PC."},
        {L"Crashes and failed recordings are sent.",
         L"With your GPU, settings and recent log lines, paths and window titles removed. A crash snapshot may "
         L"still hold one. No ID.",
         L"Crashes and failed recordings."},
        {L"Also how " OMBRA_PRODUCT_NAME L" runs on this PC.",
         L"Daily performance numbers and settings, and per game: its exe name, how long you played and how it was "
         L"recorded. With a random ID for this install.",
         L"Also daily performance, settings and the games recorded."},
    };
}

namespace {

// "Not sent: ..." in words; the raw result when it's none of the usual ones.
std::wstring FailedReason(const std::string& result) {
    if (result.rfind("WinHTTP error ", 0) == 0) {
        const int code = std::atoi(result.c_str() + 14);
        if (code == 12002) return L"timed out";
        if (code == 12007 || code == 12029 || code == 12030 || code == 12031) return L"no connection";
    }
    if (result.rfind("HTTP ", 0) == 0) {
        const int code = std::atoi(result.c_str() + 5);
        if (code == 429) return L"too many reports, tried again later";
        if (code >= 500) return L"server error (" + Utf8ToWide(result) + L")";
        if (code >= 400) return L"refused (" + Utf8ToWide(result) + L")";
    }
    return Utf8ToWide(result);
}

std::wstring SizeText(size_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" bytes";
    return std::to_wstring((bytes + 512) / 1024) + L" KB";
}

std::string ReadWhole(const std::wstring& path) {
    std::string text;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0) {
        text.resize(static_cast<size_t>((std::min)(size.QuadPart, LONGLONG{8} * 1024 * 1024)));
        DWORD got = 0;
        if (!ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(f);
    return text;
}

// The record as Notepad would show it: UTF-8 to wide, CRLF line ends.
std::wstring RecordText(const std::string& utf8) {
    std::wstring out;
    for (wchar_t ch : Utf8ToWide(utf8)) {
        if (ch == L'\r') continue;
        if (ch == L'\n') out += L'\r';
        out += ch;
    }
    return out;
}

}  // namespace

SentList::Item SentItem(const ombra::sent_records::Record& r) {
    using ombra::sent_records::Kind;
    using ombra::sent_records::Status;
    SentList::Item it;
    std::wstring summary = Utf8ToWide(r.summary);
    const std::wstring day = Utf8ToWide(r.day);
    switch (r.kind) {
        case Kind::Crash:        it.icon = IconType::Bug;      it.title = L"Crash report"; break;
        case Kind::Error:        it.icon = IconType::Alert;    it.title = L"Error report"; break;
        case Kind::Session:
            it.icon = IconType::Check;
            it.title = L"Daily health";
            if (!day.empty() && !summary.empty()) summary = day + L": " + static_cast<wchar_t>(std::towlower(summary[0])) + summary.substr(1);
            break;
        case Kind::GameSessions: it.icon = IconType::Gamepad2; it.title = L"Game sessions"; break;
        case Kind::DailyUsage:
            it.icon = IconType::Gauge;
            it.title = L"Daily usage";
            if (!day.empty()) summary = L"Settings and performance for " + day;
            break;
        case Kind::Logs:
            it.icon = IconType::Folder;
            it.title = L"Logs you sent";
            if (summary.rfind(L"Logs you sent: ", 0) == 0) summary = L"A zip of " + summary.substr(15);  // "14 files, 2.3 MB"
            break;
        default: it.title = L"Report"; break;
    }
    it.summary = summary;
    it.when = FormatWhen(r.written);
    switch (r.status) {
        case Status::Sent:        it.status = L"Sent"; it.tone = SettingRow::Tone::Good; break;
        case Status::Unconfirmed: it.status = L"Not confirmed"; break;
        case Status::Failed:      it.status = L"Not sent: " + FailedReason(r.result); it.tone = SettingRow::Tone::Warning; break;
    }
    return it;
}

std::wstring SentPreview(const std::string& text, size_t max_lines, size_t max_bytes, bool* cut) {
    size_t end = 0, lines = 0;
    while (end < text.size() && end < max_bytes && lines < max_lines) {
        const size_t nl = text.find('\n', end);
        if (nl == std::string::npos) { end = text.size(); break; }
        end = nl + 1;
        ++lines;
    }
    if (end > max_bytes) {  // one long line (an event's JSON): cut it, on a character boundary
        end = max_bytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    }
    std::string head = text.substr(0, end);
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) head.pop_back();
    if (cut) *cut = end < text.size() && text.find_first_not_of("\r\n", end) != std::string::npos;
    std::wstring out;
    for (wchar_t ch : Utf8ToWide(head))
        if (ch != L'\r') out += ch;
    return out;
}

std::shared_ptr<SentList> SettingsScreen::BuildSentList() {
    if (!sent_loaded_) {
        sent_ = ombra::sent_records::List();
        sent_loaded_ = true;
        sent_open_.clear();
        sent_open_text_.clear();
        sent_scroll_ = 0.0f;
    }
    auto list = std::make_shared<SentList>();
    list->heading = L"Sent from this PC";
    list->help = L"Each report is written here before it leaves this PC. The last " +
                 std::to_wstring(ombra::sent_records::kKeep) + L" are kept.";
    list->empty_text = L"Nothing has been sent from this PC.";
    for (size_t i = 0; i < sent_.size(); ++i) {
        list->items.push_back(SentItem(sent_[i]));
        if (!sent_open_.empty() && sent_[i].file_name == sent_open_) list->open = static_cast<int>(i);
    }
    list->folder->label = L"Open folder";
    list->folder->icon = IconType::Folder;
    list->folder->in_row = true;
    list->folder->on_click = [this]() {
        const std::wstring dir = ombra::sent_records::Dir();
        if (host_) host_->CloseOverlay();  // Explorer takes the foreground
        if (!NativeWindowBlocked(L"Explorer at " + dir)) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    if (list->open >= 0) {
        const auto& r = sent_[static_cast<size_t>(list->open)];
        list->open_text = sent_open_text_;
        list->text_scroll = sent_scroll_;
        if (sent_copied_) {
            list->open_note = L"Copied the whole record.";
        } else {
            list->open_note = SizeText(r.bytes) + (sent_open_cut_ ? L"; the first lines." : L".");
            if (r.kind == ombra::sent_records::Kind::Crash) list->open_note += L" The minidump itself isn't in it.";
        }
    }
    list->copy->label = L"Copy";
    list->copy->icon = IconType::Copy;
    list->copy->in_row = true;
    list->copy->on_click = [this]() { CopySentRecord(); };
    list->notepad->label = L"Open in Notepad";
    list->notepad->in_row = true;
    list->notepad->on_click = [this]() { OpenSentRecord(); };
    list->on_toggle = [this](int i) { ToggleSentRecord(i); };
    list->on_scroll = [this](float y) { sent_scroll_ = y; };
    sent_list_ = list;
    return list;
}

void SettingsScreen::ToggleSentRecord(int index) {
    if (index < 0 || index >= static_cast<int>(sent_.size())) return;
    const auto& r = sent_[static_cast<size_t>(index)];
    sent_copied_ = false;
    sent_scroll_ = 0.0f;
    if (sent_open_ == r.file_name) {
        sent_open_.clear();
        sent_open_text_.clear();
    } else {
        sent_open_ = r.file_name;
        // The start only: wrapping a 100 KB record on every layout is the expensive path, and
        // Notepad has the whole file.
        sent_open_text_ = SentPreview(ReadWhole(r.path), 40, 6 * 1024, &sent_open_cut_);
    }
    RebuildGroupRows(current_dpi_scale_);
    if (host_) host_->RequestRepaint();
}

void SettingsScreen::CopySentRecord() {
    const auto it = std::find_if(sent_.begin(), sent_.end(), [&](const auto& r) { return r.file_name == sent_open_; });
    if (it == sent_.end()) return;
    bool ok = NativeWindowBlocked(L"clipboard: " + it->path);
    if (!ok) {
        const std::wstring text = RecordText(ReadWhole(it->path));
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
            memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (OpenClipboard(host_ ? host_->OwnerWindow() : nullptr)) {
                EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, mem)) {
                    mem = nullptr;  // the clipboard owns it now
                    ok = true;
                }
                CloseClipboard();
            }
            if (mem) GlobalFree(mem);
        }
    }
    sent_copied_ = ok;
    RebuildGroupRows(current_dpi_scale_);
    if (host_) host_->RequestRepaint();
}

void SettingsScreen::OpenSentRecord() {
    const auto it = std::find_if(sent_.begin(), sent_.end(), [&](const auto& r) { return r.file_name == sent_open_; });
    if (it == sent_.end()) return;
    const std::wstring path = it->path;
    if (host_) host_->CloseOverlay();  // Notepad takes the foreground
    if (!NativeWindowBlocked(L"Notepad with " + path))
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

namespace {

struct ShortcutDef {
    const char*               key;
    std::string ombra::Config::* field;  // for its default
    const wchar_t*            title;
    const wchar_t*            detail;  // save_replay's is built from buffer.seconds instead
};
constexpr ShortcutDef kShortcutDefs[] = {
    {"hotkeys.save_replay", &ombra::Config::hotkeys_save_replay, L"Save replay", nullptr},
    {"hotkeys.screenshot", &ombra::Config::hotkeys_screenshot, L"Screenshot", L"Saves the current frame as a PNG"},
    {"hotkeys.toggle_buffer", &ombra::Config::hotkeys_toggle_buffer, L"Buffer on/off",
     L"Pauses or resumes the replay buffer"},
    {"hotkeys.overlay", &ombra::Config::hotkeys_overlay, L"Open overlay", L"Opens and closes this overlay"},
};

const wchar_t* ShortcutTitle(const std::string& key) {
    for (const auto& d : kShortcutDefs) {
        if (key == d.key) return d.title;
    }
    return L"another shortcut";
}

const wchar_t* BackendDisplayName(const std::string& backend) {
    if (backend == "register_hotkey") return L"RegisterHotKey";
    if (backend == "ll_hook") return L"keyboard hook";
    if (backend == "poll") return L"polling";
    return L"unknown";
}

std::wstring ClipLengthText(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", total / 60, total % 60);
    return buf;
}

constexpr const char* kBackendValues[] = {"auto", "register_hotkey", "ll_hook", "poll"};

}  // namespace

void SettingsScreen::ApplyHotkeyStatus(const ombra::JsonValue& status) {
    const auto* hk = status.Find("hotkeys");
    if (!hk || hk->type != ombra::JsonValue::Type::Object) return;  // an older engine
    const auto* susp = hk->Find("suspended");
    // Our own recording suspends them; that snapshot says nothing about what's registered.
    if (susp && susp->type == ombra::JsonValue::Type::Bool && susp->b) return;
    auto str = [](const ombra::JsonValue& obj, const char* key) {
        const auto* v = obj.Find(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string();
    };
    HotkeyStatusView view;
    view.valid = true;
    view.policy = str(*hk, "policy");
    if (const auto* list = hk->Find("bindings"); list && list->type == ombra::JsonValue::Type::Array) {
        for (const auto& b : list->arr) {
            if (b.type != ombra::JsonValue::Type::Object) continue;
            const auto* ok = b.Find("ok");
            view.bindings.push_back({str(b, "key"), str(b, "spec"), str(b, "backend"),
                                     ok && ok->type == ombra::JsonValue::Type::Bool && ok->b});
        }
    }
    hotkey_status_ = std::move(view);
}

void SettingsScreen::SeedHotkeyStatusForSnapshot(const char* backend, const char* failed_key) {
    hotkey_status_ = {};
    hotkey_status_.valid = true;
    const auto* policy = EffectiveConfigValue("hotkeys.backend");
    hotkey_status_.policy = (policy && policy->type == ombra::JsonValue::Type::String) ? policy->str : "poll";
    for (const auto& d : kShortcutDefs) {
        const auto* v = EffectiveConfigValue(d.key);
        const std::string spec = (v && v->type == ombra::JsonValue::Type::String) ? v->str : "";
        const bool failed = failed_key && std::string(failed_key) == d.key;
        hotkey_status_.bindings.push_back({d.key, spec, failed ? "" : backend, !failed});
    }
    rows_dirty_ = true;
}

void SettingsScreen::SeedShortcutRecordingForSnapshot(const char* key, UINT held_mods, const std::wstring& error) {
    // Picked up by BuildShortcutsPage, the same way a real recording survives a rebuild.
    recording_key_ = key;
    recording_restore_mods_ = held_mods;
    recording_restore_error_ = error;
    rows_dirty_ = true;
}

SettingsScreen::ShortcutRowView SettingsScreen::ShortcutRowForTest(const std::string& key) const {
    auto it = shortcut_rows_.find(key);
    if (it == shortcut_rows_.end()) return {};
    const ShortcutRowRefs& r = it->second;
    return {r.row.lock(), r.capture.lock(), r.reset.lock(), r.detail, r.default_spec};
}

void SettingsScreen::RefreshShortcutLine(const std::string& key) {
    auto it = shortcut_rows_.find(key);
    if (it == shortcut_rows_.end()) return;
    auto row = it->second.row.lock();
    auto capture = it->second.capture.lock();
    if (!row || !capture) return;
    const bool refusing = capture->Recording() && !capture->Refusal().empty();
    row->help = !capture->Recording() ? it->second.detail
              : refusing              ? capture->Refusal()
                                      : std::wstring(L"Press the new shortcut. Esc cancels.");
    row->help_tone = refusing ? SettingRow::Tone::Warning : SettingRow::Tone::Neutral;
}

void SettingsScreen::OnShortcutRecordingChanged(const std::shared_ptr<KeyCaptureControl>& capture, const std::string& key,
                                                bool recording) {
    if (recording) {
        // One row records at a time.
        if (auto prev = recording_row_.lock(); prev && prev != capture) prev->CancelRecording();
        recording_row_ = capture;
        recording_key_ = key;
    } else if (recording_key_ == key) {
        recording_row_.reset();
        recording_key_.clear();
    }
    RefreshShortcutLine(key);
    if (host_) {
        host_->SetHotkeysSuspended(!recording_key_.empty());
        host_->RequestRepaint();
    }
}

void SettingsScreen::OnShortcutKey(UINT vk, bool down) {
    if (auto capture = recording_row_.lock()) {
        const std::string key = recording_key_;
        capture->OnCaptureKey(vk, down);
        RefreshShortcutLine(key);
    }
}

void SettingsScreen::CancelShortcutRecording() {
    if (auto capture = recording_row_.lock()) capture->CancelRecording();
}

void SettingsScreen::BuildShortcutsPage() {
    GroupSection sec;
    sec.name = L"Shortcuts";
    shortcut_rows_.clear();
    static const ombra::Config kDefaults;

    auto effective_str = [this](const char* key) {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string();
    };

    bool all_shared = !hotkey_status_.bindings.empty();
    for (const auto& b : hotkey_status_.bindings) all_shared &= b.ok && b.backend == "ll_hook";

    auto card = std::make_shared<CardContainer>();
    card->title = L"Shortcuts";
    card->help = L"Click a shortcut, then press the new keys. Each needs Ctrl, Alt or Win, so it can't fire while you type.";
    for (const auto& d : kShortcutDefs) {
        const FieldSpec* spec = FindSpecIn(specs_, d.key);
        if (!spec) continue;
        const std::string key = d.key;
        ShortcutRowRefs refs;
        if (d.detail) {
            refs.detail = d.detail;
        } else {
            const auto* secs = EffectiveConfigValue("buffer.seconds");
            refs.detail = (secs && secs->type == ombra::JsonValue::Type::Number)
                              ? L"Saves the last " + ClipLengthText(secs->num) + L" as a clip"
                              : L"Saves the replay buffer as a clip";
        }
        const std::string spec_now = effective_str(d.key);
        refs.default_spec = Utf8ToWide(kDefaults.*d.field);

        auto row = std::make_shared<SettingRow>();
        row->title = d.title;
        row->help = refs.detail;
        row->help_one_line = true;

        // What the engine says about this combo, only while it's the one actually committed.
        const HotkeyBindingView* live = nullptr;
        for (const auto& b : hotkey_status_.bindings) {
            if (b.key == d.key && b.spec == spec_now) live = &b;
        }
        if (live && !live->ok) {
            row->note = hotkey_status_.policy == "register_hotkey"
                            ? L"Not working: another app holds this shortcut. Pick another, or use Automatic below."
                            : L"Not working: Windows refused it. The log has the details.";
            row->note_tone = SettingRow::Tone::Error;
        }
        // NVIDIA's and Game Bar's defaults overlap ours. A combo another app actually holds says
        // so, unless every one
        // does: then the method's status line ("via keyboard hook") already says it once.
        if (live && live->ok && live->backend == "ll_hook" && hotkey_status_.policy == "auto" && !all_shared) {
            row->tag = L"Shared";
            row->tag_tooltip = L"Another app registered this shortcut first. " OMBRA_PRODUCT_NAME " still hears it through a keyboard hook.";
        }

        auto capture = std::make_shared<KeyCaptureControl>();
        capture->id = key;
        capture->spec = Utf8ToWide(spec_now);
        capture->validate = [this, key](const std::wstring& candidate) -> std::wstring {
            ombra::HotkeyCombo want;
            if (!ombra::ParseHotkeySpec(WideToUtf8(candidate), want)) return L"" OMBRA_PRODUCT_NAME " can't bind that key.";
            for (const auto& other : kShortcutDefs) {
                if (key == other.key) continue;
                const auto* v = EffectiveConfigValue(other.key);
                ombra::HotkeyCombo theirs;
                if (v && v->type == ombra::JsonValue::Type::String && ombra::ParseHotkeySpec(v->str, theirs) &&
                    theirs == want) {
                    return std::wstring(L"already used by ") + other.title + L".";
                }
            }
            return L"";
        };
        const FieldSpec spec_copy = *spec;
        capture->on_commit = [this, spec_copy](const std::wstring& new_spec) {
            ApplySettingChange(spec_copy, ombra::JsonValue::MakeStr(WideToUtf8(new_spec)));
        };
        std::weak_ptr<KeyCaptureControl> weak = capture;
        capture->on_recording_changed = [this, weak, key](bool recording) {
            OnShortcutRecordingChanged(weak.lock(), key, recording);
        };

        // Back to the default: commits it, or records with the reason when another action has it.
        auto reset = std::make_shared<IconButtonControl>();
        reset->glyph = IconButtonControl::Glyph::Reset;
        reset->tooltip = L"Reset to " + refs.default_spec;
        reset->visible = !refs.default_spec.empty() && capture->spec != refs.default_spec;
        const std::wstring def = refs.default_spec;
        reset->on_click = [this, weak, def, key] {
            auto c = weak.lock();
            if (!c) return;
            const std::wstring why = c->validate ? c->validate(def) : L"";
            if (why.empty()) {
                if (c->on_commit) c->on_commit(def);
                return;
            }
            c->BeginRecording();
            c->Refuse(def + L": " + why);
            RefreshShortcutLine(key);
        };
        row->controls = {reset, capture};
        controls_by_key_[key] = capture;

        refs.row = row;
        refs.capture = capture;
        refs.reset = reset;
        shortcut_rows_[key] = refs;
        // A rebuild while this key was recording (an async status reply) keeps it recording.
        if (recording_key_ == key) {
            capture->SetRecordingStateForSnapshot(recording_restore_mods_, recording_restore_error_);
            recording_row_ = capture;
            recording_restored_ = true;
            RefreshShortcutLine(key);
        }
        card->rows.push_back(row);
    }
    sec.AddRow(card);

    auto trouble = std::make_shared<CardContainer>();
    trouble->title = L"Troubleshooting";
    trouble->help = L"Polling, the default, reads the keys itself, so it works in games that block the other methods; "
                    L"an app sharing a shortcut still reacts to it. Automatic registers each shortcut with Windows and "
                    L"falls back to a keyboard hook when another app already holds it.";
    auto method = std::make_shared<SettingRow>();
    method->title = L"Hotkey method";
    if (const FieldSpec* spec = FindSpecIn(specs_, "hotkeys.backend")) {
        auto seg = std::make_shared<SegmentedControl>();
        seg->id = spec->key;
        seg->options = {L"Automatic", L"RegisterHotKey", L"Keyboard hook", L"Polling"};
        const std::string now = effective_str("hotkeys.backend");
        for (int i = 0; i < 4; ++i) {
            if (now == kBackendValues[i]) seg->selected = i;
        }
        const FieldSpec spec_copy = *spec;
        seg->on_change = [this, spec_copy](int i) {
            if (i >= 0 && i < 4) ApplySettingChange(spec_copy, ombra::JsonValue::MakeStr(kBackendValues[i]));
        };
        method->controls = {seg};
        controls_by_key_[spec->key] = seg;
    }
    // What the engine reports is live, as the row's status line.
    if (!hotkey_status_.valid) {
        method->note_tone = SettingRow::Tone::Neutral;
        method->note = L"Can't reach the engine, so which method is live is unknown.";
    } else {
        std::wstring failed;
        std::map<std::string, int> by_backend;
        for (const auto& b : hotkey_status_.bindings) {
            if (!b.ok) failed += (failed.empty() ? L"" : L", ") + std::wstring(ShortcutTitle(b.key));
            else ++by_backend[b.backend];
        }
        if (!failed.empty()) {
            method->note_tone = SettingRow::Tone::Error;
            method->note = L"Not working: " + failed + L".";
        } else {
            method->note_tone = SettingRow::Tone::Good;
            std::wstring how;
            for (const auto& [backend, n] : by_backend) {
                how += (how.empty() ? L"" : L", ") + std::wstring(BackendDisplayName(backend));
                if (by_backend.size() > 1) how += L" (" + std::to_wstring(n) + L")";
            }
            method->note = L"All shortcuts working, via " + how + L".";
        }
    }
    trouble->rows = {method};
    sec.AddRow(trouble);
    groups_.push_back(std::move(sec));
}

constexpr int kCaptureTileColumns = 4;  // displays and windows alike

// The running game as a tile that only reports: its name and icon, or a placeholder. No live picture,
// which would be a second capture. The engine names a game by its Games-list name, sanitized.
std::shared_ptr<TileControl> SettingsScreen::MakeGameTile(bool recording_now) {
    auto tile = std::make_shared<TileControl>();
    tile->picture = true;
    tile->picture_glyph = IconType::Gamepad2;
    if (running_game_.empty()) {
        tile->title = L"No game running";
        tile->detail = L"Starts when a game opens";
        tile->dimmed = true;
        return tile;
    }
    tile->title = running_game_;
    tile->detail = recording_now ? L"Recording now" : L"Open now";
    if (!games_fetched_ && !games_pending_) FetchGamesAsync(false);
    std::wstring source;
    for (const auto& g : ombra::MergeGames(games_detected_, StagedGameDecisions())) {
        // Only for the icon, so a near match is enough: the characters and length the engine drops.
        std::wstring name;
        for (wchar_t c : g.name) if (c >= 0x20 && !wcschr(L"<>:\"/\\|?*", c)) name.push_back(c);
        if (name.compare(0, running_game_.size(), running_game_) != 0) continue;
        source = GameIconSource(g.icon, g.path);
        break;
    }
    if (source.empty() || !host_) return tile;
    if (source == running_game_icon_source_) {
        tile->picture_icon = running_game_icon_;
        return tile;
    }
    running_game_icon_source_ = source;
    running_game_icon_.reset();
    // Larger than a list row's icon: this one fills the picture.
    const UINT px = static_cast<UINT>(Sizes{}.control_sm() * 2 * current_dpi_scale_ + 0.5f);
    host_->RequestFileIconsAsync({source}, px, [this, source](std::vector<FileIcon> icons) {
        if (source != running_game_icon_source_ || icons.empty() || icons[0].bgra.empty()) return;
        running_game_icon_ = std::make_shared<const FileIcon>(std::move(icons[0]));
        RebuildGroupRows(current_dpi_scale_);
        if (host_) host_->RequestRepaint();
    });
    return tile;
}

// Capture > Video: a Recording card (what, when, and the cursor in it), then whatever the target
// needs: the game and displays, or the windows to pick from. Every value read here is the staged
// one; a claim about what is recording now needs the engine to be running those same settings.
void SettingsScreen::BuildCaptureVideoPage() {
    GroupSection sec;
    sec.name = L"Video";
    auto str_of = [&](const char* key, const char* fallback) -> std::string {
        const auto* v = EffectiveConfigValue(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string(fallback);
    };
    const std::string target = str_of("capture.target", "monitor");
    const bool only_in_games = str_of("capture.mode", "desktop") == "game";

    auto card = std::make_shared<CardContainer>();
    card->title = L"Recording";
    if (const FieldSpec* spec = FindSpecIn(specs_, "capture.target")) {
        const wchar_t* help =
            target == "window" ? L"One program's window, while it's open."
            : target == "game" ? (only_in_games ? L"The game's window, while a game from your Games list is open."
                                                : L"The game's window while one is open, the checked displays otherwise.")
                               : L"Each checked display, as its own file.";
        auto row = std::make_shared<SettingRow>();
        row->title = L"Record";
        row->help = help;
        row->controls = {MakeSegmented(*spec, [key = spec->key](const std::string& v) { return OptionLabelFor(key, v); })};
        card->rows.push_back(row);
    }
    if (const FieldSpec* spec = FindSpecIn(specs_, "capture.mode")) {
        auto row = std::make_shared<SettingRow>();
        row->title = L"When to record";
        row->help = target == "window" ? L"Doesn't apply while Capture records one window: that records while its program is open."
                    : !only_in_games   ? L"Always, or only while a game from your Games list is open."
                    : target == "game" ? L"Records only while a game from your Games list is open."
                                       : L"Records all of each checked display, but only while a game from your Games list is open.";
        row->controls = {MakeSegmented(*spec, [key = spec->key](const std::string& v) { return OptionLabelFor(key, v); })};
        card->rows.push_back(row);
    }
    if (const FieldSpec* spec = FindSpecIn(specs_, "capture.capture_cursor")) {
        const auto* v = EffectiveConfigValue(spec->key);
        auto sw = std::make_shared<SwitchControl>();
        sw->id = spec->key;
        sw->on = !v || v->type != ombra::JsonValue::Type::Bool || v->b;
        const FieldSpec s = *spec;
        sw->on_change = [this, s](bool on) { ApplySettingChange(s, ombra::JsonValue::MakeBool(on)); };
        controls_by_key_[spec->key] = sw;
        auto row = std::make_shared<SettingRow>();
        row->title = L"Mouse cursor";
        row->help = L"Shows the pointer in the recording.";
        row->controls = {sw};
        card->rows.push_back(row);
    }
    sec.AddRow(card);

    const bool target_running = run_enabled_ && run_target_ == target;
    if (target == "window") {
        sec.AddRow(BuildWindowCard());
    } else if (target == "game" && only_in_games) {
        // Nothing but the game records: no displays to pick.
        auto what = std::make_shared<CardContainer>();
        what->title = L"What gets recorded";
        auto sources = std::make_shared<RecordingSources>();
        sources->game = MakeGameTile(target_running);
        what->rows.push_back(sources);
        sec.AddRow(what);
    } else if (target == "game") {
        sec.AddRow(BuildDisplaysCard(MakeGameTile(target_running)));
    } else {
        // A status line, not a tile: the game isn't a source here, it only decides when.
        if (only_in_games && target_running && run_mode_ == "game") {
            auto strip = std::make_shared<StatusStrip>();
            strip->live = !running_game_.empty();
            strip->text = strip->live ? running_game_ + L" is open: recording your displays" : L"No game open: not recording";
            sec.AddRow(strip);
        }
        sec.AddRow(BuildDisplaysCard(nullptr));
    }
    groups_.push_back(std::move(sec));
}

std::shared_ptr<CardContainer> SettingsScreen::BuildDisplaysCard(std::shared_ptr<TileControl> game_tile) {
    auto card = std::make_shared<CardContainer>();
    card->title = game_tile ? L"What gets recorded" : L"Displays";
    const FieldSpec* spec_p = FindSpecIn(specs_, "capture.monitors");
    if (!spec_p) return card;
    const FieldSpec spec = *spec_p;

    // Which devices are checked, from the staged value. Reading current_config_ here reverted a
    // just-clicked tile on the rebuild that ApplySettingChange triggers, until Save.
    auto checked_from_config = [this](std::set<std::string>& checked) {
        const auto* arr = EffectiveConfigValue("capture.monitors");
        if (!arr || arr->type != ombra::JsonValue::Type::Array || arr->arr.empty()) return false;
        for (const auto& e : arr->arr) {
            if (e.type == ombra::JsonValue::Type::String) checked.insert(e.str);
        }
        return true;
    };

    // Fetched once per overlay open (monitor_list_fetched_, cleared in InitSchema) and off the UI
    // thread to avoid a visible stall picking a target. The callback re-runs RebuildGroupRows once
    // the list lands; an empty/failed reply never overwrites a non-empty monitor_rows_.
    if (!monitor_list_fetched_) {
        if (!monitor_list_pending_ && host_) {
            monitor_list_pending_ = true;
            host_->RequestMonitorListAsync([this, checked_from_config](std::vector<MonitorEntry> monitors) {
                monitor_list_pending_ = false;
                if (!monitors.empty()) {
                    // An absent/empty array matches ResolveCaptureMonitors' default (config.cpp):
                    // only the primary display.
                    std::set<std::string> checked_devices;
                    const bool have_config_list = checked_from_config(checked_devices);
                    std::vector<MonitorRow> new_rows;
                    for (const auto& m : monitors) {
                        MonitorRow mr;
                        mr.device = m.device;
                        mr.label = Utf8ToWide(m.label.empty() ? m.device : m.label);
                        if (m.width > 0 && m.height > 0) {
                            mr.resolution = std::to_wstring(m.width) + L" × " + std::to_wstring(m.height);
                        }
                        mr.primary = m.primary;
                        mr.left = m.left;
                        mr.top = m.top;
                        mr.width = m.width;
                        mr.height = m.height;
                        mr.positioned = m.positioned;
                        mr.checked = have_config_list
                            ? (checked_devices.count(mr.device) > 0 || (m.primary && checked_devices.count("primary") > 0))
                            : m.primary;
                        mr.hidden = hidden_monitor_devices_.count(mr.device) > 0;
                        new_rows.push_back(std::move(mr));
                    }
                    monitor_rows_ = std::move(new_rows);
                    monitor_list_fetched_ = true;
                    // Only rebuild on real data: an empty reply leaves monitor_list_fetched_ false so
                    // the next rebuild retries, and a host that resolves synchronously (tests) doesn't
                    // recurse through here.
                    RebuildGroupRows(current_dpi_scale_);
                    if (host_) host_->RequestRepaint();
                }
            });
        }
    } else {
        std::set<std::string> checked_devices;
        const bool have_config_list = checked_from_config(checked_devices);
        for (auto& mr : monitor_rows_) {
            if (have_config_list) {
                mr.checked = checked_devices.count(mr.device) > 0 ||
                             (mr.primary && checked_devices.count("primary") > 0);
            }
            mr.hidden = hidden_monitor_devices_.count(mr.device) > 0;
        }
    }

    if (monitor_rows_.empty()) {
        card->help = L"Loading displays…";
        return card;
    }
    card->help = game_tile ? L"Changing which displays are checked restarts " OMBRA_PRODUCT_NAME " and clears the replay buffer."
                           : L"Changing which are checked restarts " OMBRA_PRODUCT_NAME " and clears the replay buffer.";

    int visible_count = 0;
    for (const auto& mr : monitor_rows_) visible_count += mr.hidden ? 0 : 1;

    // One thumbnail fetch per overlay open (monitor_thumb_cache_), off the UI thread, all missing
    // devices in one call: ScreenHost runs them in sequence on one worker, so N calls here would
    // mean N concurrent WGC sessions on the user's GPU.
    for (const auto& mr : monitor_rows_) {
        if (!mr.hidden && !monitor_thumb_cache_.count(mr.device)) {
            MonitorThumbCache& entry = monitor_thumb_cache_[mr.device];
            DecodeThumbnailPng(ombra::MonitorThumbPath(mr.device), entry.w, entry.h, entry.bgra);
        }
    }
    if (host_) {
        std::vector<std::string> missing;
        for (const auto& mr : monitor_rows_) {
            if (mr.hidden) continue;
            if (!monitor_thumb_fresh_.count(mr.device) && !monitor_thumb_pending_.count(mr.device)) {
                missing.push_back(mr.device);
            }
        }
        if (!missing.empty()) {
            for (const auto& d : missing) monitor_thumb_pending_.insert(d);
            host_->RequestMonitorThumbnailsAsync(
                missing, [this](std::string device, bool ok, UINT w, UINT h, std::vector<uint8_t> bgra) {
                    monitor_thumb_pending_.erase(device);
                    // A failure keeps whatever the disk had and isn't retried this session, same
                    // reason as the window thumbnails.
                    monitor_thumb_fresh_.insert(device);
                    const bool got_image = ok && w > 0 && h > 0;
                    if (got_image) monitor_thumb_cache_[device] = MonitorThumbCache{w, h, std::move(bgra)};
                    if (got_image) {
                        RebuildGroupRows(current_dpi_scale_);
                        if (host_) host_->RequestRepaint();
                    }
                });
        }
    }

    auto diagram = std::make_shared<MonitorDiagram>();
    for (size_t i = 0; i < monitor_rows_.size(); ++i) {
        const MonitorRow& row = monitor_rows_[i];
        if (row.hidden) continue;  // shown instead as a "Show" row below

        auto screen = std::make_shared<ScreenControl>();
        screen->title = row.label;
        screen->detail = row.resolution;
        screen->selected = row.checked;
        if (row.width > 0 && row.height > 0) {
            screen->width = row.width;
            screen->height = row.height;
        }
        screen->left = row.left;
        screen->top = row.top;
        screen->positioned = row.positioned;
        if (auto it = monitor_thumb_cache_.find(row.device); it != monitor_thumb_cache_.end()) {
            screen->thumb_bgra = it->second.bgra;
            screen->thumb_w = it->second.w;
            screen->thumb_h = it->second.h;
        }

        std::weak_ptr<ScreenControl> screen_weak = screen;
        screen->on_change = [this, spec, i, screen_weak](bool checked) {
            if (i >= monitor_rows_.size()) return;
            if (!checked) {
                int checked_count = 0;
                for (const auto& mr : monitor_rows_) checked_count += mr.checked ? 1 : 0;
                if (checked_count <= 1) {
                    // Press already flipped `selected` -- put it back rather than stage an empty list.
                    if (auto sc = screen_weak.lock()) sc->selected = true;
                    if (host_) host_->RequestRepaint();
                    return;
                }
            }
            monitor_rows_[i].checked = checked;
            ombra::JsonValue arr = ombra::JsonValue::MakeArray();
            for (const auto& mr : monitor_rows_) {
                if (mr.checked) arr.arr.push_back(ombra::JsonValue::MakeStr(mr.device));
            }
            ApplySettingChange(spec, arr);
        };

        // "Hide from this list" is a display preference (hidden_monitor_devices_), separate from
        // the check; the last *visible* screen can't be hidden.
        if (visible_count > 1) {
            screen->on_hide = [this, i]() {
                if (i >= monitor_rows_.size()) return;
                SettingsScreen* self = this;  // the rebuild frees this closure
                self->hidden_monitor_devices_.insert(self->monitor_rows_[i].device);
                self->RebuildGroupRows(self->current_dpi_scale_);
                if (self->host_) self->host_->RequestRepaint();
            };
        }
        diagram->screens.push_back(screen);
    }
    if (game_tile) {
        auto sources = std::make_shared<RecordingSources>();
        sources->game = std::move(game_tile);
        sources->displays = diagram;
        sources->columns = kCaptureTileColumns;
        sources->game_caption = L"While a game is open";
        sources->displays_caption = L"While no game is open";
        card->rows.push_back(sources);
    } else if (!diagram->screens.empty()) {
        card->rows.push_back(diagram);
    }

    for (size_t i = 0; i < monitor_rows_.size(); ++i) {
        if (!monitor_rows_[i].hidden) continue;
        auto show = std::make_shared<ButtonControl>();
        show->label = L"Show";
        show->in_row = true;
        show->on_click = [this, i]() {
            if (i >= monitor_rows_.size()) return;
            hidden_monitor_devices_.erase(monitor_rows_[i].device);
            RebuildGroupRows(current_dpi_scale_);
            if (host_) host_->RequestRepaint();
        };
        auto row = std::make_shared<SettingRow>();
        row->title = monitor_rows_[i].label;
        row->help = L"Hidden from this list.";
        row->controls = {show};
        card->rows.push_back(row);
    }
    return card;
}

std::shared_ptr<CardContainer> SettingsScreen::BuildWindowCard() {
    auto card = std::make_shared<CardContainer>();
    card->title = L"Window";
    card->help = L"Changing it briefly pauses recording.";
    const FieldSpec* spec_p = FindSpecIn(specs_, "capture.target_window");
    if (!spec_p) return card;
    const FieldSpec window_spec = *spec_p;

    std::string saved;
    if (const auto* v = EffectiveConfigValue("capture.target_window"); v && v->type == ombra::JsonValue::Type::String) {
        saved = v->str;
    }

    PrefetchProcessNames();  // "Google Chrome" instead of "chrome.exe" on the tiles
    if (!window_list_fetched_ && !window_list_pending_ && host_) {
        window_list_pending_ = true;
        host_->RequestWindowListAsync([this](std::vector<WindowEntry> windows) {
            window_list_pending_ = false;
            // Latches on any reply, empty included: no capturable windows is a real answer, and a
            // host that resolves synchronously (tests) must not loop back into this rebuild.
            const bool first = !window_list_fetched_;
            window_list_fetched_ = true;
            window_list_ = std::move(windows);
            if (first && !window_list_.empty()) {
                RebuildGroupRows(current_dpi_scale_);
                if (host_) host_->RequestRepaint();
            }
        });
    }
    if (!window_list_fetched_) {
        card->help = L"Loading windows…";
        return card;
    }

    auto lower = [](std::wstring w) {
        for (auto& c : w) c = static_cast<wchar_t>(towlower(c));
        return w;
    };
    // Mirrors the engine's ResolveWindowByMatch (capture.cpp): case-insensitive substring of the
    // title, then of the process image name; the first window in enumeration order wins.
    const std::wstring want = lower(Utf8ToWide(saved));
    auto matches = [&](const WindowEntry& w) {
        if (want.empty()) return false;
        return lower(w.title).find(want) != std::wstring::npos ||
               lower(Utf8ToWide(w.process)).find(want) != std::wstring::npos;
    };
    auto app_name = [this](const std::string& process) -> std::wstring {
        if (const auto it = process_display_names_.find(process); it != process_display_names_.end() && !it->second.empty()) {
            return Utf8ToWide(it->second);
        }
        std::string base = process;
        if (base.size() > 4 && _stricmp(base.c_str() + base.size() - 4, ".exe") == 0) base.resize(base.size() - 4);
        return Utf8ToWide(base);
    };

    int match_index = -1;
    for (size_t i = 0; i < window_list_.size(); ++i) {
        if (matches(window_list_[i])) { match_index = static_cast<int>(i); break; }
    }

    auto grid = std::make_shared<TileGrid>();
    grid->columns = kCaptureTileColumns;
    // The saved window always leads, regardless of what is running. Not running, it's still the
    // chosen tile, pale.
    const bool saved_absent = !saved.empty() && match_index < 0;
    if (saved_absent) {
        auto tile = std::make_shared<TileControl>();
        tile->title = app_name(saved);
        tile->detail = L"Not running";
        tile->picture = true;
        tile->selected = true;
        tile->dimmed = true;
        grid->tiles.push_back(tile);
    }

    std::vector<size_t> order;
    if (match_index >= 0) order.push_back(static_cast<size_t>(match_index));
    for (size_t i = 0; i < window_list_.size(); ++i) {
        if (static_cast<int>(i) != match_index) order.push_back(i);
    }
    constexpr size_t kCollapsedCount = 2 * kCaptureTileColumns;  // two rows, the absent saved one among them
    const size_t room = kCollapsedCount - (saved_absent ? 1 : 0);
    const size_t shown = windows_show_all_ ? order.size() : (std::min)(order.size(), room);
    if (order.size() > room) {
        auto toggle = std::make_shared<ButtonControl>();
        toggle->label = windows_show_all_ ? L"Show fewer" : L"Show all " + std::to_wstring(order.size());
        toggle->in_row = true;
        toggle->on_click = [this]() {
            windows_show_all_ = !windows_show_all_;
            RebuildGroupRows(current_dpi_scale_);
            if (host_) host_->RequestRepaint();
        };
        card->actions = {toggle};
    }
    if (order.empty() && !saved_absent) card->help = L"No windows to capture right now.";

    // Thumbnails for the tiles on screen only, and not while the display thumbnails are still being
    // captured: the engine opens a WGC session per thumbnail, and two fetches at once would mean two
    // sessions on the GPU that may be in use for gaming. The display fetch's own rebuild retries this.
    for (size_t k = 0; k < shown; ++k) {
        const uint64_t h = window_list_[order[k]].hwnd;
        if (h != 0 && !window_thumb_cache_.count(h)) {
            MonitorThumbCache& entry = window_thumb_cache_[h];
            DecodeThumbnailPng(ombra::WindowThumbPath(h), entry.w, entry.h, entry.bgra);
        }
    }
    if (host_ && monitor_thumb_pending_.empty()) {
        std::vector<uint64_t> missing;
        for (size_t k = 0; k < shown; ++k) {
            const uint64_t h = window_list_[order[k]].hwnd;
            if (h != 0 && !window_thumb_fresh_.count(h) && !window_thumb_pending_.count(h)) missing.push_back(h);
        }
        if (!missing.empty()) {
            for (uint64_t h : missing) window_thumb_pending_.insert(h);
            host_->RequestWindowThumbnailsAsync(
                missing, [this](uint64_t h, bool ok, UINT w, UINT hh, std::vector<uint8_t> bgra) {
                    window_thumb_pending_.erase(h);
                    // A failure isn't retried this session (a minimized or protected window fails
                    // every time, and each retry is another WGC session); the tile keeps whatever
                    // the disk had.
                    window_thumb_fresh_.insert(h);
                    const bool got_image = ok && w > 0 && hh > 0;
                    if (got_image) window_thumb_cache_[h] = MonitorThumbCache{w, hh, std::move(bgra)};
                    if (got_image) {
                        RebuildGroupRows(current_dpi_scale_);
                        if (host_) host_->RequestRepaint();
                    }
                });
        }
    }

    // The user's own decisions, staged ones included; detected games come from list_windows' flags.
    const auto staged_games = StagedGameDecisions();
    for (size_t k = 0; k < shown; ++k) {
        const WindowEntry& w = window_list_[order[k]];
        auto tile = std::make_shared<TileControl>();
        tile->title = w.process.empty() ? w.title : app_name(w.process);
        tile->detail = w.title;
        tile->picture = true;
        tile->selected = static_cast<int>(order[k]) == match_index;
        // What the Games list says, the staged answer first: a "Mark as game" click shows at once.
        ombra::GameVerdict verdict = ombra::GameVerdict::Unknown;
        if (!w.image_path.empty()) verdict = ombra::ClassifyImage(staged_games, w.image_path);
        if (verdict == ombra::GameVerdict::Unknown)
            verdict = w.is_game ? ombra::GameVerdict::Game : w.not_game ? ombra::GameVerdict::NotGame : verdict;
        if (verdict == ombra::GameVerdict::Game) {
            tile->title_icon = IconType::Gamepad2;
        } else if (verdict == ombra::GameVerdict::Unknown && !w.image_path.empty()) {
            tile->action_label = L"Mark as game";
            ombra::GameEntry entry;
            entry.name = tile->title;
            entry.path = w.image_path;  // the exe's full path, never a bare name (games.h)
            entry.source = ombra::GameSource::User;
            tile->on_action = [this, entry]() { StageGameDecision(entry, true); };
        }
        if (auto it = window_thumb_cache_.find(w.hwnd); it != window_thumb_cache_.end()) {
            tile->thumb_bgra = it->second.bgra;
            tile->thumb_w = it->second.w;
            tile->thumb_h = it->second.h;
        }
        // Saves the process name, not the title: a title changes with every tab or document
        // ("YouTube - Google Chrome"), and the engine then stops finding the window. The engine
        // already falls back to matching the process image name. A window with no process name
        // (shouldn't happen) keeps the old title match.
        const std::string value = w.process.empty() ? WideToUtf8(w.title) : w.process;
        tile->on_change = [this, window_spec, value](bool) {
            ApplySettingChange(window_spec, ombra::JsonValue::MakeStr(value));
        };
        grid->tiles.push_back(tile);
    }
    if (!grid->tiles.empty()) card->rows.push_back(grid);
    return card;
}

void SettingsScreen::SeedWindowsForSnapshot(int count, const char* saved) {
    static const char* const kApps[][2] = {
        {"LeagueofLegends.exe", "League of Legends (TM) Client"},
        {"Discord.exe", "#general | Ombra - Discord"},
        {"chrome.exe", "YouTube - Google Chrome"},
        {"Spotify.exe", "Spotify Premium"},
        {"Code.exe", "screens.cpp - Ombra - Visual Studio Code"},
        {"explorer.exe", "Videos"},
        {"obs64.exe", "OBS 30.2.3 - Profile: Untitled"},
        {"steamwebhelper.exe", "Steam"},
    };
    window_list_.clear();
    window_thumb_cache_.clear();
    for (int i = 0; i < count; ++i) {
        const auto& a = kApps[i % std::size(kApps)];
        WindowEntry w;
        w.process = a[0];
        w.title = Utf8ToWide(a[1]);
        w.hwnd = 0x1000 + static_cast<uint64_t>(i);
        // A game (controller icon), two the list says aren't (no action), the rest unknown ("Mark as game").
        w.image_path = L"C:\\Apps\\" + Utf8ToWide(a[0]);
        w.is_game = i == 0;
        w.not_game = i == 1 || i == 5;
        window_list_.push_back(w);

        // Portrait for one tile, so the aspect-fit letterboxing shows up in the snapshot.
        MonitorThumbCache t;
        t.w = (i == 3) ? 180 : 320;
        t.h = (i == 3) ? 320 : 200;
        t.bgra.resize(static_cast<size_t>(t.w) * t.h * 4);
        for (UINT y = 0; y < t.h; ++y) {
            for (UINT x = 0; x < t.w; ++x) {
                uint8_t* px = &t.bgra[(static_cast<size_t>(y) * t.w + x) * 4];
                const bool bar = y < 18;
                px[0] = static_cast<uint8_t>(bar ? 60 : 30 + (i * 40) % 160);
                px[1] = static_cast<uint8_t>(bar ? 60 : 50 + x / 5);
                px[2] = static_cast<uint8_t>(bar ? 70 : 70 + y / 4);
                px[3] = 255;
            }
        }
        window_thumb_cache_.emplace(w.hwnd, std::move(t));
        window_thumb_fresh_.insert(w.hwnd);
    }
    window_list_fetched_ = true;
    UpdateConfigValue("capture.target", ombra::JsonValue::MakeStr("window"));
    UpdateConfigValue("capture.target_window", ombra::JsonValue::MakeStr(saved ? saved : ""));
    RebuildGroupRows(current_dpi_scale_);
}

int SettingsScreen::FindAudioSourceIndexByKind(const std::string& kind) const {
    for (size_t i = 0; i < audio_sources_ui_.size(); ++i) {
        const auto* k = audio_sources_ui_[i].Find("kind");
        if (k && k->type == ombra::JsonValue::Type::String && k->str == kind) return static_cast<int>(i);
    }
    return -1;
}

int SettingsScreen::FindAudioSourceIndexById(const std::string& id) const {
    for (size_t i = 0; i < audio_sources_ui_.size(); ++i) {
        const auto* v = audio_sources_ui_[i].Find("id");
        if (v && v->type == ombra::JsonValue::Type::String && v->str == id) return static_cast<int>(i);
    }
    return -1;
}

ombra::JsonValue SettingsScreen::NewAppSource(const std::string& exe, const std::string& group_id) const {
    const std::string base_id = "only_" + exe;
    std::string       new_id  = base_id;
    for (int suffix = 2; FindAudioSourceIndexById(new_id) >= 0; ++suffix) new_id = base_id + "_" + std::to_string(suffix);
    ombra::JsonValue a = ombra::JsonValue::MakeObject();
    a.Set("id", ombra::JsonValue::MakeStr(new_id));
    a.Set("kind", ombra::JsonValue::MakeStr("output_loopback"));
    a.Set("device", ombra::JsonValue::MakeStr("default"));
    a.Set("mode", ombra::JsonValue::MakeStr("only"));
    a.Set("target", ombra::JsonValue::MakeStr(exe));
    a.Set("enabled", ombra::JsonValue::MakeBool(true));
    if (!group_id.empty()) a.Set("group_id", ombra::JsonValue::MakeStr(group_id));
    if (const auto it = process_display_names_.find(exe); it != process_display_names_.end() && !it->second.empty())
        a.Set("name", ombra::JsonValue::MakeStr(it->second));
    return a;
}

void SettingsScreen::CommitAudioSourcesNoRealloc() {
    // See screens.h's comment on audio_sources_dirty_: this used to set_config immediately.
    // audio_sources_ui_ already holds the caller's edit -- just stage it.
    StageAudioSources();
}

void SettingsScreen::StageAudioSources() {
    ++audio_sources_generation_;  // see screens.h's comment on audio_sources_generation_
    const auto* saved_mode = current_config_.Find("audio.per_app_mode");
    const bool saved_per_app = saved_mode && saved_mode->type == ombra::JsonValue::Type::Bool && saved_mode->b;
    // What the page would show for the saved config: an absent or empty list is the default Desktop
    // source the page materializes (BuildAudioPage), so undoing an edit there matches again.
    std::vector<ombra::JsonValue> saved;
    if (const auto* v = current_config_.Find("audio.sources"); v && v->type == ombra::JsonValue::Type::Array)
        for (const auto& e : v->arr)
            if (e.type == ombra::JsonValue::Type::Object) saved.push_back(e);
    if (std::none_of(saved.begin(), saved.end(), [](const ombra::JsonValue& e) {
            const auto* k = e.Find("kind");
            return k && k->type == ombra::JsonValue::Type::String && k->str == "output_loopback";
        }))
        saved.insert(saved.begin(), DefaultDesktopSource());
    bool same = audio_per_app_mode_ == saved_per_app && saved.size() == audio_sources_ui_.size();
    for (size_t i = 0; same && i < audio_sources_ui_.size(); ++i)
        same = ombra::JsonEquals(audio_sources_ui_[i], saved[i]);
    audio_sources_dirty_ = !same;
    RebuildGroupRows(current_dpi_scale_);
    if (host_) host_->RequestRepaint();
}

void SettingsScreen::CommitAudioGroupDrop(const std::string& dragged_id,
                                          const std::string& target_id) {
    if (dragged_id.empty() || target_id.empty() || dragged_id == target_id) return;
    const int target_idx = FindAudioSourceIndexById(target_id);
    if (target_idx < 0) return;
    const std::string target_group = EnsureAudioGroup(target_idx);

    const int dragged_idx = FindAudioSourceIndexById(dragged_id);
    if (dragged_idx >= 0) {
        // Already a configured source -- only group_id changes, read only at save time.
        audio_sources_ui_[dragged_idx].Set("group_id", ombra::JsonValue::MakeStr(target_group));
        CommitAudioSourcesNoRealloc();
        return;
    }

    // "live:<exe_name>" -- a running process with no audio.sources entry yet. Same synthesis
    // BuildAudioSourceRows' "Add app on its own track" flow already does, pre-grouped with the
    // drop target. A brand new capture source does need the pipeline rebuilt, same as any other
    // staged add -- but that now waits for SaveAudioSources() like everything else instead of
    // popping its own Realloc-confirm modal right here.
    static const char kLivePrefix[] = "live:";
    if (dragged_id.rfind(kLivePrefix, 0) != 0) return;
    const std::string exe_name = dragged_id.substr(sizeof(kLivePrefix) - 1);
    if (exe_name.empty()) return;

    audio_sources_ui_.push_back(NewAppSource(exe_name, target_group));
    StageAudioSources();
}

void SettingsScreen::DiscardAudioSources() {
    audio_sources_dirty_ = false;
    RebuildGroupRows(current_dpi_scale_);
    if (host_) host_->RequestRepaint();
}

void SettingsScreen::OpenFreshProcessPicker(
    const D2D1_RECT_F& anchor, std::function<void(const std::vector<ProcessEntry>&)> open_picker) {
    if (!host_) return;
    host_->ShowDropdown(anchor, {L"Loading…"}, -1, [](int) {});
    host_->RequestProcessListAsync([this, open_picker](std::vector<ProcessEntry> processes) {
        ApplyProcessListResult(processes);
        if (!host_) return;
        open_picker(processes.empty() ? process_list_cache_ : processes);
    });
}

void SettingsScreen::ApplyProcessListResult(std::vector<ProcessEntry> processes) {
    // Shared by the prefetch below and both on-click pickers (BuildAudioSourceRows' "Add app",
    // BuildExclusionRows' "Add process") -- see screens.h's comment on process_list_cache_ for why
    // fetched_ latches true on every reply, not just a non-empty one.
    process_list_pending_ = false;
    process_list_fetched_ = true;
    if (!processes.empty()) {
        process_list_cache_ = processes;  // full list -- every picker's option set, display-named or not
        for (const auto& p : processes) {
            if (!p.name.empty() && !p.display_name.empty()) process_display_names_[p.name] = p.display_name;
        }
    }
}

void SettingsScreen::PrefetchProcessNames() {
    if (!process_list_fetched_ && !process_list_pending_ && host_) {
        process_list_pending_ = true;
        host_->RequestProcessListAsync([this](std::vector<ProcessEntry> processes) {
            const bool had_data = !processes.empty();
            ApplyProcessListResult(std::move(processes));
            // Only rebuild when there's something new to show -- keeps a host whose async call
            // happens to resolve synchronously (tests' FakeScreenHost) from recursing into this
            // function through RebuildGroupRows below on every reply, same guard shape
            // BuildDisplaysCard uses.
            if (had_data) {
                RebuildGroupRows(current_dpi_scale_);
                if (host_) host_->RequestRepaint();
            }
        });
    }
}

void SettingsScreen::RequestUpdateStatus() {
    if (update_pending_ || update_seeded_ || !host_) return;
    update_pending_ = true;
    ombra::JsonValue req = ombra::JsonValue::MakeObject();
    req.Set("type", ombra::JsonValue::MakeStr("update_status"));
    host_->RequestAsync(std::move(req), 1000, [this](std::optional<ombra::JsonValue> resp) {
        update_pending_ = false;
        ApplyUpdateStatus(resp ? &*resp : nullptr);
    });
}

void SettingsScreen::ApplyUpdateStatus(const ombra::JsonValue* resp) {
    if (update_seeded_) return;
    UpdateView next = resp ? ParseUpdateView(*resp) : UpdateView{};
    if (!resp) next.state = "unreachable";
    const bool changed = next.state != update_.state || next.latest != update_.latest ||
                         next.error != update_.error || next.last_check_unix != update_.last_check_unix ||
                         next.kind != update_.kind || next.current != update_.current;
    update_ = std::move(next);
    if (changed && system_page_built_) {
        RebuildGroupRows(current_dpi_scale_);
        if (host_) host_->RequestRepaint();
    }
}

void SettingsScreen::SendUpdateCommand(const char* type) {
    if (!host_ || update_seeded_) return;
    ombra::JsonValue req = ombra::JsonValue::MakeObject();
    req.Set("type", ombra::JsonValue::MakeStr(type));
    // Shown at once; the next update_status confirms it or says what went wrong.
    if (std::strcmp(type, "update_check") == 0) update_.state = "checking";
    RebuildGroupRows(current_dpi_scale_);
    host_->RequestRepaint();
    host_->RequestAsync(std::move(req), 1000, [this](std::optional<ombra::JsonValue>) { RequestUpdateStatus(); });
}

void SettingsScreen::SeedUpdateForSnapshot(const std::string& state, const std::string& kind) {
    update_seeded_ = true;
    update_ = {};
    update_.state = state;
    update_.current = "0.1.0";
    update_.kind = kind.empty() ? "installer" : kind;
    update_.latest = state == "up_to_date" ? "0.1.0" : "0.2.0";
    update_.page_url = "https://github.com/Zyrecx/ombra/releases/tag/v0.2.0";
    update_.last_check_unix = static_cast<int64_t>(std::time(nullptr)) - 2 * 3600;
    if (state == "failed") update_.error = "no connection (12007)";
    rows_dirty_ = true;
}

UpdateView ParseUpdateView(const ombra::JsonValue& resp) {
    UpdateView u;
    const auto str = [&](const char* key) {
        const auto* v = resp.Find(key);
        return v && v->type == ombra::JsonValue::Type::String ? v->str : std::string();
    };
    u.state = str("state");
    u.current = str("current");
    u.latest = str("latest");
    u.page_url = str("page_url");
    u.error = str("error");
    u.kind = str("install_kind");
    if (const auto* t = resp.Find("last_check_unix"); t && t->type == ombra::JsonValue::Type::Number)
        u.last_check_unix = static_cast<int64_t>(t->num);
    if (u.state.empty()) u.state = "unreachable";
    return u;
}

std::wstring UpdateStatusLine(const UpdateView& u, bool auto_install, int64_t now_unix, bool* is_error) {
    *is_error = false;
    const std::wstring latest = Utf8ToWide(u.latest);
    const auto checked = [&]() -> std::wstring {
        if (u.last_check_unix <= 0) return L"";
        const int64_t ago = (std::max<int64_t>)(0, now_unix - u.last_check_unix);
        if (ago < 120) return L" \u00b7 checked just now";
        if (ago < 7200) return L" \u00b7 checked " + std::to_wstring(ago / 60) + L" minutes ago";
        if (ago < 2 * 86400) return L" \u00b7 checked " + std::to_wstring(ago / 3600) + L" hours ago";
        return L" \u00b7 checked " + std::to_wstring(ago / 86400) + L" days ago";
    };
    const std::string& s = u.state;
    if (s.empty()) return L"Asking " OMBRA_PRODUCT_NAME L"\u2026";
    if (s == "unreachable") return L"" OMBRA_PRODUCT_NAME " isn't answering, so the update status is unknown.";
    if (s == "off") return L"Updates are off in this test instance.";
    if (s == "checking") return L"Checking for updates\u2026";
    if (s == "downloading") return L"Downloading the update\u2026";
    if (s == "installing") return L"Installing " + latest + L". " OMBRA_PRODUCT_NAME " restarts by itself.";
    if (s == "up_to_date") return L"Up to date" + checked();
    if (s == "ready")
        return auto_install ? latest + L" is ready. It installs the next time you're away and no game is running."
                            : latest + L" is ready to install.";
    if (s == "available") {
        if (u.kind == "scoop") return latest + L" is out. Scoop installs it: scoop update ombra";
        if (u.kind == "portable") return latest + L" is out. Download it from the release page.";
        if (!u.error.empty()) {
            *is_error = true;
            return latest + L" is out, but the download failed: " + Utf8ToWide(u.error);
        }
        return latest + L" is out.";
    }
    if (s == "failed") {
        if (u.error == "no release is published yet") return L"No release is published yet.";
        *is_error = true;
        return L"Couldn't check for updates: " + Utf8ToWide(u.error);
    }
    return L"Not checked yet.";  // idle: the first check runs a few minutes after start
}

void SettingsScreen::OnStatusTick() {
    if (system_page_built_ && (update_.state == "checking" || update_.state == "downloading" ||
                               update_.state == "installing"))
        RequestUpdateStatus();
    if (!audio_page_built_ || audio_status_pending_ || !host_) return;
    audio_status_pending_ = true;
    ombra::JsonValue req = ombra::JsonValue::MakeObject();
    req.Set("type", ombra::JsonValue::MakeStr("audio_status"));
    host_->RequestAsync(std::move(req), 1000, [this](std::optional<ombra::JsonValue> resp) {
        audio_status_pending_ = false;
        ApplyAudioStatus(resp ? &*resp : nullptr);
        if (host_) host_->RequestRepaint();
    });
}

void SettingsScreen::ApplyAudioStatus(const ombra::JsonValue* resp) {
    audio_status_.clear();
    const auto* arr = resp ? resp->Find("sources") : nullptr;
    if (arr && arr->type == ombra::JsonValue::Type::Array) {
        for (const auto& e : arr->arr) {
            const auto* id = e.Find("id");
            const auto* st = e.Find("state");
            if (!id || id->type != ombra::JsonValue::Type::String || !st || st->type != ombra::JsonValue::Type::String)
                continue;
            const std::string& v = st->str;
            audio_status_[id->str] = v == "live"    ? AudioSourceStatus::Live
                                   : v == "quiet"   ? AudioSourceStatus::Quiet
                                   : v == "waiting" ? AudioSourceStatus::Waiting
                                   : v == "no_game" ? AudioSourceStatus::NoGame
                                   : v == "failed"  ? AudioSourceStatus::Failed
                                   : v == "off"     ? AudioSourceStatus::Off
                                   : v == "paused"  ? AudioSourceStatus::Paused
                                                    : AudioSourceStatus::None;
        }
    }
    // In place while the text only changes; a line that appears or goes changes the row's height,
    // so the page is rebuilt (not mid-drag: the drop rebuilds it anyway).
    bool resized = false;
    for (const auto& r : audio_rows_) {
        if (r.id.rfind("live:", 0) == 0) continue;
        const bool had = !r.row->note.empty();
        StatusNote(r.id, r.enabled, r.row.get());
        resized |= had != !r.row->note.empty();
    }
    if (resized && !dragging_audio_) RebuildGroupRows(current_dpi_scale_);
}

// Capture > Audio: a card per track of the clip, its sources as rows. Every edit stages into
// audio_sources_ui_ (StageAudioSources) until Save.
void SettingsScreen::BuildAudioPage() {
    audio_page_built_ = true;
    // Friendly names for app sources (and the Add app picker), off the UI thread.
    PrefetchProcessNames();

    // Desktop's and the mic's device menus: fetched once per overlay open.
    if (!audio_device_list_fetched_ && !audio_device_list_pending_ && host_) {
        audio_device_list_pending_ = true;
        host_->RequestAudioDeviceListAsync([this](std::vector<AudioDeviceEntry> devices) {
            audio_device_list_pending_ = false;
            if (!devices.empty()) {
                audio_device_list_fetched_ = true;
                audio_device_list_cache_ = std::move(devices);
                RebuildGroupRows(current_dpi_scale_);
                if (host_) host_->RequestRepaint();
            }
        });
    }

    // A staged edit must survive this rebuild; only Save and Discard decide what it becomes.
    if (!audio_sources_dirty_) {
        const auto* per_app = current_config_.Find("audio.per_app_mode");
        audio_per_app_mode_ = per_app && per_app->type == ombra::JsonValue::Type::Bool && per_app->b;
        audio_sources_ui_.clear();
        if (const auto* v = current_config_.Find("audio.sources"); v && v->type == ombra::JsonValue::Type::Array) {
            for (const auto& e : v->arr) {
                if (e.type == ombra::JsonValue::Type::Object) audio_sources_ui_.push_back(e);
            }
        }
    }
    // An empty audio.sources is the engine's default Desktop source (ResolveAudioSources): edit a
    // real entry, always index 0, the "primary" source main.cpp's start-up depends on.
    if (FindAudioSourceIndexByKind("output_loopback") < 0)
        audio_sources_ui_.insert(audio_sources_ui_.begin(), DefaultDesktopSource());

    // Apps playing right now (list_audio_sessions), fetched once per overlay open; an empty reply
    // is a normal state, so it latches too.
    if (!audio_sessions_fetched_ && !audio_sessions_pending_ && host_) {
        audio_sessions_pending_ = true;
        host_->RequestAudioSessionListAsync([this](std::vector<AudioSessionEntry> sessions) {
            audio_sessions_pending_ = false;
            audio_sessions_fetched_ = true;
            if (sessions.empty()) return;
            audio_sessions_cache_.clear();
            std::set<std::string> seen;
            for (const auto& sess : sessions) {
                if (seen.insert(sess.name).second) audio_sessions_cache_.push_back(sess.name);
                // A windowless process is missing from list_processes: this is its only friendly name.
                if (!sess.display_name.empty()) process_display_names_[sess.name] = sess.display_name;
            }
            RebuildGroupRows(current_dpi_scale_);
            if (host_) host_->RequestRepaint();
        });
    }

    auto str_field = [](const ombra::JsonValue& obj, const char* key) -> std::string {
        const auto* v = obj.Find(key);
        return (v && v->type == ombra::JsonValue::Type::String) ? v->str : std::string();
    };
    const std::string primary_id = [&] {
        const int i = FindAudioSourceIndexByKind("output_loopback");
        const std::string id = str_field(audio_sources_ui_[i], "id");
        return id.empty() ? std::string("desktop") : id;
    }();
    const bool has_mic = FindAudioSourceIndexByKind("input") >= 0;
    auto is_app = [&](const ombra::JsonValue& s) {
        return str_field(s, "id") != primary_id && str_field(s, "kind") != "input";
    };
    auto group_of = [&](const ombra::JsonValue& s) {
        const std::string g = str_field(s, "group_id");
        return g.empty() ? str_field(s, "id") : g;
    };
    auto display_name = [&](const std::string& exe) {
        const auto it = process_display_names_.find(exe);
        return Utf8ToWide(it != process_display_names_.end() && !it->second.empty() ? it->second : exe);
    };
    // A track card says what the saved file calls the track (AudioTrackName); a row what the source is.
    auto track_name = [&](const ombra::JsonValue& s) {
        ombra::AudioSourceSpec spec;
        spec.kind = str_field(s, "kind");
        spec.mode = str_field(s, "mode");
        spec.target = str_field(s, "target");
        spec.name = str_field(s, "name");
        return Utf8ToWide(ombra::AudioTrackName(spec));
    };
    auto row_title = [&](const ombra::JsonValue& s) -> std::wstring {
        if (str_field(s, "kind") == "input") return L"Microphone";
        if (str_field(s, "id") == primary_id) return L"Desktop";
        const std::string t = str_field(s, "target");
        if (const auto it = process_display_names_.find(t); it != process_display_names_.end() && !it->second.empty())
            return Utf8ToWide(it->second);
        return track_name(s);
    };
    const bool desktop_excluding = [&] {
        const int di = FindAudioSourceIndexById(primary_id);
        return di >= 0 && str_field(audio_sources_ui_[di], "mode") == "all_except" &&
               !str_field(audio_sources_ui_[di], "target").empty();
    }();
    auto anchor_of = [this](const Widget& w) {
        D2D1_RECT_F at = w.bounds;
        at.top -= scroll_area_.scroll_y;
        at.bottom -= scroll_area_.scroll_y;
        return at;
    };
    // A Select's menu through the host, as MakeSelect does it.
    auto wire_select = [this, anchor_of](const std::shared_ptr<SelectControl>& sel) {
        std::weak_ptr<SelectControl> weak = sel;
        sel->on_open = [this, weak, anchor_of](SelectControl& s) {
            if (!host_) {
                s.open = false;
                return;
            }
            host_->ShowDropdown(anchor_of(s), s.options, s.selected, s.on_change, [this, weak] {
                if (auto self = weak.lock()) self->open = false;
                if (host_) host_->RequestRepaint();
            });
        };
    };
    // A ⋯ button whose menu runs `actions[i]` for option i.
    struct MenuItem { std::wstring label; std::function<void(const D2D1_RECT_F&)> run; };
    auto more_button = [this, anchor_of](std::vector<MenuItem> items) {
        auto b = std::make_shared<IconButtonControl>();
        b->glyph = IconButtonControl::Glyph::More;
        IconButtonControl* raw = b.get();
        b->on_click = [this, raw, items, anchor_of] {
            if (!host_) return;
            const D2D1_RECT_F at = anchor_of(*raw);
            std::vector<std::wstring> labels;
            for (const auto& it : items) labels.push_back(it.label);
            host_->ShowDropdown(at, labels, -1, [items, at](int i) {
                if (i >= 0 && i < static_cast<int>(items.size())) items[i].run(at);
            });
        };
        return b;
    };
    // The process picker: `group_id` empty puts the app on a track of its own.
    auto pick_app = [this, str_field](const D2D1_RECT_F& at, std::string group_id) {
        OpenFreshProcessPicker(at, [this, at, group_id, str_field](const std::vector<ProcessEntry>& processes) {
            std::vector<std::wstring> options_w;
            std::vector<std::string> options_utf8;
            ProcessListToDropdownOptions(processes, /*prefer_display_name=*/true, &options_w, &options_utf8);
            // The same app can have several tracks: marked, so a second is a choice.
            for (size_t k = 0; k < options_utf8.size(); ++k) {
                const bool tracked = std::any_of(audio_sources_ui_.begin(), audio_sources_ui_.end(), [&](const ombra::JsonValue& src) {
                    return str_field(src, "mode") == "only" && _stricmp(str_field(src, "target").c_str(), options_utf8[k].c_str()) == 0;
                });
                if (tracked) options_w[k] += L" (already has a track)";
            }
            host_->ShowDropdown(at, options_w, -1, [this, options_utf8, group_id](int idx) {
                if (idx < 0 || idx >= static_cast<int>(options_utf8.size())) return;
                audio_sources_ui_.push_back(NewAppSource(options_utf8[idx], group_id));
                StageAudioSources();
            });
        });
    };

    audio_rows_.clear();
    meters_.clear();
    audio_track_cards_.clear();
    audio_drop_slot_.reset();
    GroupSection sec;
    sec.name = L"Audio";

    // --- Sources: App tracks, and ⋯ to add an app or the mic or edit the list.
    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Sources";
        auto sw = std::make_shared<SwitchControl>();
        sw->id = "audio.per_app_mode";
        sw->on = audio_per_app_mode_;
        sw->on_change = [this](bool on) {
            audio_per_app_mode_ = on;
            if (!on) audio_edit_list_ = false;
            StageAudioSources();
        };
        auto row = std::make_shared<SettingRow>();
        row->title = L"App tracks";
        row->help = L"Record chosen apps on tracks of their own.";
        row->controls = {sw};
        card->rows.push_back(row);

        const bool any_removable = std::any_of(audio_sources_ui_.begin(), audio_sources_ui_.end(), [&](const ombra::JsonValue& s) {
            return str_field(s, "id") != primary_id && (audio_per_app_mode_ || !is_app(s));
        });
        if (audio_edit_list_ && !any_removable) audio_edit_list_ = false;
        if (audio_edit_list_) {
            auto done = std::make_shared<ButtonControl>();
            done->label = L"Done";
            done->on_click = [this] {
                audio_edit_list_ = false;
                RebuildGroupRows(current_dpi_scale_);
                if (host_) host_->RequestRepaint();
            };
            card->actions = {done};
        } else {
            std::vector<MenuItem> items;
            if (audio_per_app_mode_) items.push_back({L"Add app", [pick_app](const D2D1_RECT_F& at) { pick_app(at, {}); }});
            if (!has_mic) {
                items.push_back({L"Add microphone", [this](const D2D1_RECT_F&) {
                    ombra::JsonValue m = ombra::JsonValue::MakeObject();
                    m.Set("id", ombra::JsonValue::MakeStr("mic"));
                    m.Set("kind", ombra::JsonValue::MakeStr("input"));
                    m.Set("device", ombra::JsonValue::MakeStr("default"));
                    m.Set("enabled", ombra::JsonValue::MakeBool(true));
                    audio_sources_ui_.push_back(std::move(m));
                    StageAudioSources();
                }});
            }
            if (any_removable) {
                items.push_back({L"Edit list", [this](const D2D1_RECT_F&) {
                    audio_edit_list_ = true;
                    RebuildGroupRows(current_dpi_scale_);
                    if (host_) host_->RequestRepaint();
                }});
            }
            if (!items.empty()) card->actions = {more_button(std::move(items))};
        }
        sec.AddRow(card);
    }

    // --- A card per track: the visible sources grouped by group_id in list order, as the engine
    // builds tracks (AudioTrackHub::RebuildTracksLocked). Simple hides app sources (they don't record).
    struct Track { std::string gid; std::vector<size_t> members; };
    std::vector<Track> tracks;
    for (size_t i = 0; i < audio_sources_ui_.size(); ++i) {
        const auto& s = audio_sources_ui_[i];
        if (str_field(s, "id").empty() || (!audio_per_app_mode_ && is_app(s))) continue;
        const std::string g = group_of(s);
        auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.gid == g; });
        if (it == tracks.end()) tracks.push_back({g, {i}});
        else it->members.push_back(i);
    }
    std::vector<std::wstring> track_titles;
    for (const auto& t : tracks) {
        std::wstring title;
        for (size_t i : t.members) title += (title.empty() ? L"" : L" + ") + track_name(audio_sources_ui_[i]);
        track_titles.push_back(std::move(title));
    }
    const std::string dragged_gid = [&]() -> std::string {
        const int i = FindAudioSourceIndexById(drag_audio_id_);
        return dragging_audio_ && i >= 0 ? group_of(audio_sources_ui_[i]) : std::string();
    }();

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const Track& t = tracks[ti];
        const std::string first_id = str_field(audio_sources_ui_[t.members.front()], "id");
        auto card = std::make_shared<CardContainer>();
        card->title = track_titles[ti];
        if (t.members.size() > 1) card->help = L"One track: these are mixed.";

        std::vector<MenuItem> items;
        for (size_t oi = 0; oi < tracks.size(); ++oi) {
            if (oi == ti) continue;
            const std::string to = str_field(audio_sources_ui_[tracks[oi].members.front()], "id");
            items.push_back({L"Mix with " + track_titles[oi], [this, gid = t.gid, to](const D2D1_RECT_F&) { MixAudioTrackInto(gid, to); }});
        }
        if (t.members.size() > 1) {
            std::vector<std::string> ids;
            for (size_t i : t.members) ids.push_back(str_field(audio_sources_ui_[i], "id"));
            items.push_back({L"Split into separate tracks", [this, ids](const D2D1_RECT_F&) {
                for (const auto& id : ids)
                    if (const int idx = FindAudioSourceIndexById(id); idx >= 0)
                        audio_sources_ui_[idx].Set("group_id", ombra::JsonValue::MakeStr(id));
                StageAudioSources();
            }});
        }
        if (audio_per_app_mode_) {
            items.push_back({L"Add app to this track", [this, pick_app, first_id](const D2D1_RECT_F& at) {
                const int idx = FindAudioSourceIndexById(first_id);
                if (idx >= 0) pick_app(at, EnsureAudioGroup(idx));
            }});
        }
        if (!items.empty() && !audio_edit_list_) card->actions = {more_button(std::move(items))};

        for (size_t i : t.members) {
            const ombra::JsonValue& src = audio_sources_ui_[i];
            const std::string id = str_field(src, "id");
            const bool is_desktop = id == primary_id;
            const bool is_mic = str_field(src, "kind") == "input";
            const bool enabled = [&] {
                const auto* en = src.Find("enabled");
                return !(en && en->type == ombra::JsonValue::Type::Bool && !en->b);
            }();
            auto row = std::make_shared<SettingRow>();
            row->title = row_title(src);
            row->grip = true;
            {
                // What the source hears now, after its gain (Windows' peak meters; audio_levels.h).
                // Desktop shows the whole endpoint, also while it leaves an app out or keeps to the game.
                auto meter = std::make_shared<LevelMeter>();
                meter->enabled = enabled;
                MeterBinding b;
                b.target.id = id;
                b.target.input = is_mic;
                b.target.device = str_field(src, "device");
                if (!is_desktop && !is_mic) b.target.app = str_field(src, "target");
                const auto* g = src.Find("gain_db");
                b.gain_db = g && g->type == ombra::JsonValue::Type::Number ? static_cast<float>(g->num) : 0.0f;
                if (const auto it = meter_db_.find(id); it != meter_db_.end()) meter->db = it->second;
                b.meter = meter;
                meters_.push_back(std::move(b));
                row->below = meter;
            }
            row->lifted = dragging_audio_ && drag_audio_id_ == id;
            row->on_grip = [this, id] { StartAudioDrag(id); };
            StatusNote(id, enabled, row.get());

            if (is_desktop) {
                const std::string mode = str_field(src, "mode");
                const std::string target = str_field(src, "target");
                // An exclusion is set by popping an app out (Playing now), and cleared by removing
                // that app's track: the menu has nothing true to show meanwhile. A legacy bare
                // "only" keeps the menu, since there is no track to remove to leave it.
                // Simple records Desktop whole whatever is saved (SelectActiveAudioSources).
                if (audio_per_app_mode_ && mode == "all_except" && !target.empty())
                    row->help = L"Leaving out " + display_name(target) + L".";
                else if (audio_per_app_mode_ && mode == "only" && !target.empty())
                    row->help = L"Recording only " + display_name(target) + L".";
                if (mode != "all_except" || !audio_per_app_mode_) {
                    static const char* const kModeValues[] = {"all", "game_only"};
                    auto sel = std::make_shared<SelectControl>();
                    sel->options = {L"All desktop audio", L"Only the detected game"};
                    sel->selected = mode == "game_only" ? 1 : 0;
                    sel->width_dip = 170.0f;
                    sel->on_change = [this, id](int k) {
                        const int idx = FindAudioSourceIndexById(id);
                        if (idx < 0 || k < 0 || k > 1) return;
                        audio_sources_ui_[idx].Set("mode", ombra::JsonValue::MakeStr(kModeValues[k]));
                        audio_sources_ui_[idx].Set("target", ombra::JsonValue::MakeStr(""));
                        StageAudioSources();
                    };
                    wire_select(sel);
                    row->controls.push_back(sel);
                }
            }
            // Which endpoint it records, without touching the system default. The first option
            // follows Windows; choosing another device by name pins that device.
            if (is_desktop || is_mic) {
                std::vector<AudioDeviceEntry> devices;
                for (const auto& d : audio_device_list_cache_)
                    if (d.input == is_mic) devices.push_back(d);
                if (!devices.empty()) {
                    const std::string cur = str_field(src, "device");
                    auto sel = std::make_shared<SelectControl>();
                    sel->width_dip = 210.0f;
                    // The default leads, so a trimmed label still says it, and an id that no longer
                    // exists shows what the engine falls back to (OpenStream).
                    std::stable_partition(devices.begin(), devices.end(), [](const AudioDeviceEntry& d) { return d.is_default; });
                    const bool follows_default = cur.empty() || cur == "default";
                    for (size_t di = 0; di < devices.size(); ++di) {
                        const auto& d = devices[di];
                        sel->options.push_back((d.is_default ? L"Default: " : L"") + Utf8ToWide(d.name));
                        if (d.id == cur || (follows_default && d.is_default)) sel->selected = static_cast<int>(di);
                    }
                    sel->tooltip = sel->options[static_cast<size_t>(sel->selected)];
                    sel->on_change = [this, id, devices](int k) {
                        const int idx = FindAudioSourceIndexById(id);
                        if (idx < 0 || k < 0 || k >= static_cast<int>(devices.size())) return;
                        // The default's entry stores "default", so it keeps following Windows.
                        audio_sources_ui_[idx].Set("device", ombra::JsonValue::MakeStr(devices[k].is_default ? "default" : devices[k].id));
                        StageAudioSources();
                    };
                    wire_select(sel);
                    row->controls.push_back(sel);
                }
            }
            {
                // Recording-only gain, mixed in at encode time (audio_tracks.h); never the system volume.
                auto gain = std::make_shared<StepperControl>();
                const auto* g = src.Find("gain_db");
                gain->value = std::round(g && g->type == ombra::JsonValue::Type::Number ? g->num : 0.0);
                gain->min = kGainDbMin;
                gain->max = kGainDbMax;
                gain->unit = L"dB";
                gain->on_change = [this, id](double v) {
                    const int idx = FindAudioSourceIndexById(id);
                    if (idx < 0) return;
                    audio_sources_ui_[idx].Set("gain_db", ombra::JsonValue::MakeNum(v));
                    StageAudioSources();
                };
                row->controls.push_back(gain);
            }
            if (audio_edit_list_ && !is_desktop) {
                // Before the switch, so every switch keeps the right edge.
                auto remove = std::make_shared<IconButtonControl>();
                remove->glyph = IconButtonControl::Glyph::Remove;
                remove->tooltip = L"Remove";
                remove->on_click = [this, id, primary_id, str_field] {
                    const int idx = FindAudioSourceIndexById(id);
                    if (idx < 0) return;
                    const std::string removed_target = str_field(audio_sources_ui_[idx], "target");
                    audio_sources_ui_.erase(audio_sources_ui_.begin() + idx);
                    // Desktop must not keep leaving out an app that no longer has a track anywhere: its
                    // audio would be in no track at all. The same app can have several tracks.
                    const bool still_tracked = std::any_of(audio_sources_ui_.begin(), audio_sources_ui_.end(), [&](const ombra::JsonValue& s) {
                        return str_field(s, "mode") == "only" && str_field(s, "target") == removed_target;
                    });
                    const int di = FindAudioSourceIndexById(primary_id);
                    if (!removed_target.empty() && !still_tracked && di >= 0 &&
                        str_field(audio_sources_ui_[di], "mode") == "all_except" &&
                        str_field(audio_sources_ui_[di], "target") == removed_target) {
                        audio_sources_ui_[di].Set("mode", ombra::JsonValue::MakeStr("all"));
                        audio_sources_ui_[di].Set("target", ombra::JsonValue::MakeStr(""));
                    }
                    StageAudioSources();
                };
                row->controls.push_back(remove);
            }
            {
                // On/off applies live on Save: the audio target watcher stops or starts the capture.
                auto sw = std::make_shared<SwitchControl>();
                sw->on = enabled;
                sw->on_change = [this, id](bool on) {
                    const int idx = FindAudioSourceIndexById(id);
                    if (idx < 0) return;
                    audio_sources_ui_[idx].Set("enabled", ombra::JsonValue::MakeBool(on));
                    StageAudioSources();
                };
                row->controls.push_back(sw);
            }
            card->rows.push_back(row);
            audio_rows_.push_back({id, row, enabled});
        }
        audio_track_cards_.push_back({first_id, card});
        sec.AddRow(card);
    }

    // Apps making sound that have no track yet (per-app only).
    std::vector<std::string> live_only;
    if (audio_per_app_mode_) {
        std::set<std::string> configured;
        for (const auto& s : audio_sources_ui_) {
            const std::string m = str_field(s, "mode");
            if (m == "only" || m == "all_except") configured.insert(str_field(s, "target"));
        }
        for (const std::string& exe : audio_sessions_cache_)
            if (!configured.count(exe)) live_only.push_back(exe);
    }
    const bool live_drag = dragging_audio_ && drag_audio_id_.rfind("live:", 0) == 0;

    // While dragging: where a row lands to get a track of its own. A source already alone on its
    // track has nowhere new to go, and a playing app can't while Desktop already leaves one out.
    if (dragging_audio_) {
        const bool alone = !live_drag && std::count_if(tracks.begin(), tracks.end(), [&](const Track& t) {
            return t.gid == dragged_gid && t.members.size() == 1;
        }) > 0;
        if (!alone && !(live_drag && desktop_excluding)) {
            audio_drop_slot_ = std::make_shared<DropSlot>();
            audio_drop_slot_->label = L"Drop here for its own track";
            sec.AddRow(audio_drop_slot_);
        }
    }

    if (!live_only.empty()) {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Playing now";
        card->help = desktop_excluding
            ? L"Apps making sound that aren't on a track. Desktop already leaves one app out (a Windows limit): "
              L"remove that app's track to give another its own."
            : L"Apps making sound that aren't on a track. \"Own track\" also leaves the app out of Desktop "
              L"(one app at a time, a Windows limit).";
        for (const std::string& exe : live_only) {
            const std::string id = "live:" + exe;
            auto row = std::make_shared<SettingRow>();
            row->title = display_name(exe);
            row->help = Utf8ToWide(exe);
            row->grip = true;
            row->lifted = dragging_audio_ && drag_audio_id_ == id;
            row->on_grip = [this, id] { StartAudioDrag(id); };
            auto own = std::make_shared<ButtonControl>();
            own->label = L"Own track";
            own->in_row = true;
            own->enabled = !desktop_excluding;
            own->on_click = [this, exe] { PopOutAudioApp(exe); };
            auto mix = std::make_shared<ButtonControl>();
            mix->label = L"Mix into\x2026";
            mix->in_row = true;
            ButtonControl* mix_raw = mix.get();
            std::vector<std::pair<std::wstring, std::string>> targets;  // title, a member id
            for (size_t ti = 0; ti < tracks.size(); ++ti)
                targets.push_back({track_titles[ti], str_field(audio_sources_ui_[tracks[ti].members.front()], "id")});
            mix->on_click = [this, mix_raw, targets, id, anchor_of] {
                if (!host_) return;
                std::vector<std::wstring> labels;
                for (const auto& t : targets) labels.push_back(t.first);
                host_->ShowDropdown(anchor_of(*mix_raw), labels, -1, [this, targets, id](int i) {
                    if (i >= 0 && i < static_cast<int>(targets.size())) CommitAudioGroupDrop(id, targets[i].second);
                });
            };
            row->controls = {own, mix};
            card->rows.push_back(row);
            audio_rows_.push_back({id, row, true});
        }
        sec.AddRow(card);
    }

    // --- Saved audio: what every saved clip's audio is made of.
    {
        auto card = std::make_shared<CardContainer>();
        card->title = L"Saved audio";
        if (const FieldSpec* spec = FindSpecIn(specs_, "audio.full_mix_track")) {
            const auto* v = EffectiveConfigValue(spec->key);
            auto sw = std::make_shared<SwitchControl>();
            sw->id = spec->key;
            sw->on = v && v->type == ombra::JsonValue::Type::Bool && v->b;
            const FieldSpec s = *spec;
            sw->on_change = [this, s](bool on) { ApplySettingChange(s, ombra::JsonValue::MakeBool(on)); };
            controls_by_key_[spec->key] = sw;
            auto row = std::make_shared<SettingRow>();
            row->title = L"Combined track";
            row->help = L"Also save every source mixed into one extra track, for a quick upload.";
            row->controls = {sw};
            card->rows.push_back(row);
        }
        if (const FieldSpec* spec = FindSpecIn(specs_, "audio.bitrate_kbps")) {
            auto row = std::make_shared<SettingRow>();
            row->title = L"Bitrate";
            row->help = L"Kbps, per track.";
            row->controls = {MakeSegmented(*spec, [](const std::string& v) { return Utf8ToWide(v); })};
            card->rows.push_back(row);
        }
        sec.AddRow(card);
    }
    groups_.push_back(std::move(sec));
}

// The status line under a source: what audio_status says it is doing, or Off while switched off.
void SettingsScreen::StatusNote(const std::string& id, bool enabled, SettingRow* row) const {
    using Tone = SettingRow::Tone;
    AudioSourceStatus st = AudioSourceStatus::None;
    if (const auto it = audio_status_.find(id); it != audio_status_.end()) st = it->second;
    if (!enabled) st = AudioSourceStatus::Off;
    row->note_tone = Tone::Neutral;
    switch (st) {
        case AudioSourceStatus::Live:    row->note = L"Live"; row->note_tone = Tone::Good; break;
        case AudioSourceStatus::Quiet:   row->note = L"Quiet"; break;
        case AudioSourceStatus::Waiting: row->note = L"Not running"; break;
        case AudioSourceStatus::NoGame:  row->note = L"No game running"; break;
        case AudioSourceStatus::Failed:  row->note = L"Can't capture"; row->note_tone = Tone::Error; break;
        case AudioSourceStatus::Off:     row->note = L"Off"; break;
        case AudioSourceStatus::Paused:  row->note = L"Paused"; break;
        case AudioSourceStatus::None:    row->note.clear(); break;
    }
}

std::string SettingsScreen::EnsureAudioGroup(int idx) {
    const auto* g = audio_sources_ui_[idx].Find("group_id");
    const auto* idv = audio_sources_ui_[idx].Find("id");
    const std::string id = idv && idv->type == ombra::JsonValue::Type::String ? idv->str : std::string();
    std::string gid = g && g->type == ombra::JsonValue::Type::String ? g->str : std::string();
    // A source alone on its track has group_id == its id (or none). Mixing under that id would
    // make splitting it back out (group_id = id) a no-op, so the group gets an id of its own.
    if (gid.empty() || gid == id) {
        gid = "grp_" + id;
        audio_sources_ui_[idx].Set("group_id", ombra::JsonValue::MakeStr(gid));
    }
    return gid;
}

void SettingsScreen::MixAudioTrackInto(const std::string& from_gid, const std::string& to_member) {
    const int to = FindAudioSourceIndexById(to_member);
    if (to < 0) return;
    const std::string gid = EnsureAudioGroup(to);
    for (auto& s : audio_sources_ui_) {
        const auto* g = s.Find("group_id");
        const auto* idv = s.Find("id");
        const std::string sg = g && g->type == ombra::JsonValue::Type::String && !g->str.empty() ? g->str
                             : idv && idv->type == ombra::JsonValue::Type::String ? idv->str : std::string();
        if (sg == from_gid) s.Set("group_id", ombra::JsonValue::MakeStr(gid));
    }
    StageAudioSources();
}

void SettingsScreen::PopOutAudioApp(const std::string& exe) {
    audio_sources_ui_.push_back(NewAppSource(exe));
    const int di = FindAudioSourceIndexByKind("output_loopback");
    if (di >= 0) {
        // Process loopback can exclude one process tree per stream (audio.cpp), so Desktop leaves out
        // only this one; the button and the drop slot are gone while an exclusion is set.
        audio_sources_ui_[di].Set("mode", ombra::JsonValue::MakeStr("all_except"));
        audio_sources_ui_[di].Set("target", ombra::JsonValue::MakeStr(exe));
    }
    StageAudioSources();
}

void SettingsScreen::StartAudioDrag(const std::string& id) {
    // The moving copy: the row's text, raised (drawn by Draw above everything).
    drag_audio_ghost_.reset();
    for (const auto& r : audio_rows_) {
        if (r.id != id) continue;
        auto ghost = std::make_shared<SettingRow>();
        ghost->title = r.row->title;
        ghost->help = r.row->help;
        ghost->note = r.row->note;
        ghost->note_tone = r.row->note_tone;
        ghost->grip = true;
        ghost->hovered = true;
        drag_audio_ghost_ = ghost;
    }
    dragging_audio_ = true;
    drag_audio_id_ = id;
    drag_audio_hover_id_.clear();
    drag_audio_pt_ = last_mouse_pt_;
    RebuildGroupRows(current_dpi_scale_);  // fades the row and shows the drop slot
    if (host_) host_->RequestRepaint();
}

const ombra::JsonValue* SettingsScreen::EffectiveConfigValue(const std::string& key) const {
    if (auto it = pending_changes_.find(key); it != pending_changes_.end()) return &it->second;
    return current_config_.Find(key);
}

void SettingsScreen::ApplySettingChange(const FieldSpec& spec, const ombra::JsonValue& new_val) {
    // engine.autotune's "Run" action stays immediate, not staged -- it isn't a config value to
    // save/discard, it's a one-shot command (starts a bench run right now); staging it would mean
    // clicking "Run" does nothing until some later unrelated Save.
    if (spec.key == "engine.autotune") {
        if (host_) {
            ombra::JsonValue patch = ombra::JsonValue::MakeObject();
            patch.Set(spec.key, new_val);
            const uint64_t my_seq = ++set_config_seq_by_key_[spec.key];
            host_->RequestSetConfigAsync(
                patch, [this, spec, new_val, my_seq](std::optional<ombra::JsonValue> resp) {
                    const bool ok = resp && resp->type == ombra::JsonValue::Type::Object &&
                                    [&]() {
                                        const auto* v = resp->Find("ok");
                                        return v && v->type == ombra::JsonValue::Type::Bool && v->b;
                                    }();
                    if (ok && set_config_seq_by_key_[spec.key] == my_seq) current_config_.Set(spec.key, new_val);
                    RebuildGroupRows(current_dpi_scale_);
                    if (host_) host_->RequestRepaint();
                    // engine.autotune's bench window draws on the bare desktop and refuses to run
                    // at all if a fullscreen-shaped window already owns the foreground
                    // (capturebench.cpp's own safety gate) -- and this overlay covers the whole
                    // monitor while it's open, so it would trip that exact gate. Close it once the
                    // confirmed run has actually started; the result is latched engine-side
                    // (AutotuneContext) and surfaces via a modal the next time the overlay is
                    // opened, whenever that is.
                    if (ok && new_val.type == ombra::JsonValue::Type::String && new_val.str == "run" && host_) {
                        host_->CloseOverlay();
                    }
                });
        } else {
            current_config_.Set(spec.key, new_val);
        }
        return;
    }

    // Settings Save/Discard bar: stage instead of committing -- no pipe call, no
    // Realloc confirm modal, here. SaveAll() does both, once, for every staged key together.
    // An edit put back to the saved value is no edit: it leaves staging, and
    // the bar goes once nothing else is staged.
    save_error_.clear();
    // Not while a Save of this key is on its way: its reply moves the saved value, and this edit would
    // be gone. The reply settles it.
    if (const auto* saved = current_config_.Find(spec.key);
        saved && ombra::JsonEquals(*saved, new_val) && !keys_in_flight_.count(spec.key))
        pending_changes_.erase(spec.key);
    else
        pending_changes_[spec.key] = new_val;
    RecomputePendingWorstApply();
    SyncUiTheme();  // before the repaint below, or the edit only shows on the next redraw
    RebuildGroupRows(current_dpi_scale_);
    if (host_) host_->RequestRepaint();
    // What the staged config would cost -- the reservation line and memory card. Where clips go
    // and their names cost nothing, and the file name restages on every keystroke.
    if (spec.key.rfind("output.", 0) != 0) FetchPreviewAsync();
}

void SettingsScreen::RecomputePendingWorstApply() {
    pending_worst_apply_ = ApplyKind::Free;
    for (const auto& [key, val] : pending_changes_) {
        (void)val;
        auto it = std::find_if(specs_.begin(), specs_.end(), [&](const FieldSpec& s) { return s.key == key; });
        if (it != specs_.end()) pending_worst_apply_ = std::max(pending_worst_apply_, it->apply);
        // Under a preset the engine re-derives the bitrate from the new framerate or resolution, which
        // resizes the ring: confirm it as a Realloc even if Save beats SyncDerivedBitrateAsync's reply.
        if (key == "capture.framerate" || key == "capture.resolution") {
            const auto* preset_v = EffectiveConfigValue("encode.quality_preset");
            if (preset_v && preset_v->type == ombra::JsonValue::Type::String && preset_v->str != "custom")
                pending_worst_apply_ = ApplyKind::Realloc;
        }
    }
}

void SettingsScreen::ApplySettingChanges(const std::vector<std::pair<FieldSpec, ombra::JsonValue>>& changes) {
    for (const auto& c : changes) ApplySettingChange(c.first, c.second);
}

void SettingsScreen::SaveAll() {
    auto do_save = [this]() {
        if (!host_) {
            for (const auto& [key, val] : pending_changes_) current_config_.Set(key, val);
            pending_changes_.clear();
            pending_worst_apply_ = ApplyKind::Free;
            if (audio_sources_dirty_) {
                ombra::JsonValue arr = ombra::JsonValue::MakeArray();
                for (const auto& s : audio_sources_ui_) arr.arr.push_back(s);
                current_config_.Set("audio.sources", arr);
                current_config_.Set("audio.per_app_mode", ombra::JsonValue::MakeBool(audio_per_app_mode_));
                audio_sources_dirty_ = false;
            }
            return;
        }

        save_error_.clear();
        ombra::JsonValue patch = ombra::JsonValue::MakeObject();
        std::vector<std::pair<std::string, ombra::JsonValue>> staged(pending_changes_.begin(), pending_changes_.end());
        for (const auto& [key, val] : staged) patch.Set(key, val);
        const bool audio_included = audio_sources_dirty_;
        ombra::JsonValue sent_audio_sources = ombra::JsonValue::MakeArray();
        const bool sent_per_app_mode = audio_per_app_mode_;
        if (audio_included) {
            for (const auto& s : audio_sources_ui_) sent_audio_sources.arr.push_back(s);
            patch.Set("audio.sources", sent_audio_sources);
            patch.Set("audio.per_app_mode", ombra::JsonValue::MakeBool(sent_per_app_mode));
        }

        // Same per-key sequence guard ApplySettingChange used to take individually (screens.h's
        // set_config_seq_by_key_ comment) -- now one entry per staged key in this one batch.
        std::vector<uint64_t> my_seqs;
        my_seqs.reserve(staged.size());
        for (const auto& [key, val] : staged) {
            (void)val;
            my_seqs.push_back(++set_config_seq_by_key_[key]);
            ++keys_in_flight_[key];
        }
        uint64_t my_audio_generation = audio_sources_generation_;

        host_->RequestSetConfigAsync(
            patch, [this, staged, my_seqs, audio_included, my_audio_generation, sent_audio_sources, sent_per_app_mode]
                (std::optional<ombra::JsonValue> resp) {
                const bool engine_ok = resp && resp->type == ombra::JsonValue::Type::Object &&
                                       [&]() {
                                           const auto* v = resp->Find("ok");
                                           return v && v->type == ombra::JsonValue::Type::Bool && v->b;
                                       }();
                std::string error;
                if (resp) {
                    if (const auto* err = resp->Find("error"); err && err->type == ombra::JsonValue::Type::String) error = err->str;
                }
                // ok with an error: live now, but config.json wasn't written. The edits stay staged,
                // so Save tries the write again.
                const bool ok = engine_ok && error.empty();
                if (ok) {
                    for (size_t i = 0; i < staged.size(); ++i) {
                        if (set_config_seq_by_key_[staged[i].first] == my_seqs[i]) {
                            current_config_.Set(staged[i].first, staged[i].second);
                            // Controls stay live during the round trip: an edit made since Save is
                            // the user's next change and stays staged.
                            const auto now = pending_changes_.find(staged[i].first);
                            if (now != pending_changes_.end() && ombra::JsonEquals(now->second, staged[i].second))
                                pending_changes_.erase(now);
                        }
                    }
                    RecomputePendingWorstApply();
                    if (audio_included) {
                        // What was sent, not what's on screen: an edit since Save isn't saved, and
                        // Discard has to go back to this.
                        current_config_.Set("audio.per_app_mode", ombra::JsonValue::MakeBool(sent_per_app_mode));
                        current_config_.Set("audio.sources", sent_audio_sources);
                        // Same generation guard SaveAudioSources used to take (screens.h's
                        // audio_sources_generation_ comment): only clear dirty if nothing was staged
                        // since this Save was issued.
                        if (audio_sources_generation_ == my_audio_generation) audio_sources_dirty_ = false;
                    }
                } else if (resp) {
                    ombra::Log::Warn("set_config (batch) rejected: %s", error.c_str());
                    if (error.empty()) {
                        save_error_ = L"The engine refused these settings.";
                    } else {
                        save_error_ = Utf8ToWide(error);
                        save_error_[0] = static_cast<wchar_t>(towupper(save_error_[0]));
                    }
                } else {
                    ombra::Log::Warn("set_config (batch): no response from engine (unreachable?)");
                    save_error_ = L"The engine didn't answer. Your changes are still here, so try Save again.";
                }
                // An edit put back to the saved value during the round trip leaves staging only now,
                // against the value this reply settled.
                for (const auto& [key, val] : staged) {
                    (void)val;
                    if (const auto it = keys_in_flight_.find(key); it != keys_in_flight_.end() && --it->second <= 0)
                        keys_in_flight_.erase(it);
                    const auto now = pending_changes_.find(key);
                    const auto* saved = current_config_.Find(key);
                    if (!keys_in_flight_.count(key) && now != pending_changes_.end() && saved &&
                        ombra::JsonEquals(now->second, *saved))
                        pending_changes_.erase(now);
                }
                RecomputePendingWorstApply();
                FetchPreviewAsync();
                RebuildGroupRows(current_dpi_scale_);
                if (host_) host_->RequestRepaint();
            });
    };

    if (pending_worst_apply_ == ApplyKind::Realloc && host_) {
        std::wstring msg = L"This change requires reallocating the replay ring buffer. Any unsaved replay buffer will be dropped.\n\nDo you want to continue?";
        // No on_cancel rebuild-from-current_config_ here, unlike the old per-edit Realloc modal --
        // declining a Save must leave every staged edit exactly as it was (SaveAudioSources' own
        // "no on_cancel" precedent, screens.cpp), not discard it. The user can still hit Discard
        // separately.
        host_->ShowModal(L"Reallocate Replay Buffer", msg, /*is_danger=*/true, do_save, nullptr);
    } else {
        do_save();
    }
}

std::wstring PickFolderDialog(HWND owner, const std::wstring& initial) { return PickFolder(owner, initial); }

void SettingsScreen::SyncUiTheme() {
    if (!host_) return;
    host_->GetTheme().ApplyUiConfig([this](const char* key) { return EffectiveConfigValue(key); });
}

void SettingsScreen::DiscardAll() {
    ++bitrate_sync_generation_;  // an in-flight derived bitrate belongs to the edit being thrown away
    save_error_.clear();
    pending_changes_.clear();
    pending_worst_apply_ = ApplyKind::Free;
    SyncUiTheme();  // staged appearance edits were being previewed live; put them back
    if (audio_sources_dirty_) DiscardAudioSources();
    else {
        RebuildGroupRows(current_dpi_scale_);
        if (host_) host_->RequestRepaint();
    }
    FetchPreviewAsync();
}

void SettingsScreen::Layout(const D2D1_RECT_F& bounds, float dpi_scale) {
    bounds_ = bounds;

    const float gap = kPanelGapDip * dpi_scale;

    // The memory card is a sibling box: the host widened `bounds` by MemoryCardExtraWidth(), and
    // the card takes that slice on the side away from the rail; the panel gets the rest.
    D2D1_RECT_F row = bounds;
    memory_card_.visible = memory_card_slot_;
    if (memory_card_slot_) {
        const float cw = MemoryCard::Width(dpi_scale);
        if (memory_card_inside_) {  // attached panel: the host widened `bounds`, carve the slice out
            if (memory_card_left_) {
                memory_card_.bounds = D2D1::RectF(bounds.left, bounds.top, bounds.left + cw, bounds.bottom);
                row.left = bounds.left + cw + gap;
            } else {
                memory_card_.bounds = D2D1::RectF(bounds.right - cw, bounds.top, bounds.right, bounds.bottom);
                row.right = bounds.right - cw - gap;
            }
        } else if (memory_card_left_) {  // floating: hangs off the outside, row untouched
            memory_card_.bounds = D2D1::RectF(bounds.left - gap - cw, bounds.top, bounds.left - gap, bounds.bottom);
        } else {
            memory_card_.bounds = D2D1::RectF(bounds.right + gap, bounds.top, bounds.right + gap + cw, bounds.bottom);
        }
        UpdateMemoryCard();
    }

    // One panel: the rail at its measured size on the side
    // ui.rail_position names, then the content column -- the page's sub-tabs, the page, and the
    // Save bar at the foot. The Save bar's height is taken from the page, so it never covers it.
    card_bounds_ = row;
    static const Theme kNoTheme;
    const Theme& t = host_ ? host_->GetTheme() : kNoTheme;
    const LayoutContext ctx{dpi_scale, &t};
    const float pad = t.size.space_xl() * dpi_scale, space = t.size.space_md() * dpi_scale;
    D2D1_RECT_F col = D2D1::RectF(row.left + pad, row.top + pad, row.right - pad, row.bottom - pad);
    const RailPosition side = t.rail_position;
    rail_.horizontal = side == RailPosition::Top || side == RailPosition::Bottom;
    const D2D1_SIZE_F rs = rail_.Measure(LayoutConstraints{}, ctx);
    switch (side) {
        case RailPosition::Right:
            rail_.Arrange(D2D1::RectF(col.right - rs.width, col.top, col.right, col.top + rs.height), ctx);
            col.right -= rs.width + pad;
            break;
        case RailPosition::Top:
            rail_.Arrange(D2D1::RectF(col.left, col.top, col.left + rs.width, col.top + rs.height), ctx);
            col.top += rs.height + space;
            break;
        case RailPosition::Bottom:
            rail_.Arrange(D2D1::RectF(col.left, col.bottom - rs.height, col.left + rs.width, col.bottom), ctx);
            col.bottom -= rs.height + space;
            break;
        default:
            rail_.Arrange(D2D1::RectF(col.left, col.top, col.left + rs.width, col.top + rs.height), ctx);
            col.left += rs.width + pad;
            break;
    }
    const float edge = col.right - kScrollbarRoomDip * dpi_scale;
    if (!tabs_.tabs.empty()) {
        const float th = tabs_.Measure(LayoutConstraints::Available(edge - col.left), ctx).height;
        tabs_.Arrange(D2D1::RectF(col.left, col.top, edge, col.top + th), ctx);
        col.top += th + space;
    }
    save_bar_.visible = HasPendingChanges();
    if (save_bar_.visible) {
        save_bar_.failed = !save_error_.empty();
        save_bar_.title = save_bar_.failed ? L"Couldn't save" : L"Unsaved changes";
        save_bar_.detail = save_bar_.failed ? save_error_
                         : pending_worst_apply_ == ApplyKind::Realloc ? L"Saving reallocates the replay buffer." : L"";
        const float bh = save_bar_.Measure(LayoutConstraints::Available(edge - col.left), ctx).height;
        save_bar_.Arrange(D2D1::RectF(col.left, col.bottom - bh, edge, col.bottom), ctx);
        col.bottom -= bh + space;
    }
    scroll_area_.bounds = col;

    // Layout() runs on every RenderFrame (every repaint, not just on real geometry changes) --
    // only rebuild the row/control widgets when something that actually affects them changed.
    // Rebuilding unconditionally here used to destroy and recreate every widget on every frame,
    // silently discarding transient state a click had just set (e.g. a text field's caret
    // right after the user clicked into it -- the very next repaint wiped it back to
    // unfocused before it could ever be drawn).
    const bool bounds_changed = rows_dirty_ ||
        scroll_area_.bounds.left != last_rebuild_bounds_.left ||
        scroll_area_.bounds.top != last_rebuild_bounds_.top ||
        scroll_area_.bounds.right != last_rebuild_bounds_.right ||
        scroll_area_.bounds.bottom != last_rebuild_bounds_.bottom ||
        dpi_scale != last_rebuild_dpi_scale_;
    if (bounds_changed) {
        RebuildGroupRows(dpi_scale);
    } else {
        current_dpi_scale_ = dpi_scale;
    }
}

void SettingsScreen::Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) {
    {
        const paint::Paint p{dc, theme, dpi_scale};
        const float r = theme.size.radius_panel() * dpi_scale;
        p.Fill(card_bounds_, r, theme.color.panel);
        p.CardEdge(card_bounds_, r);
    }

    if (memory_card_.visible) {
        memory_card_.bounds.bottom = (std::min)(memory_card_.bounds.bottom,
            memory_card_.bounds.top + memory_card_.ContentHeight(dc, theme, dpi_scale));
    }
    memory_card_.Draw(dc, theme, dpi_scale);
    rail_.Draw(dc, theme, dpi_scale);
    if (!tabs_.tabs.empty()) tabs_.Draw(dc, theme, dpi_scale);
    scroll_area_.Draw(dc, theme, dpi_scale);
    save_bar_.Draw(dc, theme, dpi_scale);  // draws nothing while hidden

    // The audio row being dragged, raised under the pointer, above the page and its clip.
    if (dragging_audio_ && drag_audio_ghost_) {
        const paint::Paint p{dc, theme, dpi_scale};
        const LayoutContext ctx{dpi_scale, &theme};
        const float w = 360.0f * dpi_scale;
        const float h = drag_audio_ghost_->Measure(LayoutConstraints::Exact(w), ctx).height;
        const float x = drag_audio_pt_.x - 2.0f * theme.size.card_padding() * dpi_scale, y = drag_audio_pt_.y - h * 0.5f;
        const auto box = D2D1::RectF(x, y, x + w, y + h);
        const float rad = theme.size.radius_card() * dpi_scale;
        drag_audio_shadow_.Draw(dc, theme, dpi_scale, box, rad);
        p.Fill(box, rad, theme.color.raised);
        p.Stroke(box, rad, theme.color.line_strong);
        drag_audio_ghost_->Arrange(box, ctx);
        drag_audio_ghost_->Draw(dc, theme, dpi_scale);
    }
}

bool SettingsScreen::OnMouseMove(D2D1_POINT_2F pt) {
    last_mouse_pt_ = pt;
    if (dragging_audio_) {
        drag_audio_pt_ = pt;
        D2D1_POINT_2F content_pt = pt;
        content_pt.y += scroll_area_.scroll_y;
        drag_audio_hover_id_.clear();
        for (const auto& t : audio_track_cards_) {
            // Its own track is no target: the row is already there.
            const bool own = std::any_of(t.card->rows.begin(), t.card->rows.end(), [&](const std::shared_ptr<Widget>& r) {
                const auto row = std::dynamic_pointer_cast<SettingRow>(r);
                return row && row->lifted;
            });
            t.card->drop_target = !own && drag_audio_hover_id_.empty() && PointIn(t.card->bounds, content_pt);
            if (t.card->drop_target) drag_audio_hover_id_ = t.member_id;
        }
        if (audio_drop_slot_) {
            audio_drop_slot_->active = drag_audio_hover_id_.empty() && PointIn(audio_drop_slot_->bounds, content_pt);
            if (audio_drop_slot_->active) drag_audio_hover_id_ = "new";
        }
        return true;
    }
    bool rep = rail_.OnMouseMove(pt);
    if (!tabs_.tabs.empty()) rep |= tabs_.OnMouseMove(pt);
    rep |= save_bar_.OnMouseMove(pt);
    rep |= scroll_area_.OnMouseMove(pt);
    return rep;
}

CursorKind SettingsScreen::CursorAt(D2D1_POINT_2F pt) const {
    if (dragging_audio_) return CursorKind::Move;
    if (rail_.CursorAt(pt) != CursorKind::Arrow) return CursorKind::Hand;
    if (!tabs_.tabs.empty() && tabs_.CursorAt(pt) != CursorKind::Arrow) return CursorKind::Hand;
    if (save_bar_.CursorAt(pt) != CursorKind::Arrow) return CursorKind::Hand;
    return scroll_area_.CursorAt(pt);
}

bool SettingsScreen::OnMouseDown(D2D1_POINT_2F pt) {
    if (auto tf = template_field_.lock(); tf && tf->focused) {
        D2D1_POINT_2F scrolled = pt;
        scrolled.y += scroll_area_.scroll_y;
        if (!tf->HitTest(scrolled)) EndTemplateEdit(false);
    }
    // Same for a recording shortcut row: Save, Discard and the tabs never route a click to it, and
    // their rebuild would otherwise put it straight back into recording.
    if (auto row = recording_row_.lock(); row && row->Recording()) {
        D2D1_POINT_2F scrolled = pt;
        scrolled.y += scroll_area_.scroll_y;
        if (!row->HitTest(scrolled)) row->CancelRecording();
    }
    if (rail_.OnMouseDown(pt)) return true;
    if (!tabs_.tabs.empty() && tabs_.OnMouseDown(pt)) return true;
    if (save_bar_.OnMouseDown(pt)) return true;
    if (scroll_area_.OnMouseDown(pt)) {
        ApplyQueuedNavigate();
        return true;
    }
    return false;
}

void SettingsScreen::ApplyQueuedNavigate() {
    const auto [rail, subtab] = queued_nav_;
    queued_nav_ = {-1, -1};
    if (rail < 0) return;
    ShowPage(rail, subtab >= 0 ? subtab : subtab_[static_cast<size_t>(rail)]);
    if (host_) host_->RequestRepaint();
}

bool SettingsScreen::OnMouseUp(D2D1_POINT_2F pt) {
    if (dragging_audio_) {
        const std::string dragged = drag_audio_id_, target = drag_audio_hover_id_;
        dragging_audio_ = false;
        drag_audio_id_.clear();
        drag_audio_hover_id_.clear();
        drag_audio_ghost_.reset();
        static const char kLive[] = "live:";
        if (target == "new" && dragged.rfind(kLive, 0) == 0) {
            PopOutAudioApp(dragged.substr(sizeof(kLive) - 1));
        } else if (target == "new") {
            // Out of a mix: its own track again.
            if (const int idx = FindAudioSourceIndexById(dragged); idx >= 0) {
                audio_sources_ui_[idx].Set("group_id", ombra::JsonValue::MakeStr(dragged));
                StageAudioSources();
            }
        } else if (!target.empty()) {
            CommitAudioGroupDrop(dragged, target);
        } else {
            RebuildGroupRows(current_dpi_scale_);  // the row comes back, the slot goes
            if (host_) host_->RequestRepaint();
        }
        return true;
    }
    rail_.OnMouseUp(pt);
    tabs_.OnMouseUp(pt);
    save_bar_.OnMouseUp(pt);
    scroll_area_.OnMouseUp(pt);
    return false;
}

bool SettingsScreen::OnChar(wchar_t ch) {
    // Held while the field restages and the page rebuilds under it.
    if (auto tf = template_field_.lock(); tf && tf->focused) {
        if (!tf->OnChar(ch)) return false;
        if (host_) host_->RequestRepaint();
        return true;
    }
    return false;
}

bool SettingsScreen::OnKeyDown(UINT vk) {
    if (auto tf = template_field_.lock(); tf && tf->focused) {
        if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_TAB) EndTemplateEdit(vk == VK_ESCAPE);
        else if (!tf->OnKeyDown(vk)) return false;
        if (host_) host_->RequestRepaint();
        return true;
    }
    if (vk == VK_ESCAPE) {
        if (host_) host_->SwitchScreen(ActiveScreen::Home);
        return true;
    }
    if (scroll_area_.OnKeyDown(vk)) return true;
    return false;
}

void SettingsScreen::OnMouseWheel(float delta) {
    if (auto sent = sent_list_.lock(); sent && PointIn(scroll_area_.bounds, last_mouse_pt_)) {
        const D2D1_POINT_2F content_pt = D2D1::Point2F(last_mouse_pt_.x, last_mouse_pt_.y + scroll_area_.scroll_y);
        if (sent->OnWheel(content_pt, delta)) {
            if (host_) host_->RequestRepaint();
            return;
        }
    }
    if (auto list = games_list_.lock(); list && PointIn(scroll_area_.bounds, last_mouse_pt_)) {
        const D2D1_POINT_2F content_pt = D2D1::Point2F(last_mouse_pt_.x, last_mouse_pt_.y + scroll_area_.scroll_y);
        if (list->OnWheel(content_pt, delta)) {
            scroll_area_.OnMouseMove(last_mouse_pt_);  // the rows moved under a still mouse
            if (host_) host_->RequestRepaint();
            return;
        }
    }
    scroll_area_.OnMouseWheel(delta);
    // The page moved under a still mouse: hover (and its tooltip, or a drag's target) follows what
    // is under it now.
    if (dragging_audio_) OnMouseMove(last_mouse_pt_);
    else scroll_area_.OnMouseMove(last_mouse_pt_);
}

bool SettingsScreen::QueryHoverTooltip(std::wstring* text_out, D2D1_POINT_2F* pos_out) const {
    // The rail sits outside the scroll area and never scrolls, so its anchor needs no adjustment.
    {
        D2D1_POINT_2F anchor{};
        if (rail_.QueryTooltip(text_out, &anchor)) {
            if (pos_out) *pos_out = anchor;
            return true;
        }
    }
    for (const auto& child : scroll_area_.children) {
        D2D1_POINT_2F anchor{};
        if (child && child->QueryTooltip(text_out, &anchor)) {
            if (pos_out) {
                // The anchor is in the scroll area's absolute (unscrolled) coordinate space (widget
                // bounds are built that way -- see RebuildGroupRows); the visible screen position
                // subtracts scroll_y, same as ScrollAreaWidget::Draw's own translate. The caller
                // (OverlayHost) draws the tooltip in screen space, so return it already adjusted.
                anchor.y -= scroll_area_.scroll_y;
                *pos_out = anchor;
            }
            return true;
        }
    }
    return false;
}

}  // namespace ombra_ui
