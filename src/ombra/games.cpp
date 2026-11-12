#include "ombra/games.h"

#include "ombra/window_target.h"

#include <windows.h>

namespace ombra {

namespace {

// Kept local: ombra-ui links this file without util.cpp.
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool HasSeparator(const std::wstring& s) { return s.find_first_of(L"\\/") != std::wstring::npos; }

bool EndsWithExe(const std::wstring& lower) {
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0;
}

// Desktop chrome that can hold the foreground covering a monitor (the desktop itself, Start, the
// lock-screen host) but is never a game.
constexpr const wchar_t* kShellFloor[] = {
    L"explorer.exe", L"dwm.exe", L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
    L"searchhost.exe", L"lockapp.exe", L"textinputhost.exe",
};

// How specifically `e` covers `image_lower`: 0 = not at all; an exact exe path outranks every
// folder, a longer folder outranks a shorter one, a bare name ranks lowest.
size_t MatchRank(const GameEntry& e, const std::wstring& image_lower) {
    const std::wstring key = GameKey(e.path);
    if (key.empty()) return 0;
    if (!HasSeparator(key)) return FileNameOf(image_lower) == key ? 1 : 0;
    if (EndsWithExe(key)) return image_lower == key ? static_cast<size_t>(-1) : 0;
    if (image_lower.size() > key.size() + 1 && image_lower.compare(0, key.size(), key) == 0 &&
        image_lower[key.size()] == L'\\') {
        return key.size() + 1;
    }
    return 0;
}

}  // namespace

const char* GameSourceName(GameSource s) {
    switch (s) {
        case GameSource::Riot:  return "riot";
        case GameSource::Steam: return "steam";
        case GameSource::Epic:  return "epic";
        case GameSource::Gog:   return "gog";
        case GameSource::Ubisoft: return "ubisoft";
        case GameSource::User:  return "user";
    }
    return "user";
}

bool ParseGameSource(const std::string& s, GameSource* out) {
    for (GameSource g : {GameSource::Riot, GameSource::Steam, GameSource::Epic, GameSource::Gog, GameSource::Ubisoft, GameSource::User}) {
        if (s == GameSourceName(g)) {
            *out = g;
            return true;
        }
    }
    return false;
}

std::wstring GameKey(const std::wstring& path) { return NormalizeFolderPath(path); }

bool ValidateGamesList(const JsonValue& v, std::string& err) {
    if (v.type != JsonValue::Type::Array) { err = "must be an array"; return false; }
    for (const JsonValue& e : v.arr) {
        if (e.type != JsonValue::Type::Object) { err = "every entry must be an object"; return false; }
        const JsonValue* path = e.Find("path");
        if (!path || path->type != JsonValue::Type::String || path->str.empty()) {
            err = "each entry needs a non-empty string \"path\"";
            return false;
        }
        if (const JsonValue* name = e.Find("name"); name && name->type != JsonValue::Type::String) {
            err = "\"name\", if present, must be a string";
            return false;
        }
        if (const JsonValue* source = e.Find("source")) {
            GameSource s;
            if (source->type != JsonValue::Type::String || !ParseGameSource(source->str, &s)) {
                err = "\"source\" must be one of: riot|steam|epic|user";
                return false;
            }
        }
        if (const JsonValue* game = e.Find("game"); game && game->type != JsonValue::Type::Bool) {
            err = "\"game\", if present, must be a bool";
            return false;
        }
    }
    return true;
}

std::vector<GameEntry> ParseGamesList(const JsonValue& v) {
    std::vector<GameEntry> out;
    std::string err;
    if (!ValidateGamesList(v, err)) return out;
    for (const JsonValue& e : v.arr) {
        GameEntry g;
        g.path = Widen(e.Find("path")->str);
        if (const JsonValue* name = e.Find("name")) g.name = Widen(name->str);
        if (const JsonValue* source = e.Find("source")) ParseGameSource(source->str, &g.source);
        if (const JsonValue* game = e.Find("game")) g.is_game = game->b;
        if (g.name.empty()) g.name = FileNameOf(g.path);
        out.push_back(std::move(g));
    }
    return out;
}

JsonValue SerializeGamesList(const std::vector<GameEntry>& entries) {
    JsonValue arr = JsonValue::MakeArray();
    for (const GameEntry& g : entries) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("name", JsonValue::MakeStr(Narrow(g.name)));
        o.Set("path", JsonValue::MakeStr(Narrow(g.path)));
        o.Set("source", JsonValue::MakeStr(GameSourceName(g.source)));
        o.Set("game", JsonValue::MakeBool(g.is_game));
        arr.arr.push_back(std::move(o));
    }
    return arr;
}

std::vector<GameEntry> MergeGames(const std::vector<GameEntry>& detected,
                                  const std::vector<GameEntry>& decisions) {
    std::vector<GameEntry> out;
    std::vector<bool>      used(decisions.size(), false);
    for (const GameEntry& d : detected) {
        GameEntry merged = d;
        merged.detected  = true;
        const std::wstring key = GameKey(d.path);
        for (size_t i = 0; i < decisions.size(); ++i) {
            if (GameKey(decisions[i].path) != key) continue;
            merged.is_game = decisions[i].is_game;
            used[i]        = true;
        }
        out.push_back(std::move(merged));
    }
    for (size_t i = 0; i < decisions.size(); ++i) {
        if (used[i]) continue;
        GameEntry g = decisions[i];
        g.detected  = false;
        out.push_back(std::move(g));
    }
    return out;
}

GameVerdict ClassifyImage(const std::vector<GameEntry>& games, const std::wstring& image_path,
                          const GameEntry** match) {
    if (match) *match = nullptr;
    const std::wstring image_lower = NormalizeFolderPath(image_path);
    if (image_lower.empty()) return GameVerdict::Unknown;
    const std::wstring file = FileNameOf(image_lower);
    for (const wchar_t* shell : kShellFloor)
        if (file == shell) return GameVerdict::NotGame;

    const GameEntry* best      = nullptr;
    size_t           best_rank = 0;
    for (const GameEntry& g : games) {
        const size_t rank = MatchRank(g, image_lower);
        if (rank > best_rank) {
            best      = &g;
            best_rank = rank;
        }
    }
    if (!best) return GameVerdict::Unknown;
    if (match) *match = best;
    return best->is_game ? GameVerdict::Game : GameVerdict::NotGame;
}

std::vector<GameEntry> MigrateLegacyGameLists(const JsonValue& exclusions, const JsonValue& overrides,
                                              const std::vector<GameEntry>& existing) {
    std::vector<GameEntry> out;
    auto add = [&](const JsonValue& list, bool is_game) {
        if (list.type != JsonValue::Type::Array) return;
        for (const JsonValue& v : list.arr) {
            if (v.type != JsonValue::Type::String || v.str.empty()) continue;
            GameEntry g;
            g.path    = Widen(v.str);
            g.name    = FileNameOf(g.path);
            g.source  = GameSource::User;
            g.is_game = is_game;
            const std::wstring key = GameKey(g.path);
            bool dup = false;
            for (const GameEntry& e : existing) dup = dup || GameKey(e.path) == key;
            for (const GameEntry& e : out) dup = dup || GameKey(e.path) == key;
            if (!dup) out.push_back(std::move(g));
        }
    };
    // The first list to name a program decides it (duplicates are skipped). Exclusions go first:
    // the old controller let an exclusion beat an override.
    add(exclusions, false);
    add(overrides, true);
    return out;
}

}  // namespace ombra
