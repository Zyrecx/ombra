// Sends crash dumps (crash.h) to Sentry as envelopes: an event, the minidump, and the scrubbed log
// lines before the crash. Also sends error events (ReportError): failures that don't crash, queued
// by the engine as files and sent by the same child.
//
// The network side runs in a short-lived child, `ombra.exe crash-upload`, so WinHTTP and schannel
// never stay loaded in the resident engine (update.h's pattern). Only the engine starts it, and it
// covers the overlay's dumps too, so a dump is sent by exactly one process.
//
// A dump is sent only if sharing was on when it was written (the `share: yes` line crash.cpp puts in
// its .txt) and is still on now. Dumps from before that line existed are never sent. A crash before
// setup was finished (`share: ask`) is sent only once the user answered Send to the overlay's ask
// (`<base>.asked`), whatever sharing is then.
//
// What leaves the PC is first written to <data_dir>\sent_reports\<event id>.txt.
//
// A DSN is compiled in only by release builds (CMake OMBRA_SENTRY_DSN), so a development build never
// uploads. An isolated instance (OMBRA_HOME) ignores that DSN and sends only to a 127.0.0.1 DSN in
// OMBRA_TEST_SENTRY_DSN, for tests; only an isolated instance reads that variable.
#pragma once

#include "ombra/json.h"
#include "ombra/latency.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra::crash_upload {

// ---- Pure parts (tests/crash_upload_tests.cpp) ----

struct Dsn {
    bool          secure = true;
    std::string   key;
    std::string   host;
    unsigned short port = 443;
    std::string   project;
};
// https://KEY@HOST[:PORT]/PROJECT. Plain http only for 127.0.0.1.
std::optional<Dsn> ParseDsn(const std::string& text);

// A dump's .txt, as crash.cpp wrote it.
struct CrashText {
    std::string exe, version, commit, kind, thread, reason;
    bool        share = false;
    bool        ask   = false;  // before setup: sent only if the user answers Send (crash::AnswerCrash)
};
std::optional<CrashText> ParseCrashText(const std::string& text);

// The lines up to and including the one logging this crash; empty if the log doesn't hold it.
std::string LogBeforeCrash(const std::string& log, const CrashText& crash, size_t max_lines);

// The settings the engine logged at its last start before this crash; empty if there is no such line.
std::string SettingsBeforeCrash(const std::string& log, const CrashText& crash);

// The date in "<exe>-YYYYMMDD-HHMMSS-<pid>" (exe may hold dashes); empty if it isn't that shape.
std::string DumpDate(const std::wstring& base_name);

struct EventInput {
    std::string event_id;
    double      timestamp = 0;  // Unix seconds
    CrashText   crash;          // reason already scrubbed
    std::string system;         // GPUs and Windows build
    std::string settings;
    std::string user_id;        // the install ID at the usage stop, else empty (no `user`)
};
std::string BuildEventJson(const EventInput& input);
std::string BuildEnvelope(const std::string& event_id, const std::string& event_json, const std::string& dump,
                          const std::string& dump_name, const std::string& log);

// ---- Error events: failures the engine recovers from or reports, without a crash ----

// Exit: the previous engine run ended without exiting and without a crash dump (BSOD, power loss,
// __fastfail, killed).
enum class ErrorKind { Save, Encoder, Capture, Audio, Update, Exit };
const char* ErrorKindName(ErrorKind kind);

// Queues one error for the next upload pass, if uploading is allowed now (an error while sharing is
// Off is never sent later). kind + code + vendor is the Sentry issue, so `code` and `vendor` are
// stable tokens: no paths, device names, sizes or other text that varies per machine. `detail` is
// a fixed phrase for the event title. At most kMaxErrorsPerFingerprint events per issue and
// kMaxErrorsPerDay in all per UTC day; past that an error is only counted, and each event sent
// carries the day's count so far. `log` replaces the tail of today's log. Also marks today's
// session errored. File I/O: never from the pacer's tick path.
void ReportError(ErrorKind kind, const std::string& code, const std::string& vendor, const std::string& detail = {},
                 const std::optional<std::string>& log = std::nullopt);

inline constexpr int kMaxErrorsPerFingerprint = 3;
inline constexpr int kMaxErrorsPerDay         = 25;

