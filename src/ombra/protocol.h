// The ombra.exe <-> ombra-ui.exe pipe protocol.
//
// This header names the wire contract; it implements nothing. pipe.h/.cpp carries the bytes,
// engine_pipe.cpp implements the handlers (kPipeCommands), ombra-ui is the client.
// Versioned from message one: this pipe is the extension boundary too.
//
// Transport: newline-delimited compact JSON (json.h's WriteJsonCompact — never the pretty
// printer, whose embedded real newlines would be mistaken for message boundaries). Every message
// is a JSON object. A request carries "type" and an "id" the response echoes back, so a client
// with multiple requests in flight can match responses without serializing them one at a time.
// A response additionally carries "ok": bool, and either the type-specific payload fields or an
// "error" string.
//
// The first message on every connection must be a "hello" carrying "v": kProtocolVersion.
//
//   -> {"type":"hello","id":1,"v":2}
//   <- {"type":"hello","id":1,"ok":true,"v":2,"engine_version":"..."}
//
// Message types:
//
//   get_schema      -> {}
//                   <- {"settings":[{"key":..., "kind":"bool"|"int"|"double"|"string"|"enum"|"raw",
//                                    "title":..., "help":..., "tradeoff":..., "tier":"preset"|
//                                    "common"|"advanced", "group":..., "presets":..., "unit":...,
//                                    "apply":"free"|"rebuild"|"realloc", "min":..., "max":...,
//                                    "allowed":"a|b|c", "implemented":bool}, ...]}
//                      Serializes config.h's AllSettings() verbatim — static data, no engine state.
//
//   get_config      -> {}
//                   <- {"config": {<dotted-key>: <value>, ...}}
//
//   set_config      -> {"config": {<dotted-key>: <value>, ...}}
//                   <- {"accepted":[<key>, ...], "rejected":[{"key":..., "reason":...}, ...],
//                       "apply_tier":"free"|"rebuild"|"realloc", "buffer_dropped":bool,
//                       "note":"..."}
//
//   preview_config  -> {"config": {<dotted-key>: <value>, ...}}
//                   <- {"apply_tier":"free"|"rebuild"|"realloc", "ring_bytes":number,
//                       "mb_per_minute":number, "derived":{<key>:<val>, ...},
//                       "warnings":[string, ...]}
//                      Calculates consequences of applying a candidate patch before doing so.
//
//   get_status      -> {}
//                   <- {"enabled":bool (the Instant Replay switch), "armed":bool (recording now),
//                       "mode":"desktop"|"game", "target":"monitor"|"game"|"window", "current_game":string|null,
//                       "buffer_seconds":number, "last_save_path":string|null,
//                       "last_save_time_qpc":number|null, "rss_ws_mb":number, "rss_private_mb":number,
//                       "gpu_topology":"T1".."T6"|"Unknown", "gpu_topology_desc":string,
//                       "gpu_display_owner":string|null, "gpu_nvidia_present":bool}
//
//   save_now        -> {}
//                   <- {"saved":bool, "path":string|null, "error":string|null,
//                       "reason":"saved"|"nothing_new"|"failed"|"busy"}
//                      Also returns monitors:[{monitor,reason,path}] for every configured monitor.
//                      saved means at least one file was saved; reason is failed if any monitor
//                      failed or was busy. path is the first saved file, or null. Uses the same
//                      grouped save and optional toast as the hotkey. Never automatically retry:
//                      a lost response does not mean the files were not written.
//                      nothing_new: under 1 s of footage since the last truncate save or engine
//                      start, so no file was written.
//
//   screenshot      -> {}
//                   <- {"ok":bool, "error":string|null}
//
//   set_armed       -> {"armed":bool}
//                   <- {"ok":bool, "armed":bool}
//
//   notify          -> {"title":string, "message":string}
//                   <- {"ok":true}
//
//   list_monitors   -> {}   (Settings monitor picker -- capture.monitors)
//                   <- {"ok":true, "monitors":[{"device":string, "label":string, "left":number,
//                                               "top":number, "width":number, "height":number,
//                                               "primary":bool}, ...]}   (physical pixels)
//
//   list_windows    -> {}   (Settings window-target picker: title, process, image_path, minimized,
//                            foreground, covers_monitor, area, hwnd, is_game, not_game per window)
//                   <- {"ok":true, "windows":[{"title":string, "process":string}, ...]}
//
//   list_processes  -> {}   (the audio "Add app" picker)
//                   <- {"ok":true, "processes":[{"name":string, "path":string}, ...]}
//
//   list_games      -> {"rescan"?:bool}   (the Games page; game_library.h)
//                   <- {"ok":true, "games":[{"name", "path", "source", "game":bool,
//                       "detected":bool}, ...], "seen":[{"name", "path", "last_seen"}, ...]}
//
//   audio_status    -> {}   (Settings Audio page, polled ~1 s while visible; no config lock)
//                   <- {"ok":true, "sources":[{"id":string, "state":"live"|"quiet"|"waiting"|
//                                              "no_game"|"failed"}, ...]}
//                      One entry per recording source (audio.sources after filtering), by id.
//                      A configured source missing here isn't recording (disabled, or staged
//                      and not yet saved).
//
//   shutdown        -> {}
//                   <- {"ok":true} | {"ok":false, "error":string}
//                      Resident engine only. Exits the way tray Exit and Ctrl+C do: in-flight
//                      saves finish, then the pipe server stops. The response is sent first; the
//                      connection then closes, and the caller waits on the process to know it's gone.
#pragma once

namespace ombra {

inline constexpr int kProtocolVersion = 2;

// The pipe's name is per instance: paths.h's Instance().pipe_name.

// Win32 window class of ombra-ui's overlay window (overlay_window.cpp). Shared here, not
// file-local to overlay_window.cpp, because the engine needs it too: FindWindowW(...) is how a
// hotkey handler in ombra.exe (main.cpp) discovers whether an overlay is already on screen before
// deciding to spawn a new ombra-ui.exe or close the existing one — see Tray::ToggleOverlay.
inline constexpr const wchar_t* kOverlayWindowClassW = L"OmbraOverlayWindowClass";

}  // namespace ombra
