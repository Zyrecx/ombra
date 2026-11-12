#include "ombra/game_library.h"

#include "ombra/log.h"
#include "ombra/util.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ombra {

namespace {

namespace fs = std::filesystem;

std::string ReadFileText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

// Backslashes, no trailing separator: how a path is shown and stored.
std::wstring TidyPath(std::wstring p) {
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool IsDir(const std::wstring& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(fs::path(p), ec);
}

std::wstring LastComponent(const std::wstring& p) {
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

std::wstring ParentOf(const std::wstring& p) {
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring{} : p.substr(0, slash);
}

std::wstring LowerAscii(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

bool IsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// Steam's own 32x32 icon for `appid`, from the cache its library list draws from. Since the 2024
// library cache layout it is the one file named by a 40-hex-digit hash directly in
// appcache\librarycache\<appid>\ (the other files there are named header/logo/library_*; the
// hashed subfolders hold other art). Before that it was librarycache\<appid>_icon.jpg. Anything
// else -- none, or more than one hashed file -- gives no icon rather than a guess.
std::wstring SteamIcon(const std::wstring& steam_root, const std::string& appid) {
    const fs::path cache = fs::path(steam_root) / L"appcache" / L"librarycache";
    const std::wstring id = WideFromUtf8(appid);
    std::error_code ec;
    std::wstring found;
    int          hashed = 0;
    for (const auto& f : fs::directory_iterator(cache / id, ec)) {
        if (!f.is_regular_file(ec)) continue;
        const std::wstring stem = f.path().stem().wstring();
        if (LowerAscii(f.path().extension().wstring()) != L".jpg" || stem.size() != 40 ||
            !std::all_of(stem.begin(), stem.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; })) {
            continue;
        }
        ++hashed;
        found = f.path().wstring();
    }
    if (hashed == 1) return found;
    const fs::path old = cache / (id + L"_icon.jpg");
    return IsFile(old) ? old.wstring() : std::wstring{};
}

// Valve KeyValues text (.vdf/.acf): "key" "value" pairs and "key" { ... } blocks.
struct VdfNode {
    std::string                                   value;
    std::vector<std::pair<std::string, VdfNode>> kids;

    const VdfNode* Find(const std::string& key) const {
        for (const auto& [k, v] : kids)
            if (_stricmp(k.c_str(), key.c_str()) == 0) return &v;
        return nullptr;
    }
    std::string Str(const std::string& key) const {
        const VdfNode* n = Find(key);
        return n ? n->value : std::string{};
    }
};

class VdfParser {
public:
    explicit VdfParser(const std::string& text) : s_(text) {}

    VdfNode ParseRoot() {
        VdfNode root;
        ParseBody(root, /*nested=*/false);
        return root;
    }

private:
    enum class Tok { Str, Open, Close, End };

    Tok Next(std::string& out) {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (std::isspace(static_cast<unsigned char>(c))) { ++i_; continue; }
            if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            break;
        }
        if (i_ >= s_.size()) return Tok::End;
        const char c = s_[i_];
        if (c == '{') { ++i_; return Tok::Open; }
        if (c == '}') { ++i_; return Tok::Close; }
        out.clear();
        if (c == '"') {
            ++i_;
            while (i_ < s_.size() && s_[i_] != '"') {
                if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                    const char e = s_[i_ + 1];
                    out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    i_ += 2;
                    continue;
                }
                out.push_back(s_[i_++]);
            }
            ++i_;  // closing quote
            return Tok::Str;
        }
        while (i_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i_])) && s_[i_] != '{' &&
               s_[i_] != '}' && s_[i_] != '"') {
            out.push_back(s_[i_++]);
        }
        return Tok::Str;
    }

    void ParseBody(VdfNode& node, bool nested) {
        std::string key, val;
        for (int guard = 0; guard < 1'000'000; ++guard) {
            const Tok t = Next(key);
            if (t == Tok::End || (t == Tok::Close && nested)) return;
            if (t != Tok::Str) continue;  // stray brace: skip it rather than fail the whole file
            const Tok v = Next(val);
            if (v == Tok::Open) {
                VdfNode child;
                if (++depth_ < 64) ParseBody(child, true);
                --depth_;
                node.kids.emplace_back(key, std::move(child));
            } else if (v == Tok::Str) {
                VdfNode leaf;
                leaf.value = val;
                node.kids.emplace_back(key, std::move(leaf));
            } else {
                return;
            }
        }
    }

    const std::string& s_;
    size_t             i_ = 0;
    int                depth_ = 0;
};

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2]{};
    for (DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
        DWORD bytes = sizeof(buf);  // a failed call may have rewritten it
        if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ | view, nullptr, buf, &bytes) == ERROR_SUCCESS) return buf;
    }
    return {};
}

