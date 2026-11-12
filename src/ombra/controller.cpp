#include "ombra/controller.h"
#include "ombra/brand.h"

#include "ombra/capturebench.h"
#include "ombra/log.h"
#include "ombra/protocol.h"
#include "ombra/util.h"
#include "ombra/window_target.h"

#include <winver.h>

#include <algorithm>
#include <cwctype>
#include <map>

namespace ombra {

namespace {

// Windows reserved device names, case-insensitive, with or without an extension — a folder or
// file literally named "CON" is refused by the filesystem. This is an allowlist-adjacent explicit
// check rather than "assume control characters are the only thing NTFS rejects": that assumption
// was wrong once already in this codebase (a control character silently created a real directory).
// Windows reserves these with any extension too (CON.foo), so only the part before the first dot
// counts, without the spaces Windows would strip from it.
bool IsReservedDeviceName(std::wstring stem) {
    stem.resize((std::min)(stem.find(L'.'), stem.size()));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    static const wchar_t* kReserved[] = {L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2",
                                          L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8",
                                          L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
                                          L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    std::wstring upper = stem;
    for (auto& c : upper) c = static_cast<wchar_t>(std::towupper(c));
    for (const wchar_t* r : kReserved)
        if (upper == r) return true;
    return false;
}

// Sanitizes a resolved game name (or a bare exe stem fallback) into something safe to use as a
// single path component: strips NTFS-illegal characters and control characters, trims trailing
// dots/spaces (Windows silently strips these from the *displayed* name but they can still corrupt
// a path built by string concatenation), and renames a bare reserved device name out of the way.
// Cap, not just character-filter: FileDescription is untrusted data from a third-party binary's
// version resource with no length limit Windows enforces, and this name later gets sprintf'd into
// a fixed MAX_PATH buffer alongside the output directory and filename (main.cpp's DoSave). An
// oversized result there overflows swprintf_s, which calls the CRT's invalid-parameter handler and
// terminates the process — exactly the always-resident-process failure class this exists
// to eliminate, reached here from data this process doesn't control (advisor review).
constexpr size_t kMaxFolderNameChars = 64;

}  // namespace

std::wstring SanitizeFolderName(const std::wstring& raw) {
    std::wstring out;
    out.reserve((std::min)(raw.size(), kMaxFolderNameChars));
    for (wchar_t c : raw) {
        if (out.size() >= kMaxFolderNameChars) break;
        if (c < 0x20) continue;  // control characters
        if (wcschr(L"<>:\"/\\|?*", c)) continue;
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    if (out.empty()) out = L"unknown_game";
    if (IsReservedDeviceName(out)) {
        out.insert((std::min)(out.find_first_of(L". "), out.size()), L"_");  // CON -> CON_, CON.foo -> CON_.foo
        if (out.size() > kMaxFolderNameChars) out.resize(kMaxFolderNameChars);
    }
    return out;
}

namespace {

// The exe's own filename, no directory, no extension — the fallback when a
// version resource has neither FileDescription nor ProductName (or, for `modetest`, when the
// injected "exe path" isn't a real file at all).
std::wstring ExeStem(const std::wstring& path_or_name) {
    size_t slash = path_or_name.find_last_of(L"\\/");
    std::wstring base = (slash == std::wstring::npos) ? path_or_name : path_or_name.substr(slash + 1);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) base = base.substr(0, dot);
    return base;
}

// Same pattern as capture.cpp's window enumeration / main.cpp's process listing: works against a
// protected/anti-cheat process too, since PROCESS_QUERY_LIMITED_INFORMATION is the deliberately
// low-privilege query Windows still grants against those, including anti-cheat-protected titles.
std::wstring ResolveExeImagePath(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t image[MAX_PATH]{};
    DWORD   len = MAX_PATH;
    std::wstring result = QueryFullProcessImageNameW(h, 0, image, &len) ? std::wstring(image, len) : L"";
    CloseHandle(h);
    return result;
}

}  // namespace

// FileDescription first, ProductName as a fallback. Returns empty if the file has
// no version resource at all (common for indie/homebrew games), which the caller treats as "fall
// back to the exe name." Exposed via controller.h (not anonymous-namespace-private like this
// file's other helpers) so audio.cpp/main.cpp can resolve the same friendly name Windows' own
// Volume Mixer shows for a process, instead of the bare .exe filename -- see controller.h's own
// comment on why this is safe to call from a pipe-handler worker thread (disk I/O) but never from
// the WinEventHook callback thread.
std::wstring ReadVersionResourceName(const std::wstring& exe_path) {
    DWORD handle = 0;
    DWORD size   = GetFileVersionInfoSizeW(exe_path.c_str(), &handle);
    if (size == 0) return L"";
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoW(exe_path.c_str(), handle, size, buf.data())) return L"";

