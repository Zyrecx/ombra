// Crash upload (crash_upload.h): parsing, the envelope, the consent gates, and one whole pass against
// a local stub standing in for Sentry.

#include <doctest/doctest.h>

#include "support/process.h"
#include "support/test_env.h"

#include "ombra/config.h"
#include "ombra/crash.h"
#include "ombra/crash_upload.h"
#include "ombra/paths.h"
#include "ombra/sent_records.h"
#include "ombra/util.h"
#include "ombra/zip_writer.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace ombra::test {

using namespace crash_upload;

namespace {

// Answers every POST with `status` and keeps the bodies.
class StubServer {
public:
    explicit StubServer(int status) : status_(status) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listen_, 8);
        int len = sizeof(addr);
        getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_   = ntohs(addr.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }
    ~StubServer() {
        closesocket(listen_);
        thread_.join();
        WSACleanup();
    }
    int port() const { return port_; }
    std::vector<std::string> requests;  // read after the pass; the pass is synchronous

private:
    void Serve() {
        for (;;) {
            const SOCKET c = accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::string data;
            char        buf[65536];
            size_t      need = std::string::npos;
            for (;;) {
                const int got = recv(c, buf, sizeof(buf), 0);
                if (got <= 0) break;
                data.append(buf, got);
                const size_t end = data.find("\r\n\r\n");
                if (end != std::string::npos && need == std::string::npos) {
                    std::string lower = data.substr(0, end);
                    for (char& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                    const size_t cl = lower.find("content-length:");
                    need = end + 4 + (cl == std::string::npos ? 0 : std::stoull(lower.substr(cl + 15)));
                }
                if (need != std::string::npos && data.size() >= need) break;
            }
            requests.push_back(data);
            const std::string resp = "HTTP/1.1 " + std::to_string(status_) + " X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, resp.data(), static_cast<int>(resp.size()), 0);
            closesocket(c);
        }
    }
    SOCKET      listen_ = INVALID_SOCKET;
    int         port_   = 0;
    int         status_;
    std::thread thread_;
};

void WriteText(const std::wstring& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

std::string ReadText(const std::wstring& path) {
    std::ifstream      f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A clean crashes folder holding one dump named `base`, with consent as given.
void SeedDump(const std::wstring& base, bool share) {
    const std::wstring dir = crash::CrashesDir();
    CreateDirectoryW(Instance().data_dir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    WriteText(dir + L"\\" + base + L".dmp", std::string("MDMP\0binary", 11));
    WriteText(dir + L"\\" + base + L".txt", std::string("ombra 1.2.3 (abc123)\nkind: die\nthread: main\nshare: ") +
                                                (share ? "yes" : "no") + "\nreason: boom at C:\\Users\\Ana\\x.mp4\n");
}

void ClearCrashes() {
    const std::wstring dir = crash::CrashesDir();
    WIN32_FIND_DATAW   fd{};
    const HANDLE       h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

struct UploadEnv {
    explicit UploadEnv(int port) {
        ClearCrashes();
        const std::wstring dsn = L"http://testkey@127.0.0.1:" + std::to_wstring(port) + L"/42";
        SetEnvironmentVariableW(L"OMBRA_TEST_SENTRY_DSN", dsn.c_str());
        CreateDirectoryW(Instance().config_dir.c_str(), nullptr);
        WriteText(SetupCompletedPath(), "");
        crash::SetSharingEnabled(true);
    }
    ~UploadEnv() {
        SetEnvironmentVariableW(L"OMBRA_TEST_SENTRY_DSN", nullptr);
        DeleteFileW(SetupCompletedPath().c_str());
        ClearCrashes();
    }
};

}  // namespace

TEST_SUITE("crash_upload") {

TEST_CASE("ParseDsn") {
    const auto d = ParseDsn("https://abc@o1.ingest.de.sentry.io/456");
    REQUIRE(d.has_value());
    CHECK(d->key == "abc");
    CHECK(d->host == "o1.ingest.de.sentry.io");
    CHECK(d->port == 443);
    CHECK(d->project == "456");
    CHECK(d->secure);
    const auto local = ParseDsn("http://k@127.0.0.1:9000/1");
    REQUIRE(local.has_value());
    CHECK(local->port == 9000);
    CHECK_FALSE(local->secure);
    CHECK_FALSE(ParseDsn("").has_value());
    CHECK_FALSE(ParseDsn("http://k@sentry.io/1").has_value());  // never in the clear to a real host
    CHECK_FALSE(ParseDsn("https://sentry.io/1").has_value());
    CHECK_FALSE(ParseDsn("https://k@sentry.io/").has_value());
}

TEST_CASE("ParseCrashText") {
    const auto c = ParseCrashText("ombra-ui 1.2.3 (abc)\nkind: die\nthread: main\nshare: yes\nreason: a\nb\n");
    REQUIRE(c.has_value());
    CHECK(c->exe == "ombra-ui");
    CHECK(c->version == "1.2.3");
    CHECK(c->commit == "abc");
    CHECK(c->share);
    CHECK(c->reason == "a\nb");
    // A dump from before consent was recorded.
    const auto old = ParseCrashText("ombra 1.0.0 (abc)\nkind: die\nthread: main\nreason: x\n");
    REQUIRE(old.has_value());
    CHECK_FALSE(old->share);
    CHECK_FALSE(ParseCrashText("garbage").has_value());
}

TEST_CASE("DumpDate handles dashes in the exe name") {
    CHECK(DumpDate(L"ombra-ui-20261106-120000-42") == "20261106");
    CHECK(DumpDate(L"ombra-20261106-120000-42") == "20261106");
    CHECK(DumpDate(L"nonsense") == "");
}

TEST_CASE("LogBeforeCrash stops at the crash and keeps only the last lines") {
    CrashText c;
    c.kind   = "die";
    c.reason = "boom";
    const std::string log = "one\ntwo\nthree\n[E] FATAL die: boom\nafter the restart\n";
    CHECK(LogBeforeCrash(log, c, 2) == "three\n[E] FATAL die: boom\n");
    CHECK(LogBeforeCrash(log, c, 100) == "one\ntwo\nthree\n[E] FATAL die: boom\n");
    c.reason = "not in this log";
    CHECK(LogBeforeCrash(log, c, 100) == "");
}

TEST_CASE("SettingsBeforeCrash takes the last settings line before the crash") {
    CrashText c;
    c.kind   = "die";
    c.reason = "boom";
    const std::string log = "[E] settings: old=1\r\n[E] settings: new=2\r\n[E] FATAL die: boom\r\n[E] settings: later=3\r\n";
    CHECK(SettingsBeforeCrash(log, c) == "new=2");
    CHECK(SettingsBeforeCrash("[E] FATAL die: boom\n", c) == "");
}

TEST_CASE("the event carries no identity and the envelope has exact lengths") {
    EventInput in;
    in.event_id      = "0123456789abcdef0123456789abcdef";
    in.crash.exe     = "ombra";
    in.crash.version = "1.2.3";
    in.crash.commit  = "abc";
    in.crash.kind    = "die";
    const std::string e = BuildEventJson(in);
    CHECK(e.find("\"release\":\"ombra@1.2.3+abc\"") != std::string::npos);
    CHECK(e.find("\"level\":\"fatal\"") != std::string::npos);
    CHECK(e.find("server_name") == std::string::npos);
    CHECK(e.find("\"user\"") == std::string::npos);

    const std::string dump("MD\nMP\0", 6);
    const std::string env = BuildEnvelope(in.event_id, e, dump, "x.dmp", "log\n");
    CHECK(env.rfind("{\"event_id\":\"" + in.event_id + "\"}\n", 0) == 0);
    CHECK(env.find("{\"type\":\"event\",\"length\":" + std::to_string(e.size()) + "}\n" + e + "\n") != std::string::npos);
    CHECK(env.find("\"attachment_type\":\"event.minidump\",\"filename\":\"x.dmp\"}\n" + dump + "\n") != std::string::npos);
    CHECK(env.find("\"length\":4,\"content_type\":\"text/plain\",\"filename\":\"log.txt\"}\nlog\n\n") != std::string::npos);
}

TEST_CASE("only dumps stamped with consent are pending") {
    UploadEnv env(1);
    SeedDump(L"ombra-20261106-120000-1", false);
    CHECK(PendingDumps().empty());
    SeedDump(L"ombra-20261106-120000-2", true);
    const auto pending = PendingDumps();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0] == L"ombra-20261106-120000-2");
}

TEST_CASE("a pass sends, records first, scrubs, and doesn't send twice") {
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring base = L"ombra-20261106-120000-7";
    SeedDump(base, true);

    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 1);
    const std::string& req = server.requests[0];
    CHECK(req.find("POST /api/42/envelope/") == 0);
    CHECK(req.find("sentry_key=testkey") != std::string::npos);
    CHECK(req.find("event.minidump") != std::string::npos);
    CHECK(req.find("Ana") == std::string::npos);
    CHECK(req.find("x.mp4") != std::string::npos);
    CHECK(Exists(crash::CrashesDir() + L"\\" + base + L".sent"));

    const std::wstring sent = crash::CrashesDir() + L"\\" + base + L".sent";
    const std::string  id   = ReadText(sent).substr(0, 32);
    const std::string  record = ReadText(Instance().data_dir + L"\\sent_reports\\" + std::wstring(id.begin(), id.end()) + L".txt");
    CHECK(record.find("Result: HTTP 200") != std::string::npos);
    CHECK(record.find("ombra@1.2.3+abc123") != std::string::npos);
    const auto parsed = sent_records::Parse(std::wstring(id.begin(), id.end()) + L".txt", record);
    CHECK(parsed.kind == sent_records::Kind::Crash);
    CHECK(parsed.status == sent_records::Status::Sent);
    CHECK(parsed.result == "HTTP 200");
    CHECK(parsed.summary.rfind("Ombra crashed: boom", 0) == 0);

    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.size() == 1);
}

TEST_CASE("the real ombra.exe child uploads") {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self;
    exe = exe.substr(0, exe.find_last_of(L'\\') + 1) + L"ombra.exe";
    if (!Exists(exe)) return;
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring base = L"ombra-20261106-120000-20";
    SeedDump(base, true);
    const ProcessResult r = RunProcess(Quote(exe) + L" crash-upload", 60000);
    REQUIRE(r.launched);
    CHECK(r.exit_code == 0);
    CHECK(server.requests.size() == 1);
    CHECK(Exists(crash::CrashesDir() + L"\\" + base + L".sent"));
}

TEST_CASE("nothing is sent before setup finished") {
    StubServer server(200);
    UploadEnv  env(server.port());
    SeedDump(L"ombra-20261106-120000-8", true);
    DeleteFileW(SetupCompletedPath().c_str());
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.empty());
}