// Every subkey of `key` in the 64-bit and the 32-bit registry view, each opened for reading.
template <typename Fn>
void ForEachSubkey(HKEY hive, const wchar_t* key, Fn&& fn) {
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY parent = nullptr;
        if (RegOpenKeyExW(hive, key, 0, KEY_READ | view, &parent) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            if (RegEnumKeyExW(parent, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY sub = nullptr;
            if (RegOpenKeyExW(parent, name, 0, KEY_READ | view, &sub) != ERROR_SUCCESS) continue;
            fn(std::wstring(name, len), sub);
            RegCloseKey(sub);
        }
        RegCloseKey(parent);
    }
}

// A key that isn't redirected between the two registry views shows up in both walks.
void AddUnique(std::vector<GameEntry>& out, GameEntry g) {
    for (const auto& o : out)
        if (GameKey(o.path) == GameKey(g.path)) return;
    out.push_back(std::move(g));
}

std::wstring ReadKeyString(HKEY key, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD   bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Riot product ids look like "game.live"; a folder like ...\Game\live is named by its parent.
std::wstring RiotName(const std::wstring& folder, const std::string& shortcut, const std::wstring& product_id) {
    if (!shortcut.empty()) {
        std::wstring s = WideFromUtf8(shortcut);
        const size_t dot = s.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) s = s.substr(0, dot);
        if (!s.empty()) return s;
    }
    std::wstring last = LastComponent(folder);
    const std::wstring lower = LowerAscii(last);
    if (lower == L"live" || lower == L"pbe") {
        std::wstring parent = LastComponent(ParentOf(folder));
        if (!parent.empty()) return lower == L"pbe" ? parent + L" (PBE)" : parent;
    }
    return last.empty() ? product_id : last;
}

std::string YamlValue(const std::string& text, const char* key) {
    const std::string want = std::string(key) + ":";
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        if (line.compare(0, want.size(), want) != 0) continue;  // top level only: no indent
        std::string v = line.substr(want.size());
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

}  // namespace

LauncherRoots DefaultLauncherRoots() {
    LauncherRoots r;
    PWSTR pd = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd)) && pd) r.program_data = pd;
    CoTaskMemFree(pd);
    std::wstring steam = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (!steam.empty()) r.steam_roots.push_back(TidyPath(steam));
    r.scan_registry = true;
    return r;
}