    struct LangCodepage { WORD lang; WORD codepage; };
    LangCodepage* translations = nullptr;
    UINT translations_len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translations), &translations_len) ||
        translations_len < sizeof(LangCodepage))
        return L"";

    for (UINT i = 0; i * sizeof(LangCodepage) < translations_len; ++i) {
        for (const wchar_t* field : {L"FileDescription", L"ProductName"}) {
            wchar_t subblock[128];
            swprintf_s(subblock, L"\\StringFileInfo\\%04x%04x\\%ls", translations[i].lang,
                       translations[i].codepage, field);
            wchar_t* value = nullptr;
            UINT     value_len = 0;
            if (VerQueryValueW(buf.data(), subblock, reinterpret_cast<void**>(&value), &value_len) &&
                value_len > 1 && value && value[0] != L'\0')
                return value;
        }
    }
    return L"";
}

std::atomic<BufferController*> BufferController::self_for_hook_{nullptr};

namespace {
// The heuristic. A process clearing kGameArmThreshold (controller.h) counts as a game for the rest
// of the run. Deliberately conservative on the GPU side: 3D-engine utilization is noisy and bursty
// for plenty of non-game apps (a browser scrolling a WebGL page, a video call), so the score alone
// rarely reaches the threshold -- the fullscreen/borderless booster is what pushes a real game over
// it, while a windowed game with sustained real load (a windowed Java game) still clears
// it on GPU signal alone.
constexpr double kGpuMinPercent   = 8.0;   // below this, GPU signal contributes nothing
constexpr double kGpuScoreSpan    = 60.0;  // 8%..100% util maps onto 40..100 score points
constexpr double kFullscreenBoost = 25.0;
// A game installed while the engine runs shows up on the next foreground change after this long.
constexpr std::chrono::seconds kGameRescanAge{30};
constexpr std::chrono::seconds kPollInterval{1};
// A game can take the foreground on a splash screen and only load the GPU later, with no further
// foreground event: score the same window again for a while. Not forever: a window that has had a
// minute to look like a game hasn't, and a long-focused app shouldn't arm on one busy moment.
constexpr int64_t kRescoreEveryMs = 5'000;
constexpr int64_t kRescoreForMs   = 60'000;

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

bool ForegroundRescoreDue(int64_t ms_since_focus, int64_t ms_since_score) {
    return ms_since_focus < kRescoreForMs && ms_since_score >= kRescoreEveryMs;
}

GameArmDecision DecideGameArm(GameVerdict verdict, double confidence) {
    switch (verdict) {
        case GameVerdict::Game:    return {true, false};
        case GameVerdict::NotGame: return {false, false};
        case GameVerdict::Unknown: break;
    }
    const bool arm = confidence >= kGameArmThreshold;
    return {arm, arm};
}

bool WantArmed(const ArmInputs& in) {
    if (!in.user_enabled || in.display_off) return false;
    if (in.window_target) return in.target_present;
    return !in.only_in_games || in.target_present;
}

bool NeedsWindowPoll(const std::string& mode, const std::string& capture_target) {
    return mode == "game" || capture_target != "monitor";
}

bool StepArm(ArmState& state, const ArmInputs& in, int64_t now_ms, int64_t settle_ms) {
    if (WantArmed(in)) {
        state.armed   = true;
        state.leaving = false;
        return true;
    }
    if (!in.user_enabled) {
        state.armed   = false;
        state.leaving = false;
        return false;
    }
    if (!state.armed) return false;
    if (!state.leaving) {
        state.leaving       = true;
        state.leaving_since = now_ms;
    }
    if (now_ms - state.leaving_since >= settle_ms) {
        state.armed   = false;
        state.leaving = false;
    }
    return state.armed;
}

bool BufferController::InitialArmed(const std::string& mode, const std::string& capture_target) {
    return !NeedsWindowPoll(mode, capture_target) ||
           (mode != "game" && capture_target == "game");  // Always + "The game": the display until one opens
}

BufferController::BufferController(std::string mode, std::string capture_target, std::string target_window,
                                   std::vector<PipelineSupervisor*> supervisors, GameLibrary* games)
    : supervisors_(std::move(supervisors)), games_(games) {
    settings_ = {std::move(mode), std::move(capture_target), std::move(target_window)};
    arm_.armed = InitialArmed(settings_.mode, settings_.target);
    for (PipelineSupervisor* s : supervisors_) s->SetGameFallbackToDisplay(settings_.mode != "game");
}

BufferController::BufferController(PipelineSupervisor* supervisor)
    : BufferController("desktop", "monitor", "", std::vector<PipelineSupervisor*>{supervisor}) {}

BufferController::~BufferController() {
    if (running_.load(std::memory_order_acquire)) Stop();
}

void BufferController::Start() {
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&BufferController::WorkerLoop, this);

