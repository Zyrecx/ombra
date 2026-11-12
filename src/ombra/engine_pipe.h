// The pipe request handler and the state it shares with the engine (engine.cpp's RunEngine).
// In ombra_core, apart from the engine wiring, so tests can drive it without a real engine.
#pragma once

#include "ombra/audio.h"
#include "ombra/audio_tracks.h"
#include "ombra/config.h"
#include "ombra/controller.h"
#include "ombra/game_library.h"
#include "ombra/gputopo.h"
#include "ombra/hotkeys.h"
#include "ombra/json.h"
#include "ombra/reconfig.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/supervisor.h"
#include "ombra/tray.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

// Defined in engine_pipe.cpp, set by RunEngine (engine.cpp); null/zero outside the resident engine.
extern std::atomic<HotkeyRegistry*> g_hotkeys_for_engine;
extern std::atomic<GameLibrary*>    g_games_for_engine;
extern std::atomic<DWORD>           g_pump_thread_id_for_ctrl_handler;

struct ScreenshotContext {
    std::mutex     request_mutex;
    std::wstring    pending_path;
    std::wstring    pending_fallback_path;  // empty: an explicit path, which doesn't fall back
    std::wstring    fallback_root;          // FallbackClipsDir, set before the worker's first job
    // The worker's only way to tell the user: L"shot_failed" or L"shot_elsewhere". Set before it starts.
    std::function<void(const wchar_t*)> notify;
    std::atomic<bool> requested{false};
    std::atomic<int>  shot_count{0};

    std::mutex              job_mutex;
    std::condition_variable job_cv;
    bool                     job_ready = false;
    std::wstring              job_path;
    std::wstring              job_fallback_path;
    uint32_t                   job_width = 0, job_height = 0, job_row_pitch = 0;
    std::vector<uint8_t>       job_bgra;

    std::atomic<bool> running{true};
    std::thread        worker;
};

// engine.autotune's result path: the pipe's
// set_config spawns ombra.exe capturebench detached and returns immediately (ApplySettingChange's
// 3s Request timeout can't wait out an ~14s bench run), so completion has to be surfaced through
// get_status instead of the set_config response. state is the only field the UI polls each Show();
// last_result mirrors capturebench.cpp's AutotuneResultFilePath() JSON once the watcher thread below
// has read it. "done"/"failed" stay latched until the UI acks them (set_config{"engine.autotune":
// "ack"}) back to "idle" — otherwise every future get_status would keep re-reporting a stale result
// and the UI would have to invent its own "have I already shown this" bookkeeping.
struct AutotuneContext {
    enum class State { Idle, Running, Done, Failed };
    std::atomic<State> state{State::Idle};
    std::mutex          result_mutex;
    JsonValue           last_result;  // valid when state is Done or Failed; Null otherwise
    HANDLE              process = nullptr;
};

// One monitor's whole pipeline — its own Ring (video, encoded bitstream) and
// PipelineSupervisor (capture+encoder+pacer), plus the SaveContext a save against this pipeline
// reads. Ring/PipelineSupervisor/SaveContext all hold a mutex or atomic member directly (not via
// pointer), so none of them are movable — this struct lives behind a unique_ptr in wmain's
// pipelines vector precisely so the vector can grow without invalidating the Ring*/SaveContext*
// pointers every PipelineSupervisor and DoSave call already holds onto. supervisor is built after
// ring (see wmain) since its constructor needs &ring, which must already exist; a raw
// unique_ptr rather than by-value keeps that two-phase construction explicit at the call site
// instead of hidden in a constructor. Audio is NOT per-pipeline: there is one set of audio
// devices regardless of monitor count, so every pipeline's SaveContext points at the one
// AudioTrackHub RunEngine owns.
struct MonitorPipeline {
    MonitorSpec                        spec;
    Ring                                ring;
    std::unique_ptr<PipelineSupervisor> supervisor;
    SaveContext                         save_ctx;
};

// Between the audio target watcher and pipe threads: what the watcher runs (audio_status reads the
// board) and the next source list for it to run (set_config posts; the latest post wins). The
// watcher is the only thread that applies a list, so captures are never started or stopped
// anywhere else.
class AudioSourceControl {
public:
    AudioSourceBoard board;