std::vector<GameEntry> ScanRiot(const std::wstring& program_data) {
    std::vector<GameEntry> out;
    if (program_data.empty()) return out;
    const fs::path meta = fs::path(program_data) / L"Riot Games" / L"Metadata";
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(meta, ec)) {
        if (!dir.is_directory(ec)) continue;
        const std::wstring id = dir.path().filename().wstring();
        const fs::path settings = dir.path() / (id + L".product_settings.yaml");
        const std::string text = ReadFileText(settings);
        if (text.empty()) continue;
        const std::wstring folder = TidyPath(WideFromUtf8(YamlValue(text, "product_install_full_path")));
        if (!IsDir(folder)) continue;  // left over from an uninstall
        GameEntry g;
        g.path   = folder;
        g.name   = RiotName(folder, YamlValue(text, "shortcut_name"), id);
        g.source = GameSource::Riot;
        // The icon the Riot Client made for the product's desktop shortcut.
        if (const fs::path ico = dir.path() / (id + L".ico"); IsFile(ico)) g.icon = ico.wstring();
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<GameEntry> ScanSteam(const std::wstring& steam_root) {
    std::vector<GameEntry> out;
    if (steam_root.empty()) return out;

    std::vector<std::wstring> libraries{TidyPath(steam_root)};
    const VdfNode lf = VdfParser(ReadFileText(fs::path(steam_root) / L"steamapps" / L"libraryfolders.vdf")).ParseRoot();
    if (const VdfNode* folders = lf.Find("libraryfolders")) {
        for (const auto& [key, node] : folders->kids) {
            // Current format: "0" { "path" "..." }. The pre-2021 one: "1" "D:\\SteamLibrary".
            const std::string p = node.kids.empty() ? node.value : node.Str("path");
            if (p.empty() || !std::isdigit(static_cast<unsigned char>(key.empty() ? 'x' : key[0]))) continue;
            const std::wstring lib = TidyPath(WideFromUtf8(p));
            bool dup = false;
            for (const auto& l : libraries) dup = dup || GameKey(l) == GameKey(lib);
            if (!dup) libraries.push_back(lib);
        }
    }

    std::error_code ec;
    for (const std::wstring& lib : libraries) {
        const fs::path apps = fs::path(lib) / L"steamapps";
        for (const auto& f : fs::directory_iterator(apps, ec)) {
            const std::wstring name = LowerAscii(f.path().filename().wstring());
            // Exact suffix check: a directory pattern like *.acf also matches a .tmp manifest's
            // 8.3 short name on some volumes.
            if (name.rfind(L"appmanifest_", 0) != 0 || name.size() < 4 ||
                name.compare(name.size() - 4, 4, L".acf") != 0) {
                continue;
            }
            const VdfNode acf = VdfParser(ReadFileText(f.path())).ParseRoot();
            const VdfNode* state = acf.Find("AppState");
            if (!state) continue;
            const std::string appid = state->Str("appid");
            const std::string title = state->Str("name");
            const std::string dir   = state->Str("installdir");
            const long long   flags = std::atoll(state->Str("StateFlags").c_str());
            if (!(flags & 4) || appid == "228980" || dir.empty()) continue;
            if (title.rfind("Steam Linux Runtime", 0) == 0 || title.rfind("Proton ", 0) == 0) continue;
            const std::wstring folder = TidyPath((apps / L"common" / WideFromUtf8(dir)).wstring());
            if (!IsDir(folder)) continue;
            GameEntry g;
            g.path   = folder;
            g.name   = title.empty() ? WideFromUtf8(dir) : WideFromUtf8(title);
            g.source = GameSource::Steam;
            g.icon   = SteamIcon(steam_root, appid);
            out.push_back(std::move(g));
        }
    }
    return out;
}

std::vector<GameEntry> ScanEpic(const std::wstring& program_data) {
    std::vector<GameEntry> out;
    if (program_data.empty()) return out;
    const fs::path manifests = fs::path(program_data) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(manifests, ec)) {
        if (!f.is_regular_file(ec) || LowerAscii(f.path().extension().wstring()) != L".item") continue;
        JsonValue m;
        try {
            m = ParseJson(ReadFileText(f.path()));
        } catch (const std::string&) {
            continue;
        }
        auto str = [&](const char* k) {
            const JsonValue* v = m.Find(k);
            return v && v->type == JsonValue::Type::String ? v->str : std::string{};
        };
        if (const JsonValue* inc = m.Find("bIsIncompleteInstall"); inc && inc->type == JsonValue::Type::Bool && inc->b)
            continue;
        const std::string app = str("AppName"), main_app = str("MainGameAppName");
        if (!main_app.empty() && app != main_app) continue;  // DLC
        bool addon = false, launchable = false, plugin = false;
        if (const JsonValue* cats = m.Find("AppCategories"); cats && cats->type == JsonValue::Type::Array) {
            for (const JsonValue& c : cats->arr) {
                addon      = addon || c.str == "addons";
                launchable = launchable || c.str == "addons/launchable";
                plugin     = plugin || c.str == "plugins" || c.str == "plugins/engine";
            }
        }
        if ((addon && !launchable) || plugin) continue;
        const std::wstring folder = TidyPath(WideFromUtf8(str("InstallLocation")));
        if (!IsDir(folder)) continue;
        GameEntry g;
        g.path   = folder;
        g.name   = WideFromUtf8(str("DisplayName"));
        if (g.name.empty()) g.name = LastComponent(folder);
        g.source = GameSource::Epic;
        // The exe the launcher starts (relative, forward slashes); its own icon resource.
        if (const std::string exe = str("LaunchExecutable"); !exe.empty()) {
            const fs::path p = fs::path(folder) / fs::path(WideFromUtf8(exe)).make_preferred();
            if (IsFile(p)) g.icon = p.wstring();
        }
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<GameEntry> ScanGog(HKEY hive, const wchar_t* uninstall_key) {
    std::vector<GameEntry> out;
    ForEachSubkey(hive, uninstall_key, [&](const std::wstring& name, HKEY k) {
        // "<digits>_is1" only: "GOGPACK..." entries are GOG's own redistributables.
        if (name.size() < 5 || name.compare(name.size() - 4, 4, L"_is1") != 0) return;
        if (!std::all_of(name.begin(), name.end() - 4, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return;
        if (ReadKeyString(k, L"Publisher") != L"GOG.com") return;
        const std::wstring folder = TidyPath(ReadKeyString(k, L"InstallLocation"));
        if (!IsDir(folder)) return;
        GameEntry g;
        g.path   = folder;
        g.name   = ReadKeyString(k, L"DisplayName");
        if (g.name.empty()) g.name = LastComponent(folder);
        g.source = GameSource::Gog;
        // DisplayIcon is "C:\path\Game.exe" or "...exe",0; the exe's own icon resource.
        std::wstring icon = ReadKeyString(k, L"DisplayIcon");
        if (const size_t comma = icon.find_last_of(L','); comma != std::wstring::npos) icon.resize(comma);
        if (icon.size() >= 2 && icon.front() == L'"' && icon.back() == L'"') icon = icon.substr(1, icon.size() - 2);
        if (IsFile(icon)) g.icon = icon;
        AddUnique(out, std::move(g));
    });
    return out;
}

std::vector<GameEntry> ScanUbisoft(HKEY hive, const wchar_t* installs_key) {
    std::vector<GameEntry> out;
    ForEachSubkey(hive, installs_key, [&](const std::wstring&, HKEY k) {
        const std::wstring folder = TidyPath(ReadKeyString(k, L"InstallDir"));  // forward slashes
        if (!IsDir(folder)) return;
        GameEntry g;
        g.path   = folder;
        g.name   = LastComponent(folder);  // the launcher's record has no title
        g.source = GameSource::Ubisoft;
        AddUnique(out, std::move(g));
    });
    return out;
}

std::vector<GameEntry> ScanLaunchers(const LauncherRoots& roots) {
    std::vector<GameEntry> all = ScanRiot(roots.program_data);
    for (const auto& root : roots.steam_roots) {
        auto steam = ScanSteam(root);
        all.insert(all.end(), steam.begin(), steam.end());
    }
    auto epic = ScanEpic(roots.program_data);
    all.insert(all.end(), epic.begin(), epic.end());
    if (roots.scan_registry) {
        for (auto&& list : {ScanGog(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
                            ScanUbisoft(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ubisoft\\Launcher\\Installs")})
            all.insert(all.end(), list.begin(), list.end());
    }

    std::vector<GameEntry> out;
    for (auto& g : all) {
        bool dup = false;
        for (const auto& o : out) dup = dup || GameKey(o.path) == GameKey(g.path);
        if (!dup) out.push_back(std::move(g));
    }
    return out;
}

GameLibrary::GameLibrary(LauncherRoots roots, std::wstring seen_file)
    : roots_(std::move(roots)), seen_file_(std::move(seen_file)) {
    LoadSeen();
}

void GameLibrary::SetDecisions(std::vector<GameEntry> decisions) {
    std::lock_guard<std::mutex> lock(mutex_);
    decisions_ = std::move(decisions);
    RebuildLocked();
}

void GameLibrary::Rescan() {
    std::vector<GameEntry> detected = ScanLaunchers(roots_);  // file reads outside the lock
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scanned_ || detected.size() != detected_.size())
        Log::Info("games  launcher scan: %zu installed", detected.size());
    detected_  = std::move(detected);
    last_scan_ = std::chrono::steady_clock::now();
    scanned_   = true;
    RebuildLocked();
}

void GameLibrary::RescanIfOlderThan(std::chrono::seconds max_age) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scanned_ && std::chrono::steady_clock::now() - last_scan_ < max_age) return;
    }
    Rescan();
}

void GameLibrary::RebuildLocked() {
    merged_ = MergeGames(detected_, decisions_);
    const size_t before = seen_.size();
    seen_.erase(std::remove_if(seen_.begin(), seen_.end(),
                               [&](const SeenApp& s) { return ClassifyImage(merged_, s.path) != GameVerdict::Unknown; }),
                seen_.end());
    if (seen_.size() != before) SaveSeenLocked();
}

std::vector<GameEntry> GameLibrary::Games() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return merged_;
}

GameVerdict GameLibrary::Classify(const std::wstring& image_path, std::wstring* name, std::wstring* key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const GameEntry* match = nullptr;
    const GameVerdict v = ClassifyImage(merged_, image_path, &match);
    if (name) *name = match ? match->name : std::wstring{};
    if (key) *key = match ? GameKey(match->path) : std::wstring{};
    return v;
}

std::function<bool(const std::wstring&)> GameLibrary::GameFilter() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return [games = merged_, armed = heuristic_this_run_](const std::wstring& image_path) {
        const GameVerdict v = ClassifyImage(games, image_path);
        return v == GameVerdict::Game || (v == GameVerdict::Unknown && armed.count(GameKey(image_path)) > 0);
    };
}


void GameLibrary::NoteHeuristicArm(const std::wstring& name, const std::wstring& image_path) {
    if (image_path.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ClassifyImage(merged_, image_path) != GameVerdict::Unknown) return;
    const std::wstring key = GameKey(image_path);
    heuristic_this_run_.insert(key);
    seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const SeenApp& s) { return GameKey(s.path) == key; }),
                seen_.end());
    seen_.insert(seen_.begin(), SeenApp{name, image_path, static_cast<int64_t>(std::time(nullptr))});
    if (seen_.size() > kMaxSeen) seen_.resize(kMaxSeen);
    SaveSeenLocked();
}

