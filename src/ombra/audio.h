// WASAPI loopback capture.
//
// Captures the default render endpoint's loopback stream (AUDCLNT_STREAMFLAGS_LOOPBACK),
// event-driven on a dedicated thread, and delivers converted s16 stereo 48 kHz packets to a
// caller-supplied callback. Each packet's timestamp is in **raw QueryPerformanceCounter ticks**,
// the same unit `AuDescriptor::qpc` (ring.h) uses — WASAPI's own `pu64QPCPosition` is documented
// in fixed 100-nanosecond units regardless of the machine's actual QPC frequency, so it is
// converted at the capture boundary rather than carried through raw. Comparing
// an unconverted 100ns value against a raw-tick `AuDescriptor::qpc` would silently compare two
// different scales.
//
// Loopback delivers no packets at all while nothing plays; there is no silence fill here.
// AudioTrackHub (audio_tracks.h) places every packet by its QPC and treats a source that has
// delivered nothing as idle. `PcmPacket::discontinuity` carries WASAPI's DATA_DISCONTINUITY and
// TIMESTAMP_ERROR flags; nothing consumes it yet.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

struct PcmPacket {
    std::vector<int16_t> samples;         // interleaved stereo s16, 48 kHz
    int64_t               qpc_ticks = 0;  // raw QPC ticks (converted from WASAPI's 100ns units)
    bool                   discontinuity = false;  // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
};

// Which endpoint AudioCapture opens. kDesktopLoopback
// (today's only behavior) opens the render endpoint's loopback stream; kMicrophone opens a capture
// endpoint with no loopback flag. `device_id` empty means "the default endpoint for that data
// flow" (IMMDeviceEnumerator::GetDefaultAudioEndpoint); non-empty is an IMMDevice::GetId() string
// passed to GetDevice(). Deliberately not the WASAPI process-loopback mode — that's a
// third, structurally different activation path (ActivateAudioInterfaceAsync on a virtual device,
// no IMMDevice at all), layered on top of this via AudioCapture::StartProcessLoopback().
enum class AudioEndpointKind { kDesktopLoopback, kMicrophone };
struct AudioEndpoint {
    AudioEndpointKind kind = AudioEndpointKind::kDesktopLoopback;
    std::wstring       device_id;  // empty = default endpoint for `kind`
};

// Which process tree a process-loopback source captures.
// `exclude=false` (include mode) is what "game_only"/"only" resolve to; `exclude=true` is
// "all_except". `target_pid` of 0 with `exclude=false` would capture nothing (documented: a target
// with no render streams yields silence, not an error) -- callers resolve a real PID before calling
// StartProcessLoopback, typically from BufferController::CurrentGamePid() for "game_only".
struct ProcessLoopbackTarget {
    DWORD target_pid = 0;
    bool  exclude    = false;
};

// Per-app source targeting, re-evaluated continuously by the engine's audio target watcher
// (main.cpp) so a target that starts, exits or relaunches after Ombra is picked up. Pure functions,
// covered by schematest.
struct ProcessEntry {
    DWORD        pid        = 0;
    DWORD        parent_pid = 0;
    std::wstring exe;
};

// The root of the running process tree named `exe` (case-insensitive): a match whose parent isn't
// also a match. Discord, browsers and the Riot client run several same-named processes and play
// audio from a child, so the tree modes need the root. 0 if nothing named `exe` is running.
DWORD FindTargetRootPid(const std::vector<ProcessEntry>& processes, const std::wstring& exe);

enum class AudioTargetCapture { kNothing, kTargetOnly, kAllExceptTarget, kAllDesktop };

// What a mode "only"/"all_except"/"game_only" source should capture, given its target's PID
// (0 = not running, or no game detected). A missing target records silence for only/game_only:
// falling back to the whole desktop mix put duplicate copies of it on every app track.
AudioTargetCapture WantedTargetCapture(const std::string& mode, DWORD target_pid);

// What any source should capture: nothing while switched off, otherwise WantedTargetCapture for a
// loopback source. kAllDesktop means the source's own endpoint -- its render device's loopback, or
// the microphone for kind "input" (which has no mode).
AudioTargetCapture WantedSourceCapture(bool enabled, const std::string& kind, const std::string& mode,
                                       DWORD target_pid);
// Whether a source's capture depends on a running process (mode only/all_except/game_only).
bool AudioSourceHasTarget(const std::string& kind, const std::string& mode);
const char* AudioTargetCaptureName(AudioTargetCapture capture);