    self_for_hook_ = this;
    hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProcThunk,
                            0, 0, WINEVENT_OUTOFCONTEXT);
    if (!hook_)
        Log::Error("buffer controller  SetWinEventHook failed (err=%lu) -- programs not in the Games "
                   "list are never scored as games",
                   GetLastError());
    std::lock_guard<std::mutex> lock(settings_mutex_);
    Log::Info("buffer controller  when=%s  what=%s  games=%zu", settings_.mode.c_str(), settings_.target.c_str(),
              games_ ? games_->Games().size() : size_t{0});
}

void BufferController::Stop() {
    if (hook_) {
        UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
    self_for_hook_ = nullptr;
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    gpu_watcher_.reset();
    if (!session_exe_.empty()) {
        session_exe_.clear();
        if (game_sink_) game_sink_(session_exe_);
    }
}

void BufferController::SetMode(const std::string& mode) {
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        if (settings_.mode == mode) return;
        settings_.mode = mode;
    }
    for (PipelineSupervisor* s : supervisors_) s->SetGameFallbackToDisplay(mode != "game");
    Log::Info("buffer controller  when -> %s", mode.c_str());
    Wake(nullptr);
}

void BufferController::SetTarget(const std::string& capture_target, const std::string& target_window) {
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        if (settings_.target == capture_target && settings_.target_window == target_window) return;
        settings_.target        = capture_target;
        settings_.target_window = target_window;
    }
    Wake(nullptr);
}

void BufferController::SetUserEnabled(bool enabled) {
    if (user_enabled_.exchange(enabled, std::memory_order_acq_rel) == enabled) return;
    Log::Info("buffer controller  recording switched %s", enabled ? "on" : "off");
    // Applied here when the answer needs no look at the screen, not on the worker's next pass, so
    // the switch reads right the moment the pipe command returns.
    bool always = false;
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        always = !NeedsWindowPoll(settings_.mode, settings_.target) && !display_off_.load(std::memory_order_acquire);
    }
    if (!enabled || always) ApplyArmed(enabled);
    Wake(nullptr);
}

void BufferController::SetDisplayOff(bool off) {
    if (display_off_.exchange(off, std::memory_order_acq_rel) == off) return;
    Wake(nullptr);
}

