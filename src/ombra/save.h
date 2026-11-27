// The save path: pin the video ring and every audio ring over [t_end - buffer_seconds, t_end),
// build the audio tracks (grouping/mixing by group_id, gain, optional full-mix track), and mux.
#pragma once

#include "ombra/reconfig.h"

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace ombra {

enum class SaveOutcome {
    kSaved,
    // Under kMinClipSeconds of footage in the window: right after a truncate save (a double press)
    // or right after the engine starts. Nothing is written and no barrier is dropped, so the next
    // real save keeps that footage.
    kNothingNew,
    kFailed,  // output folder or file write failed; see the log
    kBusy,
};

inline constexpr double kMinClipSeconds = 1.0;

const char* SaveOutcomeName(SaveOutcome outcome);

// `path` ending in `ext` (".mp4"). Only a trailing .mp4 on the file name itself is
// replaced: a template without an extension, a dotted game name or a dotted folder name
// (C:\Users\john.doe) must never lose text.
std::wstring WithContainerExtension(const std::wstring& path, const std::wstring& ext);

// One file-name component with every character Windows rejects (<>:"/\|?* and controls) replaced
// by '_'. output.filename is user-typed and {game} comes from a process name.
std::wstring SanitizeFileName(std::wstring name);

// Room a clip of `video_bytes` asks the disk for: the margin covers the audio and the MP4's index.
inline uint64_t ClipRoomNeeded(uint64_t video_bytes) { return video_bytes + video_bytes / 8 + (16u << 20); }

// Local wall-clock time of `qpc` (a hotkey press, a tool's event), so a clip is named for when it was
// asked for, not for when the save reached naming it. Outside the last ten minutes (a synthetic test
// timeline) it falls back to now.
SYSTEMTIME LocalTimeAtQpc(int64_t qpc);

// A tool's clip title as it goes into a file or folder name: sanitized, at most 64 characters
// (SanitizeFolderName's cap), so a long one can't push the name past NTFS's limit.
std::wstring ClipTitleForName(const std::wstring& title);

// Where a clip asked for at `t_end` is written: the output.filename template under output.root (or
// ctx->out_path when a test set one), never over an existing file or a name another save or clip
// holds (`reservation`, until the destination is destroyed). `group_folder_name` as for DoSave;
// `title`, when non-empty, ends a single file's name. When output.root can't take `room_needed`
// bytes, ctx->fallback_root (`fell_back`). Empty path: neither can (logged under `log_tag`).
struct ClipDestination {
    std::wstring          path;
    bool                  fell_back = false;
    std::shared_ptr<void> reservation;
};
// Moves `full` (ending in `ext`) to the first name neither on disk nor held by another save, clip
// or recording (`_001` on), and holds it until the returned token is destroyed.
std::shared_ptr<void> ReserveClipName(std::wstring& full, const std::wstring& ext);
ClipDestination PickClipPath(SaveContext* ctx, int64_t t_end, const std::wstring* group_folder_name,
                             const std::wstring& title, uint64_t room_needed, const char* log_tag);

// `group_folder_name`, when non-null, names the subfolder a multi-monitor SaveReplay action
// writes each monitor's file into. Clears ctx->save_in_flight on every
// return path and records a telemetry event. Never dies on I/O. `fell_back`: the clip was written to
// ctx->fallback_root because output_root couldn't take it (still kSaved).
// `clip_seconds`: the saved clip's length (first to last video tick), 0 unless kSaved.
SaveOutcome DoSave(SaveContext* ctx, int64_t t_end, const std::wstring* group_folder_name = nullptr,
                   std::wstring* saved_path = nullptr, bool* fell_back = nullptr,
                   double* clip_seconds = nullptr);

struct ReplaySaveResult {
    SaveOutcome outcome = SaveOutcome::kBusy;
    std::wstring path;
    bool fell_back = false;
    double seconds = 0;  // the clip's length when kSaved: shorter than the buffer after a recent save
};
struct ReplaySaveResults {
    SaveOutcome outcome;
    std::vector<ReplaySaveResult> monitors;  // input order, including busy monitors
};
// The whole action's result, once: `outcome` folds the monitors (any failed or busy one makes it
// kFailed); a monitor's `fell_back` means its clip went to the fallback folder, not output.root.
using SaveFeedback = std::function<void(const ReplaySaveResults&)>;
using StartSaveFeedback = std::function<SaveFeedback()>;

// What an app's clip (clip.h AppClips) tells its toast: `done` with the result, as a save's;
// `extended` each time a request joins it, with the clip's new end and that request's app.
struct ClipFeedback {
    SaveFeedback                                                    done;
    std::function<void(int64_t until_qpc, const std::string& app)> extended;
};
// Made once per new clip, with the end its footage runs to (event + after) and its request's app.
using StartClipFeedback = std::function<ClipFeedback(int64_t until_qpc, const std::string& app)>;

// Claims all available rings before returning, then saves them concurrently at one timestamp.
// The caller may wait for the result (pipe) or discard the future (hotkey). Feedback resolves
// once for the whole action. Any failure/busy monitor prevents an unqualified success toast.
std::future<ReplaySaveResults> SaveReplay(const std::vector<SaveContext*>& contexts,
                                        const StartSaveFeedback& feedback = {});

// Where a marker at `qpc` stands against the footage the rings hold. `oldest_qpc`: the oldest held
// frame, MarkerTimeline::Add's reach (INT64_MAX when nothing is held). `covered`: held frames reach
// `qpc`, give or take a frame; a recording ring also kMarkerTrailSeconds past its newest frame (it
// trails "now" by the encode, and the frames after it are on their way); a stalled or disarmed one
// doesn't. An app's marker needs `covered`.
struct MarkerReach {
    int64_t oldest_qpc;
    bool    covered;
};
MarkerReach MarkerReachAt(const std::vector<SaveContext*>& contexts, int64_t qpc);

// Ombra's "clip saved" marker at `qpc` (the moment saved), labelled with the app that asked for the
// clip (empty: the user). Called once per action, after its files collected their markers: a clip
// doesn't carry its own.
void PlaceClipSavedMarker(const std::vector<SaveContext*>& contexts, int64_t qpc, const std::string& app);

// The clip_saved event (protocol.h) for an action that saved at least one file: `app` asked for it
// (empty: the user), `clip_id` is its AppClips id (0: none).
void PublishClipSaved(const std::vector<SaveContext*>& contexts, const ReplaySaveResults& results,
                      const std::string& app, uint64_t clip_id);

}  // namespace ombra
