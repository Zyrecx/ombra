// update.h's pure parts: versions, the release feed, install kinds, the apply gate and the child's
// report. No network: the feed is a fixed string shaped like GitHub's releases/latest response.

#include <doctest/doctest.h>

#include "ombra/update.h"
#include "support/test_env.h"

#include <windows.h>

#include <fstream>
#include <filesystem>
#include <string>

namespace ombra::test {

namespace {

int Cmp(const char* a, const char* b) {
    const auto va = ParseSemVer(a), vb = ParseSemVer(b);
    REQUIRE(va);
    REQUIRE(vb);
    return CompareSemVer(*va, *vb);
}

constexpr const char* kFeed = R"({
  "tag_name": "v0.2.0",
  "html_url": "https://github.com/Zyrecx/ombra/releases/tag/v0.2.0",
  "draft": false,
  "prerelease": false,
  "assets": [
    {"name": "Ombra-Setup.exe", "size": 5242880,
     "digest": "sha256:F47FD8DFB1A1DFA938FC0B573DA44088034296A2769943D84FCFB375CF3B32CC",
     "browser_download_url": "https://github.com/Zyrecx/ombra/releases/download/v0.2.0/Ombra-Setup.exe"},
    {"name": "Ombra-Portable.zip", "size": 100, "digest": null,
     "browser_download_url": "https://github.com/Zyrecx/ombra/releases/download/v0.2.0/Ombra-Portable.zip"},
    {"name": "evil.exe", "browser_download_url": "http://example.com/evil.exe"}
  ]
})";

}  // namespace