bool BufferController::ToggleUserEnabled() {
    const bool next = !UserEnabled();
    SetUserEnabled(next);
    return next;
}

void BufferController::ApplyArmed(bool armed) {
    for (PipelineSupervisor* s : supervisors_) s->SetArmed(armed);
}

void CALLBACK BufferController::WinEventProcThunk(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd, LONG id_object,
                                                  LONG /*id_child*/, DWORD /*thread_id*/, DWORD /*event_time*/) {
    // id_object != OBJID_WINDOW filters out sub-element events EVENT_SYSTEM_FOREGROUND can carry.
    if (event != EVENT_SYSTEM_FOREGROUND || id_object != OBJID_WINDOW || !hwnd) return;
    if (BufferController* self = self_for_hook_.load(std::memory_order_acquire)) self->Wake(hwnd);
}

// Runs on the pump thread when `foreground` is set: latch and wake, nothing else.
void BufferController::Wake(HWND foreground) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (foreground) pending_foreground_ = foreground;
        dirty_ = true;
    }
    queue_cv_.notify_one();
}

void BufferController::WorkerLoop() {
    Log::SetThreadName("controller");
    while (running_.load(std::memory_order_acquire)) {
        Settings s;
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            s = settings_;
        }
        const bool poll = NeedsWindowPoll(s.mode, s.target) || arm_.leaving || game_gpu_pending_;

        HWND foreground = nullptr;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            const auto ready = [&] { return dirty_ || !running_.load(std::memory_order_acquire); };
            if (poll) queue_cv_.wait_for(lock, kPollInterval, ready);
            else      queue_cv_.wait(lock, ready);
            if (!running_.load(std::memory_order_acquire)) return;
            foreground          = pending_foreground_;
            pending_foreground_ = nullptr;
            dirty_              = false;
        }
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            s = settings_;  // a SetMode/SetTarget that woke this pass
        }

        const bool scoring = s.mode == "game" && s.target != "window";
        if (scoring && !gpu_watcher_) {
            gpu_watcher_ = std::make_unique<GpuLoadWatcher>();
            // A game already focused when scoring starts sends no foreground event.
            if (!foreground) foreground = GetForegroundWindow();
        }
        if (!scoring) gpu_watcher_.reset();
        if (!scoring) {
            scored_window_ = nullptr;
        } else if (foreground) {
            scored_window_ = ScoreForeground(foreground, false) ? nullptr : foreground;
            scored_at_ms_ = last_score_ms_ = SteadyMs();
        } else if (scored_window_) {
            const int64_t now = SteadyMs();
            if (ForegroundRescoreDue(now - scored_at_ms_, now - last_score_ms_)) {
                last_score_ms_ = now;
                if (GetForegroundWindow() != scored_window_ || ScoreForeground(scored_window_, true))
                    scored_window_ = nullptr;
            } else if (now - scored_at_ms_ >= kRescoreForMs) {
                scored_window_ = nullptr;
            }
        }

        Evaluate(s);
    }
}

// True when there's nothing more to learn from this window: the list knows it, it's ours, or it armed.
bool BufferController::ScoreForeground(HWND hwnd, bool rescore) {
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 255);
    if (wcscmp(cls, kOverlayWindowClassW) == 0) return true;  // fullscreen and on the GPU, never a game

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const std::wstring exe = ResolveExeImagePath(pid);
    GameVerdict verdict = GameVerdict::Unknown;
    if (games_) {
        games_->RescanIfOlderThan(kGameRescanAge);
        verdict = games_->Classify(exe);
    }
    if (verdict != GameVerdict::Unknown) return true;  // the list decides; Evaluate reads it

    const bool   covers_monitor = ForegroundLooksFullscreen(hwnd, nullptr);
    const double confidence     = ComputeGameConfidence(pid, covers_monitor);
    const GameArmDecision decision = DecideGameArm(verdict, confidence);
    if (!rescore || decision.arm)
        Log::Info("foreground  not in the Games list  class=\"%ls\"  covers_monitor=%s  confidence=%.0f  game=%s", cls,
                  covers_monitor ? "yes" : "no", confidence, decision.arm ? "yes" : "no");
    if (decision.arm && games_) games_->NoteHeuristicArm(ResolveGameName(exe), exe);
    return decision.arm;
}