// The audio target watcher's (main.cpp) per-source poll state: what is actually running right now,
// as opposed to what WantedTargetCapture says is wanted this poll. Pulled out as its own pure state
// machine (DecideTargetWatcherAction/NextTargetWatcherState below) so the watcher's start/relaunch/
// retry logic is testable without a live WASAPI process-loopback client.
struct TargetWatcherState {
    bool               started    = false;
    AudioTargetCapture capture    = AudioTargetCapture::kNothing;
    DWORD              pid        = 0;
    DWORD              failed_pid = 0;  // last pid whose process-loopback activation failed
};

// Whether this poll should (re)activate the target, and whether doing so is a retry of a
// previously-failed activation (the caller only warn-logs a fresh failure, not a repeat one).
// False with retrying=false means "already running what's wanted, nothing to do." False with
// retrying=true means "still failing, not yet due for its next 3-poll retry attempt."
struct TargetWatcherDecision {
    bool act      = false;
    bool retrying = false;
};
TargetWatcherDecision DecideTargetWatcherAction(const TargetWatcherState& cur, AudioTargetCapture want,
                                                 DWORD want_pid, int poll, int snapshot_every_polls);

// The state to record after attempting `want`/`want_pid` this poll. `activation_failed` is
// StartProcessLoopback's return value negated — meaningful only when `want` is kTargetOnly or
// kAllExceptTarget; ignored otherwise (kAllDesktop/kNothing don't call StartProcessLoopback at all,
// so they can't fail). The caller still owns the actual Stop()/Start()/StartProcessLoopback() I/O
// and any logging; this only computes what state that I/O left the source in.
TargetWatcherState NextTargetWatcherState(AudioTargetCapture want, DWORD want_pid, bool activation_failed);

// A loopback stream delivers nothing while its endpoint plays nothing. When sound resumes, the
// first buffer's worth of packets can carry stamps that continue from before the silence: on
// 2026-10-12 two 200 ms runs arrived stamped 14 s and 47 s old, each ~20 ms after the last packet
// before the silence, with no discontinuity flag (so no overflow, and no stalled thread). They
// landed behind the hub's encode cursor and were dropped (audio_late_ms). A true stamp can't be
// older than the buffer holds, so a packet older than that is restamped: a drain pass's stale
// packets are held, then stamped back to back by frame count so the run ends where the pass
// started reading (or at the next true stamp, if one follows in the same pass). One pass per
// packet or one pass for the whole run both come out right. A whole-run burst starts one buffer
// before now, so the hub's kLagSeconds must exceed the buffer. Capture thread only.
class LoopbackStampRepair {
public:
    using Emit = std::function<void(const PcmPacket&)>;

    // `capacity_ticks`: the client's buffer (GetBufferSize) in QPC ticks; shared mode can round
    // the requested duration up.
    void SetCapacity(int64_t capacity_ticks, int64_t qpc_freq);
    void BeginPass(int64_t now_qpc);
    void Push(PcmPacket&& packet, const Emit& emit);
    void EndPass(const Emit& emit);

    // A finished run of restamped packets, once, for one log line (it may span passes).
    struct Run {
        int64_t raw_age_ticks = 0;  // how old the first packet's own stamp was
        int64_t frames        = 0;
        int     packets       = 0;
        int     passes        = 0;
        int64_t span_ticks    = 0;  // first pass to last pass: ~0 for a burst, ~the run for a trickle
    };
    bool TakeFinishedRun(Run& out);

private:
    void Flush(int64_t end_qpc, const Emit& emit);
    void FinishRun();

    int64_t                max_age_ticks_ = 0;
    int64_t                qpc_freq_      = 1;
    int64_t                pass_qpc_      = 0;
    bool                   pass_had_stale_ = false;
    std::vector<PcmPacket> held_;
    bool                   run_active_    = false;
    int64_t                run_first_pass_qpc_ = 0;
    Run                    run_{};
    bool                   finished_      = false;
    Run                    finished_run_{};
};

// Whether the audio target watcher should hold every capture closed so the machine can sleep.
// An open WASAPI stream (mic or loopback) counts as an audio stream to Windows: it keeps the USB
// audio endpoint powered, exempts the process from Desktop Activity Moderation and stops Modern
// Standby reaching low-power idle (powercfg /sleepstudy: "Audio Streams: ombra.exe", 99% of a night).
// Closing on display-off alone would lose audio still wanted (a Discord call with the
// screen locked, then Alt+F10), so captures close only once the display has been off AND every
// output device silent for `kHoldPolls` consecutive polls (1 s each), and reopen on the first poll
// that sees the display on or any output audible. Nothing is lost by the hold itself: what it
// skips was silence, except a mic heard while every output is silent. Dimmed counts as on.
class AudioStandbyGate {
public:
    static constexpr int kHoldPolls = 300;
    bool Update(bool display_off, bool output_audible) {
        if (!display_off || output_audible) { idle_polls_ = 0; return false; }
        if (idle_polls_ < kHoldPolls) ++idle_polls_;
        return idle_polls_ >= kHoldPolls;
    }
private:
    int idle_polls_ = 0;
};