TEST_CASE("update: semver parse and order") {
    CHECK(ParseSemVer("0.1.0"));
    CHECK(ParseSemVer("v1.2.3"));
    CHECK(ParseSemVer("1.2.3-rc.1+build.5"));
    CHECK_FALSE(ParseSemVer("1.2"));
    CHECK_FALSE(ParseSemVer("1.2.3.4"));
    CHECK_FALSE(ParseSemVer("1.2.x"));
    CHECK_FALSE(ParseSemVer("1.2.3-"));
    CHECK_FALSE(ParseSemVer(""));

    CHECK(Cmp("0.1.0", "0.1.0") == 0);
    CHECK(Cmp("v0.1.0", "0.1.0") == 0);
    CHECK(Cmp("0.1.0", "0.2.0") < 0);
    CHECK(Cmp("0.10.0", "0.9.0") > 0);
    CHECK(Cmp("1.0.0", "0.99.99") > 0);
    CHECK(Cmp("1.0.0-rc.1", "1.0.0") < 0);
    CHECK(Cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    CHECK(Cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
    CHECK(Cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0);
    CHECK(Cmp("1.0.0+a", "1.0.0+b") == 0);
}

TEST_CASE("update: release feed") {
    std::string error;
    const auto r = ParseReleaseJson(kFeed, &error);
    REQUIRE_MESSAGE(r, error);
    CHECK(r->version == "0.2.0");
    CHECK(r->tag == "v0.2.0");
    CHECK(r->page_url == "https://github.com/Zyrecx/ombra/releases/tag/v0.2.0");
    CHECK_FALSE(r->prerelease);
    REQUIRE(r->assets.size() == 2);  // the http:// asset is dropped
    const ReleaseAsset* setup = r->FindAsset(kInstallerAssetName);
    REQUIRE(setup);
    CHECK(setup->sha256 == "f47fd8dfb1a1dfa938fc0b573da44088034296a2769943d84fcfb375cf3b32cc");
    CHECK(setup->size == 5242880);
    CHECK(r->FindAsset("Ombra-Portable.zip")->sha256.empty());

    CHECK_FALSE(ParseReleaseJson("not json", &error));
    CHECK_FALSE(ParseReleaseJson(R"({"message":"Not Found"})", &error));
    CHECK(error.find("version") != std::string::npos);
    CHECK_FALSE(ParseReleaseJson(R"({"tag_name":"v1.0.0","draft":true})", &error));
    CHECK_FALSE(ParseReleaseJson(R"({"tag_name":"nightly"})", &error));
}

TEST_CASE("update: install kind") {
    CHECK(DetectInstallKind(L"C:\\Users\\a\\AppData\\Local\\Programs\\Ombra", true, true) == InstallKind::Installer);
    // An all-users install this user can't write to updates like a portable copy: it only notifies.
    CHECK(DetectInstallKind(L"C:\\Program Files\\Ombra", true, false) == InstallKind::Portable);
    CHECK(DetectInstallKind(L"D:\\Tools\\Ombra", false, true) == InstallKind::Portable);
    CHECK(DetectInstallKind(L"C:\\Users\\a\\Scoop\\apps\\ombra\\0.1.0", false, true) == InstallKind::Scoop);
    CHECK(DetectInstallKind(L"C:\\Users\\a\\scoop\\apps\\ombra\\current", true, true) == InstallKind::Scoop);
}

TEST_CASE("update: Scoop stable path") {
    CHECK(ScoopStablePath(L"C:\\Users\\a\\Scoop\\apps\\ombra\\0.1.0\\ombra.exe") ==
          L"C:\\Users\\a\\Scoop\\apps\\ombra\\current\\ombra.exe");
    CHECK(ScoopStablePath(L"C:\\Users\\a\\scoop\\apps\\ombra\\current\\ombra.exe") ==
          L"C:\\Users\\a\\scoop\\apps\\ombra\\current\\ombra.exe");
    CHECK(ScoopStablePath(L"C:\\Users\\a\\AppData\\Local\\Programs\\Ombra\\ombra.exe") ==
          L"C:\\Users\\a\\AppData\\Local\\Programs\\Ombra\\ombra.exe");
    CHECK(ScoopStablePath(L"C:\\scoop\\apps\\ombra") == L"C:\\scoop\\apps\\ombra");
}

TEST_CASE("update: apply gate") {
    ApplyGate g;
    g.idle_ms = kApplyIdleMs;
    CHECK(SafeToApplyNow(g));
    ApplyGate t = g;
    t.idle_ms = kApplyIdleMs - 1;
    CHECK_FALSE(SafeToApplyNow(t));
    t = g, t.busy_fullscreen = true;
    CHECK_FALSE(SafeToApplyNow(t));
    t = g, t.game_running = true;
    CHECK_FALSE(SafeToApplyNow(t));
    t = g, t.save_in_flight = true;
    CHECK_FALSE(SafeToApplyNow(t));
    t = g, t.other_ombra_processes = true;
    CHECK_FALSE(SafeToApplyNow(t));
}

TEST_CASE("update: child report") {
    const auto r = ParseUpdateCheckOutput(
        "noise from somewhere\r\nok=1\r\nlatest=0.2.0\npage=https://x/y\nnewer=1\n"
        "installer=C:\\Users\\a\\AppData\\Local\\Ombra\\updates\\Ombra-Setup-0.2.0.exe\ninstaller_sha256=ab\n");
    CHECK(r.ok);
    CHECK(r.newer);
    CHECK(r.latest == "0.2.0");
    CHECK(r.page_url == "https://x/y");
    CHECK(r.installer_path == "C:\\Users\\a\\AppData\\Local\\Ombra\\updates\\Ombra-Setup-0.2.0.exe");
    CHECK(r.installer_sha256 == "ab");

    const auto failed = ParseUpdateCheckOutput("ok=0\nerror=feed: HTTP 404\n");
    CHECK_FALSE(failed.ok);
    CHECK(failed.error == "feed: HTTP 404");
}

TEST_CASE("update: file hash") {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"ombra-tests\\update-hash.bin";
    CreateDirectoryW((std::wstring(tmp) + L"ombra-tests").c_str(), nullptr);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "abc";
    }
    CHECK(Sha256File(path) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    DeleteFileW(path.c_str());
    CHECK(Sha256File(path).empty());
}

TEST_CASE("update: startup removes applied installers but preserves newer and busy downloads") {
    const std::wstring dir = TestOutputPath(L"update-cleanup");
    REQUIRE(CreateDirectoryW(dir.c_str(), nullptr));
    const auto path = [&](const wchar_t* name) { return dir + L"\\" + name; };
    const wchar_t* names[] = {L"Ombra-Setup-0.1.0.exe", L"Ombra-Setup-0.2.0.exe",
        L"Ombra-Setup-0.3.0.exe", L"Ombra-Setup-0.1.1.exe", L"Ombra-Setup-unknown.exe",
        L"Ombra-Setup-0.1.0.exe.part", L"other.exe", L"state.json"};
    for (const auto* name : names) {
        std::ofstream out(path(name), std::ios::binary);
        REQUIRE(out.good());
        out << "download";
    }
    REQUIRE(CreateDirectoryW(path(L"Ombra-Setup-0.0.1.exe").c_str(), nullptr));
    const HANDLE busy = CreateFileW(path(names[3]).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(busy != INVALID_HANDLE_VALUE);
    CleanupStaleInstallers(dir, "invalid");
    CHECK(std::filesystem::exists(path(names[0])));
    CleanupStaleInstallers(dir, "0.2.0");
    CHECK_FALSE(std::filesystem::exists(path(names[0])));
    CHECK_FALSE(std::filesystem::exists(path(names[1])));
    for (size_t i = 2; i < std::size(names); ++i) CHECK(std::filesystem::exists(path(names[i])));
    CHECK(std::filesystem::is_directory(path(L"Ombra-Setup-0.0.1.exe")));
    CloseHandle(busy);
    CleanupStaleInstallers(dir, "0.2.0");
    CHECK_FALSE(std::filesystem::exists(path(names[3])));
    CHECK(std::filesystem::exists(path(names[2])));  // ready to hand off, never stale
}

}  // namespace ombra::test