void BufferController::Evaluate(const Settings& s) {
    ArmInputs in;
    in.user_enabled  = UserEnabled();
    in.only_in_games = s.mode == "game";
    in.window_target = s.target == "window";
    in.display_off   = display_off_.load(std::memory_order_acquire);

    const WindowCandidate* game = nullptr;
    std::vector<WindowCandidate> windows;
    // Always + displays looks too, not to arm but to name the game: {game}, per-game folders and
    // game_only audio read it. It has no poll, so this runs on a foreground change (a game opening or
    // closing is one) and costs nothing at rest.
    {
        windows = window_snapshot_ ? window_snapshot_() : SnapshotTargetWindows();
        if (in.window_target) {
            WindowTarget t;
            t.match = WideFromUtf8(s.target_window);
            in.target_present = !t.Empty() && std::any_of(windows.begin(), windows.end(), [&](const WindowCandidate& w) {
                return WindowMatchesTarget(t, w);
            });
        } else {
            if (games_) games_->RescanIfOlderThan(kGameRescanAge);  // a game installed while running
            WindowTarget t;
            t.is_game = games_ ? games_->GameFilter() : [](const std::wstring&) { return false; };
            // "The game": name the window being recorded, so {game}, game_only audio and the picture
            // agree when two games are open. ChooseTargetWindow keeps a current window that still counts.
            uint64_t current = game_window_;
            if (s.target == "game" && !supervisors_.empty())
                if (const uint64_t recorded = supervisors_.front()->RecordedWindowId()) current = recorded;
            // Recording a display, the game in front is the one on screen: don't stay on another.
            if (s.target == "monitor" && std::any_of(windows.begin(), windows.end(), [&](const WindowCandidate& w) {
                    return w.foreground && WindowMatchesTarget(t, w);
                }))
                current = 0;
            const int pick = ChooseTargetWindow(t, windows, current);
            if (pick >= 0) game = &windows[static_cast<size_t>(pick)];
            in.target_present = game != nullptr;
        }
    }
    UpdateGameGpu(game);  // before arming, so the first build already records on the game's GPU

    const bool was   = arm_.armed;
    const bool was_leaving = arm_.leaving;
    const bool armed = StepArm(arm_, in, SteadyMs(), settle_ms_);
    if (arm_.leaving && !was_leaving)
        Log::Info("buffer controller  %s -- disarming in %llds unless it comes back",
                  in.display_off ? "display off" : in.window_target ? "target window gone" : "game gone",
                  static_cast<long long>(settle_ms_ / 1000));
    // By the supervisors, not just `was`: the switch turned off and on between two passes leaves
    // arm_ unchanged, with SetUserEnabled's immediate disarm still standing.
    const bool out_of_step = std::any_of(supervisors_.begin(), supervisors_.end(),
                                         [&](const PipelineSupervisor* sup) { return sup->Armed() != armed; });
    if (armed != was) {
        Log::Info("buffer controller  %s  (%s)", armed ? "arming" : "disarming",
                  !in.user_enabled          ? "switched off"
                  : in.display_off          ? "display off"
                  : in.window_target       ? (armed ? "target window open" : "target window closed")
                  : in.only_in_games        ? (armed ? "a game is open" : "no game open")
                                            : "always");
    }
    if (armed != was || out_of_step) ApplyArmed(armed);

    if (armed && game) {
        DWORD pid = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(game->id), &pid);
        if (game->id != game_window_) {
            game_window_ = game->id;
            const std::wstring name = ResolveGameName(game->image_path);
            Log::InfoW(L"buffer controller  game  \"%s\"  %s", name.c_str(), game->image_path.c_str());
            Publish(name, pid);
        } else {
            Publish(CurrentGameFolder(), pid);
        }
    } else if (!armed) {
        Publish(CurrentGameFolder(), 0);  // keep the name: the ring holds that game's footage
        game_window_ = 0;
    } else if (!arm_.leaving) {
        Publish(L"", 0);  // recording something that isn't a game
        game_window_ = 0;
    }

    const std::wstring exe = armed && game ? game->image_path : armed && arm_.leaving ? session_exe_ : L"";
    if (exe != session_exe_) {
        session_exe_ = exe;
        if (game_sink_) game_sink_(exe);
    }
}