// crashes\error_counts: "YYYYMMDD" (UTC), then "<occurrences> <queued> <fingerprint>" per line.
struct ErrorCounts {
    struct Entry {
        std::string fingerprint;
        int         occurrences = 0;
        int         queued      = 0;
    };
    std::string        day;
    std::vector<Entry> entries;
};
ErrorCounts ParseErrorCounts(const std::string& text);
std::string WriteErrorCounts(const ErrorCounts& counts);
// Counts one occurrence on `today` (a new day starts over); true when it may also be queued.
bool CountError(ErrorCounts& counts, const std::string& today, const std::string& fingerprint);
// The day's occurrences of `fingerprint`, or 0 if `counts` is from another day.
int OccurrencesOn(const ErrorCounts& counts, const std::string& day, const std::string& fingerprint);
std::string ErrorFingerprint(const std::string& kind, const std::string& code, const std::string& vendor);

// A queued error, crashes\error-<event id>.json.
struct QueuedError {
    std::string event_id, kind, code, vendor, detail, version, commit, day, log;
    double      timestamp = 0;
    int         occurrences = 1;  // the day's count when it was queued
};
std::string           WriteQueuedError(const QueuedError& e);
std::optional<QueuedError> ParseQueuedError(const std::string& text);

struct ErrorEventInput {
    QueuedError error;  // detail and log already scrubbed
    int         occurrences_today = 1;
    std::string system;
    std::string user_id;  // the install ID at the usage stop, else empty (no `user`)
};
std::string BuildErrorEventJson(const ErrorEventInput& input);
std::string BuildErrorEnvelope(const std::string& event_id, const std::string& event_json, const std::string& log);

// ---- Release health: one session per UTC day and release, sent once the day is over ----
//
// Sentry's crash-free rate and daily active installs per version. A day's session is the worst that
// happened in it: crashed (a dump of any Ombra process) > abnormal (the engine ended without exiting
// and without a dump) > errored (an error event) > exited. No ID, except the install ID as `did` at
// the usage stop. Sentry takes sessions up to 5 days old; older days are dropped unsent.

struct DaySession {
    std::string day;  // YYYYMMDD, UTC
    std::string release;
    int64_t     started = 0;  // Unix seconds, the first start that day
    bool        errored = false, abnormal = false, crashed = false;
};
const char* SessionStatus(const DaySession& s);  // "crashed" | "abnormal" | "errored" | "exited"

// crashes\sessions: "seen <filetime of the newest dump counted>", then one DaySession a line.
struct SessionState {
    uint64_t                dumps_seen = 0;
    std::vector<DaySession> days;
};
SessionState ParseSessionState(const std::string& text);
std::string  WriteSessionState(const SessionState& s);
std::string            WriteDaySession(const DaySession& s);
std::optional<DaySession> ParseDaySession(const std::string& line);

// Moves every day before `today` into `closed` and makes sure today has a session for `release`.
void RollSessions(SessionState& s, const std::string& today, const std::string& release, int64_t now,
                  std::vector<DaySession>& closed);
// The session a crash or an unclean exit belongs to: `day`'s newest for `release`, else `day`'s
// newest, else the newest of all. Null if there are none.
DaySession* SessionFor(SessionState& s, const std::string& day, const std::string& release);

// The `sessions` item (one aggregate) and its envelope.
std::string BuildSessionsJson(const DaySession& s, const std::string& did);
std::string BuildSessionEnvelope(const std::string& sessions_json);

// ---- Usage: Sentry Logs, only at the usage stop ----
//
// One Log per game session: the game's exe name, how long, how it was recorded. Each carries the
// install ID, nothing about where the PC is (no IP, no region). The engine appends a
// session to crashes\usage-games-<UTC day it ended>.txt; the child sends a day's file once the day
// is over, so the engine never writes a file the child is reading. Logs, not error events: their
// own allowance, apart from the error quota.

