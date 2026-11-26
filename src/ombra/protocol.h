// The engine's pipe protocol: two surfaces on one pipe, chosen by the hello.
//
// This header names the wire contract; it implements nothing. pipe.h/.cpp carries the bytes and
// owns the hello, engine_pipe.cpp implements the handlers (kPipeCommands, kApiCommands).
//
// Transport: newline-delimited compact JSON (json.h's WriteJsonCompact — never the pretty
// printer, whose embedded real newlines would be mistaken for message boundaries). Every message
// is a JSON object. A request carries "type" and an "id" (number or string, chosen by the client)
// that the response echoes unchanged, along with the request's "type", "ok": bool, and either the
// type-specific fields or "error" (a stable code) plus "message" (human text). A line the engine
// can't parse gets {"type":"error","ok":false,"error":"bad_message",...} and the connection closes.
//
// The first message picks the surface; anything else first is refused and the connection closed.
//
// Public API (other apps, the CLI). Versioned: adding fields, types or events keeps the version;
// clients ignore what they don't know. Only a breaking change raises kApiVersionMax.
//
//   -> {"type":"hello","id":1,"api":1,"app":"LeagueTool"}
//   <- {"type":"hello","id":1,"ok":true,"api":1,"engine":"0.1.3"}
//   <- {"type":"hello","id":1,"ok":false,"error":"unsupported_version","message":"...",
//       "min":1,"max":1}
//
//   "app" (required, 1-64 bytes of UTF-8, no control characters) names the caller. Every request
//   after the hello needs an "id"; a line over kMaxApiMessageBytes closes the connection, as does
//   no hello within kHelloTimeoutMs. At most kMaxApiClients apps connect at once; Ombra's own
//   command line has kMaxCommandLineClients places of its own.
//   Error codes: bad_message, bad_request, unsupported_version, not_allowed, too_many_connections,
//   unknown_type, plus each command's own.
//
//   status          -> {}
//                   <- {"replay":bool (the Instant Replay switch), "buffering":bool (footage is
//                       being kept right now), "game":string|null, "buffer_seconds":number,
//                       "recordings":[{"recording":number, "seconds":number}] (the caller's own
//                       running recordings; only Ombra's command line has any until consent exists)}
//
//   clip            -> {"before"?:number (seconds before now; default buffer_seconds, capped at it),
//                       "after"?:number (seconds after now; default 0, capped at 120),
//                       "title"?:string, "wait"?:bool}
//                   <- {"clip":number (this engine run's id), "extended":bool}
//                      at once; with "wait" (or when every monitor refused at once) also
//                      "path":string|null (the first file), "monitors":[{"monitor", "reason",
//                      "path" (null unless saved), "fell_back", "seconds"}] once the files are
//                      written. A waiting connection answers nothing else until then.
//                      Errors: bad_request, not_allowed (only Ombra's command line,
//                      ombra.com/ombra.exe beside the engine, until consent exists), replay_off,
//                      nothing_new, busy (the buffer is being resized: retry), failed (also while
//                      the engine exits). A request while the same app's clip is still taking
//                      footage extends that clip to now + after ("extended", same id, one file);
//                      its title and before are ignored.
//
//   marker          -> {"label"?:string (at most 64 characters; control characters dropped),
//                       "kind"?:"generic"|"kill"|"death"|"assist"|"win"|"loss"|"objective"|
//                       "highlight"|"round_start"|"round_end" (default generic),
//                       "at"?:number (seconds from now, -buffer_seconds to 0; default 0),
//                       "important"?:bool}
//                   <- {"marker":number (this engine run's id)}
//                      Every clip covering the moment gets it: an MP4 chapter ("label · app",
//                      or the type's name without a label) and an entry in the file's "markers"
//                      tag, beside Ombra's own (kinds clip_saved, game_start, game_end; app
//                      "Ombra"), which apps can't place. Errors: bad_request, not_allowed (as clip), replay_off (no footage
//                      covers the moment), too_many_markers (256 per app in the footage held).
//
//   replay          -> {"replay":bool}
//                   <- {"replay":bool (the switch now), "changed":bool}
//                      The Instant Replay switch; off discards the buffer. A change shows a toast
//                      naming the app. Errors: bad_request, not_allowed (only Ombra's command line:
//                      no app may switch it until consent exists).
//
//   record_start    -> {"before"?:number (seconds before now; default 0, capped at buffer_seconds),
//                       "title"?:string (in the file or folder name)}
//                   <- {"recording":number (this engine run's id)}
//                      Records every monitor until record_stop (or the disk runs low), a file each
//                      (a new "part" after a resolution change); answered once the first file
//                      opens. Runs with Instant Replay off too: then nothing before the start.
//                      Errors: bad_request, not_allowed (only Ombra's command line until consent
//                      exists), already_recording (one per app; "recording" names it),
//                      nothing_new (no footage within 10 s), busy, failed.
//   record_stop     -> {"recording"?:number (default: the app's latest)}
//                   <- {"recording":number, "end":"stopped"|"disk_low"|"write_failed"|"no_footage",
//                       "seconds":number, "paths":[string], "monitors":[{"monitor":string,
//                       "reason":string, "end":string, "paths":[string], "fell_back":bool,
//                       "seconds":number}]}
//                      Answered once every file is finished; also for one that already ended
//                      (disk low), which it then forgets. An app stops only its own. Errors:
//                      bad_request, not_recording, and the ended recording's nothing_new or
//                      failed (with the fields above: what was written is kept).
//
//   subscribe       -> {"events"?:[string] (default: every event)}
//                   <- {"events":[string] (the names this engine raises, in the order below;
//                       unknown names are left out, not refused)}
//                      Replaces what this connection hears; [] hears nothing. Answered by the
//                      transport (PipeServer), not the API handler. Events then arrive between
//                      responses, unasked, as {"type":"event","event":name, ...fields}:
//                        replay      {"replay":bool}       the Instant Replay switch changed
//                        buffering   {"buffering":bool}    footage started or stopped being kept
//                        game_start  {"game":string}       a game began (no debounce)
//                        game_end    {"game":string}       that game ended
//                        clip_saved  {"app":string|null (null: the user's own save), "clip"?:number
//                                     (an app's clip id), "seconds":number, "paths"?:[string]
//                                     (only for Ombra's command line until consent exists)}
//                        recording_started {"app":string, "recording":number}
//                        recording_stopped {"app":string, "recording":number, "reason":string,
//                                     "end":string, "seconds":number, "paths"?:[string] (as clip_saved)}
//                        exiting     {}                    the engine is shutting down
//                      A connection more than kMaxQueuedEvents behind, or one whose events can't
//                      be written within 5 s, is closed (catch up with status after reconnecting).
//
// Internal (ombra-ui only): {"type":"hello","id":1,"v":kProtocolVersion}, accepted only from
// ombra-ui.exe beside the engine (PipeServer's internal gate), so apps can't reach set_config,
// shutdown or update_install around the public API. Exact version match: both ship together.
//
//   -> {"type":"hello","id":1,"v":2}
//   <- {"type":"hello","id":1,"ok":true,"v":2}
//
// Internal message types:
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

#include <cstddef>

namespace ombra {

inline constexpr int kProtocolVersion = 2;  // the internal surface
inline constexpr int kApiVersionMin   = 1;  // the public API versions this engine speaks
inline constexpr int kApiVersionMax   = 1;
inline constexpr size_t kMaxApiMessageBytes = 64 * 1024;
inline constexpr int    kMaxApiClients      = 16;    // app connections at once; the overlay doesn't count
inline constexpr int    kMaxCommandLineClients = 16;  // Ombra's own command line's, apart from the apps'
inline constexpr unsigned kHelloTimeoutMs   = 5000;  // a connection silent this long before its hello closes

enum class PipeSurface { kInternal, kApi };

// The pipe's name is per instance: paths.h's Instance().pipe_name.

// Win32 window class of ombra-ui's overlay window (overlay_window.cpp). Shared here, not
// file-local to overlay_window.cpp, because the engine needs it too: FindWindowW(...) is how a
// hotkey handler in ombra.exe (main.cpp) discovers whether an overlay is already on screen before
// deciding to spawn a new ombra-ui.exe or close the existing one — see Tray::ToggleOverlay.
inline constexpr const wchar_t* kOverlayWindowClassW = L"OmbraOverlayWindowClass";

}  // namespace ombra