double BufferController::ComputeGameConfidence(DWORD pid, bool covers_monitor) const {
    double score = 0.0;
    if (gpu_watcher_) {
        const double util = gpu_watcher_->Utilization3D(pid);
        if (util >= kGpuMinPercent) {
            const double normalized = (std::min)(util, 100.0) / 100.0;
            score = 40.0 + normalized * kGpuScoreSpan * 0.6;  // 40..76 across the observed range
        }
    }
    if (covers_monitor) score += kFullscreenBoost;
    return (std::min)(score, 100.0);
}

// The Games list's name for the program when it has one (so a game's client and match share one
// folder), else its version resource, else the exe's name.
std::wstring BufferController::ResolveGameName(const std::wstring& exe_path) {
    static std::map<std::wstring, std::wstring> cache;  // worker-thread-only
    std::wstring listed;
    if (games_) games_->Classify(exe_path, &listed);
    if (!listed.empty()) return SanitizeFolderName(listed);

    auto it = cache.find(exe_path);
    if (it != cache.end()) return it->second;
    std::wstring resolved = ReadVersionResourceName(exe_path);
    if (resolved.empty()) resolved = ExeStem(exe_path);
    resolved = SanitizeFolderName(resolved);
    cache.emplace(exe_path, resolved);
    return resolved;
}

void BufferController::UpdateGameGpu(const WindowCandidate* game) {
    constexpr int kTries = 15;  // a poll apart: a game still loading may not render yet
    const uint64_t window = game ? game->id : 0;
    bool changed = false;
    if (window != game_gpu_window_) {
        game_gpu_window_ = window;
        game_gpu_tries_  = 0;
        changed          = game_gpu_.has_value();
        game_gpu_.reset();
    }
    DWORD pid = 0;
    if (game) GetWindowThreadProcessId(reinterpret_cast<HWND>(game->id), &pid);
    if (pid && !game_gpu_ && game_gpu_tries_ < kTries) {
        LUID luid{};
        bool found = false;
        if (gpu_watcher_) {
            found = gpu_watcher_->RenderAdapter(pid, &luid);  // a lookup: no limit
        } else {
            ++game_gpu_tries_;
            const ProcessAdapterResult r = QueryProcessAdapter(pid);  // ~1 s, once per poll at most
            found = r.found && r.util_percent > 0;
            luid  = r.luid;
        }
        if (found) {
            game_gpu_ = luid;
            changed   = true;
            Log::Info("buffer controller  game renders on adapter %08lX:%08lX",
                      static_cast<unsigned long>(luid.HighPart), luid.LowPart);
        }
    }
    game_gpu_pending_ = pid && !game_gpu_ && game_gpu_tries_ < kTries;
    if (changed)
        for (PipelineSupervisor* sup : supervisors_) sup->SetGameGpu(game_gpu_);
}

std::wstring BufferController::CurrentGameFolder() const {
    std::lock_guard<std::mutex> lock(folder_mutex_);
    return folder_snapshot_;
}

DWORD BufferController::CurrentGamePid() const {
    std::lock_guard<std::mutex> lock(folder_mutex_);
    return pid_snapshot_;
}

void BufferController::Publish(const std::wstring& folder, DWORD pid) {
    std::lock_guard<std::mutex> lock(folder_mutex_);
    folder_snapshot_ = folder;
    pid_snapshot_    = pid;
}

}  // namespace ombra
