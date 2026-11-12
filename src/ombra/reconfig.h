#pragma once

#include "ombra/config.h"
#include "ombra/encoder.h"
#include "ombra/controller.h"
#include "ombra/audio_tracks.h"
#include "ombra/json.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"
#include "ombra/tray.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ombra {

// The encode.* keys an encoder opens with. Startup and both reconfigure tiers go through this.
EncoderOptions EncoderOptionsFor(const Config& cfg);

// {date}/{time} come from `at` (local time) when given, else from the current time.
std::wstring FormatFilename(const std::string& template_str, const std::wstring& game_name,
                            const std::wstring& monitor_suffix, const SYSTEMTIME* at = nullptr);
bool SetWindowsAutostart(bool enable);

// The file path a file-backed ring maps, derived from buffer.file_path (or a
// default when that's empty — %LOCALAPPDATA%\Ombra\ring.dat, matching where autotune_result.json
// already lives) with `suffix` (a MonitorSpec::suffix, e.g. " - DISPLAY2") inserted before the
// extension so N>1 file-backed pipelines don't memory-map the same file. `suffix` is "" for the
// sole/first pipeline, byte-identical to the base path.
std::wstring ResolveRingFilePath(const Config& cfg, const std::string& suffix);

// Ring::SetMaxAge for a save length: saves pin [t_end - buffer_seconds, t_end], plus 2 s for the
// hotkey-to-Pin gap. Anything that changes how far back a save reaches must go through this.
int64_t RingMaxAgeQpc(double buffer_seconds);
// The audio rings' max age: a video save starts at the keyframe at or before its floor
// (Ring::PinCovering), up to one keyframe interval earlier, and its audio starts there too. Covers
// encode.keyframe_sec's maximum, so the setting needs no live resize; at most ~10 s of AAC a track.
int64_t AudioRingMaxAgeQpc(double buffer_seconds);

struct SaveContext {
    Ring*                ring = nullptr;
    // Every pipeline shares the one hub (there is one set of audio devices whatever the monitor
    // count); it serializes concurrent saves itself. Null: a video-only save.
    AudioTrackHub*       audio = nullptr;
    double               buffer_seconds = 60.0;
    std::wstring         out_path;
    std::atomic<uint32_t> fps{60};  // a settings Save writes it while a save may read it
    PipelineSupervisor*  supervisor = nullptr;
    // GameFolderSource, not BufferController directly -- DoSave (save.cpp) only ever calls
    // CurrentGameFolder() on this, and the narrower interface is what lets a test inject a canned
    // folder name without a real BufferController (controller.h's GameFolderSource comment).
    GameFolderSource*    controller = nullptr;
    bool                 per_game_folders = false;
    std::string          output_root = "%USERPROFILE%\\Videos\\" OMBRA_PRODUCT_NAME;
    // Where a clip goes when output_root can't take it (FallbackClipsDir); empty: the save fails.
    std::wstring         fallback_root;
    std::string          output_filename = "{game} {date} {time}{monitor_suffix}";
    std::string          save_overlap_policy = "truncate";
    // {monitor_suffix} for this pipeline's saved filenames — "" for the sole
    // pipeline or the first of several (matches every single-monitor build to date byte-for-byte),
    // " - <name>" for every other monitor (MonitorSpec::suffix, config.h).
    std::wstring         monitor_suffix;
    // MonitorSpec::label (config.h), always non-empty — the per-monitor filename used inside a
    // save's group folder when N>1 pipelines exist (see DoSave's group-folder branch, main.cpp).
    std::wstring         monitor_label;
    // MonitorSpec::device: whose refresh rate capture.framerate 0 ("match display") resolves to.
    std::string          monitor_device = "primary";
    std::atomic<bool>    save_in_flight{false};
    std::atomic<int>     save_count{0};

    std::mutex           last_save_mutex;
    std::wstring         last_save_path;
    int64_t              last_save_qpc = -1;
};

struct LiveApplyResult {
    bool ok = true;
    ApplyTier tier = ApplyTier::Free;
    bool buffer_dropped = false;
    std::vector<std::string> accepted;
    std::vector<ConfigIssue> rejected;
    std::string note;
    std::string error;
    // Set when a Realloc-tier apply re-Inited the rings: what they were sized to, so get_status's
    // memory report follows a live change instead of describing the startup allocation.
    std::optional<RingSizing> realloc_sizing;
    bool realloc_file_backed = false;
};

// Guards the moment a live change replaces the Config and SaveContext's strings. Readers off the
// pipe's own lock (a hotkey on the pump thread, a screenshot request, a save's thread) take it: without
// it, reading output_root while it's replaced can read freed memory. It's separate from the pipe handler's lock because that one is held through
// a whole rebuild, which must not stall the message pump.
std::mutex& LiveConfigMutex();

template <class Read>
auto ReadLiveConfig(const Config& cfg, Read&& read) {
    std::lock_guard<std::mutex> lock(LiveConfigMutex());
    return read(cfg);
}

// One monitor's share of a live change; `suffix` is its MonitorSpec::suffix.
struct LivePipeline {
    PipelineSupervisor& supervisor;
    SaveContext&        save_ctx;
    Ring&               ring;
    std::string         suffix;
};

// Applies a patch to every pipeline, or to none if one of them refuses it. Non-empty, `pipelines[0]`
// is what the preview and the tray read.
LiveApplyResult ApplyLiveConfig(Config& live_cfg, const JsonValue& patch_json,
                                const std::vector<LivePipeline>& pipelines,
                                BufferController& controller,
                                Tray* tray = nullptr,
                                const std::vector<RecordingGpuCaps>& gpus = {});

// The same with one pipeline, the tests' usual case.
LiveApplyResult ApplyLiveConfig(Config& live_cfg, const JsonValue& patch_json,
                                PipelineSupervisor& supervisor,
                                BufferController& controller,
                                SaveContext& save_ctx,
                                Ring& ring,
                                Tray* tray = nullptr,
                                const std::vector<RecordingGpuCaps>& gpus = {});

}  // namespace ombra
