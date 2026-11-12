// The engine's live Games list: launcher scans merged with the user's games.list decisions
// (games.h), plus the "looked like a game" apps Game mode armed for on its GPU heuristic alone.
//
// Launcher scans read each launcher's own local files, nothing online:
//   - Riot: %ProgramData%\Riot Games\Metadata\<product>\<product>.product_settings.yaml,
//     product_install_full_path. That is each product's own folder (a game's client and match both
//     live under it, a second title under ...\Game\live), never the Riot Client's.
//   - Steam: the install's steamapps\libraryfolders.vdf lists every library; each library's
//     steamapps\appmanifest_<id>.acf names one app. The rules follow Playnite's SteamLocalService
//     (MIT, JosefNemec/PlayniteExtensions): only fully installed apps (StateFlags bit 4), only ones
//     whose steamapps\common\<installdir> exists, skip .tmp manifests and appid 228980 (Steamworks
//     redistributables). Local manifests carry no app type, so Linux-only runtimes are skipped by
//     name; other tools (SteamVR) stay and can be marked "not a game".
//   - Epic: %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item (JSON). Skips incomplete
//     installs, DLC (AppName != MainGameAppName) and add-ons/plugins, as Playnite's EpicLibrary does.
#pragma once

#include "ombra/games.h"

#include <windows.h>

#include <chrono>
#include <functional>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ombra {

struct LauncherRoots {
    std::wstring              program_data;  // Riot and Epic metadata live under it
    std::vector<std::wstring> steam_roots;   // Steam installs; each one's libraryfolders.vdf adds libraries
    bool                      scan_registry = false;  // GOG and Ubisoft Connect keep their installs in HKLM
};
// %ProgramData%, the Steam install from the registry (HKCU SteamPath, then HKLM InstallPath).
LauncherRoots DefaultLauncherRoots();

std::vector<GameEntry> ScanRiot(const std::wstring& program_data);
std::vector<GameEntry> ScanSteam(const std::wstring& steam_root);
std::vector<GameEntry> ScanEpic(const std::wstring& program_data);
// GOG Galaxy installs are Inno Setup uninstall entries ("<id>_is1", Publisher "GOG.com"); Ubisoft Connect
// writes Launcher\Installs\<id>\InstallDir. Both take the hive and key path so a test can use a scratch key.
std::vector<GameEntry> ScanGog(HKEY hive, const wchar_t* uninstall_key);
std::vector<GameEntry> ScanUbisoft(HKEY hive, const wchar_t* installs_key);
// Every launcher above, one entry per folder.
std::vector<GameEntry> ScanLaunchers(const LauncherRoots& roots);

// An app Game mode armed for on its GPU heuristic that the list doesn't know yet.
struct SeenApp {
    std::wstring name;
    std::wstring path;  // the exe's full path: what "It's a game" / "Not a game" store
    int64_t      last_seen = 0;  // unix seconds
};

class GameLibrary {
public:
    // `seen_file` persists the "looked like a game" apps across restarts; empty = memory only.
    GameLibrary(LauncherRoots roots, std::wstring seen_file);

    // games.list, as loaded or as set_config just wrote it.
    void SetDecisions(std::vector<GameEntry> decisions);
    // Re-reads the launchers' files (a few small files per launcher, milliseconds).
    void Rescan();
    // Rescan() unless the last scan is younger than `max_age`. Game mode calls it on each
    // foreground change, so a game installed while the engine runs is picked up without a timer.
    void RescanIfOlderThan(std::chrono::seconds max_age);

    std::vector<GameEntry> Games() const;  // merged, detected first
    // ClassifyImage against the merged list; `name` and `key` (optional) get the matched entry's
    // name and GameKey, empty when no entry matched.
    GameVerdict Classify(const std::wstring& image_path, std::wstring* name = nullptr,
                         std::wstring* key = nullptr) const;
    // The one answer to "is this program a game?" for arming and for "The game" capture target:
    // Classify == Game, or a program the list doesn't know that the heuristic armed for during this
    // run (until the user decides it either way on the Games page). A copy of the state taken now,
    // so a window resolve doesn't lock once per window.
    std::function<bool(const std::wstring& image_path)> GameFilter() const;

    // Records an app the heuristic armed for, unless the list already classifies it: it lands in
    // "Looked like a game" (persisted) and counts as a game for the rest of this run (not
    // persisted -- a heuristic hit is a guess about one session, not a decision).
    void NoteHeuristicArm(const std::wstring& name, const std::wstring& image_path);
    // Newest first, without any the list now classifies.
    std::vector<SeenApp> Seen() const;

    static constexpr size_t kMaxSeen = 20;

private:
    void RebuildLocked();
    void SaveSeenLocked() const;
    void LoadSeen();

    const LauncherRoots roots_;
    const std::wstring  seen_file_;

    mutable std::mutex                    mutex_;
    std::vector<GameEntry>                detected_;
    std::vector<GameEntry>                decisions_;
    std::vector<GameEntry>                merged_;
    std::vector<SeenApp>                  seen_;
    std::set<std::wstring>                heuristic_this_run_;  // GameKey()s
    std::chrono::steady_clock::time_point last_scan_{};
    bool                                  scanned_ = false;
};

}  // namespace ombra