// Whether anything else is using audio: a render endpoint's peak above about -60 dBFS, or another
// process with a running stream (session state Active) on any render or capture endpoint, which a
// Discord call keeps even while nobody speaks. Opens no audio stream of its own, so it works while
// every capture is closed. Create and
// call it on one COM-initialised thread. Any failure reads as audible: the watcher then never holds,
// which is the behaviour before the gate existed.
class OutputActivityProbe {
public:
    OutputActivityProbe();
    ~OutputActivityProbe();
    bool Audible();
private:
    struct Impl;
    Impl* impl_;
};

// What the Audio settings page shows on each source's card (audio_status pipe request). A coarse
// state, never a level: levels don't go over the pipe.
enum class AudioSourceState {
    kLive,     // capturing, and something audible arrived in the last kAudibleHoldSeconds
    kQuiet,    // capturing, nothing audible (speakers stream zeros; idle USB outputs send nothing)
    kWaiting,  // only: the target app isn't running
    kNoGame,   // game_only: no game detected
    kFailed,   // the app is running but process-loopback activation failed (retried every 3 s)
    kOff,      // switched off (audio.sources[].enabled = false): nothing captured
    kPaused,   // on, but closed for now: nothing is recording, so nothing would keep it
};
const char* AudioSourceStateName(AudioSourceState state);

// Twice-plus the overlay's 1 s poll, so a tag doesn't flicker between words of speech.
constexpr double kAudibleHoldSeconds = 2.5;

// `watch` is null for a source without a target (mode "all", microphones): those always capture
// while enabled. `last_audible_qpc` is AudioTrackHub::LastAudibleQpc (0 = never).
AudioSourceState ClassifyAudioSource(bool enabled, const std::string& mode, const TargetWatcherState* watch,
                                     int64_t last_audible_qpc, int64_t now_qpc, int64_t qpc_freq);

// What the audio target watcher is running, per source, for audio_status on a pipe thread. Only
// the watcher writes it: Init when its source list changes, SetWatch as it starts and stops
// captures. Off every capture and encode path.
class AudioSourceBoard {
public:
    struct Entry {
        std::string id;
        std::string mode;
        bool        watched = false;  // has a target; starts not-yet-started (kWaiting/kNoGame)
        bool        enabled = true;
        size_t      slot    = 0;      // the source's AudioTrackHub slot
    };
    // `watch`: per entry, what is running now (empty: nothing yet). One lock, so audio_status never
    // reads a new list without its watch states.
    void Init(std::vector<Entry> entries, std::vector<TargetWatcherState> watch = {});
    void SetWatch(size_t source, const TargetWatcherState& state);
    void SetPaused(bool paused);  // every capture closed because every ring is released

    struct Status {
        std::string      id;
        AudioSourceState state = AudioSourceState::kQuiet;
    };
    // `last_audible_qpc` takes an Entry::slot.
    std::vector<Status> Snapshot(const std::function<int64_t(size_t)>& last_audible_qpc, int64_t now_qpc,
                                 int64_t qpc_freq) const;

private:
    mutable std::mutex              mutex_;
    std::vector<Entry>              entries_;
    std::vector<TargetWatcherState> watch_;
    bool paused_ = false;
};

// What the endpoint actually negotiated, before conversion to the fixed s16/stereo/48kHz output
// format — diagnostic only, so a caller (e.g. audio_hw_tests.cpp) can tell whether a given run
// exercised the passthrough path or the resampler, rather than guessing from the machine's Sound
// settings.
struct NegotiatedFormat {
    uint32_t sample_rate_hz = 0;
    uint32_t channels       = 0;
    uint32_t bits_per_sample = 0;
    bool     is_float        = false;
    bool     was_resampled   = false;
};

class AudioCapture {
public:
    using PcmCallback = std::function<void(const PcmPacket&)>;

    // Opens `endpoint` (default: the render endpoint's loopback stream) and starts a dedicated
    // capture thread. `cb` is invoked once per WASAPI packet, synchronously on that thread — the
    // caller must not hold the PcmPacket past the callback's return. The first open happens before
    // Start() returns; if it fails (no such endpoint right now) the thread logs once and keeps
    // retrying every 500 ms, delivering nothing meanwhile, and `Negotiated()` stays zeroed until it
    // opens. After Stop(), Start() or StartProcessLoopback() may be called again on the instance.
    void Start(PcmCallback cb, const AudioEndpoint& endpoint = {});