TEST_CASE("sharing turned Off after the crash cancels the send") {
    StubServer server(200);
    UploadEnv  env(server.port());
    SeedDump(L"ombra-20261106-120000-9", true);
    crash::SetSharingEnabled(false);
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.empty());
}

TEST_CASE("no DSN means no upload") {
    StubServer server(200);
    UploadEnv  env(server.port());
    SeedDump(L"ombra-20261106-120000-10", true);
    SetEnvironmentVariableW(L"OMBRA_TEST_SENTRY_DSN", nullptr);
    CHECK_FALSE(UploadAllowed());
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.empty());
}

TEST_CASE("an isolated instance can't reach a real host") {
    UploadEnv env(1);
    SetEnvironmentVariableW(L"OMBRA_TEST_SENTRY_DSN", L"https://key@o1.ingest.de.sentry.io/42");
    CHECK_FALSE(UploadAllowed());
}

TEST_CASE("a 429 keeps the dump for a later pass; another 4xx gives up on it") {
    {
        StubServer server(429);
        UploadEnv  env(server.port());
        const std::wstring base = L"ombra-20261106-120000-11";
        SeedDump(base, true);
        CHECK(RunUploadChild() == 1);
        CHECK(server.requests.size() == 1);
        CHECK_FALSE(Exists(crash::CrashesDir() + L"\\" + base + L".sent"));
    }
    {
        StubServer server(400);
        UploadEnv  env(server.port());
        const std::wstring base = L"ombra-20261106-120000-12";
        SeedDump(base, true);
        CHECK(RunUploadChild() == 0);
        CHECK(ReadText(crash::CrashesDir() + L"\\" + base + L".sent").find("rejected") == 0);
    }
}

TEST_CASE("the daily cap survives restarts") {
    StubServer server(200);
    UploadEnv  env(server.port());
    for (int i = 0; i < 7; ++i) SeedDump(L"ombra-20261106-120000-" + std::to_wstring(100 + i), true);
    CHECK(RunUploadChild() == 1);
    CHECK(server.requests.size() == 5);
    CHECK(RunUploadChild() == 1);
    CHECK(server.requests.size() == 5);
}