struct GameSession {
    int64_t     start = 0, end = 0;  // Unix seconds
    std::string encoder;              // BackendName
    std::string codec;                // VideoCodecName
    uint32_t    width = 0, height = 0, fps = 0;
    int         saves = 0;
    std::string exe;  // file name only, never the path
    // Which Games-list entry the exe belongs to: a hash of the entry's GameKey, empty for a program
    // the list doesn't know. Local only, never sent: it joins one game's exes (a launcher's client
    // and its match process) into one session.
    std::string group;
};
inline constexpr int     kMaxGameSessionsPerDay = 20;
inline constexpr int64_t kGameSessionJoinGap    = 300;  // an engine restart mid-game, client <-> match
inline constexpr int64_t kMinGameSeconds        = 60;   // a launcher flashing past isn't a session

std::string                WriteGameSession(const GameSession& s);  // one line
std::optional<GameSession> ParseGameSession(const std::string& line);
// The `group` for a Games-list entry's GameKey ("" for none): FNV-1a 64 as hex.
std::string GameSessionGroup(const std::wstring& game_key);
// One day's records ready to send: parts of one game (same group, or same exe with no group, the
// next starting within kGameSessionJoinGap of the last one's end: an engine restart, a launcher's
// client and match) joined under the exe it ran longest as, recorded as its longest built part was;
// sessions under kMinGameSeconds dropped, the kMaxGameSessionsPerDay longest kept, in start order.
std::vector<GameSession> JoinGameSessions(std::vector<GameSession> sessions);

struct LogContext {
    std::string release, environment, install_id;
};
// A `log` item's payload with one Log per session, and its envelope.
std::string BuildGameSessionLogs(const std::vector<GameSession>& sessions, const LogContext& ctx);
std::string BuildLogEnvelope(const std::string& payload, size_t count);

// The engine's side: queues a finished session, if sharing is at the usage stop now.
void RecordGameSession(const GameSession& s);

// ---- The daily Log ----
//
// One Log per UTC day: the engine's own counters for that day, and its settings from an allow-list
// (never a path, a device or window name, or anything typed). Not from the telemetry CSVs: that
// tracking can be off and promises nothing leaves the PC. The engine appends one line per run to
// crashes\usage-day-<UTC day>.txt every few minutes and as the day or the run ends; the child takes
// each run's last line and sums the runs, so a restart (a monitor change, an update) loses nothing
// and a crash at most the last few minutes.

// Every setting key is in exactly one of these lists (a test checks it), so a new setting is never
// sent until it has been put in one.
// Sent as they are (enum values normalised, see UsageSettings).
inline constexpr const char* kUsageSettingsSent[] = {
    "capture.capture_cursor", "output.per_game_folders", "engine.autostart", "ui.toast_enabled",
    "ui.toast_on_save", "ui.toast_on_buffer_toggle", "ui.screen_frame", "ui.memory_card", "ui.card_borders",
    "audio.full_mix_track", "audio.per_app_mode", "updates.check", "updates.auto_install", "engine.telemetry",
    "capture.framerate", "encode.bitrate_kbps", "encode.max_bitrate_kbps", "encode.cq", "encode.keyframe_sec",
    "buffer.max_bytes", "audio.bitrate_kbps", "log.max_bytes", "ui.screen_frame_radius",
    "ui.screen_frame_thickness", "buffer.seconds", "encode.preset", "encode.quality_preset", "capture.mode",
    "capture.target", "capture.resolution", "encode.adapter", "encode.video_codec", "encode.amf_av1_unaligned", "encode.rate_control",
    "buffer.storage", "audio.codec", "save.overlap_policy", "hotkeys.backend", "ui.theme", "ui.accent",
    "ui.panel_position", "ui.panel_attach", "ui.screen_frame_color", "ui.rail_position", "ui.corners",
    "log.level", "ui.font", "ui.density", "ui.toast_position", "ui.toast_style"};
// Sent only as whether they differ from the default (`<key>.changed`): typed or picked by the user.
inline constexpr const char* kUsageSettingsChangedOnly[] = {"output.filename", "hotkeys.save_replay",
                                                            "hotkeys.toggle_buffer", "hotkeys.screenshot",
                                                            "hotkeys.overlay"};
// Never sent: paths, window titles, device and app names, user lists; diagnostics.sharing is always
// "usage" when this is sent at all.
inline constexpr const char* kUsageSettingsNever[] = {"output.root",      "buffer.file_path", "capture.target_window",
                                                      "capture.monitors", "audio.sources",    "games.list",
                                                      "ui.themes",        "engine.autotune",  "diagnostics.sharing"};

