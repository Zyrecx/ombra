// Logging (log.h): cross-process-safe appends, engine-only pruning across both name prefixes,
// repeat control, levels, and Telemetry::Event's log line. Writes only under the harness's
// OMBRA_HOME (test_env.cpp).

#include <doctest/doctest.h>

#include "support/test_env.h"

#include "ombra/log.h"
#include "ombra/telemetry.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace ombra::test {

namespace {

std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

size_t Count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) ++n;
    return n;
}

void WriteBytes(const std::wstring& path, size_t n) {
    std::ofstream f(path, std::ios::binary);
    f << std::string(n, 'x');
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Marker(const char* what) {
    return std::string(what) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(GetCurrentThreadId());
}

}  // namespace

TEST_CASE("log: two append handles on one file never tear a line") {
    const std::wstring path = TestOutputPath(L"log-append\\shared.log");
    CreateDirectoryW(TestOutputPath(L"log-append").c_str(), nullptr);
    DeleteFileW(path.c_str());
    constexpr int kLines = 2000;
    auto writer = [&](char who) {
        // Its own handle per thread, as two processes would have.
        HANDLE h = log_detail::OpenAppend(path);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        for (int i = 0; i < kLines; ++i) {
            // Varying lengths so a torn write would misalign the next line.
            const std::string line = std::string("<") + who + std::string(1 + i % 97, who) + ">\n";
            log_detail::AppendLine(h, line);
        }
        CloseHandle(h);
    };
    std::thread a(writer, 'a'), b(writer, 'b');
    a.join();
    b.join();

    std::istringstream in(ReadAll(path));
    std::string line;
    int counts[2] = {0, 0};
    while (std::getline(in, line)) {
        REQUIRE(line.size() >= 3);
        const char who = line[1];
        REQUIRE((who == 'a' || who == 'b'));
        CHECK(line.front() == '<');
        CHECK(line.back() == '>');
        CHECK(line.find_first_not_of(who, 1) == line.size() - 1);
        ++counts[who - 'a'];
    }
    CHECK(counts[0] == kLines);
    CHECK(counts[1] == kLines);
}

TEST_CASE("log: pruning goes oldest-first by date across prefixes and keeps today's file") {
    const std::wstring dir = TestOutputPath(L"log-prune");
    CreateDirectoryW(dir.c_str(), nullptr);
    WriteBytes(dir + L"\\ombra-20261002.log", 1000);
    WriteBytes(dir + L"\\umbra-20261003.log", 1000);  // before the rename, still pruned
    WriteBytes(dir + L"\\ombra-20261004.log", 1000);
    WriteBytes(dir + L"\\ombra-20260101.log", 5000);  // the oldest, but it is "today" here
    log_detail::PruneDir(dir, L"ombra-20260101.log", 6500);

    CHECK_FALSE(Exists(dir + L"\\ombra-20261002.log"));
    CHECK_FALSE(Exists(dir + L"\\umbra-20261003.log"));
    CHECK(Exists(dir + L"\\ombra-20261004.log"));
    CHECK(Exists(dir + L"\\ombra-20260101.log"));
}

TEST_CASE("log: lines carry the process tag, pid and thread name") {
    const std::wstring path = Log::Init();
    REQUIRE_FALSE(path.empty());
    const std::string marker = Marker("tagged");
    std::thread([&] {
        Log::SetThreadName("log-test");
        Log::Info("%s", marker.c_str());
    }).join();
    const std::string expected = "[E " + std::to_string(GetCurrentProcessId()) + " log-test] " + marker;
    CHECK(ReadAll(path).find(expected) != std::string::npos);
}

TEST_CASE("log: repeat control collapses a loop, not distinct lines from one format") {
    const std::wstring path = Log::Init();
    REQUIRE_FALSE(path.empty());
    log_detail::ResetRepeatState();
    const std::string loop = Marker("loop");
    for (int i = 0; i < 20; ++i) Log::Warn("%s attempt %d failed", loop.c_str(), i);
    const std::string stage = Marker("stage");
    const char* names[] = {"one", "two", "three", "four", "five", "six", "seven", "eight"};
    for (const char* n : names) Log::Info("%s %s", stage.c_str(), n);

    std::string text = ReadAll(path);
    CHECK(Count(text, loop + " attempt") == static_cast<size_t>(log_detail::kBurst));
    for (const char* n : names) CHECK(text.find(stage + " " + n) != std::string::npos);

    Log::Shutdown();  // reports what it held back
    text = ReadAll(path);
    CHECK(text.find("15 more suppressed, last: " + loop + " attempt 19 failed") != std::string::npos);
    Log::Init();
}

TEST_CASE("log: DEBUG lines appear only at log.level debug") {
    const std::wstring path = Log::Init();
    REQUIRE_FALSE(path.empty());
    const LogLevel before = Log::Level();
    const std::string hidden = Marker("debug-hidden"), shown = Marker("debug-shown");
    Log::SetLevel(ParseLogLevel("info"));
    Log::Debug("%s", hidden.c_str());
    Log::SetLevel(ParseLogLevel("debug"));
    Log::Debug("%s", shown.c_str());
    Log::SetLevel(before);
    const std::string text = ReadAll(path);
    CHECK(text.find(hidden) == std::string::npos);
    CHECK(text.find("[DEBUG] [E ") != std::string::npos);
    CHECK(text.find(shown) != std::string::npos);
}

TEST_CASE("log: a telemetry event is a log line even with telemetry off") {
    const std::wstring path = Log::Init();
    REQUIRE_FALSE(path.empty());
    const bool before = Telemetry::Enabled();
    Telemetry::SetEnabled(false);
    const std::string marker = Marker("evt");
    Telemetry::Event("log_test", "marker=%s", marker.c_str());
    Telemetry::SetEnabled(before);
    CHECK(ReadAll(path).find("event log_test marker=" + marker) != std::string::npos);
}

}  // namespace ombra::test