    void Post(std::vector<AudioSourceSpec> specs) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(specs);
        has_pending_.store(true, std::memory_order_release);
    }
    bool HasPending() const { return has_pending_.load(std::memory_order_acquire); }
    std::optional<std::vector<AudioSourceSpec>> Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        has_pending_.store(false, std::memory_order_release);
        auto out = std::move(pending_);
        pending_.reset();
        return out;
    }

private:
    std::mutex                                  mutex_;
    std::optional<std::vector<AudioSourceSpec>> pending_;
    std::atomic<bool>                           has_pending_{false};
};

std::map<std::string, std::string> HotkeySpecsFromConfig(const Config& cfg);
void WaitForAutotuneCompletion(AutotuneContext* ctx);
std::wstring TrayTooltip(bool enabled);
// Where a screenshot goes in `root`, and in `fallback_root` if `root` can't take it: the same name and
// game folder in both, each suffixed with `n` if that file exists. `fallback` is empty without a root.
struct ScreenshotPaths {
    std::wstring path;
    std::wstring fallback;
};
ScreenshotPaths ScreenshotPathsFor(const std::wstring& root, const std::wstring& fallback_root,
                                   const std::wstring& game_name, bool per_game_folders, int n);

// Writes each queued screenshot; returns once `running` is false and no job is left.
void ScreenshotWorkerLoop(ScreenshotContext* ctx);
void RequestScreenshot(ScreenshotContext* ctx, PipelineSupervisor* supervisor,
                        BufferController* controller, const std::string& output_root,
                        bool per_game_folders, const std::wstring& out_path);
std::vector<AudioSourceSpec> RecordableAudioSources(const std::vector<AudioSourceSpec>& configured,
                                                    bool per_app_mode);
std::vector<AudioTrackSource> TrackSourcesFrom(const std::vector<AudioSourceSpec>& specs);
uint32_t EngineFps(const Config& cfg, const std::string& device = "primary");

// get_schema/get_config/set_config/get_status/save_now, factored out of the
// PipeServer construction lambda so tests (engine_pipe_tests.cpp, and the gpu suite's save_now)
// can drive it directly without a real named pipe in the loop.
//
// THREADING: called on whichever PipeServer client worker thread received the request (pipe.h's
// contract), never the pump thread. Config reads and live changes serialize under config_mutex.
// Status uses the supervisor/controller's thread-safe accessors. save_now shares SaveReplay's
// per-ring claims with the hotkey, then waits on its worker without holding config_mutex.
JsonValue HandlePipeRequest(const JsonValue& req, Config& cfg, PipelineSupervisor& supervisor,
                             BufferController& controller, SaveContext& save_ctx,
                             const StartupGpuSummary& gpu_summary = StartupGpuSummary{},
                             Tray* tray = nullptr, ScreenshotContext* screenshot_ctx = nullptr,
                             AutotuneContext* autotune_ctx = nullptr,
                             // All monitor pipelines for status, config fan-out and replay saves.
                             const std::vector<std::unique_ptr<MonitorPipeline>>* all_pipelines = nullptr,
                             // Every OMBRA_CONFIG_RAW key's live value —
                             // Config carries no field for these, so this is what get_config reads
                             // from and set_config writes into (see RawConfigKeys()/
                             // ValidateRawConfigValue() in config.h). nullptr means get_config omits Raw keys and set_config can't persist
                             // them — same as every pre-step-5 build.
                             RawConfigMap* raw_store = nullptr,
                             // What the rings are sized at (constant until a reconfig; memory is
                             // committed as footage arrives, reported separately from each ring).
                             // set_config updates it after a Realloc; both run under config_lock.
                             MemoryLayout* memory = nullptr,
                             // The audio target watcher's state (audio_status) and its inbox for a
                             // new source list (set_config). nullptr: audio_status answers with no
                             // sources and audio.sources edits apply on the next start.
                             AudioSourceControl* audio_sources = nullptr,
                             const StartSaveFeedback& save_feedback = {});

}  // namespace ombra