TEST_CASE("error caps: 3 per fingerprint, 25 a day, counting past both, a new day starts over") {
    ErrorCounts c;
    for (int i = 0; i < 3; ++i) CHECK(CountError(c, "20261110", "save|no_room|"));
    CHECK_FALSE(CountError(c, "20261110", "save|no_room|"));
    CHECK(OccurrencesOn(c, "20261110", "save|no_room|") == 4);
    CHECK(OccurrencesOn(c, "20261109", "save|no_room|") == 0);
    int queued = 3;
    for (int i = 0; i < 40; ++i) queued += CountError(c, "20261110", "audio|mic:x:" + std::to_string(i) + "|") ? 1 : 0;
    CHECK(queued == kMaxErrorsPerDay);
    const ErrorCounts back = ParseErrorCounts(WriteErrorCounts(c));
    CHECK(back.day == "20261110");
    CHECK(back.entries.size() == c.entries.size());
    CHECK(OccurrencesOn(back, "20261110", "save|no_room|") == 4);
    CHECK(CountError(c, "20261111", "save|no_room|"));
    CHECK(OccurrencesOn(c, "20261111", "save|no_room|") == 1);
    CHECK(c.entries.size() == 1);
}

TEST_CASE("an error event is grouped by kind, code and vendor and carries no identity") {
    ErrorEventInput in;
    in.error.event_id  = "0123456789abcdef0123456789abcdef";
    in.error.kind      = "encoder";
    in.error.code      = "nvEncEncodePicture:8";
    in.error.vendor    = "NVENC";
    in.error.version   = "1.2.3";
    in.error.commit    = "abc";
    in.occurrences_today = 7;
    const std::string e = BuildErrorEventJson(in);
    CHECK(e.find("\"level\":\"error\"") != std::string::npos);
    CHECK(e.find("\"fingerprint\":[\"encoder\",\"nvEncEncodePicture:8\",\"NVENC\"]") != std::string::npos);
    CHECK(e.find("\"occurrences_today\":7") != std::string::npos);
    CHECK(e.find("\"release\":\"ombra@1.2.3+abc\"") != std::string::npos);
    CHECK(e.find("server_name") == std::string::npos);
    CHECK(e.find("\"user\"") == std::string::npos);
    const std::string env = BuildErrorEnvelope(in.error.event_id, e, "log\n");
    CHECK(env.find("{\"type\":\"event\",\"length\":" + std::to_string(e.size()) + "}\n" + e + "\n") != std::string::npos);
    CHECK(env.find("\"length\":4,\"content_type\":\"text/plain\",\"filename\":\"log.txt\"}\nlog\n\n") != std::string::npos);
    CHECK(env.find("minidump") == std::string::npos);
}

TEST_CASE("errors queue only while sharing is on, then a pass sends them once with the day's count") {
    StubServer server(200);
    UploadEnv  env(server.port());
    crash::SetSharingEnabled(false);
    ReportError(ErrorKind::Save, "no_room", "", "save folder can't take the clip");
    CHECK(PendingErrors().empty());  // an error while Off is never sent later
    crash::SetSharingEnabled(true);
    for (int i = 0; i < 5; ++i) ReportError(ErrorKind::Save, "no_room", "", "save folder can't take the clip");
    ReportError(ErrorKind::Encoder, "nvEncEncodePicture:8", "NVENC", "pipeline fault, rebuilding");
    CHECK(PendingErrors().size() == 4);

    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 4);
    int with_five = 0;
    for (const std::string& req : server.requests) {
        CHECK(req.find("\"level\":\"error\"") != std::string::npos);
        if (req.find("\"no_room\"") != std::string::npos && req.find("\"occurrences_today\":5") != std::string::npos)
            ++with_five;
    }
    CHECK(with_five == 3);  // the count is read when sent, so it includes the ones only counted
    bool listed = false;
    for (const sent_records::Record& r : sent_records::List())
        if (r.kind == sent_records::Kind::Error && r.summary == "save: save folder can't take the clip (no_room)") {
            CHECK(r.status == sent_records::Status::Sent);
            listed = true;
        }
    CHECK(listed);    CHECK(PendingErrors().empty());
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.size() == 4);
}

TEST_CASE("a 429 keeps queued errors; another 4xx drops them") {
    {
        StubServer server(429);
        UploadEnv  env(server.port());
        ReportError(ErrorKind::Update, "installer_changed", "");
        CHECK(RunUploadChild() == 1);
        CHECK(PendingErrors().size() == 1);
    }
    {
        StubServer server(400);
        UploadEnv  env(server.port());
        ReportError(ErrorKind::Update, "installer_changed", "");
        CHECK(RunUploadChild() == 0);
        CHECK(PendingErrors().empty());
    }
}

TEST_CASE("the sharing marker follows the setting") {
    ClearCrashes();
    crash::SetSharingEnabled(true);
    CHECK(Exists(crash::CrashesDir() + L"\\sharing_on"));
    crash::SetSharingEnabled(false);
    CHECK_FALSE(Exists(crash::CrashesDir() + L"\\sharing_on"));
}

TEST_CASE("the install ID goes on events only at the usage stop") {
    StubServer server(200);
    UploadEnv  env(server.port());
    ReportError(ErrorKind::Save, "no_room", "");
    crash::SetSharingEnabled(true, true);
    CHECK(UsageAllowed());
    ReportError(ErrorKind::Save, "write_failed", "");
    crash::SetSharingEnabled(true, false);
    CHECK_FALSE(UsageAllowed());
    CHECK(RunUploadChild() == 0);  // decided when sent, not when queued
    REQUIRE(server.requests.size() == 2);
    for (const std::string& req : server.requests) CHECK(req.find("\"user\"") == std::string::npos);

    crash::SetSharingEnabled(true, true);
    ReportError(ErrorKind::Save, "no_room", "");
    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 3);
    const std::string id = InstallId();
    CHECK(id.size() == 32);
    CHECK(InstallId() == id);  // kept
    CHECK(server.requests[2].find("\"user\":{\"id\":\"" + id + "\"}") != std::string::npos);
}