// The allow-listed part of SerializeConfigFlat's output: kUsageSettingsSent as they are, except
// ui.accent's #rrggbb as "custom", ui.theme's user<n> as "user" and an encode.preset outside p1-p7
// as "other"; kUsageSettingsChangedOnly as booleans. Anything else is dropped.
JsonValue UsageSettings(const JsonValue& flat);

// What the engine knows about itself, cumulative since it started (the telemetry probe's sources).
struct UsageCounters {
    bool         armed = false;
    TickCounters ticks;  // summed across pipelines
    uint64_t     rebuilds = 0, resolution_changes = 0, saves = 0;
    uint64_t     audio_dropped_frames = 0, audio_late_frames = 0, audio_encoder_errors = 0;
    uint64_t     ring_committed = 0;  // bytes, now
    double       private_mb = 0;      // the engine's private memory, now
};

// One engine run's share of a day, one line of a usage-day file.
struct DayRun {
    std::string run;  // "<pid>-<start, Unix seconds>"
    int64_t     up_seconds = 0, armed_seconds = 0;
    uint64_t    ticks = 0, duplicates = 0, late = 0, resyncs = 0;
    uint64_t    rebuilds = 0, resolution_changes = 0, saves = 0;
    uint64_t    audio_dropped_frames = 0, audio_late_frames = 0, audio_encoder_errors = 0;
    uint64_t    peak_private_mb = 0, peak_ring_mb = 0;
    LatencyHistogram::Snapshot encode;
    uint32_t    monitors = 0, audio_sources = 0;
    JsonValue   settings;  // UsageSettings
};
std::string           WriteDayRun(const DayRun& r);  // one line
std::optional<DayRun> ParseDayRun(const std::string& line);

// A run's day so far: counters since the day (or the run) began, recording time, peaks.
class DayUsage {
public:
    explicit DayUsage(std::string run) : run_(std::move(run)) {}
    // Starts a day: what the counters say now is its zero.
    void Begin(const UsageCounters& c, int64_t now);
    // About once a minute: time up and recording since the last sample (if it was recording then),
    // peaks. A longer gap (the PC slept, the clock moved) counts as kMaxSampleGap.
    void Sample(const UsageCounters& c, int64_t now);
    // The day so far, as of `c` (the last Sample's counters).
    DayRun Totals(const UsageCounters& c, int64_t now) const;

    static constexpr int64_t kMaxSampleGap = 150;

private:
    std::string   run_;
    UsageCounters base_;
    int64_t       last_ = 0, up_seconds_ = 0, armed_seconds_ = 0;
    bool          last_armed_ = false;
    uint64_t      peak_private_mb_ = 0, peak_ring_mb_ = 0;
};

// A day's runs (each run's last line wins) summed into one; the settings of the run that wrote last.
// Empty `run` if there were none.
DayRun SumDayRuns(const std::vector<DayRun>& lines);
// The daily Log's payload: one item stamped at the end of `day` (YYYYMMDD).
std::string BuildDailyLog(const DayRun& day_total, const std::string& day, const LogContext& ctx);

// Appends `r` to crashes\usage-day-<day>.txt, if sharing is at the usage stop now.
void RecordDayRun(const DayRun& r, const std::string& day);

// The engine hands the Worker these before Start; it samples them once a minute.
struct UsageProbe {
    std::function<UsageCounters()> counters;
    // UsageSettings of the live config, and the monitor and audio source counts.
    std::function<void(DayRun&)> describe;
};

// Past days' usage files (usage-games-*, usage-day-*) waiting to go (file names in crashes\).
// Deletes them all when sharing is below the usage stop, and those older than Sentry takes.
std::vector<std::wstring> PendingUsage();

// The lines up to and including the last one written by `tag` (a log line's "[E <pid> " part), at
// most `max_lines`; empty if `tag` never wrote. The previous engine run's last words.
std::string LogTailOf(const std::string& log, const std::string& tag, size_t max_lines);

// ---- Send logs: the user's own upload of the diagnostics bundle (diagnostics.h) ----
//
// `ombra.exe send-logs <zip> <event id>`, started by the overlay when the user presses Send. The
// press is the consent, so it ignores diagnostics.sharing and setup, but never sends without a DSN.
// A Sentry user feedback (its own category, not the error quota) with the zip attached; the event id
// is the reference the user pastes into a GitHub issue. The record lists the zip's files.

