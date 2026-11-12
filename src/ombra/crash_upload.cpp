#include "ombra/crash_upload.h"

#include "ombra/config.h"
#include "ombra/crash.h"
#include "ombra/diagnostics.h"
#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/sent_records.h"
#include "ombra/util.h"
#include "ombra/version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

namespace ombra::crash_upload {

namespace {

constexpr int      kMaxUploadsPerDay = 5;  // a crash loop must not spend the project's event quota
constexpr int      kMaxDumpAgeDays   = 14;
constexpr size_t   kMaxDumpBytes     = 32u * 1024 * 1024;
constexpr size_t   kLogLines         = 300;
constexpr size_t   kMaxLogBytes      = 128 * 1024;
constexpr DWORD    kChildTimeoutMs   = 5 * 60 * 1000;

std::string ReadFileBytes(const std::wstring& path, size_t max_bytes) {
    // Share everything: today's log is open for append in the engine.
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string data(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, static_cast<LONGLONG>(max_bytes))), '\0');
    DWORD       got = 0;
    if (!data.empty() && !ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
    CloseHandle(f);
    data.resize(got);
    return data;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << data;
    return f.good();
}

// The child reads these while the engine writes them: never a half-written file.
bool WriteFileAtomic(const std::wstring& dir, const std::wstring& name, const std::string& data) {
    const std::wstring tmp = dir + L"\\tmp-" + name;
    if (!WriteFileBytes(tmp, data)) return false;
    return MoveFileExW(tmp.c_str(), (dir + L"\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// The last whole lines of a file, up to `max_bytes` and `max_lines`.
std::string ReadFileTail(const std::wstring& path, size_t max_bytes, size_t max_lines) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - static_cast<LONGLONG>(max_bytes));
    LARGE_INTEGER  at{};
    at.QuadPart = start;
    std::string data(static_cast<size_t>(size.QuadPart - start), '\0');
    DWORD       got = 0;
    if (data.empty() || !SetFilePointerEx(f, at, nullptr, FILE_BEGIN) ||
        !ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr))
        got = 0;
    CloseHandle(f);
    data.resize(got);
    if (start > 0) {
        const size_t nl = data.find('\n');
        data.erase(0, nl == std::string::npos ? data.size() : nl + 1);
    }
    size_t cut = data.size(), lines = 0;
    while (cut > 0 && lines <= max_lines) {
        const size_t nl = data.rfind('\n', cut - 1);
        if (nl == std::string::npos) { cut = 0; break; }
        if (++lines > max_lines) { cut = nl + 1; break; }
        cut = nl;
    }
    return data.substr(cut);
}

std::string GetEnvUtf8(const wchar_t* name) {
    wchar_t     buf[512];
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n > 0 && n < ARRAYSIZE(buf) ? NarrowUtf8(std::wstring(buf, n)) : std::string();
}

std::optional<Dsn> ActiveDsn() {
    if (!Instance().isolated) return ParseDsn(SentryDsn());
    auto dsn = ParseDsn(GetEnvUtf8(L"OMBRA_TEST_SENTRY_DSN"));
    if (dsn && dsn->host != "127.0.0.1") return std::nullopt;  // tests never reach a real project
    return dsn;
}

std::string NewEventId() {
    unsigned char bytes[16] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    return hex;
}

ULONGLONG FileTimeValue(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
ULONGLONG NowFileTime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return FileTimeValue(ft);
}
constexpr ULONGLONG kTicksPerSecond = 10000000ull;
constexpr ULONGLONG kUnixEpochTicks = 116444736000000000ull;

std::string UtcDay() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::string UtcDayOf(ULONGLONG filetime) {
    FILETIME   ft{static_cast<DWORD>(filetime), static_cast<DWORD>(filetime >> 32)};
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

int64_t UnixOf(ULONGLONG filetime) { return static_cast<int64_t>((filetime - kUnixEpochTicks) / kTicksPerSecond); }

std::string ReleaseName() { return std::string("ombra@") + AppVersion() + "+" + BuildCommit(); }

void SetUser(JsonValue& event, const std::string& user_id) {
    if (user_id.empty()) return;
    JsonValue user = JsonValue::MakeObject();
    user.Set("id", JsonValue::MakeStr(user_id));
    event.Set("user", std::move(user));
}

// "YYYYMMDD n" in crashes\upload_count: today's uploads, kept across runs (a crash loop restarts).
std::wstring CountPath() { return crash::CrashesDir() + L"\\upload_count"; }
int UploadsToday() {
    std::istringstream in(ReadFileBytes(CountPath(), 64));
    std::string        day;
    int                n = 0;
    return (in >> day >> n) && day == UtcDay() ? n : 0;
}
void CountUpload() { WriteFileBytes(CountPath(), UtcDay() + " " + std::to_string(UploadsToday() + 1)); }

struct PostResult {
    bool        reached = false;  // the server answered
    DWORD       status  = 0;
    std::string error;
};

struct WinHttpHandle {
    HINTERNET h;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle&)            = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

PostResult PostEnvelope(const Dsn& dsn, const std::string& body) {
    PostResult         r;
    const std::wstring agent = L"Ombra/" + WideFromUtf8(AppVersion());
    WinHttpHandle      session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        r.error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return r;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 60000, 60000);
    WinHttpHandle      connect(WinHttpConnect(session.h, WideFromUtf8(dsn.host).c_str(), dsn.port, 0));
    const std::wstring object = L"/api/" + WideFromUtf8(dsn.project) + L"/envelope/";
    WinHttpHandle request(connect.h ? WinHttpOpenRequest(connect.h, L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         dsn.secure ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr);
    if (!request.h) {
        r.error = "couldn't open the request (" + std::to_string(GetLastError()) + ")";
        return r;
    }
    const std::wstring headers = L"Content-Type: application/x-sentry-envelope\r\nX-Sentry-Auth: Sentry sentry_version=7, "
                                 L"sentry_client=ombra/" + WideFromUtf8(AppVersion()) + L", sentry_key=" +
                                 WideFromUtf8(dsn.key) + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = "WinHTTP error " + std::to_string(GetLastError());
        return r;
    }
    DWORD size = sizeof(r.status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &r.status, &size, WINHTTP_NO_HEADER_INDEX);
    r.reached = true;
    return r;
}

std::wstring RecordsDir() { return sent_records::Dir(); }

void PruneRecords() {
    struct Entry { std::wstring name; ULONGLONG written; };
    std::vector<Entry> records;
    WIN32_FIND_DATAW   fd{};
    const HANDLE       h = FindFirstFileW((RecordsDir() + L"\\*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do records.push_back({fd.cFileName, FileTimeValue(fd.ftLastWriteTime)});
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(records.begin(), records.end(), [](const Entry& a, const Entry& b) { return a.written > b.written; });
    for (size_t i = sent_records::kKeep; i < records.size(); ++i) DeleteFileW((RecordsDir() + L"\\" + records[i].name).c_str());
}

// base.sent holds how it ended; PruneDumps removes it with the dump.
void MarkDone(const std::wstring& base, const std::string& how) {
    WriteFileBytes(crash::CrashesDir() + L"\\" + base + L".sent", how + "\n");
}

enum class Outcome { Sent, Dropped, Retry, StopPass };

Outcome UploadOne(const Dsn& dsn, const std::wstring& base, const ScrubContext& scrub, const std::string& system) {
    const std::wstring dir   = crash::CrashesDir();
    const auto         crash = ParseCrashText(ReadFileBytes(dir + L"\\" + base + L".txt", 8192));
    const std::wstring dump_path = dir + L"\\" + base + L".dmp";
    const std::string  dump      = ReadFileBytes(dump_path, kMaxDumpBytes + 1);
    if (!crash || dump.empty() || dump.size() > kMaxDumpBytes) {
        MarkDone(base, "dropped: unreadable or too large");
        return Outcome::Dropped;
    }

    std::string day_log;
    if (const std::string date = DumpDate(base); !date.empty())
        day_log = ReadFileBytes(Instance().data_dir + L"\\logs\\ombra-" + WideFromUtf8(date) + L".log", 64u * 1024 * 1024);
    std::string log = ScrubText(LogBeforeCrash(day_log, *crash, kLogLines), scrub);
    const std::string settings = SettingsBeforeCrash(day_log, *crash);
    if (log.size() > kMaxLogBytes) log.erase(0, log.size() - kMaxLogBytes);

    EventInput input;
    input.event_id     = NewEventId();
    input.crash        = *crash;
    input.crash.reason = ScrubText(crash->reason, scrub);
    input.system       = ScrubText(system, scrub);
    input.settings     = ScrubText(settings, scrub);
    if (UsageAllowed()) input.user_id = InstallId();
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    const ULONGLONG written = GetFileAttributesExW(dump_path.c_str(), GetFileExInfoStandard, &attrs)
                                  ? FileTimeValue(attrs.ftLastWriteTime) : NowFileTime();
    input.timestamp = static_cast<double>((written - kUnixEpochTicks) / kTicksPerSecond);

    const std::string event = BuildEventJson(input);
    const std::string name  = NarrowUtf8(base) + ".dmp";

    // Before anything leaves the PC.
    CreateDirectoryW(RecordsDir().c_str(), nullptr);
    const std::wstring record_path = RecordsDir() + L"\\" + WideFromUtf8(input.event_id) + L".txt";
    std::string        record = sent_records::kCrashTitle + input.event_id + "\n\nMinidump: " + name + " (" +
                         std::to_string(dump.size()) + " bytes, a memory snapshot; not copied here)\n\nEvent:\n" + event +
                         "\n\nLog lines:\n" + log + "\n";
    if (!WriteFileBytes(record_path, record)) return Outcome::Retry;  // nothing is sent unrecorded
    PruneRecords();

    CountUpload();
    const PostResult r = PostEnvelope(dsn, BuildEnvelope(input.event_id, event, dump, name, log));
    const std::string result = r.reached ? "HTTP " + std::to_string(r.status) : r.error;
    WriteFileBytes(record_path, record + sent_records::kResultPrefix + result + "\n");
    Log::Info("crash upload  %s: %s", name.c_str(), result.c_str());
    if (r.reached && r.status >= 200 && r.status < 300) {
        MarkDone(base, input.event_id);
        return Outcome::Sent;
    }
    if (r.reached && r.status == 429) return Outcome::StopPass;  // rate limited: the next pass, hours away
    if (r.reached && r.status >= 400 && r.status < 500) {
        MarkDone(base, "rejected: " + result);  // this payload will never be accepted
        return Outcome::Dropped;
    }
    return Outcome::StopPass;  // offline or a server fault: no point trying the others now
}

constexpr size_t kErrorLogLines = 100;
constexpr size_t kErrorLogBytes = 32 * 1024;

std::mutex g_error_mutex;  // the counts file's read-modify-write, and g_worker
Worker*    g_worker = nullptr;

std::wstring ErrorCountsPath() { return crash::CrashesDir() + L"\\error_counts"; }

std::string TodaysLogTail() {
    SYSTEMTIME st{};
    GetLocalTime(&st);  // log files are named by the local date (log.cpp)
    wchar_t name[32];
    swprintf_s(name, L"\\logs\\ombra-%04u%02u%02u.log", st.wYear, st.wMonth, st.wDay);
    return ReadFileTail(Instance().data_dir + name, kErrorLogBytes, kErrorLogLines);
}

Outcome UploadError(const Dsn& dsn, const std::wstring& name, const ErrorCounts& counts, const ScrubContext& scrub,
                    const std::string& system) {
    const std::wstring path = crash::CrashesDir() + L"\\" + name;
    const auto         queued = ParseQueuedError(ReadFileBytes(path, 256 * 1024));
    if (!queued) {
        DeleteFileW(path.c_str());
        return Outcome::Dropped;
    }
    ErrorEventInput input;
    input.error        = *queued;
    input.error.detail = ScrubText(queued->detail, scrub);
    input.error.log    = ScrubText(queued->log, scrub);
    input.system       = ScrubText(system, scrub);
    if (UsageAllowed()) input.user_id = InstallId();
    input.occurrences_today =
        std::max(queued->occurrences, OccurrencesOn(counts, queued->day, ErrorFingerprint(queued->kind, queued->code, queued->vendor)));
    const std::string event = BuildErrorEventJson(input);

    CreateDirectoryW(RecordsDir().c_str(), nullptr);
    const std::wstring record_path = RecordsDir() + L"\\" + WideFromUtf8(queued->event_id) + L".txt";
    const std::string  record = sent_records::kErrorTitle + queued->event_id + "\n\nEvent:\n" + event + "\n\nLog lines:\n" +
                               input.error.log + "\n";
    if (!WriteFileBytes(record_path, record)) return Outcome::Retry;
    PruneRecords();

    const PostResult  r      = PostEnvelope(dsn, BuildErrorEnvelope(queued->event_id, event, input.error.log));
    const std::string result = r.reached ? "HTTP " + std::to_string(r.status) : r.error;
    WriteFileBytes(record_path, record + sent_records::kResultPrefix + result + "\n");
    Log::Info("error upload  %s/%s: %s", queued->kind.c_str(), queued->code.c_str(), result.c_str());
    if (r.reached && r.status == 429) return Outcome::StopPass;
    if (r.reached && r.status >= 200 && r.status < 500) {  // sent, or never acceptable
        DeleteFileW(path.c_str());
        return r.status < 300 ? Outcome::Sent : Outcome::Dropped;
    }
    return Outcome::StopPass;
}

// ---- Sessions; the state file's read-modify-write is under g_error_mutex ----

constexpr int    kMaxSessionAgeDays = 5;  // Sentry ignores older sessions
constexpr size_t kExitLogLines      = 100;

std::wstring SessionsPath() { return crash::CrashesDir() + L"\\sessions"; }
std::wstring RunningPath() { return crash::CrashesDir() + L"\\running"; }

SessionState LoadSessions() {
    if (GetFileAttributesW(SessionsPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        SessionState s;
        s.dumps_seen = NowFileTime();  // dumps from before sessions existed aren't new crashes
        return s;
    }
    return ParseSessionState(ReadFileBytes(SessionsPath(), 64 * 1024));
}
void StoreSessions(const SessionState& s) { WriteFileAtomic(crash::CrashesDir(), L"sessions", WriteSessionState(s)); }

// Sentry drops a session whose start is older than 5 days when it arrives; an hour short of that
// leaves time for the send.
bool SessionFresh(int64_t started) {
    return UnixOf(NowFileTime()) - started <= kMaxSessionAgeDays * 86400LL - 3600;
}

// Dumps written since the last count become crashes in the session of the day they were written.
void CountNewDumps(SessionState& s) {
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((crash::CrashesDir() + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t newest = s.dumps_seen;
    do {
        const ULONGLONG written = FileTimeValue(fd.ftLastWriteTime);
        if (written <= s.dumps_seen) continue;
        newest = std::max<uint64_t>(newest, written);
        if (DaySession* d = SessionFor(s, UtcDayOf(written), ReleaseName())) d->crashed = true;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    s.dumps_seen = newest;
}

void TickSessionsLocked() {
    const std::wstring dir = crash::CrashesDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    SessionState s = LoadSessions();
    CountNewDumps(s);
    std::vector<DaySession> closed;
    const std::string today = UtcDay();
    RollSessions(s, today, ReleaseName(), UnixOf(NowFileTime()), closed);
    // Queued without the install ID: the child adds it if sharing is at the usage stop then.
    if (UploadAllowed())
        for (const DaySession& d : closed)
            if (SessionFresh(d.started))
                WriteFileAtomic(dir, L"session-" + WideFromUtf8(d.day) + L"-" + std::to_wstring(d.started) + L".txt",
                                WriteDaySession(d));
    StoreSessions(s);
}

Outcome UploadSession(const Dsn& dsn, const std::wstring& name) {
    const std::wstring path = crash::CrashesDir() + L"\\" + name;
    const auto         day  = ParseDaySession(ReadFileBytes(path, 4096));
    if (!day) {
        DeleteFileW(path.c_str());
        return Outcome::Dropped;
    }
    const std::string json = BuildSessionsJson(*day, UsageAllowed() ? InstallId() : std::string());
    CreateDirectoryW(RecordsDir().c_str(), nullptr);
    const std::wstring record_path = RecordsDir() + L"\\session-" + WideFromUtf8(day->day) + L"-" +
                                     std::to_wstring(day->started) + L".txt";
    const std::string record = sent_records::kSessionTitle + day->day + " (UTC)\n\n" + json + "\n";
    if (!WriteFileBytes(record_path, record)) return Outcome::Retry;
    PruneRecords();
    const PostResult  r      = PostEnvelope(dsn, BuildSessionEnvelope(json));
    const std::string result = r.reached ? "HTTP " + std::to_string(r.status) : r.error;
    WriteFileBytes(record_path, record + sent_records::kResultPrefix + result + "\n");
    Log::Info("session upload  %s %s: %s", day->day.c_str(), SessionStatus(*day), result.c_str());
    if (r.reached && r.status == 429) return Outcome::StopPass;
    if (r.reached && r.status >= 200 && r.status < 500) {
        DeleteFileW(path.c_str());
        return r.status < 300 ? Outcome::Sent : Outcome::Dropped;
    }
    return Outcome::StopPass;
}

std::wstring UsageFileName(const std::string& day) { return L"usage-games-" + WideFromUtf8(day) + L".txt"; }

// usage-games-<day>.txt or usage-day-<day>.txt: one envelope of Logs.
Outcome UploadUsage(const Dsn& dsn, const std::wstring& name) {
    const std::wstring path  = crash::CrashesDir() + L"\\" + name;
    const std::string  text  = ReadFileBytes(path, 4 * 1024 * 1024);
    const bool         daily = name.rfind(L"usage-day-", 0) == 0;
    const LogContext   ctx{ReleaseName(), "production", InstallId()};
    std::istringstream in(text);
    std::string        payload, title;
    size_t             count = 0;
    if (daily) {
        std::vector<DayRun> lines;
        for (std::string line; std::getline(in, line);)
            if (auto r = ParseDayRun(line)) lines.push_back(std::move(*r));
        const DayRun total = SumDayRuns(lines);
        if (!total.run.empty()) {
            payload = BuildDailyLog(total, NarrowUtf8(name.substr(name.size() - 12, 8)), ctx);
            title   = sent_records::kDailyTitle;
            count   = 1;
        }
    } else {
        std::vector<GameSession> parts;
        for (std::string line; std::getline(in, line);)
            if (auto s = ParseGameSession(line)) parts.push_back(std::move(*s));
        const std::vector<GameSession> sessions = JoinGameSessions(std::move(parts));
        if (!sessions.empty()) {
            payload = BuildGameSessionLogs(sessions, ctx);
            title   = sent_records::kGamesTitle;
            count   = sessions.size();
        }
    }
    if (count == 0) {
        DeleteFileW(path.c_str());
        return Outcome::Dropped;
    }
    CreateDirectoryW(RecordsDir().c_str(), nullptr);
    const std::wstring record_path = RecordsDir() + L"\\" + name;
    const std::string  record      = title + "\n\n" + payload + "\n";
    if (!WriteFileBytes(record_path, record)) return Outcome::Retry;
    PruneRecords();
    const PostResult  r      = PostEnvelope(dsn, BuildLogEnvelope(payload, count));
    const std::string result = r.reached ? "HTTP " + std::to_string(r.status) : r.error;
    WriteFileBytes(record_path, record + sent_records::kResultPrefix + result + "\n");
    Log::Info("usage upload  %s: %s", daily ? "daily" : (std::to_string(count) + " game sessions").c_str(),
              result.c_str());
    if (r.reached && r.status == 429) return Outcome::StopPass;
    if (r.reached && r.status >= 200 && r.status < 500) {
        DeleteFileW(path.c_str());
        return r.status < 300 ? Outcome::Sent : Outcome::Dropped;
    }
    return Outcome::StopPass;
}

// The previous engine run's last log lines: the newest two log files (it may have ended before
// midnight), read before this run's own lines matter since they carry another pid.
std::string PreviousRunLog(DWORD pid) {
    const std::wstring dir = Instance().data_dir + L"\\logs";
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((dir + L"\\ombra-*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do names.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.rbegin(), names.rend());  // ombra-YYYYMMDD.log: newest first
    const std::string tag = "[E " + std::to_string(pid) + " ";
    for (size_t i = 0; i < names.size() && i < 2; ++i) {
        std::string tail = LogTailOf(ReadFileTail(dir + L"\\" + names[i], 4u * 1024 * 1024, 100000), tag, kExitLogLines);
        if (tail.empty()) continue;
        if (tail.size() > kErrorLogBytes) tail.erase(0, tail.size() - kErrorLogBytes);
        return tail;
    }
    return {};
}

bool StartChild(HANDLE stop) {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring        cmd = L"\"" + std::wstring(exe) + L"\" crash-upload";
    STARTUPINFOW        si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                        nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    const HANDLE waits[2] = {pi.hProcess, stop};
    if (WaitForMultipleObjects(2, waits, FALSE, kChildTimeoutMs) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 2);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

std::optional<Dsn> ParseDsn(const std::string& text) {
    Dsn         d;
    std::string rest;
    if (text.rfind("https://", 0) == 0) rest = text.substr(8);
    else if (text.rfind("http://", 0) == 0) { rest = text.substr(7); d.secure = false; d.port = 80; }
    else return std::nullopt;
    const size_t at = rest.find('@');
    const size_t slash = rest.find('/', at == std::string::npos ? 0 : at);
    if (at == std::string::npos || at == 0 || slash == std::string::npos || slash + 1 >= rest.size()) return std::nullopt;
    d.key     = rest.substr(0, at);
    d.host    = rest.substr(at + 1, slash - at - 1);
    d.project = rest.substr(slash + 1);
    if (d.project.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    const size_t colon = d.host.find(':');
    if (colon != std::string::npos) {
        const int port = std::atoi(d.host.c_str() + colon + 1);
        if (port <= 0 || port > 65535) return std::nullopt;
        d.port = static_cast<unsigned short>(port);
        d.host.resize(colon);
    }
    if (d.host.empty() || (!d.secure && d.host != "127.0.0.1")) return std::nullopt;
    return d;
}

std::optional<CrashText> ParseCrashText(const std::string& text) {
    std::istringstream in(text);
    std::string        line;
    if (!std::getline(in, line)) return std::nullopt;
    CrashText c;
    // "<exe> <version> (<commit>)"
    const size_t sp = line.find(' '), open = line.find(" (", sp == std::string::npos ? 0 : sp);
    if (sp == std::string::npos || open == std::string::npos || line.empty() || line.back() != ')') return std::nullopt;
    c.exe     = line.substr(0, sp);
    c.version = line.substr(sp + 1, open - sp - 1);
    c.commit  = line.substr(open + 2, line.size() - open - 3);
    bool have_reason = false;
    while (std::getline(in, line)) {
        if (have_reason) { c.reason += "\n" + line; continue; }
        if (line.rfind("kind: ", 0) == 0) c.kind = line.substr(6);
        else if (line.rfind("thread: ", 0) == 0) c.thread = line.substr(8);
        else if (line.rfind("share: ", 0) == 0) {
            c.share = line.substr(7) == "yes";
            c.ask   = line.substr(7) == "ask";
        }
        else if (line.rfind("reason: ", 0) == 0) { c.reason = line.substr(8); have_reason = true; }
    }
    if (c.kind.empty()) return std::nullopt;
    return c;
}

std::string LogBeforeCrash(const std::string& log, const CrashText& crash, size_t max_lines) {
    const size_t hit = log.rfind("FATAL " + crash.kind + ": " + crash.reason.substr(0, crash.reason.find('\n')));
    if (hit == std::string::npos) return {};
    size_t end = log.find('\n', hit);
    end        = end == std::string::npos ? log.size() : end + 1;
    size_t start = end > 0 ? end - 1 : 0;  // on the crash line's newline
    for (size_t lines = 0; start > 0 && lines < max_lines; --start)
        if (log[start - 1] == '\n') {
            ++lines;
            if (lines == max_lines) break;
        }
    return log.substr(start, end - start);
}

std::string SettingsBeforeCrash(const std::string& log, const CrashText& crash) {
    const size_t hit = log.rfind("FATAL " + crash.kind + ": " + crash.reason.substr(0, crash.reason.find('\n')));
    if (hit == std::string::npos) return {};
    constexpr std::string_view kTag = "] settings: ";
    const size_t at = log.rfind(kTag, hit);
    if (at == std::string::npos) return {};
    const size_t start = at + kTag.size();
    const size_t end   = log.find_first_of("\r\n", start);
    return log.substr(start, end == std::string::npos ? end : end - start);
}

std::string DumpDate(const std::wstring& base) {
    const size_t pid_dash = base.rfind(L'-');
    if (pid_dash == std::wstring::npos || pid_dash == 0) return {};
    const size_t time_dash = base.rfind(L'-', pid_dash - 1);
    if (time_dash == std::wstring::npos || time_dash == 0) return {};
    const size_t date_dash = base.rfind(L'-', time_dash - 1);
    if (date_dash == std::wstring::npos || time_dash - date_dash != 9) return {};
    std::string date;
    for (size_t i = date_dash + 1; i < time_dash; ++i) {
        if (base[i] < L'0' || base[i] > L'9') return {};
        date.push_back(static_cast<char>(base[i]));
    }
    return date;
}

std::string BuildEventJson(const EventInput& in) {
    // Deliberately no `server_name` (SDKs default that to the computer's name); `user` only at the usage stop.
    JsonValue e = JsonValue::MakeObject();
    e.Set("event_id", JsonValue::MakeStr(in.event_id));
    e.Set("timestamp", JsonValue::MakeNum(in.timestamp));
    e.Set("platform", JsonValue::MakeStr("native"));
    e.Set("level", JsonValue::MakeStr("fatal"));
    e.Set("release", JsonValue::MakeStr("ombra@" + in.crash.version + "+" + in.crash.commit));
    SetUser(e, in.user_id);
    JsonValue tags = JsonValue::MakeObject();
    tags.Set("process", JsonValue::MakeStr(in.crash.exe));
    tags.Set("crash_kind", JsonValue::MakeStr(in.crash.kind));
    e.Set("tags", std::move(tags));
    JsonValue extra = JsonValue::MakeObject();
    extra.Set("reason", JsonValue::MakeStr(in.crash.reason));
    extra.Set("thread", JsonValue::MakeStr(in.crash.thread));
    extra.Set("system", JsonValue::MakeStr(in.system));
    extra.Set("settings", JsonValue::MakeStr(in.settings));
    e.Set("extra", std::move(extra));
    return WriteJsonCompact(e);
}

std::string BuildEnvelope(const std::string& event_id, const std::string& event_json, const std::string& dump,
                          const std::string& dump_name, const std::string& log) {
    std::string out = "{\"event_id\":\"" + event_id + "\"}\n";
    out += "{\"type\":\"event\",\"length\":" + std::to_string(event_json.size()) + "}\n" + event_json + "\n";
    out += "{\"type\":\"attachment\",\"length\":" + std::to_string(dump.size()) +
           ",\"attachment_type\":\"event.minidump\",\"filename\":\"" + dump_name + "\"}\n";
    out += dump;
    out += "\n";
    if (!log.empty())
        out += "{\"type\":\"attachment\",\"length\":" + std::to_string(log.size()) +
               ",\"content_type\":\"text/plain\",\"filename\":\"log.txt\"}\n" + log + "\n";
    return out;
}

const char* ErrorKindName(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::Save:    return "save";
        case ErrorKind::Encoder: return "encoder";
        case ErrorKind::Capture: return "capture";
        case ErrorKind::Audio:   return "audio";
        case ErrorKind::Update:  return "update";
        case ErrorKind::Exit:    return "exit";
    }
    return "unknown";
}

std::string ErrorFingerprint(const std::string& kind, const std::string& code, const std::string& vendor) {
    std::string fp = kind + "|" + code + "|" + vendor;
    for (char& c : fp)
        if (c == '\r' || c == '\n') c = ' ';
    return fp;
}

ErrorCounts ParseErrorCounts(const std::string& text) {
    ErrorCounts        counts;
    std::istringstream in(text);
    std::string        line;
    if (!std::getline(in, counts.day)) return {};
    if (!counts.day.empty() && counts.day.back() == '\r') counts.day.pop_back();
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        ErrorCounts::Entry e;
        if (!(fields >> e.occurrences >> e.queued)) continue;
        fields.get();  // the space before the fingerprint
        std::getline(fields, e.fingerprint);
        if (!e.fingerprint.empty()) counts.entries.push_back(std::move(e));
    }
    return counts;
}

std::string WriteErrorCounts(const ErrorCounts& counts) {
    std::string out = counts.day + "\n";
    for (const auto& e : counts.entries)
        out += std::to_string(e.occurrences) + " " + std::to_string(e.queued) + " " + e.fingerprint + "\n";
    return out;
}

bool CountError(ErrorCounts& counts, const std::string& today, const std::string& fingerprint) {
    if (counts.day != today) counts = ErrorCounts{today, {}};
    int queued_today = 0;
    ErrorCounts::Entry* entry = nullptr;
    for (auto& e : counts.entries) {
        queued_today += e.queued;
        if (e.fingerprint == fingerprint) entry = &e;
    }
    if (!entry) entry = &counts.entries.emplace_back(ErrorCounts::Entry{fingerprint, 0, 0});
    ++entry->occurrences;
    if (entry->queued >= kMaxErrorsPerFingerprint || queued_today >= kMaxErrorsPerDay) return false;
    ++entry->queued;
    return true;
}

int OccurrencesOn(const ErrorCounts& counts, const std::string& day, const std::string& fingerprint) {
    if (counts.day != day) return 0;
    for (const auto& e : counts.entries)
        if (e.fingerprint == fingerprint) return e.occurrences;
    return 0;
}

std::string WriteQueuedError(const QueuedError& e) {
    JsonValue j = JsonValue::MakeObject();
    j.Set("event_id", JsonValue::MakeStr(e.event_id));
    j.Set("timestamp", JsonValue::MakeNum(e.timestamp));
    j.Set("occurrences", JsonValue::MakeNum(e.occurrences));
    j.Set("day", JsonValue::MakeStr(e.day));
    j.Set("kind", JsonValue::MakeStr(e.kind));
    j.Set("code", JsonValue::MakeStr(e.code));
    j.Set("vendor", JsonValue::MakeStr(e.vendor));
    j.Set("detail", JsonValue::MakeStr(e.detail));
    j.Set("version", JsonValue::MakeStr(e.version));
    j.Set("commit", JsonValue::MakeStr(e.commit));
    j.Set("log", JsonValue::MakeStr(e.log));
    return WriteJsonCompact(j);
}

std::optional<QueuedError> ParseQueuedError(const std::string& text) {
    JsonValue j;
    try {
        j = ParseJson(text);
    } catch (const std::string&) {
        return std::nullopt;
    }
    auto str = [&](const char* key) {
        const JsonValue* v = j.Find(key);
        return v && v->type == JsonValue::Type::String ? v->str : std::string();
    };
    QueuedError e;
    e.event_id = str("event_id");
    e.day      = str("day");
    e.kind     = str("kind");
    e.code     = str("code");
    e.vendor   = str("vendor");
    e.detail   = str("detail");
    e.version  = str("version");
    e.commit   = str("commit");
    e.log      = str("log");
    if (const JsonValue* t = j.Find("timestamp"); t && t->type == JsonValue::Type::Number) e.timestamp = t->num;
    if (const JsonValue* n = j.Find("occurrences"); n && n->type == JsonValue::Type::Number)
        e.occurrences = static_cast<int>(n->num);
    if (e.event_id.size() != 32 || e.kind.empty() || e.code.empty()) return std::nullopt;
    return e;
}

std::string BuildErrorEventJson(const ErrorEventInput& in) {
    const QueuedError& q = in.error;
    // As for crashes: no `server_name`, `user` only at the usage stop.
    JsonValue e = JsonValue::MakeObject();
    e.Set("event_id", JsonValue::MakeStr(q.event_id));
    e.Set("timestamp", JsonValue::MakeNum(q.timestamp));
    e.Set("platform", JsonValue::MakeStr("native"));
    e.Set("level", JsonValue::MakeStr("error"));
    e.Set("release", JsonValue::MakeStr("ombra@" + q.version + "+" + q.commit));
    SetUser(e, in.user_id);
    e.Set("message", JsonValue::MakeStr(q.kind + ": " + (q.detail.empty() ? q.code : q.detail + " (" + q.code + ")")));
    // Without it Sentry groups stackless events by message text.
    JsonValue fp = JsonValue::MakeArray();
    for (const std::string* part : {&q.kind, &q.code, &q.vendor}) fp.arr.push_back(JsonValue::MakeStr(*part));
    e.Set("fingerprint", std::move(fp));
    JsonValue tags = JsonValue::MakeObject();
    tags.Set("process", JsonValue::MakeStr("ombra"));
    tags.Set("error_kind", JsonValue::MakeStr(q.kind));
    tags.Set("error_code", JsonValue::MakeStr(q.code));
    if (!q.vendor.empty()) tags.Set("vendor", JsonValue::MakeStr(q.vendor));
    e.Set("tags", std::move(tags));
    JsonValue extra = JsonValue::MakeObject();
    extra.Set("occurrences_today", JsonValue::MakeNum(in.occurrences_today));
    extra.Set("system", JsonValue::MakeStr(in.system));
    e.Set("extra", std::move(extra));
    return WriteJsonCompact(e);
}

std::string BuildErrorEnvelope(const std::string& event_id, const std::string& event_json, const std::string& log) {
    std::string out = "{\"event_id\":\"" + event_id + "\"}\n";
    out += "{\"type\":\"event\",\"length\":" + std::to_string(event_json.size()) + "}\n" + event_json + "\n";
    if (!log.empty())
        out += "{\"type\":\"attachment\",\"length\":" + std::to_string(log.size()) +
               ",\"content_type\":\"text/plain\",\"filename\":\"log.txt\"}\n" + log + "\n";
    return out;
}

void ReportError(ErrorKind kind, const std::string& code, const std::string& vendor, const std::string& detail,
                 const std::optional<std::string>& log) {
    if (!UploadAllowed()) return;  // never queued while Off, so never sent later
    QueuedError e;
    e.kind = ErrorKindName(kind);
    e.code = code;
    e.vendor = vendor;
    e.detail = detail;
    const std::string fingerprint = ErrorFingerprint(e.kind, code, vendor);
    std::lock_guard<std::mutex> lock(g_error_mutex);
    const std::wstring dir = crash::CrashesDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    if (kind != ErrorKind::Exit) {  // an unclean exit is the previous run's session, marked abnormal
        TickSessionsLocked();       // today's session exists, an earlier day is closed first
        SessionState s = LoadSessions();
        if (DaySession* d = SessionFor(s, UtcDay(), ReleaseName())) d->errored = true;
        StoreSessions(s);
    }
    ErrorCounts counts = ParseErrorCounts(ReadFileBytes(ErrorCountsPath(), 64 * 1024));
    e.day = UtcDay();
    const bool queue = CountError(counts, e.day, fingerprint);
    WriteFileAtomic(dir, L"error_counts", WriteErrorCounts(counts));
    if (!queue) return;
    e.occurrences = OccurrencesOn(counts, e.day, fingerprint);
    e.event_id  = NewEventId();
    e.timestamp = static_cast<double>((NowFileTime() - kUnixEpochTicks) / kTicksPerSecond);
    e.version   = AppVersion();
    e.commit    = BuildCommit();
    e.log       = log ? *log : TodaysLogTail();
    if (!WriteFileAtomic(dir, L"error-" + WideFromUtf8(e.event_id) + L".json", WriteQueuedError(e))) return;
    Log::Info("error report queued  %s", fingerprint.c_str());
    if (g_worker) g_worker->ErrorQueued();
}

bool UploadAllowed() {
    // Not LoadConfig: it can write config.json, and the engine owns that file.
    return ActiveDsn() && SetupCompleted() &&
           GetFileAttributesW((crash::CrashesDir() + L"\\sharing_on").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool UsageAllowed() {
    return UploadAllowed() && ReadFileBytes(crash::CrashesDir() + L"\\sharing_on", 16).rfind("usage", 0) == 0;
}

std::string InstallId() {
    const std::wstring path = Instance().data_dir + L"\\install_id";
    std::string        id   = ReadFileBytes(path, 64);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
    if (id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos) return id;
    id = NewEventId();  // 128 random bits
    CreateDirectoryW(Instance().data_dir.c_str(), nullptr);
    return WriteFileAtomic(Instance().data_dir, L"install_id", id + "\n") ? id : std::string();
}

const char* SessionStatus(const DaySession& s) {
    return s.crashed ? "crashed" : s.abnormal ? "abnormal" : s.errored ? "errored" : "exited";
}

std::string WriteDaySession(const DaySession& d) {
    return d.day + " " + std::to_string(d.started) + " " + (d.errored ? "1" : "0") + (d.abnormal ? "1" : "0") +
           (d.crashed ? "1" : "0") + " " + d.release;
}

std::optional<DaySession> ParseDaySession(const std::string& line) {
    std::istringstream in(line);
    DaySession         d;
    std::string        flags;
    if (!(in >> d.day >> d.started >> flags >> d.release) || d.day.size() != 8 || flags.size() != 3 ||
        flags.find_first_not_of("01") != std::string::npos)
        return std::nullopt;
    d.errored  = flags[0] == '1';
    d.abnormal = flags[1] == '1';
    d.crashed  = flags[2] == '1';
    return d;
}

SessionState ParseSessionState(const std::string& text) {
    SessionState       s;
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("seen ", 0) == 0) s.dumps_seen = std::strtoull(line.c_str() + 5, nullptr, 10);
        else if (auto d = ParseDaySession(line)) s.days.push_back(std::move(*d));
    }
    return s;
}

std::string WriteSessionState(const SessionState& s) {
    std::string out = "seen " + std::to_string(s.dumps_seen) + "\n";
    for (const DaySession& d : s.days) out += WriteDaySession(d) + "\n";
    return out;
}

void RollSessions(SessionState& s, const std::string& today, const std::string& release, int64_t now,
                  std::vector<DaySession>& closed) {
    std::vector<DaySession> open;
    for (DaySession& d : s.days) (d.day < today ? closed : open).push_back(std::move(d));
    s.days = std::move(open);
    for (const DaySession& d : s.days)
        if (d.release == release) return;
    s.days.push_back(DaySession{today, release, now});
}

DaySession* SessionFor(SessionState& s, const std::string& day, const std::string& release) {
    DaySession* same_day = nullptr;
    DaySession* newest   = nullptr;
    for (DaySession& d : s.days) {
        if (!newest || d.started >= newest->started) newest = &d;
        if (d.day != day) continue;
        if (d.release == release) return &d;  // one per day and release
        if (!same_day || d.started >= same_day->started) same_day = &d;
    }
    return same_day ? same_day : newest;
}

std::string BuildSessionsJson(const DaySession& d, const std::string& did) {
    const ULONGLONG ft = static_cast<ULONGLONG>(d.started / 60 * 60) * kTicksPerSecond + kUnixEpochTicks;
    FILETIME        f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)};
    SYSTEMTIME      st{};
    FileTimeToSystemTime(&f, &st);
    char started[32];
    std::snprintf(started, sizeof(started), "%04u-%02u-%02uT%02u:%02u:00Z", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute);
    JsonValue group = JsonValue::MakeObject();
    group.Set("started", JsonValue::MakeStr(started));
    if (!did.empty()) group.Set("did", JsonValue::MakeStr(did));
    group.Set(SessionStatus(d), JsonValue::MakeNum(1));
    JsonValue aggregates = JsonValue::MakeArray();
    aggregates.arr.push_back(std::move(group));
    JsonValue attrs = JsonValue::MakeObject();
    attrs.Set("release", JsonValue::MakeStr(d.release));
    attrs.Set("environment", JsonValue::MakeStr("production"));
    JsonValue j = JsonValue::MakeObject();
    j.Set("aggregates", std::move(aggregates));
    j.Set("attrs", std::move(attrs));
    return WriteJsonCompact(j);
}

std::string BuildSessionEnvelope(const std::string& json) {
    return "{}\n{\"type\":\"sessions\",\"length\":" + std::to_string(json.size()) + "}\n" + json + "\n";
}

std::string GameSessionGroup(const std::wstring& game_key) {
    if (game_key.empty()) return {};
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : NarrowUtf8(game_key)) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string WriteGameSession(const GameSession& s) {
    // The exe last: a file name can hold spaces.
    return std::to_string(s.start) + " " + std::to_string(s.end) + " " + std::to_string(s.saves) + " " +
           std::to_string(s.fps) + " " + std::to_string(s.width) + " " + std::to_string(s.height) + " " +
           (s.encoder.empty() ? "-" : s.encoder) + " " + (s.codec.empty() ? "-" : s.codec) + " " +
           (s.group.empty() ? "-" : s.group) + " " + s.exe;
}

std::optional<GameSession> ParseGameSession(const std::string& line) {
    std::istringstream in(line);
    GameSession        s;
    if (!(in >> s.start >> s.end >> s.saves >> s.fps >> s.width >> s.height >> s.encoder >> s.codec >> s.group))
        return std::nullopt;
    std::getline(in, s.exe);
    while (!s.exe.empty() && (s.exe.back() == '\r' || s.exe.back() == '\n')) s.exe.pop_back();
    if (!s.exe.empty() && s.exe.front() == ' ') s.exe.erase(0, 1);
    if (s.exe.empty() || s.end < s.start || s.saves < 0) return std::nullopt;
    if (s.encoder == "-") s.encoder.clear();
    if (s.codec == "-") s.codec.clear();
    if (s.group == "-") s.group.clear();
    return s;
}

std::vector<GameSession> JoinGameSessions(std::vector<GameSession> parts) {
    std::sort(parts.begin(), parts.end(), [](const GameSession& a, const GameSession& b) { return a.start < b.start; });
    struct Joined {
        GameSession                    s;
        std::map<std::string, int64_t> exe_seconds;
        int64_t                        built_seconds = -1;  // the longest part with an encoder
    };
    const auto same_game = [](const GameSession& a, const GameSession& b) {
        return a.group.empty() && b.group.empty() ? a.exe == b.exe : a.group == b.group;
    };
    std::vector<Joined> joined;
    for (GameSession& p : parts) {
        const int64_t seconds = p.end - p.start;
        Joined*       same    = nullptr;  // that game's latest session, if this continues it
        for (Joined& j : joined)
            if (same_game(j.s, p) && p.start - j.s.end <= kGameSessionJoinGap && (!same || j.s.end > same->s.end))
                same = &j;
        if (!same) {
            Joined j;
            j.exe_seconds[p.exe] = seconds;
            if (!p.encoder.empty()) j.built_seconds = seconds;
            j.s = std::move(p);
            joined.push_back(std::move(j));
            continue;
        }
        same->s.end = std::max(same->s.end, p.end);
        same->s.saves += p.saves;
        same->exe_seconds[p.exe] += seconds;
        if (!p.encoder.empty() && seconds > same->built_seconds) {
            same->built_seconds = seconds;
            same->s.encoder     = p.encoder;
            same->s.codec       = p.codec;
            same->s.width       = p.width;
            same->s.height      = p.height;
            same->s.fps         = p.fps;
        }
    }
    std::vector<GameSession> sessions;
    sessions.reserve(joined.size());
    for (Joined& j : joined) {
        int64_t most = -1;
        for (const auto& [exe, seconds] : j.exe_seconds)
            if (seconds > most) {
                most    = seconds;
                j.s.exe = exe;
            }
        sessions.push_back(std::move(j.s));
    }
    std::erase_if(sessions, [](const GameSession& s) { return s.end - s.start < kMinGameSeconds; });
    if (sessions.size() > static_cast<size_t>(kMaxGameSessionsPerDay)) {
        std::stable_sort(sessions.begin(), sessions.end(),
                         [](const GameSession& a, const GameSession& b) { return a.end - a.start > b.end - b.start; });
        sessions.resize(kMaxGameSessionsPerDay);
        std::sort(sessions.begin(), sessions.end(),
                  [](const GameSession& a, const GameSession& b) { return a.start < b.start; });
    }
    return sessions;
}

namespace {
JsonValue LogAttr(const std::string& v) {
    JsonValue a = JsonValue::MakeObject();
    a.Set("value", JsonValue::MakeStr(v));
    a.Set("type", JsonValue::MakeStr("string"));
    return a;
}
JsonValue LogAttr(int64_t v) {
    JsonValue a = JsonValue::MakeObject();
    a.Set("value", JsonValue::MakeNum(static_cast<double>(v)));
    a.Set("type", JsonValue::MakeStr("integer"));
    return a;
}
JsonValue LogAttr(double v) {
    JsonValue a = JsonValue::MakeObject();
    a.Set("value", JsonValue::MakeNum(v));
    a.Set("type", JsonValue::MakeStr("double"));
    return a;
}
JsonValue LogAttr(bool v) {
    JsonValue a = JsonValue::MakeObject();
    a.Set("value", JsonValue::MakeBool(v));
    a.Set("type", JsonValue::MakeStr("boolean"));
    return a;
}
// A `log` item's payload around `items`: never infer the sender's IP or user agent.
std::string LogPayload(JsonValue items) {
    JsonValue settings = JsonValue::MakeObject();
    settings.Set("infer_ip", JsonValue::MakeStr("never"));
    settings.Set("infer_user_agent", JsonValue::MakeStr("never"));
    JsonValue j = JsonValue::MakeObject();
    j.Set("version", JsonValue::MakeNum(2));
    j.Set("ingest_settings", std::move(settings));
    j.Set("items", std::move(items));
    return WriteJsonCompact(j);
}
}  // namespace

std::string BuildGameSessionLogs(const std::vector<GameSession>& sessions, const LogContext& ctx) {
    JsonValue items = JsonValue::MakeArray();
    for (const GameSession& s : sessions) {
        JsonValue attrs = JsonValue::MakeObject();
        attrs.Set("sentry.release", LogAttr(ctx.release));
        attrs.Set("sentry.environment", LogAttr(ctx.environment));
        if (!ctx.install_id.empty()) attrs.Set("user.id", LogAttr(ctx.install_id));
        attrs.Set("ombra.kind", LogAttr(std::string("game_session")));
        attrs.Set("ombra.game_exe", LogAttr(s.exe));
        attrs.Set("ombra.seconds", LogAttr(s.end - s.start));
        attrs.Set("ombra.saves", LogAttr(static_cast<int64_t>(s.saves)));
        if (!s.encoder.empty()) {
            attrs.Set("ombra.encoder", LogAttr(s.encoder));
            attrs.Set("ombra.codec", LogAttr(s.codec));
            attrs.Set("ombra.width", LogAttr(static_cast<int64_t>(s.width)));
            attrs.Set("ombra.height", LogAttr(static_cast<int64_t>(s.height)));
            attrs.Set("ombra.fps", LogAttr(static_cast<int64_t>(s.fps)));
        }
        JsonValue item = JsonValue::MakeObject();
        item.Set("timestamp", JsonValue::MakeNum(static_cast<double>(s.start)));
        item.Set("trace_id", JsonValue::MakeStr(NewEventId()));
        item.Set("level", JsonValue::MakeStr("info"));
        item.Set("body", JsonValue::MakeStr("game session"));
        item.Set("attributes", std::move(attrs));
        items.arr.push_back(std::move(item));
    }
    return LogPayload(std::move(items));
}

std::string BuildLogEnvelope(const std::string& payload, size_t count) {
    return "{}\n{\"type\":\"log\",\"item_count\":" + std::to_string(count) +
           ",\"content_type\":\"application/vnd.sentry.items.log+json\",\"length\":" + std::to_string(payload.size()) +
           "}\n" + payload + "\n";
}

void RecordGameSession(const GameSession& s) {
    if (!UsageAllowed()) return;
    const std::wstring dir = crash::CrashesDir();
    std::lock_guard<std::mutex> lock(g_error_mutex);
    CreateDirectoryW(dir.c_str(), nullptr);
    const ULONGLONG    end_ft = static_cast<ULONGLONG>(s.end) * kTicksPerSecond + kUnixEpochTicks;
    const std::wstring path   = dir + L"\\" + UsageFileName(UtcDayOf(end_ft));
    const HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string line  = WriteGameSession(s) + "\n";
    DWORD             wrote = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
    CloseHandle(f);
    Log::Info("game session queued  %lld s", static_cast<long long>(s.end - s.start));
}

JsonValue UsageSettings(const JsonValue& flat) {
    static const JsonValue defaults = SerializeConfigFlat(Config{});
    JsonValue              out      = JsonValue::MakeObject();
    for (const char* key : kUsageSettingsSent) {
        const JsonValue* v = flat.Find(key);
        if (!v) continue;
        JsonValue value = *v;
        if (value.type == JsonValue::Type::String) {
            const std::string_view k = key;
            std::string&           s = value.str;
            if (k == "ui.accent" && s.rfind('#', 0) == 0) s = "custom";
            else if (k == "ui.theme" && s.rfind("user", 0) == 0) s = "user";
            else if (k == "encode.preset" && !(s.size() == 2 && s[0] == 'p' && s[1] >= '1' && s[1] <= '7')) s = "other";
        }
        out.Set(key, std::move(value));
    }
    for (const char* key : kUsageSettingsChangedOnly) {
        const JsonValue* v = flat.Find(key);
        const JsonValue* d = defaults.Find(key);
        if (v && d) out.Set(std::string(key) + ".changed", JsonValue::MakeBool(v->str != d->str));
    }
    return out;
}

namespace {
uint64_t Minus(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

// A histogram as "bucket:count,..." for its nonzero buckets.
std::string WriteBuckets(const LatencyHistogram::Snapshot& h) {
    std::string out;
    for (size_t i = 0; i < h.counts.size(); ++i) {
        if (!h.counts[i]) continue;
        if (!out.empty()) out += ',';
        out += std::to_string(i) + ':' + std::to_string(h.counts[i]);
    }
    return out;
}
void ParseBuckets(const std::string& s, LatencyHistogram::Snapshot& h) {
    size_t at = 0;
    while (at < s.size()) {
        size_t       end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const size_t colon = s.find(':', at);
        if (colon != std::string::npos && colon < end) {
            const unsigned long long i = std::strtoull(s.c_str() + at, nullptr, 10);
            if (i < h.counts.size()) h.counts[i] = std::strtoull(s.c_str() + colon + 1, nullptr, 10);
        }
        at = end + 1;
    }
}

// The DayRun fields after `run` and `settings`, in one table for writing and parsing.
struct DayRunField {
    const char* name;
    uint64_t DayRun::*u64;
    bool        peak = false;  // runs combine by max, not sum
};
constexpr DayRunField kDayRunFields[] = {
    {"ticks", &DayRun::ticks},
    {"duplicates", &DayRun::duplicates},
    {"late", &DayRun::late},
    {"resyncs", &DayRun::resyncs},
    {"rebuilds", &DayRun::rebuilds},
    {"resolution_changes", &DayRun::resolution_changes},
    {"saves", &DayRun::saves},
    {"audio_dropped_frames", &DayRun::audio_dropped_frames},
    {"audio_late_frames", &DayRun::audio_late_frames},
    {"audio_encoder_errors", &DayRun::audio_encoder_errors},
    {"peak_private_mb", &DayRun::peak_private_mb, true},
    {"peak_ring_mb", &DayRun::peak_ring_mb, true},
};
}  // namespace

std::string WriteDayRun(const DayRun& r) {
    JsonValue j = JsonValue::MakeObject();
    j.Set("run", JsonValue::MakeStr(r.run));
    j.Set("up_seconds", JsonValue::MakeNum(static_cast<double>(r.up_seconds)));
    j.Set("armed_seconds", JsonValue::MakeNum(static_cast<double>(r.armed_seconds)));
    for (const DayRunField& f : kDayRunFields) j.Set(f.name, JsonValue::MakeNum(static_cast<double>(r.*f.u64)));
    j.Set("monitors", JsonValue::MakeNum(r.monitors));
    j.Set("audio_sources", JsonValue::MakeNum(r.audio_sources));
    j.Set("encode_ms", JsonValue::MakeStr(WriteBuckets(r.encode)));
    j.Set("settings", r.settings.type == JsonValue::Type::Object ? r.settings : JsonValue::MakeObject());
    return WriteJsonCompact(j);
}

std::optional<DayRun> ParseDayRun(const std::string& line) {
    JsonValue j;
    try {
        j = ParseJson(line);
    } catch (...) {
        return std::nullopt;  // a line cut short by a crash
    }
    const JsonValue* run = j.Find("run");
    if (!run || run->type != JsonValue::Type::String || run->str.empty()) return std::nullopt;
    const auto num = [&](const char* k) -> uint64_t {
        const JsonValue* v = j.Find(k);
        return v && v->type == JsonValue::Type::Number && v->num > 0 ? static_cast<uint64_t>(v->num) : 0;
    };
    DayRun r;
    r.run           = run->str;
    r.up_seconds    = static_cast<int64_t>(num("up_seconds"));
    r.armed_seconds = static_cast<int64_t>(num("armed_seconds"));
    for (const DayRunField& f : kDayRunFields) r.*f.u64 = num(f.name);
    r.monitors      = static_cast<uint32_t>(num("monitors"));
    r.audio_sources = static_cast<uint32_t>(num("audio_sources"));
    if (const JsonValue* e = j.Find("encode_ms"); e && e->type == JsonValue::Type::String) ParseBuckets(e->str, r.encode);
    const JsonValue* s = j.Find("settings");
    r.settings         = s && s->type == JsonValue::Type::Object ? *s : JsonValue::MakeObject();
    return r;
}

void DayUsage::Begin(const UsageCounters& c, int64_t now) {
    base_            = c;
    last_            = now;
    up_seconds_      = 0;
    armed_seconds_   = 0;
    last_armed_      = c.armed;
    peak_private_mb_ = static_cast<uint64_t>(c.private_mb);
    peak_ring_mb_    = c.ring_committed >> 20;
}

void DayUsage::Sample(const UsageCounters& c, int64_t now) {
    // A sleeping PC or a clock change isn't time up or recording: a gap counts at most kMaxSampleGap.
    const int64_t gap = std::clamp<int64_t>(now - last_, 0, kMaxSampleGap);
    up_seconds_ += gap;
    if (last_armed_) armed_seconds_ += gap;
    last_            = now;
    last_armed_      = c.armed;
    peak_private_mb_ = std::max(peak_private_mb_, static_cast<uint64_t>(c.private_mb));
    peak_ring_mb_    = std::max<uint64_t>(peak_ring_mb_, c.ring_committed >> 20);
}

DayRun DayUsage::Totals(const UsageCounters& c, int64_t now) const {
    DayRun r;
    r.run                  = run_;
    r.up_seconds           = up_seconds_ + std::clamp<int64_t>(now - last_, 0, kMaxSampleGap);
    r.armed_seconds        = armed_seconds_;
    r.ticks                = Minus(c.ticks.ticks, base_.ticks.ticks);
    r.duplicates           = Minus(c.ticks.duplicates, base_.ticks.duplicates);
    r.late                 = Minus(c.ticks.late, base_.ticks.late);
    r.resyncs              = Minus(c.ticks.resyncs, base_.ticks.resyncs);
    r.rebuilds             = Minus(c.rebuilds, base_.rebuilds);
    r.resolution_changes   = Minus(c.resolution_changes, base_.resolution_changes);
    r.saves                = Minus(c.saves, base_.saves);
    r.audio_dropped_frames = Minus(c.audio_dropped_frames, base_.audio_dropped_frames);
    r.audio_late_frames    = Minus(c.audio_late_frames, base_.audio_late_frames);
    r.audio_encoder_errors = Minus(c.audio_encoder_errors, base_.audio_encoder_errors);
    r.peak_private_mb      = peak_private_mb_;
    r.peak_ring_mb         = peak_ring_mb_;
    for (size_t i = 0; i < r.encode.counts.size(); ++i)
        r.encode.counts[i] = Minus(c.ticks.encode.counts[i], base_.ticks.encode.counts[i]);
    return r;
}

DayRun SumDayRuns(const std::vector<DayRun>& lines) {
    std::map<std::string, size_t> last;  // run -> its last line
    for (size_t i = 0; i < lines.size(); ++i) last[lines[i].run] = i;
    DayRun total;
    size_t newest = 0;
    for (const auto& [run, i] : last) {
        const DayRun& r = lines[i];
        total.up_seconds += r.up_seconds;
        total.armed_seconds += r.armed_seconds;
        // Peaks are the highest run's, not a sum.
        for (const DayRunField& f : kDayRunFields)
            total.*f.u64 = f.peak ? std::max(total.*f.u64, r.*f.u64) : total.*f.u64 + r.*f.u64;
        total.encode.Add(r.encode);
        newest = std::max(newest, i);
    }
    if (last.empty()) return total;
    total.run           = std::to_string(last.size());  // how many runs
    total.monitors      = lines[newest].monitors;
    total.audio_sources = lines[newest].audio_sources;
    total.settings      = lines[newest].settings;
    return total;
}

std::string BuildDailyLog(const DayRun& d, const std::string& day, const LogContext& ctx) {
    JsonValue attrs = JsonValue::MakeObject();
    attrs.Set("sentry.release", LogAttr(ctx.release));
    attrs.Set("sentry.environment", LogAttr(ctx.environment));
    if (!ctx.install_id.empty()) attrs.Set("user.id", LogAttr(ctx.install_id));
    attrs.Set("ombra.kind", LogAttr(std::string("daily")));
    attrs.Set("ombra.runs", LogAttr(static_cast<int64_t>(std::strtoll(d.run.c_str(), nullptr, 10))));
    attrs.Set("ombra.up_seconds", LogAttr(d.up_seconds));
    attrs.Set("ombra.recording_seconds", LogAttr(d.armed_seconds));
    for (const DayRunField& f : kDayRunFields) {
        const std::string name = std::string("ombra.") + (std::string_view(f.name) == "ticks" ? "frames" : f.name);
        attrs.Set(name, LogAttr(static_cast<int64_t>(d.*f.u64)));
    }
    if (d.encode.Total()) {
        attrs.Set("ombra.encode_p50_ms", LogAttr(d.encode.Percentile(0.50)));
        attrs.Set("ombra.encode_p99_ms", LogAttr(d.encode.Percentile(0.99)));
        attrs.Set("ombra.encode_max_ms", LogAttr(d.encode.Max()));
    }
    attrs.Set("ombra.monitors", LogAttr(static_cast<int64_t>(d.monitors)));
    attrs.Set("ombra.audio_sources", LogAttr(static_cast<int64_t>(d.audio_sources)));
    for (const auto& [key, v] : d.settings.obj) {
        const std::string name = "setting." + key;
        if (v.type == JsonValue::Type::Bool) attrs.Set(name, LogAttr(v.b));
        else if (v.type == JsonValue::Type::Number) attrs.Set(name, LogAttr(v.num));
        else if (v.type == JsonValue::Type::String) attrs.Set(name, LogAttr(v.str));
    }
    // Stamped at the day's last second (UTC).
    std::tm tm{};
    tm.tm_year = std::atoi(day.substr(0, 4).c_str()) - 1900;
    tm.tm_mon  = std::atoi(day.substr(4, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(day.substr(6, 2).c_str());
    const int64_t end = static_cast<int64_t>(_mkgmtime(&tm)) + 86399;
    JsonValue     item = JsonValue::MakeObject();
    item.Set("timestamp", JsonValue::MakeNum(static_cast<double>(end)));
    item.Set("trace_id", JsonValue::MakeStr(NewEventId()));
    item.Set("level", JsonValue::MakeStr("info"));
    item.Set("body", JsonValue::MakeStr("daily usage"));
    item.Set("attributes", std::move(attrs));
    JsonValue items = JsonValue::MakeArray();
    items.arr.push_back(std::move(item));
    return LogPayload(std::move(items));
}

void RecordDayRun(const DayRun& r, const std::string& day) {
    if (!UsageAllowed() || day.size() != 8) return;
    const std::wstring dir = crash::CrashesDir();
    std::lock_guard<std::mutex> lock(g_error_mutex);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\usage-day-" + WideFromUtf8(day) + L".txt";
    const HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string line  = WriteDayRun(r) + "\n";
    DWORD             wrote = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
    CloseHandle(f);
}

std::vector<std::wstring> PendingUsage() {
    const std::wstring dir = crash::CrashesDir();
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((dir + L"\\usage-*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    const bool        allowed = UsageAllowed();
    const std::string today   = UtcDay();
    const ULONGLONG   oldest  = NowFileTime() - kMaxSessionAgeDays * 86400ull * kTicksPerSecond;
    const std::string stale   = UtcDayOf(oldest);  // that day's sessions can already be too old
    do {
        // usage-games-<day>.txt, usage-day-<day>.txt
        const std::wstring name = fd.cFileName;
        const std::string  day  = name.size() >= 18 ? NarrowUtf8(name.substr(name.size() - 12, 8)) : std::string();
        if (!allowed || day.size() != 8 || day <= stale) DeleteFileW((dir + L"\\" + name).c_str());
        else if (day < today) out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::string LogTailOf(const std::string& log, const std::string& tag, size_t max_lines) {
    const size_t hit = log.rfind(tag);
    if (hit == std::string::npos) return {};
    size_t end = log.find('\n', hit);
    end        = end == std::string::npos ? log.size() : end + 1;
    size_t start = end - 1;
    for (size_t lines = 0; start > 0; --start)
        if (log[start - 1] == '\n' && ++lines == max_lines) break;
    return log.substr(start, end - start);
}

void BeginRun() {
    const std::wstring dir = crash::CrashesDir();
    if (dir.empty()) return;
    DWORD       pid = 0;
    std::string exit_code;
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        CreateDirectoryW(Instance().data_dir.c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        // "<pid> <unix start> <release>"
        std::istringstream prev(ReadFileBytes(RunningPath(), 512));
        int64_t            started = 0;
        std::string        release;
        const bool         unclean = static_cast<bool>(prev >> pid >> started >> release);
        WriteFileAtomic(dir, L"running", std::to_string(GetCurrentProcessId()) + " " +
                                             std::to_string(UnixOf(NowFileTime())) + " " + ReleaseName() + "\n");
        // A dump of that run means it crashed, which CountNewDumps records; without one its fate is unknown.
        WIN32_FIND_DATAW fd{};
        const HANDLE     h = unclean ? FindFirstFileW((dir + L"\\ombra-*-" + std::to_wstring(pid) + L".dmp").c_str(), &fd)
                                     : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        if (unclean && h == INVALID_HANDLE_VALUE) {
            SessionState s = LoadSessions();
            DaySession*  d = nullptr;
            for (DaySession& day : s.days)  // that run's newest day
                if (day.release == release && (!d || day.started >= d->started)) d = &day;
            if (!d) d = SessionFor(s, "", release);
            if (d) d->abnormal = true;
            StoreSessions(s);
            // Restarted since that run began: a BSOD, power loss or hard reset; else it was killed.
            const int64_t boot = UnixOf(NowFileTime()) - static_cast<int64_t>(GetTickCount64() / 1000);
            exit_code = boot > started ? "unclean_reboot" : "unclean";
            Log::Warn("the previous run (pid %lu) ended without exiting or a crash dump (%s)", pid, exit_code.c_str());
        }
        TickSessionsLocked();
    }
    if (!exit_code.empty())
        ReportError(ErrorKind::Exit, exit_code, "", "Ombra stopped without exiting", PreviousRunLog(pid));
}

void EndRun() { DeleteFileW(RunningPath().c_str()); }

void TickSessions() {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    TickSessionsLocked();
}

namespace {

// Unsent dumps under kMaxDumpAgeDays: sharing was on (`share: yes`), or, with `answered`, the user
// answered Send to the ask about one from before setup. Newest first.
std::vector<std::wstring> UnsentDumps(bool answered) {
    const std::wstring dir = crash::CrashesDir();
    std::vector<std::pair<ULONGLONG, std::wstring>> found;
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    const ULONGLONG now = NowFileTime();
    do {
        const ULONGLONG written = FileTimeValue(fd.ftLastWriteTime);
        if (now > written && (now - written) / kTicksPerSecond > kMaxDumpAgeDays * 86400ull) continue;
        const std::wstring name = fd.cFileName;
        const std::wstring base = name.substr(0, name.size() - 4);
        if (GetFileAttributesW((dir + L"\\" + base + L".sent").c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        const auto crash = ParseCrashText(ReadFileBytes(dir + L"\\" + base + L".txt", 8192));
        if (!crash) continue;
        const bool ok = answered ? crash->ask && ReadFileBytes(dir + L"\\" + base + L".asked", 16).rfind("send", 0) == 0
                                 : crash->share;
        if (ok) found.push_back({written, base});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

}  // namespace

std::vector<std::wstring> PendingDumps() { return UnsentDumps(false); }
std::vector<std::wstring> AnsweredDumps() { return UnsentDumps(true); }

std::vector<std::wstring> PendingErrors() {
    const std::wstring dir = crash::CrashesDir();
    std::vector<std::pair<ULONGLONG, std::wstring>> found;
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((dir + L"\\error-*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    const ULONGLONG now = NowFileTime();
    do {
        const ULONGLONG written = FileTimeValue(fd.ftLastWriteTime);
        if (now > written && (now - written) / kTicksPerSecond > kMaxDumpAgeDays * 86400ull)
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());  // never got through: stale now
        else
            found.push_back({written, fd.cFileName});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end());
    std::vector<std::wstring> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

std::vector<std::wstring> PendingSessions() {
    const std::wstring dir = crash::CrashesDir();
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    const HANDLE     h = FindFirstFileW((dir + L"\\session-*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        // session-<day>-<started>.txt
        const std::wstring name    = fd.cFileName;
        const int64_t      started = name.size() > 17 ? _wtoi64(name.c_str() + 17) : 0;
        if (!SessionFresh(started)) DeleteFileW((dir + L"\\" + name).c_str());
        else out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

int RunUploadChild() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);  // nothing on the desktop mid-game
    const bool allowed = UploadAllowed();
    if (!allowed && (!ActiveDsn() || AnsweredDumps().empty())) return 0;
    // An old engine's child may still be sending after a restart.
    const HANDLE lock = CreateFileW((crash::CrashesDir() + L"\\upload.lock").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return 1;
    struct CloseLock {
        HANDLE h;
        ~CloseLock() { CloseHandle(h); }
    } close_lock{lock};
    const auto dsn = ActiveDsn();
    // An answered crash goes first and on its own say-so; the rest only while sharing allows it.
    std::vector<std::wstring> pending = AnsweredDumps();
    std::vector<std::wstring> errors, days, usage;
    if (allowed) {
        for (auto& base : PendingDumps()) pending.push_back(std::move(base));
        errors = PendingErrors();
        days   = PendingSessions();
        usage  = PendingUsage();
    }
    if (pending.empty() && errors.empty() && days.empty() && usage.empty()) return 0;
    const std::string  system = MachineInfo();
    const ScrubContext scrub  = MachineScrubContext();
    bool dumps_left = false;  // over the crash cap; errors have their own, applied when queued
    for (const std::wstring& base : pending) {
        if (UploadsToday() >= kMaxUploadsPerDay) {
            dumps_left = true;
            break;
        }
        const Outcome o = UploadOne(*dsn, base, scrub, system);
        if (o == Outcome::StopPass || o == Outcome::Retry) return 1;
    }
    const ErrorCounts counts = ParseErrorCounts(ReadFileBytes(ErrorCountsPath(), 64 * 1024));
    for (const std::wstring& name : errors) {
        const Outcome o = UploadError(*dsn, name, counts, scrub, system);
        if (o == Outcome::StopPass || o == Outcome::Retry) return 1;
    }
    for (const std::wstring& name : days) {
        const Outcome o = UploadSession(*dsn, name);
        if (o == Outcome::StopPass || o == Outcome::Retry) return 1;
    }
    for (const std::wstring& name : usage) {
        const Outcome o = UploadUsage(*dsn, name);
        if (o == Outcome::StopPass || o == Outcome::Retry) return 1;
    }
    return dumps_left ? 1 : 0;
}

std::string BuildLogsFeedbackJson(const LogsInput& in) {
    // A feedback event: as for crashes, no `server_name` and no `user`; nobody typed anything, so
    // the message is fixed.
    JsonValue e = JsonValue::MakeObject();
    e.Set("event_id", JsonValue::MakeStr(in.event_id));
    e.Set("timestamp", JsonValue::MakeNum(in.timestamp));
    e.Set("platform", JsonValue::MakeStr("native"));
    e.Set("level", JsonValue::MakeStr("info"));
    e.Set("type", JsonValue::MakeStr("feedback"));
    e.Set("release", JsonValue::MakeStr(in.release));
    JsonValue feedback = JsonValue::MakeObject();
    feedback.Set("message", JsonValue::MakeStr("Logs sent from Settings. Reference " + in.event_id));
    feedback.Set("source", JsonValue::MakeStr("send_logs"));
    JsonValue contexts = JsonValue::MakeObject();
    contexts.Set("feedback", std::move(feedback));
    e.Set("contexts", std::move(contexts));
    JsonValue tags = JsonValue::MakeObject();
    tags.Set("process", JsonValue::MakeStr("ombra"));
    tags.Set("source", JsonValue::MakeStr("send_logs"));
    e.Set("tags", std::move(tags));
    JsonValue extra = JsonValue::MakeObject();
    extra.Set("system", JsonValue::MakeStr(in.system));
    e.Set("extra", std::move(extra));
    return WriteJsonCompact(e);
}

std::string BuildLogsEnvelope(const std::string& event_id, const std::string& feedback_json, const std::string& zip,
                              const std::string& zip_name) {
    std::string out = "{\"event_id\":\"" + event_id + "\"}\n";
    out += "{\"type\":\"feedback\",\"length\":" + std::to_string(feedback_json.size()) + "}\n" + feedback_json + "\n";
    out += "{\"type\":\"attachment\",\"length\":" + std::to_string(zip.size()) +
           ",\"content_type\":\"application/zip\",\"filename\":\"" + zip_name + "\"}\n";
    out += zip;
    out += "\n";
    return out;
}

std::vector<std::string> ZipEntryNames(const std::string& zip) {
    const auto u16 = [&](size_t at) { return static_cast<uint32_t>(static_cast<uint8_t>(zip[at])) | static_cast<uint32_t>(static_cast<uint8_t>(zip[at + 1])) << 8; };
    const auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    // The end-of-central-directory record: 22 bytes plus a comment of at most 64 KB, at the end.
    if (zip.size() < 22) return {};
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i <= 22 + 65535;)
        if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return {};
    const uint32_t count = u16(eocd + 10);
    size_t         at    = u32(eocd + 16);
    std::vector<std::string> names;
    for (uint32_t n = 0; n < count; ++n) {
        if (at + 46 > zip.size() || u32(at) != 0x02014b50) return {};
        const size_t name_len = u16(at + 28), extra = u16(at + 30), comment = u16(at + 32);
        if (at + 46 + name_len > zip.size()) return {};
        names.push_back(zip.substr(at + 46, name_len));
        at += 46 + name_len + extra + comment;
    }
    return names;
}

namespace {

// "YYYYMMDD n" in crashes\logs_count: today's Send logs posts.
std::wstring LogsCountPath() { return crash::CrashesDir() + L"\\logs_count"; }
int LogsSentToday() {
    std::istringstream in(ReadFileBytes(LogsCountPath(), 64));
    std::string        day;
    int                n = 0;
    return (in >> day >> n) && day == UtcDay() ? n : 0;
}

std::string SizeText(size_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), "%zu KB", (bytes + 1023) / 1024);
    return buf;
}

SendLogsResult SendLogs(const std::wstring& zip_path, const std::string& event_id) {
    const auto dsn = ActiveDsn();
    if (!dsn) return SendLogsResult::NoDsn;
    if (event_id.size() != 32 || event_id.find_first_not_of("0123456789abcdef") != std::string::npos)
        return SendLogsResult::Unreadable;
    const std::string zip = ReadFileBytes(zip_path, kMaxLogsBytes + 1);
    const std::vector<std::string> names = ZipEntryNames(zip);
    if (zip.empty() || zip.size() > kMaxLogsBytes || names.empty()) return SendLogsResult::Unreadable;
    CreateDirectoryW(crash::CrashesDir().c_str(), nullptr);
    if (LogsSentToday() >= kMaxLogsSendsPerDay) return SendLogsResult::TooMany;

    LogsInput in;
    in.event_id  = event_id;
    in.timestamp = static_cast<double>(UnixOf(NowFileTime()));
    in.release   = ReleaseName();
    in.system    = ScrubText(MachineInfo(), MachineScrubContext());
    const std::string feedback = BuildLogsFeedbackJson(in);
    const size_t      slash    = zip_path.find_last_of(L"\\/");
    const std::string zip_name = NarrowUtf8(slash == std::wstring::npos ? zip_path : zip_path.substr(slash + 1));

    // Before anything leaves the PC. The zip itself is already scrubbed.
    CreateDirectoryW(RecordsDir().c_str(), nullptr);
    const std::wstring record_path = RecordsDir() + L"\\" + WideFromUtf8(event_id) + L".txt";
    std::string record = sent_records::kLogsTitle + event_id + "\n" + sent_records::kLogsSizeLabel +
                         std::to_string(names.size()) + (names.size() == 1 ? " file, " : " files, ") + SizeText(zip.size()) +
                         "\n\nFiles in " + zip_name + ":\n";
    for (const std::string& n : names) record += n + "\n";
    record += "\nFeedback:\n" + feedback + "\n";
    if (!WriteFileBytes(record_path, record)) return SendLogsResult::Failed;
    PruneRecords();

    WriteFileBytes(LogsCountPath(), UtcDay() + " " + std::to_string(LogsSentToday() + 1));
    const PostResult  r      = PostEnvelope(*dsn, BuildLogsEnvelope(event_id, feedback, zip, zip_name));
    const std::string result = r.reached ? "HTTP " + std::to_string(r.status) : r.error;
    WriteFileBytes(record_path, record + sent_records::kResultPrefix + result + "\n");
    Log::Info("send logs  %zu files, %zu bytes: %s", names.size(), zip.size(), result.c_str());
    return r.reached && r.status >= 200 && r.status < 300 ? SendLogsResult::Sent : SendLogsResult::Failed;
}

}  // namespace

SendLogsResult RunSendLogs(const std::wstring& zip_path, const std::string& event_id) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    const SendLogsResult result = SendLogs(zip_path, event_id);
    // The overlay's own copy, made only for this post, goes whatever happened; a zip the user saved stays.
    if (zip_path.rfind(Instance().data_dir + L"\\" + crash::kSendLogsZipPrefix, 0) == 0) DeleteFileW(zip_path.c_str());
    return result;
}

void UploadSoon() {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    if (g_worker) g_worker->ErrorQueued();
}

Worker::~Worker() { Stop(); }

void Worker::Start() {
    if (thread_.joinable() || Instance().isolated) return;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        g_worker = this;
    }
    thread_     = std::thread([this] {
        Log::SetThreadName("crash-upload");
        Loop();
    });
}

void Worker::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        if (g_worker == this) g_worker = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    if (stop_event_) SetEvent(stop_event_);
    wake_.notify_all();
    thread_.join();
    if (stop_event_) CloseHandle(stop_event_);
    stop_event_ = nullptr;
}

void Worker::ErrorQueued() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_queued_ = true;
    }
    wake_.notify_all();
}

void Worker::SampleUsage(bool final) {
    if (!usage_probe_.counters) return;
    if (!UsageAllowed()) {  // nothing counted before the user allows it
        usage_.reset();
        return;
    }
    const UsageCounters c   = usage_probe_.counters();
    const int64_t       now = static_cast<int64_t>(std::time(nullptr));
    const std::string   day = UtcDay();
    const auto          record = [&] {
        DayRun r = usage_->Totals(c, now);
        if (usage_probe_.describe) usage_probe_.describe(r);
        RecordDayRun(r, usage_day_);
    };
    if (!usage_) {
        usage_.emplace(std::to_string(GetCurrentProcessId()) + "-" + std::to_string(now));
        usage_->Begin(c, now);
        usage_day_ = day;
        return;
    }
    usage_->Sample(c, now);
    if (day != usage_day_) {  // the last word on the day that ended; the new one starts from here
        record();
        usage_->Begin(c, now);
        usage_day_     = day;
        usage_samples_ = 0;
        return;
    }
    if (final || ++usage_samples_ % 15 == 0) record();
}

void Worker::Loop() {
    std::chrono::seconds wait{60};  // not on the startup path; a crash loop never gets this far
    const auto start     = std::chrono::steady_clock::now();
    auto       next_pass = start + wait;
    auto       next_sample = start;
    for (;;) {
        // A minute past the next UTC midnight, to close the day.
        const ULONGLONG now      = NowFileTime();
        const ULONGLONG day      = kTicksPerSecond * 86400;
        const auto      midnight = std::chrono::steady_clock::now() +
                              std::chrono::seconds((day - now % day) / kTicksPerSecond + 60);
        bool day_over = false, pass = true;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto until = std::min(next_pass, midnight);
            if (usage_probe_.counters) until = std::min(until, next_sample);
            if (wake_.wait_until(lock, until, [this] { return stopping_ || error_queued_; })) {
                if (stopping_) break;
                // Errors come in bursts (a rebuild loop, a dying disk): one child for the lot.
                if (wake_.wait_for(lock, std::chrono::minutes(2), [this] { return stopping_; })) break;
                error_queued_ = false;
            } else if (until == midnight) {
                day_over = true;
            } else if (until != next_pass) {
                pass = false;  // only a usage sample is due
            }
        }
        if (std::chrono::steady_clock::now() >= next_sample || day_over) {
            SampleUsage(false);  // before a pass, so a day's last line goes with it
            next_sample = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        }
        if (!pass) continue;
        if (day_over) TickSessions();  // then a pass, so the closed day goes now
        next_pass = std::chrono::steady_clock::now() + std::chrono::hours(6);
        const bool waiting = ((!PendingDumps().empty() || !PendingErrors().empty() || !PendingSessions().empty() ||
                               !PendingUsage().empty()) &&
                              UploadAllowed()) ||
                             (!AnsweredDumps().empty() && ActiveDsn());
        if (waiting && !StartChild(stop_event_))
            Log::Warn("crash upload: couldn't start the child (error %lu)", GetLastError());
    }
    SampleUsage(true);  // the run's last line
}

}  // namespace ombra::crash_upload