TEST_CASE("a day's session is the worst that happened in it") {
    DaySession d{"20261110", "ombra@1.2.3+abc", 1762790000};
    CHECK(std::string(SessionStatus(d)) == "exited");
    d.errored = true;
    CHECK(std::string(SessionStatus(d)) == "errored");
    d.abnormal = true;
    CHECK(std::string(SessionStatus(d)) == "abnormal");
    d.crashed = true;
    CHECK(std::string(SessionStatus(d)) == "crashed");

    SessionState s;
    s.dumps_seen = 133000000000000000ull;
    s.days.push_back(DaySession{"20261109", "ombra@1.2.2+old", 1762700000, true});
    s.days.push_back(DaySession{"20261110", "ombra@1.2.2+old", 1762750000});
    const SessionState back = ParseSessionState(WriteSessionState(s));
    CHECK(back.dumps_seen == s.dumps_seen);
    REQUIRE(back.days.size() == 2);
    CHECK(back.days[0].errored);
    CHECK(back.days[1].release == "ombra@1.2.2+old");

    // An update mid-day: the old release keeps its session, the new one gets its own.
    SessionState            rolled = back;
    std::vector<DaySession> closed;
    RollSessions(rolled, "20261110", "ombra@1.2.3+abc", 1762790000, closed);
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].day == "20261109");
    REQUIRE(rolled.days.size() == 2);
    RollSessions(rolled, "20261110", "ombra@1.2.3+abc", 1762790100, closed);
    CHECK(rolled.days.size() == 2);  // one per day and release
    CHECK(SessionFor(rolled, "20261110", "ombra@1.2.3+abc")->started == 1762790000);
    CHECK(SessionFor(rolled, "20261110", "ombra@9+x")->release == "ombra@1.2.3+abc");  // that day's newest
    CHECK(SessionFor(rolled, "20250101", "")->release == "ombra@1.2.3+abc");            // else the newest

    const std::string json = BuildSessionsJson(*SessionFor(rolled, "20261110", "ombra@1.2.2+old"), "");
    CHECK(json == "{\"aggregates\":[{\"started\":\"2025-11-10T04:46:00Z\",\"exited\":1}],"
                  "\"attrs\":{\"release\":\"ombra@1.2.2+old\",\"environment\":\"production\"}}");
    CHECK(BuildSessionsJson(closed[0], "ab").find("\"did\":\"ab\",\"errored\":1") != std::string::npos);
    CHECK(BuildSessionEnvelope(json) ==
          "{}\n{\"type\":\"sessions\",\"length\":" + std::to_string(json.size()) + "}\n" + json + "\n");
}

TEST_CASE("LogTailOf ends at the run's last line") {
    const std::string log = "a [E 10 1] one\nb [E 10 1] two\nc [E 22 1] other\nd [E 10 1] three\ne [E 22 1] new\n";
    CHECK(LogTailOf(log, "[E 10 ", 2) == "c [E 22 1] other\nd [E 10 1] three\n");
    CHECK(LogTailOf(log, "[E 10 ", 50) == log.substr(0, log.find("e [E 22")));
    CHECK(LogTailOf(log, "[E 99 ", 5).empty());
}

TEST_CASE("an unclean exit becomes an abnormal session and an exit error; a clean one doesn't") {
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring dir = crash::CrashesDir();
    CreateDirectoryW((Instance().data_dir + L"\\logs").c_str(), nullptr);
    WriteText(Instance().data_dir + L"\\logs\\ombra-20991231.log", "x [E 4242 7] last words of C:\\Users\\Ana\\a.mp4\n");

    BeginRun();  // the first run: nothing before it
    CHECK(PendingErrors().empty());
    EndRun();
    CHECK_FALSE(Exists(dir + L"\\running"));
    BeginRun();
    CHECK(PendingErrors().empty());
    CHECK(Exists(dir + L"\\running"));

    // A run that never exited, with no dump.
    WriteText(dir + L"\\running", "4242 " + std::to_string(time(nullptr) - 10) + " ombra@0.0.1+prev\n");
    BeginRun();
    const auto errors = PendingErrors();
    REQUIRE(errors.size() == 1);
    const std::string queued = ReadText(dir + L"\\" + errors[0]);
    CHECK(queued.find("\"kind\":\"exit\"") != std::string::npos);
    CHECK(queued.find("\"code\":\"unclean\"") != std::string::npos);  // no reboot since it started
    CHECK(queued.find("last words") != std::string::npos);
    const SessionState s = ParseSessionState(ReadText(dir + L"\\sessions"));
    REQUIRE_FALSE(s.days.empty());
    CHECK(s.days.back().abnormal);  // no session of its release: the newest one
    CHECK_FALSE(s.days.back().errored);

    // One that crashed: its dump says so, no exit error.
    ClearCrashes();
    crash::SetSharingEnabled(true);
    WriteText(dir + L"\\running", "5151 " + std::to_string(time(nullptr) - 10) + " ombra@0.0.1+prev\n");
    WriteText(dir + L"\\ombra-20991231-120000-5151.dmp", "MDMP");
    BeginRun();
    CHECK(PendingErrors().empty());
    DeleteFileW((Instance().data_dir + L"\\logs\\ombra-20991231.log").c_str());
}

TEST_CASE("a closed day is queued and sent as a sessions item, with the install ID only at usage") {
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring dir = crash::CrashesDir();
    WriteText(dir + L"\\sessions", "seen 0\n20000101 946684800 000 ombra@0+old\n");  // far too old: dropped
    TickSessions();
    CHECK(PendingSessions().empty());

    SYSTEMTIME st{};
    GetSystemTime(&st);
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER t{};
    t.LowPart  = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart -= 86400ull * 10000000ull;
    ft.dwLowDateTime  = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char yesterday[16];
    std::snprintf(yesterday, sizeof(yesterday), "%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    const long long yesterday_unix = static_cast<long long>(std::time(nullptr)) - 86400;
    WriteText(dir + L"\\sessions",
              std::string("seen 0\n") + yesterday + " " + std::to_string(yesterday_unix) + " 100 ombra@1+y\n");
    TickSessions();
    REQUIRE(PendingSessions().size() == 1);
    const SessionState s = ParseSessionState(ReadText(dir + L"\\sessions"));
    REQUIRE(s.days.size() == 1);  // today's, opened
    CHECK(s.days[0].day != yesterday);

    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests[0].find("{\"type\":\"sessions\",") != std::string::npos);
    CHECK(server.requests[0].find("\"errored\":1") != std::string::npos);
    CHECK(server.requests[0].find("\"did\"") == std::string::npos);
    CHECK(PendingSessions().empty());

    crash::SetSharingEnabled(true, true);
    WriteText(dir + L"\\sessions", std::string("seen 0\n") + yesterday + " " + std::to_string(yesterday_unix) + " 000 ombra@1+y\n");
    TickSessions();
    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests[1].find("\"did\":\"" + InstallId() + "\",\"exited\":1") != std::string::npos);

    // Off: the day closes but isn't queued.
    crash::SetSharingEnabled(false);
    WriteText(dir + L"\\sessions", std::string("seen 0\n") + yesterday + " " + std::to_string(yesterday_unix) + " 000 ombra@1+y\n");
    TickSessions();
    CHECK(PendingSessions().empty());
}

