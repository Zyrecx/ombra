// Where one Ombra instance keeps its files, and the names other processes reach it by.
//
// OMBRA_HOME unset: the installed layout. Config under %APPDATA%\Ombra; logs, telemetry, thumbs,
// autotune_result.json and the default ring.dat under %LOCALAPPDATA%\Ombra; pipe
// \\.\pipe\OmbraEngine. OMBRA_HOME set to a folder (tests): all of it lives in that folder, and the
// pipe and single-instance mutex names get a suffix derived from the folder, so an isolated engine
// and its UI never meet the real ones. One variable on purpose: separate overrides allow a
// half-isolated instance (own config, real pipe) that fights the real engine.
//
// Child processes inherit the environment, so a self-restart, ombra-ui spawning an engine, and
// toasts all stay inside the same instance without forwarding anything.
#pragma once

#include <cstdint>
#include <string>

namespace ombra {

struct InstancePaths {
    std::wstring config_dir;      // config.json, first_run_done. Empty if unresolvable.
    std::wstring data_dir;        // logs\, telemetry\, thumbs\, autotune_result.json, ring.dat
    std::wstring pipe_name;
    std::wstring instance_mutex;
    bool         isolated = false;  // OMBRA_HOME is set: leave machine-wide state (the Run key) alone
};

// `home` empty = the installed layout. The same folder spelled differently (case, trailing slash,
// relative) resolves to the same names.
InstancePaths ResolveInstancePaths(const std::wstring& home);

// This process's instance: OMBRA_HOME is read on the first call and fixed for the process lifetime,
// because the instance mutex and every pipe instance the accept loop creates must share one name.
const InstancePaths& Instance();

// The engine's last preview grab of a monitor (`device` as MonitorSpec carries it) or a window, in
// data_dir	humbs. Stable names, overwritten by each grab, so a freshly spawned ombra-ui can draw the
// previous grab the moment Settings opens instead of a placeholder. Empty if data_dir is unresolvable.
// Where a clip goes when its output folder can't take it (a removed drive, a full disk): the default
// output folder, or inside OMBRA_HOME for an isolated instance. Empty if unresolvable.
std::wstring FallbackClipsDir();

// The marker that says someone answered the setup to the end, and so chose a diagnostics level.
// Stricter than first_run_done, which also appears once the setup stops asking. Empty if unresolvable.
std::wstring SetupCompletedPath();
bool SetupCompleted();

std::wstring ThumbsDir();
std::wstring MonitorThumbPath(const std::string& device);
std::wstring WindowThumbPath(uint64_t hwnd);

}  // namespace ombra