std::vector<SeenApp> GameLibrary::Seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
}

void GameLibrary::SaveSeenLocked() const {
    if (seen_file_.empty()) return;
    JsonValue arr = JsonValue::MakeArray();
    for (const SeenApp& s : seen_) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("name", JsonValue::MakeStr(NarrowUtf8(s.name)));
        o.Set("path", JsonValue::MakeStr(NarrowUtf8(s.path)));
        o.Set("last_seen", JsonValue::MakeNum(static_cast<double>(s.last_seen)));
        arr.arr.push_back(std::move(o));
    }
    if (!EnsureParentDir(seen_file_)) return;
    const std::wstring tmp = seen_file_ + L".tmp";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << WriteJsonCompact(arr) << "\n";
        if (!f) return;
    }
    if (!MoveFileExW(tmp.c_str(), seen_file_.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

void GameLibrary::LoadSeen() {
    if (seen_file_.empty()) return;
    JsonValue arr;
    try {
        arr = ParseJson(ReadFileText(fs::path(seen_file_)));
    } catch (const std::string&) {
        return;
    }
    if (arr.type != JsonValue::Type::Array) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const JsonValue& o : arr.arr) {
        const JsonValue* path = o.Find("path");
        if (!path || path->type != JsonValue::Type::String || path->str.empty()) continue;
        SeenApp s;
        s.path = WideFromUtf8(path->str);
        if (const JsonValue* n = o.Find("name"); n && n->type == JsonValue::Type::String) s.name = WideFromUtf8(n->str);
        if (const JsonValue* t = o.Find("last_seen"); t && t->type == JsonValue::Type::Number)
            s.last_seen = static_cast<int64_t>(t->num);
        seen_.push_back(std::move(s));
        if (seen_.size() >= kMaxSeen) break;
    }
}

}  // namespace ombra
