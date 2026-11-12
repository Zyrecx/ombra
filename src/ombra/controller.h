// BufferController: the one owner of "should the pipelines be recording right now?".
//
// Inputs, all funnelled here so no other caller races it with a direct SetArmed():
//   - the Instant Replay switch (set_armed pipe command, hotkeys.toggle_buffer): SetUserEnabled.
//   - capture.mode ("When"): "desktop" = Always, "game" = Only in games.
//   - capture.target ("What"): "monitor" | "game" | "window", plus capture.target_window.
//   - what's on screen: a 1 s poll of the top-level windows (SnapshotTargetWindows).
//   - the display state: SetDisplayOff, from the engine window's power notification.
//
// The rule (WantArmed): switched off never records, and nor does a PC with every display off
// (Modern Standby would otherwise encode duplicates all night). One window records while its program has a
// window, whatever When says. Otherwise Always records all the time and Only in games while a
// window the Games list calls a game exists -- foreground or not, so alt-tabbing never stops it
// (replacing foreground arming and its 30 s grace period). Losing the
// target disarms only after kDisarmSettleMs, because every disarm->arm cycle drops a ring barrier
// and a save can't reach past one: a game recreating its window must not cost the buffer.
//
// The foreground hook stays for one job: scoring a program the list doesn't know (GPU 3D load +
// fullscreen coverage). A hit counts as a game for the rest of the run (GameLibrary::
// NoteHeuristicArm) and lands in the Games page's "Looked like a game".
//
// THREADING: SetWinEventHook's callback (WINEVENT_OUTOFCONTEXT) runs on the thread that installed
// it, inside its message pump -- wmain's pump thread, the one that latches WM_HOTKEY's t_end. So
// the hook is installed once in Start() (on that thread), never from a reconfig thread, and the
// callback only latches the hwnd. Everything slow (PDH, window enumeration, version resources)
// runs on the worker thread.
#pragma once

#include "ombra/game_library.h"
#include "ombra/gputopo.h"
#include "ombra/gpuwatch.h"
#include "ombra/supervisor.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

// FileDescription first, ProductName as a fallback -- the name Windows' own Volume Mixer shows for
// a process. Returns empty if the file has no version resource; callers fall back to the exe name.
// Hits disk (GetFileVersionInfoW); never call it from the WinEventHook callback.
std::wstring ReadVersionResourceName(const std::wstring& exe_path);

// What DoSave (save.cpp) needs from a live BufferController, as its own interface so a test can
// hand SaveContext::controller a canned folder name.
class GameFolderSource {
public:
    virtual ~GameFolderSource() = default;
    virtual std::wstring CurrentGameFolder() const = 0;
};

// The heuristic for a foreground program: the Games list decides whenever it knows the program;
// only an unknown one falls back to the confidence score (0..100, ComputeGameConfidence), and an
// arm made that way is `heuristic`.
struct GameArmDecision {
    bool arm       = false;
    bool heuristic = false;
};
constexpr double kGameArmThreshold = 55.0;
GameArmDecision DecideGameArm(GameVerdict verdict, double confidence);
// Whether to score the focused window again, an unknown program that hasn't armed yet.
bool ForegroundRescoreDue(int64_t ms_since_focus, int64_t ms_since_score);
// A game's name as one safe path component: no illegal characters, at most 64 of them, never a
// reserved device name (with or without an extension).
std::wstring SanitizeFolderName(const std::wstring& raw);

struct ArmInputs {
    bool user_enabled   = true;   // the Instant Replay switch
    bool only_in_games  = false;  // capture.mode == "game"
    bool window_target  = false;  // capture.target == "window"
    bool target_present = false;  // One window: its program has a window. Otherwise: a game window exists.
    bool display_off    = false;  // Windows reports every display off
};
bool WantArmed(const ArmInputs& in);
// Whether these settings need the screen polled: for the arm decision, or to follow the game "The
// game" records. Always + displays doesn't: it looks only when the foreground changes.
bool NeedsWindowPoll(const std::string& mode, const std::string& capture_target);

// Arming is immediate. A disarm because the target went away waits until it has been missing for
// `settle_ms` (its return cancels it); a disarm because the user switched recording off is
// immediate. Returns the armed state to apply.
struct ArmState {
    bool    armed         = false;
    bool    leaving       = false;
    int64_t leaving_since = 0;
};
bool StepArm(ArmState& state, const ArmInputs& in, int64_t now_ms, int64_t settle_ms);
// Not measured: display recordings do not show how long a game's
// window is gone while it recreates it. 10 s of extra recording after a game closes costs nothing;
// a barrier mid-match costs the footage before it.
constexpr int64_t kDisarmSettleMs = 10'000;

class BufferController : public GameFolderSource {
public:
    // `supervisors` and `games` are not owned and must outlive the controller. `games` null: no
    // program is a game (tests).
    BufferController(std::string mode, std::string capture_target, std::string target_window,
                     std::vector<PipelineSupervisor*> supervisors, GameLibrary* games = nullptr);
    // Single pipeline, Always, displays: the test modes and tests.
    explicit BufferController(PipelineSupervisor* supervisor);
    ~BufferController();