TEST_CASE("a game session line round-trips, exe names with spaces included") {
    const GameSession s{100, 4000, "AMF", "av1", 1920, 1080, 60, 3, "League of Legends.exe", GameSessionGroup(L"e:\\games\\lol")};
    const auto        back = ParseGameSession(WriteGameSession(s) + "\r\n");
    REQUIRE(back);
    CHECK(back->start == 100);
    CHECK(back->end == 4000);
    CHECK(back->encoder == "AMF");
    CHECK(back->codec == "av1");
    CHECK(back->width == 1920);
    CHECK(back->height == 1080);
    CHECK(back->fps == 60);
    CHECK(back->saves == 3);
    CHECK(back->exe == "League of Legends.exe");
    CHECK(back->group.size() == 16);
    CHECK(back->group == s.group);

    const auto unbuilt = ParseGameSession(WriteGameSession(GameSession{1, 2, "", "", 0, 0, 0, 0, "a.exe", ""}));
    REQUIRE(unbuilt);
    CHECK(unbuilt->encoder.empty());
    CHECK(unbuilt->group.empty());
    CHECK(GameSessionGroup(L"").empty());
    CHECK(GameSessionGroup(L"e:\\games\\a") != GameSessionGroup(L"e:\\games\\b"));
    CHECK_FALSE(ParseGameSession("1 2 0 60 1 1 NVENC h264 -"));  // no exe
    CHECK_FALSE(ParseGameSession("5 2 0 60 1 1 NVENC h264 - a.exe"));  // ends before it starts
}

TEST_CASE("one game's exes join into one session under the exe it ran longest as") {
    const std::string lol = GameSessionGroup(L"e:\\games\\riot games\\league of legends");
    const std::vector<GameSession> parts = {
        {1000, 1300, "AMF", "h264", 1280, 720, 60, 0, "LeagueClientUx.exe", lol},
        {1310, 3100, "NVENC", "h264", 1920, 1080, 60, 2, "League of Legends.exe", lol},  // the match
        {3100, 3150, "AMF", "h264", 1280, 720, 60, 0, "LeagueClientUx.exe", lol},     // too short alone
        {3300, 5000, "NVENC", "h264", 1920, 1080, 60, 1, "League of Legends.exe", lol},  // 150 s in queue
        {5000, 5600, "AMF", "h264", 1920, 1080, 60, 0, "other.exe", ""},  // no entry: by exe alone
        {5000, 5600, "AMF", "h264", 1920, 1080, 60, 0, "x.exe", GameSessionGroup(L"e:\\x")},
    };
    const auto joined = JoinGameSessions(parts);
    REQUIRE(joined.size() == 3);
    CHECK(joined[0].start == 1000);
    CHECK(joined[0].end == 5000);
    CHECK(joined[0].saves == 3);
    CHECK(joined[0].exe == "League of Legends.exe");
    CHECK(joined[0].encoder == "NVENC");  // the longest built part
    CHECK(joined[0].width == 1920);
    CHECK(joined[1].exe != joined[2].exe);
}

TEST_CASE("game sessions split by a restart are joined; short ones dropped; the longest kept") {
    std::vector<GameSession> parts = {
        {2000, 2100, "NVENC", "h264", 1920, 1080, 60, 2, "game.exe", ""},  // restart 10 s after the first part
        {1000, 1990, "", "", 0, 0, 0, 1, "game.exe", ""},
        {5000, 5030, "AMF", "h264", 1, 1, 30, 0, "launcher.exe", ""},  // too short
        {9000, 9500, "AMF", "h264", 1, 1, 30, 0, "game.exe", ""},     // a new session: past the gap
    };
    const auto joined = JoinGameSessions(parts);
    REQUIRE(joined.size() == 2);
    CHECK(joined[0].start == 1000);
    CHECK(joined[0].end == 2100);
    CHECK(joined[0].saves == 3);
    CHECK(joined[0].encoder == "NVENC");  // how it ended up being recorded
    CHECK(joined[0].fps == 60);
    CHECK(joined[1].start == 9000);

    std::vector<GameSession> many;
    for (int i = 0; i < kMaxGameSessionsPerDay + 5; ++i)
        many.push_back({i * 10000LL, i * 10000LL + 100 + i, "", "", 0, 0, 0, 0, "g" + std::to_string(i) + ".exe", ""});
    const auto capped = JoinGameSessions(many);
    REQUIRE(capped.size() == static_cast<size_t>(kMaxGameSessionsPerDay));
    CHECK(capped.front().exe == "g5.exe");  // the 5 shortest went
    CHECK(capped.back().exe == "g24.exe");
}

TEST_CASE("game session Logs: one item each, typed attributes, no IP inference") {
    const std::vector<GameSession> s = {{1762790000, 1762793600, "NVENC", "av1", 2560, 1440, 120, 4, "cs2.exe", ""},
                                        {1762800000, 1762800600, "", "", 0, 0, 0, 0, "b.exe", ""}};
    const std::string payload = BuildGameSessionLogs(s, {"ombra@1+x", "production", "id1"});
    CHECK(payload.rfind("{\"version\":2,\"ingest_settings\":{\"infer_ip\":\"never\",\"infer_user_agent\":\"never\"},"
                        "\"items\":[{\"timestamp\":1762790000,\"trace_id\":\"",
                        0) == 0);
    CHECK(payload.find("\"ombra.game_exe\":{\"value\":\"cs2.exe\",\"type\":\"string\"}") != std::string::npos);
    CHECK(payload.find("\"ombra.seconds\":{\"value\":3600,\"type\":\"integer\"}") != std::string::npos);
    CHECK(payload.find("\"ombra.height\":{\"value\":1440,\"type\":\"integer\"}") != std::string::npos);
    CHECK(payload.find("\"user.id\":{\"value\":\"id1\"") != std::string::npos);
    CHECK(payload.find("country") == std::string::npos);
    CHECK(payload.find("\"sentry.release\":{\"value\":\"ombra@1+x\"") != std::string::npos);
    const size_t second = payload.find("b.exe");
    REQUIRE(second != std::string::npos);
    CHECK(payload.find("ombra.encoder", second) == std::string::npos);  // never built: no encoder fields
    CHECK(BuildLogEnvelope(payload, 2) ==
          "{}\n{\"type\":\"log\",\"item_count\":2,\"content_type\":\"application/vnd.sentry.items.log+json\",\"length\":" +
              std::to_string(payload.size()) + "}\n" + payload + "\n");
}