    // Process loopback via ActivateAudioInterfaceAsync on
    // the VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK virtual device -- a different activation path from
    // Start() entirely (no IMMDevice; the virtual device isn't tied to a physical endpoint, which is
    // why a source built this way survives a default-output-device switch mid-session that would
    // break plain loopback). Returns false (does not die()) if activation fails anywhere in the
    // chain -- ActivateAudioInterfaceAsync itself, or the resulting IAudioClient::Initialize -- so
    // the caller can fall back to a plain Start() (`mode: "all"`), the documented fallback
    // for this exact failure. `cb`/`Stop()`/`Negotiated()` are otherwise
    // identical to the Start() path. One capture runs per instance at a time: Stop() before
    // starting it again either way.
    bool StartProcessLoopback(PcmCallback cb, const ProcessLoopbackTarget& target);

    void Stop();

    // A process-loopback stream that started and then died (Windows Audio restarted, its device
    // went away). The thread has exited; the watcher starts it over, being the only thread that may.
    // A plain Start() capture reopens by itself and never reports this.
    bool StreamLost() const { return stream_lost_.load(std::memory_order_acquire); }

    const NegotiatedFormat& Negotiated() const { return negotiated_; }

    // Negotiated output rate — always 48000. Exposed for callers that need to reason
    // about packet durations without hardcoding the constant a second place.
    static constexpr uint32_t kSampleRateHz = 48000;
    static constexpr uint32_t kChannels     = 2;

private:
    void Run(PcmCallback cb, AudioEndpoint endpoint);
    // Same shape as Run() but activation can genuinely fail in ordinary operation (a driver too old
    // for process loopback, a Windows build below 20348) -- unlike Run()'s die()-on-setup-failure,
    // this reports success/failure into `*ok` instead.
    void RunProcessLoopback(PcmCallback cb, ProcessLoopbackTarget target, bool* ok);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stream_lost_{false};
    NegotiatedFormat  negotiated_;

    // Set once by Run()/RunProcessLoopback() during setup and read by Start()/StartProcessLoopback()
    // to fail loudly (or return false) on the caller's thread instead of only inside the capture
    // thread, where a die() would be invisible until the process exit code was checked.
    HANDLE ready_or_failed_ = nullptr;
};

// "which processes are making sound right now", for the
// drag-to-group audio panel's auto-populated card list (list_audio_sessions pipe query, main.cpp).
// Distinct from ProcessLoopbackTarget above -- this is discovery ("what's live"), that is capture
// ("record exactly this PID"); the panel resolves a card's exe name back to a `target` string the
// same way today's `mode=only`/`all_except` matching already does (FindPidByExeName, main.cpp), not
// by keeping this call's PID around.
struct AudioSessionInfo {
    DWORD       pid = 0;
    std::string exe_name;  // bare filename, e.g. "discord.exe" -- empty if the owning process
                            // couldn't be opened (PROCESS_QUERY_LIMITED_INFORMATION denied)
    std::string path;       // full image path, empty under the same condition as exe_name
    std::string display_name;  // controller.h's ReadVersionResourceName(path), e.g. "Discord" for
                                // discord.exe -- the same FileDescription Windows' own Volume Mixer
                                // shows. Falls back to exe_name (never empty when exe_name isn't).
};
// Enumerates active sessions on the default render endpoint via IAudioSessionManager2 (deduped by
// PID -- one process can own several sessions, e.g. multiple tabs), skipping the system-sounds
// session. Never die()s: this backs a Settings picker, same "routine, retryable, must survive
// failure" contract as EnumerateAllMonitors/EnumerateCapturableWindows (capture.h) -- an empty
// vector is returned on any COM/WASAPI failure, not an error out-param, since there's no
// distinction a caller could usefully act on beyond "nothing to show right now". Must be called
// with COM already initialized MTA on the caller's thread, same contract as
// CaptureWindowThumbnailToPng (capture.h).
std::vector<AudioSessionInfo> ListActiveAudioSessions();

struct AudioDeviceInfo {
    std::string id;     // IMMDevice::GetId() UTF-8, what AudioSourceSpec::device/AudioEndpoint::
                         // device_id store -- stable across reboots, tied to the physical endpoint.
    std::string name;   // PKEY_Device_FriendlyName UTF-8, e.g. "Speakers (Realtek Audio)"
    bool        is_default = false;  // eConsole's current default endpoint for this direction
};
// Every active (DEVICE_STATE_ACTIVE) render endpoint (`input` false: the Desktop device picker) or
// capture endpoint (`input` true: the Mic device picker) -- same never-die()s/empty-on-failure
// contract as ListActiveAudioSessions above. Must be called with COM already initialized MTA on the
// caller's thread.
std::vector<AudioDeviceInfo> ListAudioDevices(bool input);

}  // namespace ombra
