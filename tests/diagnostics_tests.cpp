// Diagnostics export (diagnostics.h): scrubbing rules, the zip writer read back by Windows'
// own tar.exe (bsdtar, an independent unzipper), and the bundle's day filter. Writes only under
// the harness's OMBRA_HOME (test_env.cpp).

#include <doctest/doctest.h>

#include "support/process.h"
#include "support/test_env.h"

#include "ombra/diagnostics.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/zip_writer.h"

#include <windows.h>

#include <fstream>
#include <string>

namespace ombra::test {

namespace {

ScrubContext Names() {
    ScrubContext ctx;
    ctx.names = {{"Ana", "<user>"}, {"ana.silva", "<user>"}, {"DESKTOP-7Q2", "<computer>"}, {"Al", "<user>"}};
    return ctx;
}

void ExpectScrub(const std::string& in, const ScrubContext& ctx, const std::string& want) {
    INFO("in:   ", in);
    CHECK(ScrubText(in, ctx) == want);
}

std::wstring Tar() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\tar.exe";
}

void WriteFile(const std::wstring& path, const std::string& bytes) {
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::ofstream f(path, std::ios::binary);
    f << bytes;
}

}  // namespace

TEST_SUITE("diagnostics") {

TEST_CASE("scrub: a «» span is redacted to the line's last », unclosed or multi-line ones are left alone") {
    const ScrubContext none;
    ExpectScrub("capture target window  Code.exe  «secret.txt - Visual Studio Code»", none, "capture target window  Code.exe  «redacted»");
    // A title holding » itself (a breadcrumb) is redacted whole; text between two spans goes with it.
    ExpectScrub("window «Forum » Private thread - Chrome» done", none, "window «redacted» done");
    ExpectScrub("a «x» b «y»", none, "a «redacted»");
    ExpectScrub("one «x»\ntwo «y» end", none, "one «redacted»\ntwo «redacted» end");
    ExpectScrub("open « no close\nnext » line", none, "open « no close\nnext » line");
}

TEST_CASE("scrub: the engine's wide log calls write « » as UTF-8") {
    Log::InfoW(L"diag-test window  «%s»", L"Título");
    const std::wstring dir = Instance().data_dir + L"\\logs\\";
    std::string        all;
    WIN32_FIND_DATAW   fd{};
    const HANDLE       h = FindFirstFileW((dir + L"*.log").c_str(), &fd);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    do {
        std::ifstream f(dir + fd.cFileName, std::ios::binary);
        all.append(std::istreambuf_iterator<char>(f), {});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    CHECK(all.find("diag-test window  \xC2\xABT\xC3\xADtulo\xC2\xBB") != std::string::npos);
}

TEST_CASE("scrub: an absolute path keeps only its last part") {
    const ScrubContext none;
    ExpectScrub(R"(save: "C:\Users\X\Videos\Ombra\Valorant 2026-11-06 12-00-00.mp4" written)", none, R"(save: "<path>\Valorant 2026-11-06 12-00-00.mp4" written)");
    // Unquoted: ends at two spaces (log columns) or at the next key=value pair.
    ExpectScrub(R"(output           D:\Clips\Game (1)\a.mp4  done)", none, R"(output           <path>\a.mp4  done)");
    ExpectScrub(R"(dir=C:\Program Files\Ombra\ombra.exe pid=4)", none, R"(dir=<path>\ombra.exe pid=4)");
    ExpectScrub(R"(moved C:\a\one.mp4 C:\b\two.mp4)", none, R"(moved <path>\one.mp4 <path>\two.mp4)");
    ExpectScrub(R"(written to C:\x\y\clip.mp4.)", none, R"(written to <path>\clip.mp4.)");
    ExpectScrub(R"(share \\nas\clips\a.mp4)", none, R"(share <path>\a.mp4)");
    ExpectScrub(R"(saved C:\Users\X\Ana's Clips\x.mp4)", none, R"(saved <path>\x.mp4)");
    ExpectScrub("C:/Users/X/file.txt", none, R"(<path>\file.txt)");
    // Not paths: a word ending in a letter and colon, a monitor device name, a pipe name prefix.
    ExpectScrub(R"(ratio:\n x=1:\2)", none, R"(ratio:\n x=1:\2)");
    ExpectScrub(R"(monitor \\.\DISPLAY1)", none, R"(monitor \\.\DISPLAY1)");
}

TEST_CASE("scrub: JSON-escaped paths stay valid JSON") {
    const ScrubContext none;
    ExpectScrub(R"({"output.folder": "C:\\Users\\X\\Videos\\Ombra"})", none, R"({"output.folder": "<path>\\Ombra"})");
    ExpectScrub(R"(settings: output.folder="E:\\Clips" buffer.seconds=120)", none, R"(settings: output.folder="<path>\\Clips" buffer.seconds=120)");
    ExpectScrub(R"({"output.folder": "\\\\nas\\share\\Clips"})", none, R"({"output.folder": "<path>\\Clips"})");
    // An environment-relative folder (the default output.folder) names nothing private.
    ExpectScrub(R"({"output.folder": "%USERPROFILE%\\Videos\\Ombra"})", none,
                R"({"output.folder": "%USERPROFILE%\\Videos\\Ombra"})");
    ExpectScrub(R"(dir %USERPROFILE%\Videos\Ombra)", none, R"(dir %USERPROFILE%\Videos\Ombra)");
}

TEST_CASE("scrub: user and computer names, case-insensitive, longest first, none under 3 chars") {
    const ScrubContext ctx = Names();
    ExpectScrub("user ANA.SILVA on desktop-7q2", ctx, "user <user> on <computer>");
    ExpectScrub("Banana", ctx, "B<user>na");  // substring match is the price of not missing one
    ExpectScrub("Also", ctx, "Also");          // "Al" is under 3 characters: ignored
    // A replacement is never rescanned: a user named "user" doesn't become "<<user>>".
    ScrubContext self;
    self.names = {{"user", "<user>"}, {"user", "<user>"}};
    ExpectScrub("hello user", self, "hello <user>");
    // The last part of a path still loses the name: C:\Users\Ana -> <path>\Ana -> <path>\<user>.
    ExpectScrub(R"(profile C:\Users\Ana)", ctx, R"(profile <path>\<user>)");
}

TEST_CASE("scrub: the machine context carries this account's and computer's names") {
    const ScrubContext ctx = MachineScrubContext();
    REQUIRE(ctx.names.size() >= 2);
    char  computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    REQUIRE(GetComputerNameA(computer, &n));
    const std::string name(computer, n);
    if (name.size() >= 3) ExpectScrub("on " + name + ".", ctx, "on <computer>.");
}

TEST_CASE("zip: CRC-32 matches the standard check value") {
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(Crc32("", 0) == 0u);
}

TEST_CASE("zip: an archive tar.exe lists and extracts byte for byte") {
    ZipWriter zip;
    const std::string big(300000, 'z');
    std::string       binary;
    for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
    zip.Add("README.txt", "hello\r\n");
    zip.Add("logs/ombra-20260101.log", big);
    zip.Add("crashes/x.dmp", binary);
    zip.Add("empty.txt", "");
    zip.Finish();
    const std::wstring path = TestOutputPath(L"diag\\zip_roundtrip.zip");
    CreateDirectoryW(TestOutputPath(L"diag").c_str(), nullptr);
    REQUIRE(zip.Write(path));

    const ProcessResult list = RunProcess(Quote(Tar()) + L" -tf " + Quote(path));
    REQUIRE(list.launched);
    REQUIRE(list.exit_code == 0);
    CHECK(list.out.find("README.txt") != std::string::npos);
    CHECK(list.out.find("logs/ombra-20260101.log") != std::string::npos);
    CHECK(list.out.find("crashes/x.dmp") != std::string::npos);
    CHECK(list.out.find("empty.txt") != std::string::npos);

    const ProcessResult log = RunProcess(Quote(Tar()) + L" -xOf " + Quote(path) + L" logs/ombra-20260101.log");
    CHECK(log.exit_code == 0);
    CHECK(log.out == big);
    const ProcessResult dmp = RunProcess(Quote(Tar()) + L" -xOf " + Quote(path) + L" crashes/x.dmp");
    CHECK(dmp.exit_code == 0);
    CHECK(dmp.out == binary);
}

TEST_CASE("bundle: the last N days of logs and telemetry, every crash report, all scrubbed") {
    const std::wstring data = Instance().data_dir;
    REQUIRE(!data.empty());
    // Dates far from today so the harness's own live log (today's date) is never in the window.
    WriteFile(data + L"\\logs\\ombra-20250301.log", "[INFO ] save: \"C:\\Users\\X\\Videos\\a.mp4\" «Title»\n");
    WriteFile(data + L"\\logs\\ombra-ui-20250302.log", "ui log\n");
    WriteFile(data + L"\\logs\\ombra-20250226.log", "too old\n");
    WriteFile(data + L"\\logs\\ombra-20250303.log", "after today\n");
    WriteFile(data + L"\\telemetry\\samples-20250302.csv", "t,cpu\n1,2\n");
    WriteFile(data + L"\\telemetry\\samples-20250302-1.csv", "t,cpu\n3,4\n");  // a second file that day
    WriteFile(data + L"\\telemetry\\events-20250301.csv", "t,event\n");
    WriteFile(data + L"\\crashes\\ombra-20240101-000000.txt", "FATAL at D:\\src\\ombra\\x.cpp\n");
    WriteFile(data + L"\\crashes\\ombra-20240101-000000.dmp", std::string("MDMP\0\x01", 6));

    BundleOptions opt;
    opt.days  = 3;
    opt.today = "20250302";
    const std::wstring path = TestOutputPath(L"diag\\bundle.zip");
    CreateDirectoryW(TestOutputPath(L"diag").c_str(), nullptr);
    const BundleResult r = WriteDiagnosticsBundle(path, opt);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.bytes > 0);

    const ProcessResult list = RunProcess(Quote(Tar()) + L" -tf " + Quote(path));
    REQUIRE(list.exit_code == 0);
    CHECK(list.out.find("README.txt") != std::string::npos);
    CHECK(list.out.find("system.txt") != std::string::npos);
    CHECK(list.out.find("logs/ombra-20250301.log") != std::string::npos);
    CHECK(list.out.find("logs/ombra-ui-20250302.log") != std::string::npos);
    CHECK(list.out.find("logs/ombra-20250226.log") == std::string::npos);
    CHECK(list.out.find("logs/ombra-20250303.log") == std::string::npos);
    CHECK(list.out.find("telemetry/samples-20250302.csv") != std::string::npos);
    CHECK(list.out.find("telemetry/samples-20250302-1.csv") != std::string::npos);
    CHECK(list.out.find("telemetry/events-20250301.csv") != std::string::npos);
    CHECK(list.out.find("crashes/ombra-20240101-000000.txt") != std::string::npos);
    CHECK(list.out.find("crashes/ombra-20240101-000000.dmp") != std::string::npos);

    const ProcessResult log = RunProcess(Quote(Tar()) + L" -xOf " + Quote(path) + L" logs/ombra-20250301.log");
    CHECK(log.out == "[INFO ] save: \"<path>\\a.mp4\" «redacted»\n");
    const ProcessResult crash =
        RunProcess(Quote(Tar()) + L" -xOf " + Quote(path) + L" crashes/ombra-20240101-000000.txt");
    CHECK(crash.out == "FATAL at <path>\\x.cpp\n");
    const ProcessResult dmp = RunProcess(Quote(Tar()) + L" -xOf " + Quote(path) + L" crashes/ombra-20240101-000000.dmp");
    CHECK(dmp.out == std::string("MDMP\0\x01", 6));  // dumps are copied as is
    const ProcessResult sys = RunProcess(Quote(Tar()) + L" -xOf " + Quote(path) + L" system.txt");
    CHECK(sys.out.find("Windows build") != std::string::npos);
}

TEST_CASE("bundle: an isolated instance's default path is a new .zip in its own folder, not Downloads") {
    const std::wstring path = DefaultBundlePath();
    REQUIRE(!path.empty());
    CHECK(path.rfind(TestRoot(), 0) == 0);
    CHECK(path.find(L"\\Ombra logs ") != std::wstring::npos);
    CHECK(path.size() > 4);
    CHECK(path.substr(path.size() - 4) == L".zip");
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("bundle: a zip path whose folder is missing fails with an error, not a crash") {
    const BundleResult r = WriteDiagnosticsBundle(TestOutputPath(L"diag\\no\\such\\folder\\b.zip"));
    CHECK_FALSE(r.ok);
    CHECK(!r.error.empty());
}

TEST_CASE("system info: the machine part leaves out Ombra's own version") {
    const std::string machine = MachineInfo();
    CHECK(machine.rfind("Windows build ", 0) == 0);
    CHECK(machine.find("Ombra ") == std::string::npos);
    const std::string system = SystemInfo();
    CHECK(system.rfind("Ombra ", 0) == 0);
    CHECK(system.substr(system.find('\n') + 1) == machine);
}

TEST_CASE("bug report link: version, Windows and real GPUs filled in, encoded, reference optional") {
    const std::string machine =
        "Windows build 26100.1\n"
        "GPU 0: AMD Radeon(TM) 780M  vendor 0x1002  driver 32.0.1  512 MB\n"
        "GPU 1: NVIDIA GeForce RTX 5060 Laptop GPU  vendor 0x10DE  driver 32.0.2  8000 MB\n"
        "GPU 2: Microsoft Basic Render Driver  vendor 0x1414  driver ?  0 MB\n";
    const std::string url = BugReportUrl(machine, "");
    CHECK(url.rfind("https://github.com/Zyrecx/ombra/issues/new?template=bug_report.yml&version=", 0) == 0);
    CHECK(url.find("&gpu=AMD%20Radeon%28TM%29%20780M%20%2B%20NVIDIA%20GeForce%20RTX%205060%20Laptop%20GPU&") !=
          std::string::npos);
    CHECK(url.find("Microsoft") == std::string::npos);
    CHECK(url.find("&windows=Windows%2011%20%28build%2026100.1%29") != std::string::npos);
    CHECK(url.find("&logs=") == std::string::npos);
    CHECK(BugReportUrl(machine, "0123abcd").find("&logs=Sent%20from%20Settings%2C%20reference%200123abcd") !=
          std::string::npos);
    CHECK(BugReportUrl("", "").find("&gpu=") == std::string::npos);
}

}  // TEST_SUITE

}  // namespace ombra::test