    // Whether the pipelines should start armed: only Always + a display target is known without
    // looking at the screen. Everything else boots disarmed and the worker's first pass arms it.
    static bool InitialArmed(const std::string& mode, const std::string& capture_target);

    // Installs the foreground hook and starts the worker. Call on the thread that pumps messages.
    void Start();
    void Stop();

    // Live config (reconfig.cpp): record the value and wake the worker.
    void SetMode(const std::string& mode);
    void SetTarget(const std::string& capture_target, const std::string& target_window);

    // The Instant Replay switch. Off disarms at once; on lets the rule decide.
    void SetUserEnabled(bool enabled);

    // Every display off: disarms after kDisarmSettleMs, so a brief blank costs no barrier. Arming
    // again on the way back is immediate. Called on the pump thread: only latches and wakes.
    void SetDisplayOff(bool off);

    // Test seam: the windows Evaluate sees, in place of SnapshotTargetWindows. Set before Start.
    void SetWindowSnapshotForTest(std::function<std::vector<WindowCandidate>()> snapshot) {
        window_snapshot_ = std::move(snapshot);
    }
    // Called on the worker thread when the game being recorded changes: its exe's full path, or empty
    // when recording stops being for a game (it closed, the buffer disarmed, Stop). Same exe in a new
    // window is the same game; the settle after a game leaves still counts as its time. Set before Start.
    void SetGameSessionSink(std::function<void(const std::wstring& exe_path)> sink) { game_sink_ = std::move(sink); }
    // Test seam: a settle a test can wait out. Set before Start.
    void SetDisarmSettleForTest(int64_t ms) { settle_ms_ = ms; }
    bool ToggleUserEnabled();  // returns the new value
    bool UserEnabled() const { return user_enabled_.load(std::memory_order_acquire); }

    // The sanitized name of the game being recorded, for {game} and per-game folders. Kept after a
    // disarm until another game is picked, so a save made just after quitting still lands in that
    // game's folder: the ring holds only its footage. Empty when no game was involved.
    std::wstring CurrentGameFolder() const override;
    // The game's PID while armed for it, else 0 (audio "game_only" captures this process).
    DWORD CurrentGamePid() const;

private:
    struct Settings {
        std::string mode;
        std::string target;
        std::string target_window;
    };

    static void CALLBACK WinEventProcThunk(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                            LONG id_object, LONG id_child, DWORD thread_id,
                                            DWORD event_time);
    void Wake(HWND foreground);
    void WorkerLoop();
    bool ScoreForeground(HWND hwnd, bool rescore);
    void Evaluate(const Settings& s);
    std::function<std::vector<WindowCandidate>()> window_snapshot_;  // SetWindowSnapshotForTest
    void ApplyArmed(bool armed);
    std::wstring ResolveGameName(const std::wstring& exe_path);
    void UpdateGameGpu(const WindowCandidate* game);
    double ComputeGameConfidence(DWORD pid, bool covers_monitor) const;

    std::vector<PipelineSupervisor*> supervisors_;
    GameLibrary*      games_ = nullptr;  // thread-safe itself
    std::atomic<bool> user_enabled_{true};
    std::atomic<bool> display_off_{false};
    int64_t           settle_ms_ = kDisarmSettleMs;

    mutable std::mutex settings_mutex_;
    Settings           settings_;

    HWINEVENTHOOK hook_ = nullptr;
    // SetWinEventHook's callback has no user-data parameter; one controller per process.
    static std::atomic<BufferController*> self_for_hook_;

    std::thread             worker_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    HWND                    pending_foreground_ = nullptr;
    bool                    dirty_ = true;
    std::atomic<bool>       running_{false};

    // Worker-thread-only.
    std::unique_ptr<GpuLoadWatcher> gpu_watcher_;  // only while Only in games
    ArmState               arm_;
    uint64_t               game_window_ = 0;  // the game window last named
    std::function<void(const std::wstring&)> game_sink_;
    std::wstring           session_exe_;  // what game_sink_ was last told
    HWND                   scored_window_ = nullptr;  // focused, unknown, not armed: scored again for a while
    int64_t                scored_at_ms_ = 0;
    int64_t                last_score_ms_ = 0;
    // The adapter the named game renders on, passed to every supervisor (SetGameGpu). A game can
    // open its device a while after its window, so an unknown one is asked again on the poll.
    uint64_t               game_gpu_window_ = 0;
    int                    game_gpu_tries_  = 0;
    std::optional<LUID>    game_gpu_;
    bool                   game_gpu_pending_ = false;

    mutable std::mutex folder_mutex_;
    std::wstring       folder_snapshot_;
    DWORD              pid_snapshot_ = 0;
    void Publish(const std::wstring& folder, DWORD pid);
};

}  // namespace ombra
