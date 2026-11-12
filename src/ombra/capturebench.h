// Tier B on-device benchmark. Draws on screen, so it is NOT run
// automatically by anything: it exists only as `ombra.exe capturebench`, invoked by a person who
// is at the keyboard and expecting a window to appear. See capturebench.cpp's top
// comment for the full safety contract before touching this file.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace ombra {

// Runs the end-to-end capture-pipeline latency benchmark. Returns 0 on a completed run (whether or
// not it drew anything — a safety-gate refusal is still success, not failure, from the shell's
// point of view: refusing correctly is the point), non-zero on an actual error. Uses the caller's
// loaded encoder options, output-size policy and first configured monitor. Measures one desktop
// pipeline with a small test pattern, not game load or the cost of all monitors. It can suggest a
// faster preset; it does not determine device-specific setup or measure visual quality.
// The caller runs the AMF probe first (SetAmfAdapterCaps).
struct Config;
int RunCaptureBench(const Config& cfg, const std::string& monitor_device);

// Path to the JSON result file RunCaptureBench writes on every exit path (refusal excepted —
// nothing ran) — currently "%LOCALAPPDATA%\Ombra\autotune_result.json". Shared so the engine's
// autotune-completion watcher (main.cpp) reads the exact path this writes, with no second
// hardcoded constant to drift out of sync.
std::wstring AutotuneResultFilePath();

// The fullscreen-foreground gate, exported because game-mode
// arming needs the exact same "is this a real fullscreen app, not the desktop" test — Progman/
// WorkerW exclusion included. `out_label`, if non-null, gets that process's full image path
// (best-effort; "(unknown process)" if it couldn't be queried). One definition, two callers, so
// the two never drift apart on what "fullscreen" means.
//
// `window` selects which window is being asked about: `nullptr` (capturebench's own use) means
// "the current foreground window, right now" (GetForegroundWindow()) — correct for a synchronous
// gate check immediately before drawing. An explicit HWND (controller.cpp's use) answers the
// question for *that* window specifically, which is the one a SetWinEventHook
// EVENT_SYSTEM_FOREGROUND callback actually reported — by the time a queued event reaches the
// worker thread, GetForegroundWindow() may already disagree with what the event was about
// (advisor review: mixing the two silently attributed one window's fullscreen-ness to a
// different window's pid).
bool ForegroundLooksFullscreen(HWND window = nullptr, std::wstring* out_label = nullptr);

}  // namespace ombra