inline constexpr size_t kMaxLogsBytes      = 20u * 1024 * 1024;
inline constexpr int    kMaxLogsSendsPerDay = 5;

struct LogsInput {
    std::string event_id;
    double      timestamp = 0;  // Unix seconds
    std::string release;
    std::string system;  // scrubbed
};
std::string BuildLogsFeedbackJson(const LogsInput& input);
std::string BuildLogsEnvelope(const std::string& event_id, const std::string& feedback_json, const std::string& zip,
                              const std::string& zip_name);
// The file names in a zip's central directory, in order; empty if it isn't a zip.
std::vector<std::string> ZipEntryNames(const std::string& zip);

enum class SendLogsResult : int { Sent = 0, Failed = 1, NoDsn = 2, Unreadable = 3, TooMany = 4 };
// The child's body; its exit code. `event_id` comes from the overlay (crash::NewReportId). Failed:
// the post didn't get a 2xx (the record has the answer). Whatever the outcome it deletes the zip if
// it is the overlay's own copy (data_dir\<crash::kSendLogsZipPrefix>...).
SendLogsResult RunSendLogs(const std::wstring& zip_path, const std::string& event_id);

// ---- The engine's side of sessions (resident engine only) ----

// At start, after the single-instance lock: notes how the previous run ended (an unclean exit
// becomes an abnormal session and an Exit error), writes this run's crashes\running marker, then
// TickSessions.
void BeginRun();
// A clean exit: the pump loop ended, or Windows is ending the session (WM_ENDSESSION). Removes the
// marker, so the next start doesn't call this run abnormal.
void EndRun();
// Counts new dumps as crashes, closes past days into the upload queue (crashes\session-*.txt) if
// uploading is allowed, opens today's. The worker calls it after each UTC midnight.
void TickSessions();

// ---- The upload pass ----

// True when uploading is possible and allowed now: a usable DSN, setup finished, sharing not Off
// (the crashes\\sharing_on marker the engine keeps in step with the setting).
bool UploadAllowed();
// UploadAllowed, and sharing is at the usage stop (the marker holds "usage").
bool UsageAllowed();
// The random install ID (data_dir\install_id), created on first use. Only sent at the usage stop.
std::string InstallId();

// Dumps waiting to go (base names), newest first.
std::vector<std::wstring> PendingDumps();
// `share: ask` dumps the user answered Send for, not sent yet, newest first. Go whenever a DSN is
// active, apart from UploadAllowed.
std::vector<std::wstring> AnsweredDumps();
// Queued errors (file names in crashes\), oldest first.
std::vector<std::wstring> PendingErrors();
// Closed days waiting to go (file names in crashes\); deletes those past Sentry's 5 days.
std::vector<std::wstring> PendingSessions();

// Brings the running Worker's next pass forward, as a newly queued error does (the pipe's
// `upload_soon`, after the user answered Send to a crash ask). No-op without a Worker.
void UploadSoon();

// `ombra.exe crash-upload`. 0 when nothing is left to retry, 1 otherwise.
int RunUploadChild();

// The engine's side: runs the child a minute after start, then every few hours, if a dump or an
// error or a closed day is waiting; a newly queued error brings the next pass forward to a couple of
// minutes later, so a burst goes in one child. Also wakes just after each UTC midnight to close the day,
// and, given a UsageProbe, once a minute to keep the daily Log's counts.
class Worker {
public:
    ~Worker();
    void SetUsageProbe(UsageProbe probe) { usage_probe_ = std::move(probe); }  // before Start
    void Start();
    void Stop();
    void ErrorQueued();

private:
    void Loop();
    // A sample; a usage-day line every few minutes, when the UTC day changes, and with `final`.
    void SampleUsage(bool final);

    UsageProbe                usage_probe_;
    std::optional<DayUsage>   usage_;
    std::string               usage_day_;
    int                       usage_samples_ = 0;

    std::thread             thread_;
    std::mutex              mutex_;
    std::condition_variable wake_;
    bool                    stopping_ = false;
    bool                    error_queued_ = false;
    HANDLE                  stop_event_ = nullptr;  // ends a running child's wait
};

}  // namespace ombra::crash_upload