TEST_CASE("game sessions queue only at the usage stop and go once their day is over") {
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring dir = crash::CrashesDir();
    const int64_t      now = static_cast<int64_t>(std::time(nullptr));
    const auto         usage_files = [&] {
        int              n = 0;
        WIN32_FIND_DATAW fd{};
        const HANDLE     h = FindFirstFileW((dir + L"\\usage-games-*.txt").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do ++n;
        while (FindNextFileW(h, &fd));
        FindClose(h);
        return n;
    };
    RecordGameSession({now - 600, now, "NVENC", "h264", 1920, 1080, 60, 1, "game.exe", ""});  // filed under today
    CHECK(usage_files() == 0);  // errors stop: not even queued

    crash::SetSharingEnabled(true, true);
    RecordGameSession({now - 600, now, "NVENC", "h264", 1920, 1080, 60, 1, "game.exe", ""});  // filed under today
    CHECK(usage_files() == 1);
    CHECK(PendingUsage().empty());  // today's: still open

    // Yesterday's file (as if written then) goes in one envelope; a stale one is deleted unsent.
    const int64_t y = now - 86400;
    const auto    day_of = [](int64_t unix_s) {
        const std::time_t t = static_cast<std::time_t>(unix_s);
        std::tm           tm{};
        gmtime_s(&tm, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        return std::string(buf);
    };
    WriteText(dir + L"\\usage-games-" + WideFromUtf8(day_of(y)) + L".txt",
              WriteGameSession({y - 600, y, "AMF", "av1", 1920, 1080, 60, 0, "a.exe", ""}) + "\n" +
                  WriteGameSession({y + 1, y + 1200, "AMF", "av1", 1920, 1080, 60, 2, "b c.exe", ""}) + "\n");
    WriteText(dir + L"\\usage-games-" + WideFromUtf8(day_of(now - 10 * 86400)) + L".txt",
              WriteGameSession({now - 10 * 86400, now - 10 * 86400 + 600, "", "", 0, 0, 0, 0, "old.exe", ""}) + "\n");
    REQUIRE(PendingUsage().size() == 1);
    CHECK(usage_files() == 2);  // the stale one went

    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 1);
    const std::string& req = server.requests[0];
    CHECK(req.find("\"type\":\"log\",\"item_count\":2,") != std::string::npos);
    CHECK(req.find("\"value\":\"b c.exe\"") != std::string::npos);
    const std::wstring games_record = L"usage-games-" + WideFromUtf8(day_of(y)) + L".txt";
    const auto         games = sent_records::Parse(games_record, ReadText(sent_records::Dir() + L"\\" + games_record));
    CHECK(games.kind == sent_records::Kind::GameSessions);
    CHECK(games.status == sent_records::Status::Sent);
    CHECK(games.summary == "2 game sessions: a.exe, b c.exe");
    CHECK(games.day == day_of(y).substr(0, 4) + "-" + day_of(y).substr(4, 2) + "-" + day_of(y).substr(6, 2));    CHECK(req.find("\"user.id\":{\"value\":\"" + InstallId() + "\"") != std::string::npos);
    CHECK(PendingUsage().empty());
    CHECK(usage_files() == 1);  // today's stays

    // Below the usage stop the queue is dropped, unsent.
    crash::SetSharingEnabled(true, false);
    CHECK(PendingUsage().empty());
    CHECK(usage_files() == 0);
}

TEST_CASE("daily Log: every setting is sent, sent as changed, or never sent, decided once") {
    std::map<std::string, int> listed;
    for (const char* k : kUsageSettingsSent) ++listed[k];
    for (const char* k : kUsageSettingsChangedOnly) ++listed[k];
    for (const char* k : kUsageSettingsNever) ++listed[k];
    for (const SettingMeta& m : AllSettings()) {
        INFO(std::string(m.key));
        CHECK(listed[m.key] == 1);  // a new setting: put it in one of the lists in crash_upload.h
        listed.erase(m.key);
    }
    for (const auto& [key, n] : listed) {
        INFO(key);
        CHECK(false);  // listed, but no such setting
    }
}

TEST_CASE("daily Log: settings from the allow-list only, normalised") {
    Config cfg;
    cfg.output_root           = "D:\\Clips\\secret";
    cfg.output_filename       = "{game} by Alice";
    cfg.capture_target_window = "Discord - Alice";
    cfg.ui_accent             = "#123456";
    cfg.ui_theme              = "user2";
    cfg.encode_preset         = "C:\\x";
    cfg.buffer_seconds        = 90;
    const JsonValue   s   = UsageSettings(SerializeConfigFlat(cfg));
    const std::string out = WriteJsonCompact(s);
    for (const auto& [key, v] : s.obj) {
        INFO(key);
        CHECK(v.str.find_first_of(":\\/ ") == std::string::npos);  // no path or typed text in a value
    }
    CHECK(out.find("Alice") == std::string::npos);
    CHECK(out.find("secret") == std::string::npos);
    CHECK(s.Find("output.root") == nullptr);
    CHECK(s.Find("diagnostics.sharing") == nullptr);
    CHECK(s.Find("ui.accent")->str == "custom");
    CHECK(s.Find("ui.theme")->str == "user");
    CHECK(s.Find("encode.preset")->str == "other");
    CHECK(s.Find("buffer.seconds")->num == 90);
    CHECK(s.Find("output.filename.changed")->b);
    CHECK_FALSE(s.Find("hotkeys.save_replay.changed")->b);
}

TEST_CASE("daily Log: each run's last line, summed across runs") {
    UsageCounters c;
    DayUsage      a("1-100");
    a.Begin(c, 100);
    c.armed = true;
    a.Sample(c, 160);  // not recording in the minute before
    c.ticks.ticks = 600;
    c.ticks.late  = 3;
    c.ticks.encode.counts[8] = 600;  // 2.25 ms
    c.saves      = 2;
    c.private_mb = 250;
    a.Sample(c, 220);
    const DayRun a1 = a.Totals(c, 220);
    CHECK(a1.up_seconds == 120);
    CHECK(a1.armed_seconds == 60);
    CHECK(a1.ticks == 600);
    CHECK(a1.peak_private_mb == 250);
    c.ticks.ticks = 1200;
    c.private_mb  = 100;
    a.Sample(c, 280);
    DayRun a2 = a.Totals(c, 280);
    a2.settings = JsonValue::MakeObject();
    a2.settings.Set("buffer.seconds", JsonValue::MakeNum(60));
    // The day after: counts from the new zero.
    DayUsage b("1-100");
    b.Begin(c, 300);
    c.ticks.ticks = 1300;
    b.Sample(c, 360);
    CHECK(b.Totals(c, 360).ticks == 100);
    CHECK(b.Totals(c, 360).saves == 0);
    // Asleep for 8 hours while armed (as since run 1's second sample): not 8 hours up or recording.
    b.Sample(c, 420);
    b.Sample(c, 420 + 8 * 3600);
    CHECK(b.Totals(c, 420 + 8 * 3600).up_seconds == 120 + DayUsage::kMaxSampleGap);
    CHECK(b.Totals(c, 420 + 8 * 3600).armed_seconds == 120 + DayUsage::kMaxSampleGap);
    b.Sample(c, 100);  // the clock went back
    CHECK(b.Totals(c, 100).up_seconds == 120 + DayUsage::kMaxSampleGap);

    // A second run (after a restart) and a line cut short by a crash.
    DayRun other;
    other.run           = "2-500";
    other.up_seconds    = 30;
    other.ticks         = 50;
    other.peak_private_mb = 300;
    other.monitors      = 2;
    other.settings      = JsonValue::MakeObject();
    other.settings.Set("buffer.seconds", JsonValue::MakeNum(120));
    const std::string text = WriteDayRun(a1) + "\n" + WriteDayRun(a2) + "\n" + WriteDayRun(other) + "\n" +
                             WriteDayRun(other).substr(0, 20);
    std::vector<DayRun> lines;
    std::istringstream  in(text);
    for (std::string line; std::getline(in, line);)
        if (auto r = ParseDayRun(line)) lines.push_back(std::move(*r));
    REQUIRE(lines.size() == 3);
    CHECK(lines[1].encode.counts[8] == 600);
    const DayRun total = SumDayRuns(lines);
    CHECK(total.run == "2");
    CHECK(total.ticks == 1250);  // run 1's last line, not its first plus its last
    CHECK(total.up_seconds == 210);
    CHECK(total.late == 3);
    CHECK(total.peak_private_mb == 300);
    CHECK(total.monitors == 2);
    CHECK(total.settings.Find("buffer.seconds")->num == 120);  // the last line written
    CHECK(SumDayRuns({}).run.empty());

    const std::string payload = BuildDailyLog(total, "20261110", {"ombra@1+x", "production", "id1"});
    CHECK(payload.find("\"timestamp\":1794355199,") != std::string::npos);  // 2026-11-10 23:59:59 UTC
    CHECK(payload.find("\"ombra.kind\":{\"value\":\"daily\"") != std::string::npos);
    CHECK(payload.find("\"ombra.runs\":{\"value\":2,\"type\":\"integer\"}") != std::string::npos);
    CHECK(payload.find("\"ombra.frames\":{\"value\":1250,\"type\":\"integer\"}") != std::string::npos);
    CHECK(payload.find("\"ombra.encode_p50_ms\":{\"value\":2.25,\"type\":\"double\"}") != std::string::npos);
    CHECK(payload.find("\"setting.buffer.seconds\":{\"value\":120,\"type\":\"double\"}") != std::string::npos);
    CHECK(payload.find("\"user.id\":{\"value\":\"id1\"") != std::string::npos);
    CHECK(payload.find("\"infer_ip\":\"never\"") != std::string::npos);
}

TEST_CASE("daily Log: a real day's payload carries every allowed setting") {
    DayRun d;
    d.run                     = "1";
    d.ticks                   = 216000;
    d.encode.counts[8]        = 200000;
    d.encode.counts[20]       = 16000;
    d.settings                = UsageSettings(SerializeConfigFlat(Config{}));
    const std::string payload = BuildDailyLog(d, "20261110", {"ombra@1+x", "test", "id1"});
    for (const char* k : kUsageSettingsSent) {
        INFO(k);
        CHECK(payload.find("\"setting." + std::string(k) + "\":") != std::string::npos);
    }
    CHECK(payload.find("\"setting.hotkeys.overlay.changed\":{\"value\":false,\"type\":\"boolean\"}") != std::string::npos);
    CHECK(payload.find("\"ombra.encode_p99_ms\":{\"value\":5.25,\"type\":\"double\"}") != std::string::npos);
}

TEST_CASE("daily Log: queued only at the usage stop, sent once the day is over") {
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring dir = crash::CrashesDir();
    DayRun r;
    r.run      = "1-1";
    r.ticks    = 10;
    r.settings = JsonValue::MakeObject();
    r.settings.Set("ui.memory_card", JsonValue::MakeBool(true));
    const std::string yesterday = [] {
        const std::time_t t = std::time(nullptr) - 86400;
        std::tm           tm{};
        gmtime_s(&tm, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        return std::string(buf);
    }();
    const std::wstring file = dir + L"\\usage-day-" + WideFromUtf8(yesterday) + L".txt";
    RecordDayRun(r, yesterday);
    CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);  // errors stop: not written

    crash::SetSharingEnabled(true, true);
    RecordDayRun(r, yesterday);
    r.ticks = 20;
    RecordDayRun(r, yesterday);
    REQUIRE(PendingUsage().size() == 1);
    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 1);
    const std::string& req = server.requests[0];
    CHECK(req.find("\"type\":\"log\",\"item_count\":1,") != std::string::npos);
    CHECK(req.find("\"ombra.frames\":{\"value\":20,") != std::string::npos);
    CHECK(req.find("\"setting.ui.memory_card\":{\"value\":true,\"type\":\"boolean\"}") != std::string::npos);
    CHECK(PendingUsage().empty());
    CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("a crash before setup waits for an answer, and only Send sends it") {
    StubServer server(200);
    UploadEnv  env(server.port());
    const std::wstring dir = crash::CrashesDir();
    const auto seed_ask = [&](const std::wstring& base) {
        SeedDump(base, false);
        std::string t = ReadText(dir + L"\\" + base + L".txt");
        t.replace(t.find("share: no"), 9, "share: ask");
        WriteText(dir + L"\\" + base + L".txt", t);
    };
    const auto c = ParseCrashText("ombra 1.2.3 (abc)\nkind: die\nthread: main\nshare: ask\nreason: x\n");
    REQUIRE(c.has_value());
    CHECK(c->ask);
    CHECK_FALSE(c->share);

    // Unanswered: never sent, even with setup done and sharing on.
    seed_ask(L"ombra-20261106-120000-30");
    CHECK(PendingDumps().empty());
    CHECK(AnsweredDumps().empty());
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.empty());
    // A reason line that happens to read "share: ask" doesn't make a dump an ask.
    WriteText(dir + L"\\ombra-20261106-120000-31.dmp", "MDMP");
    WriteText(dir + L"\\ombra-20261106-120000-31.txt", "ombra 1.2.3 (abc)\nkind: die\nthread: main\nshare: no\nreason: x\nshare: ask\n");
    CHECK(crash::UnansweredCrash() == L"ombra-20261106-120000-30");

    // Don't send: done, nothing goes.
    crash::AnswerCrash(L"ombra-20261106-120000-30", false);
    CHECK(crash::UnansweredCrash().empty());
    CHECK(ReadText(dir + L"\\ombra-20261106-120000-30.sent").find("declined") == 0);
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.empty());

    // Send, with sharing off and setup not finished: the answered one goes, the older ask doesn't.
    seed_ask(L"ombra-20261107-120000-32");
    seed_ask(L"ombra-20261108-120000-33");
    Sleep(20);  // newest by write time
    WriteText(dir + L"\\ombra-20261108-120000-33.dmp", "MDMP");
    CHECK(crash::UnansweredCrash() == L"ombra-20261108-120000-33");
    crash::SetSharingEnabled(false);
    DeleteFileW(SetupCompletedPath().c_str());
    crash::AnswerCrash(L"ombra-20261108-120000-33", true);
    CHECK(crash::UnansweredCrash().empty());
    CHECK(ReadText(dir + L"\\ombra-20261107-120000-32.sent").find("declined") == 0);
    REQUIRE(AnsweredDumps().size() == 1);
    CHECK(RunUploadChild() == 0);
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests[0].find("event.minidump") != std::string::npos);
    CHECK(Exists(dir + L"\\ombra-20261108-120000-33.sent"));
    CHECK(RunUploadChild() == 0);
    CHECK(server.requests.size() == 1);
}

