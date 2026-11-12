// The save path: pin the video ring and every audio ring over [t_end - buffer_seconds, t_end),
// build the audio tracks (grouping/mixing by group_id, gain, optional full-mix track), and mux.
#pragma once

#include "ombra/reconfig.h"

#include <cstdint>
#include <functional>
#include <future>
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

// Claims all available rings before returning, then saves them concurrently at one timestamp.
// The caller may wait for the result (pipe) or discard the future (hotkey). Feedback resolves
// once for the whole action. Any failure/busy monitor prevents an unqualified success toast.
std::future<ReplaySaveResults> SaveReplay(const std::vector<SaveContext*>& contexts,
                                        const StartSaveFeedback& feedback = {});

}  // namespace ombra
