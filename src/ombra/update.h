// Updates: is there a newer release, fetch it, and install it when nobody is using the PC.
//
// The network side runs in a short-lived child, `ombra.exe update-check [--download]`, so WinHTTP,
// schannel and the certificate stack never stay loaded in the resident engine (the AMF probe's
// pattern, amf_probe.h). The child reads the release feed (GitHub's releases/latest JSON), and with
// --download streams the installer to data_dir\updates\ and checks it against the SHA-256 GitHub
// lists for the asset. It reports key=value lines on stdout; the engine's Updater parses them.
//
// What an update does depends on how this copy was installed (DetectInstallKind):
//   Installer  setup.exe put it here (its uninstaller sits beside ombra.exe) and the folder is
//              writable without elevation: download, then run the new setup silently. winget
//              installs run the same setup, so they land here too, and winget sees the new version
//              in Apps & features.
//   Scoop      under scoop\apps\: Scoop owns the versioned folders, so only say a version exists.
//   Portable   anything else (or an installer copy under a folder that needs elevation): only say.
//
// When an installer update applies without being asked (updates.auto_install): restarting the
// engine drops the replay buffer, so "no game running" isn't enough. SafeToApplyNow also wants the
// user away from the PC for a while, nothing fullscreen, no save in flight, and no other Ombra
// process (the overlay, a trim export, a toast). Until then the verified installer waits on disk.
//
// No update traffic from an isolated instance (OMBRA_HOME: tests, --drive) or before the first-run
// setup has finished.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

// ---- Pure parts (tests/update_tests.cpp) ----

struct SemVer {
    int         major = 0, minor = 0, patch = 0;
    std::string pre;  // "beta.1" in 1.2.0-beta.1; empty for a release
};
// "1.2.3", "v1.2.3", "1.2.3-rc.1". Build metadata (+...) is ignored. nullopt for anything else.
std::optional<SemVer> ParseSemVer(const std::string& text);
// <0, 0, >0. A prerelease sorts before its release; prerelease tags compare as strings per
// dot-separated part, numeric parts numerically (semver 2.0 section 11).
int CompareSemVer(const SemVer& a, const SemVer& b);

struct ReleaseAsset {
    std::string name;
    std::string url;     // browser_download_url
    std::string sha256;  // lowercase hex from GitHub's "digest", empty if it lists none
    uint64_t    size = 0;
};
struct ReleaseInfo {
    std::string               tag;       // "v0.2.0"
    std::string               version;   // "0.2.0"
    std::string               page_url;  // html_url, for "Release notes"
    bool                      prerelease = false;
    std::vector<ReleaseAsset> assets;
    const ReleaseAsset* FindAsset(const std::string& name) const;
};
// GitHub's release object. nullopt (and *error) when it isn't one, or its tag isn't a version.
std::optional<ReleaseInfo> ParseReleaseJson(const std::string& text, std::string* error);

enum class InstallKind { Installer, Scoop, Portable };
const char* InstallKindName(InstallKind kind);
// `exe_dir` without a trailing backslash. Looks at the path and for unins000.exe beside the exe;
// `dir_writable` says whether this user can replace files there (an all-users install can't).
InstallKind DetectInstallKind(const std::wstring& exe_dir, bool has_uninstaller, bool dir_writable);
// Scoop runs an app from scoop\apps\<app>\<version>\ and repoints the `current` junction on every
// update, so a path that must outlive this version (the autostart Run value) goes through
// `current`. Any other path comes back unchanged.
std::wstring ScoopStablePath(const std::wstring& exe_path);

// What SafeToApplyNow looks at, gathered by the engine (GatherApplyGate).
struct ApplyGate {
    uint32_t idle_ms               = 0;      // since the last keyboard or mouse input
    bool     busy_fullscreen       = false;  // SHQueryUserNotificationState: fullscreen, D3D, presenting
    bool     game_running          = false;  // armed for a game from the Games list
    bool     save_in_flight        = false;
    bool     other_ombra_processes = false;  // ombra-ui (overlay, toast) or another ombra.exe (export)
};
inline constexpr uint32_t kApplyIdleMs = 10 * 60 * 1000;
bool SafeToApplyNow(const ApplyGate& gate);

// The child's report.
struct UpdateCheckResult {
    bool        ok = false;      // the feed was read and parsed
    std::string error;           // why not, or why a download failed
    std::string latest;          // newest release's version
    std::string page_url;
    bool        newer = false;   // latest > this build
    std::string installer_path;  // with --download: the verified installer on disk
    std::string installer_sha256;
};
UpdateCheckResult ParseUpdateCheckOutput(const std::string& text);

// The asset the installer path downloads, uploaded unversioned by every release so
// releases/latest/download/<name> keeps working (the README's download button).
inline constexpr const char* kInstallerAssetName = "Ombra-Setup.exe";
// Override with OMBRA_UPDATE_FEED (a manual test against another repo's release).
inline constexpr const wchar_t* kDefaultUpdateFeed =
    L"https://api.github.com/repos/Zyrecx/ombra/releases/latest";

// ---- Process side ----

// Child (`ombra.exe update-check [--download]`): never touches the log, the config or the lock.
int RunUpdateCheckChild(int argc, wchar_t** argv);

// Lowercase hex SHA-256 of a file, empty on any read error.
std::string Sha256File(const std::wstring& path);

// Startup only, before any download or handoff: remove recognized installers no newer than
// this build. Busy files (including a setup still running) and reparse points are left alone.
void CleanupStaleInstallers(const std::wstring& dir, const std::string& current_version);

// The resident engine's scheduler: a check a few minutes after start and then every 12 hours,
// a download when this install kind can apply one, and an apply when SafeToApplyNow says so.
class Updater {
public:
    struct Hooks {
        std::function<bool()>      check_enabled;  // updates.check
        std::function<bool()>      auto_install;   // updates.auto_install
        std::function<ApplyGate()> gate;
        std::function<void()>      request_exit;   // the engine's normal exit path
        std::function<void(const std::string& version)> on_ready;  // optional: tray balloon etc.
    };
    struct Status {
        std::string state = "idle";  // idle|checking|up_to_date|available|downloading|ready|installing|failed
        std::string current;         // this build
        std::string latest;
        std::string page_url;
        std::string error;
        std::string install_kind;    // installer|scoop|portable
        int64_t     last_check_unix = 0;
    };

    explicit Updater(Hooks hooks);
    ~Updater();
    Updater(const Updater&)            = delete;
    Updater& operator=(const Updater&) = delete;

    void Start();
    void Stop();

    Status GetStatus() const;
    // "Check now": a check (and a download, if this kind installs) as soon as the worker is free.
    void RequestCheck();
    // "Install now" from Settings or the tray: applies a ready installer without waiting for idle.
    // false (with *error) when nothing is ready.
    bool RequestInstall(std::string* error);
    InstallKind Kind() const { return kind_; }

private:
    void WorkerLoop();
    void RunCheck(bool download);
    bool Apply(std::string* error);
    void SetState(const char* state, const std::string& error = {});

    Hooks                   hooks_;
    InstallKind             kind_ = InstallKind::Portable;
    std::wstring            updates_dir_;
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    Status                  status_;
    std::wstring            ready_installer_;   // verified path, when state == ready
    std::string             ready_sha256_;
    bool                    check_requested_   = false;
    bool                    install_requested_ = false;
    bool                    stop_              = false;
    int64_t                 last_attempt_unix_ = 0;  // worker thread only
    std::thread             worker_;
};

// The resident engine's Updater for the pipe and the tray; null in every other mode.
extern std::atomic<Updater*> g_updater_for_engine;

}  // namespace ombra