TEST_CASE("send logs: a feedback item and the zip, recorded first, capped per day") {
    ZipWriter zw;
    zw.Add("logs/ombra-20261111.log", "line\n");
    zw.Add("system.txt", "Windows 11\n");
    zw.Finish();
    const std::vector<std::string> names = ZipEntryNames(zw.Bytes());
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "logs/ombra-20261111.log");
    CHECK(names[1] == "system.txt");
    CHECK(ZipEntryNames("not a zip").empty());
    CHECK(ZipEntryNames(zw.Bytes().substr(0, zw.Bytes().size() - 30)).empty());

    StubServer server(200);
    UploadEnv  env(server.port());
    crash::SetSharingEnabled(false);  // sending logs is its own ask, not the sharing level
    const std::wstring zip = Instance().data_dir + L"\\ombra-logs-test.zip";
    REQUIRE(zw.Write(zip));
    const std::string id = crash::NewReportId();
    REQUIRE(id.size() == 32);
    CHECK(RunSendLogs(zip, "nothex") == SendLogsResult::Unreadable);
    CHECK(RunSendLogs(zip + L".missing", id) == SendLogsResult::Unreadable);
    CHECK(server.requests.empty());

    CHECK(RunSendLogs(zip, id) == SendLogsResult::Sent);
    REQUIRE(server.requests.size() == 1);
    const std::string& req = server.requests[0];
    CHECK(req.find("{\"type\":\"feedback\",\"length\":") != std::string::npos);
    CHECK(req.find("\"content_type\":\"application/zip\",\"filename\":\"ombra-logs-test.zip\"}\n" + zw.Bytes() + "\n") !=
          std::string::npos);
    CHECK(req.find("server_name") == std::string::npos);

    const std::wstring wid(id.begin(), id.end());
    const std::string  record = ReadText(Instance().data_dir + L"\\sent_reports\\" + wid + L".txt");
    CHECK(record.find("PK") == std::string::npos);  // the manifest, not the zip
    CHECK(record.find("system.txt") != std::string::npos);
    const auto parsed = sent_records::Parse(wid + L".txt", record);
    CHECK(parsed.kind == sent_records::Kind::Logs);
    CHECK(parsed.status == sent_records::Status::Sent);
    CHECK(parsed.summary == "Logs you sent: 2 files, 1 KB");

    for (int i = 1; i < kMaxLogsSendsPerDay; ++i) CHECK(RunSendLogs(zip, crash::NewReportId()) == SendLogsResult::Sent);
    CHECK(RunSendLogs(zip, crash::NewReportId()) == SendLogsResult::TooMany);
    CHECK(server.requests.size() == static_cast<size_t>(kMaxLogsSendsPerDay));

    SetEnvironmentVariableW(L"OMBRA_TEST_SENTRY_DSN", nullptr);
    CHECK(RunSendLogs(zip, crash::NewReportId()) == SendLogsResult::NoDsn);
    DeleteFileW(zip.c_str());
}

TEST_CASE("a failed send logs post says so in its record") {
    StubServer server(500);
    UploadEnv  env(server.port());
    ZipWriter  zw;
    zw.Add("a.txt", "a");
    zw.Finish();
    const std::wstring zip = Instance().data_dir + L"\\ombra-logs-test.zip";
    REQUIRE(zw.Write(zip));
    const std::string id = crash::NewReportId();
    CHECK(RunSendLogs(zip, id) == SendLogsResult::Failed);
    const std::wstring wid(id.begin(), id.end());
    const auto parsed = sent_records::Parse(wid + L".txt", ReadText(Instance().data_dir + L"\\sent_reports\\" + wid + L".txt"));
    CHECK(parsed.status == sent_records::Status::Failed);
    CHECK(parsed.summary == "Logs you sent: 1 file, 1 KB");
    DeleteFileW(zip.c_str());
}

TEST_CASE("a sent record reads back as what it was, and whether it arrived") {
    using namespace sent_records;
    const std::string session =
        std::string(kSessionTitle) + "20261110 (UTC)\n\n{\"aggregates\":[{\"started\":\"2026-11-10T08:00:00Z\",\"abnormal\":1}],"
        "\"attrs\":{\"release\":\"ombra@1.2.3+abc\",\"environment\":\"production\"}}\n";
    Record r = Parse(L"session-20261110-1794300000.txt", session);
    CHECK(r.kind == Kind::Session);
    CHECK(r.status == Status::Unconfirmed);  // no result: the post was cut off
    CHECK(r.result.empty());
    CHECK(r.day == "2026-11-10");
    CHECK(r.summary == "Closed unexpectedly");

    r = Parse(L"session-20261110-1794300000.txt", session + kResultPrefix + "WinHTTP error 12007\n");
    CHECK(r.status == Status::Failed);
    CHECK(r.result == "WinHTTP error 12007");
    CHECK(Parse(L"x.txt", session + kResultPrefix + "HTTP 413\n").status == Status::Failed);

    r = Parse(L"usage-day-20261109.txt", std::string(kDailyTitle) + "\n\n{\"items\":[]}\n" + kResultPrefix + "HTTP 200\n");
    CHECK(r.kind == Kind::DailyUsage);
    CHECK(r.status == Status::Sent);
    CHECK(r.day == "2026-11-09");

    // The overlay's crash, the address left out; a file this version didn't write.
    r = Parse(L"0123.txt", std::string(kCrashTitle) + "0123\n\nMinidump: x.dmp\n\nEvent:\n"
                           "{\"tags\":{\"process\":\"ombra-ui\"},\"extra\":{\"reason\":\"access violation (0xC0000005) at "
                           "00007FF7C8835FE0, writing 0000000000000000\"}}\n");
    CHECK(r.summary == "The overlay crashed: access violation (0xC0000005)");
    r = Parse(L"notes.txt", "something else\n");
    CHECK(r.kind == Kind::Unknown);
    CHECK(r.summary == "something else");
}

}  // TEST_SUITE

}  // namespace ombra::test
