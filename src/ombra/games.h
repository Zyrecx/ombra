// The Games list: which programs are games. One list answers that question for Game mode's
// arming, the "The game" capture target and the window picker's controller icons. It replaced
// engine.exclusions ("Never record") and engine.game_overrides ("Always a game"), which only ever
// answered the same question with exe-name substrings.
//
// Two halves, merged on every read:
//   - Detected entries, recomputed from the launchers' own local files (game_library.h). Never
//     stored, so an uninstalled game drops out on its own.
//   - The user's decisions (config key games.list): games they added, "not a game" marks, and any
//     override of a detected entry. Keyed by normalized path, so a decision sticks to its game
//     across rescans.
//
// An entry names a program by where it lives, never by a bare exe name: A Java game is
// javaw.exe, so "javaw.exe is a game" would make every Java program one. `path` is an install
// folder (any image under it, at any depth) or one exe's full path. The one exception is a bare
// name carried over from the old lists by LoadConfig's migration: it matches that file name
// exactly, and only the user's own earlier entries produce one.
#pragma once

#include "ombra/json.h"

#include <string>
#include <vector>

namespace ombra {

enum class GameSource { Riot, Steam, Epic, Gog, Ubisoft, User };
const char* GameSourceName(GameSource s);  // "riot" | "steam" | "epic" | "gog" | "ubisoft" | "user"
bool        ParseGameSource(const std::string& s, GameSource* out);

struct GameEntry {
    std::wstring name;
    std::wstring path;  // install folder, or an exe's full path; see the header comment
    GameSource   source  = GameSource::User;
    bool         is_game = true;
    bool         detected = false;  // came from a launcher scan this run; not serialized
    // A local file the Games page draws as the entry's icon (.ico, .exe or .jpg), found by the
    // launcher scan; empty when it has none. Not serialized: the scan finds it again every run.
    std::wstring icon;
};

// games.list: [{name, path, source, game}]. Validate says what's wrong; Parse of an invalid value
// returns nothing.
bool                   ValidateGamesList(const JsonValue& v, std::string& err);
std::vector<GameEntry> ParseGamesList(const JsonValue& v);
JsonValue              SerializeGamesList(const std::vector<GameEntry>& entries);

// Detected entries with the user's decisions applied, then the decisions that match no detected
// entry. A decision on a detected entry keeps the detected name and source and sets is_game.
std::vector<GameEntry> MergeGames(const std::vector<GameEntry>& detected,
                                  const std::vector<GameEntry>& decisions);

// NormalizeFolderPath of `path`; the key decisions and detected entries meet on.
std::wstring GameKey(const std::wstring& path);

enum class GameVerdict { Unknown, Game, NotGame };

// What the list says about the process at `image_path` (a full path). The most specific entry
// wins: an exact exe path over a folder, a deeper folder over a shallower one, so "not a game" on
// one tool inside a game's folder holds. OS shell processes (explorer, dwm, ...) are NotGame
// whatever the list says; they are a built-in floor, never list entries. `match` (optional)
// receives the deciding entry, null for the floor or Unknown.
GameVerdict ClassifyImage(const std::vector<GameEntry>& games, const std::wstring& image_path,
                          const GameEntry** match = nullptr);

// The old keys' values as games.list decisions: exclusions become "not a game", overrides "a game".
// Each keeps its string as the path (usually a bare exe name). Entries already present in
// `existing` (same key) are left alone.
std::vector<GameEntry> MigrateLegacyGameLists(const JsonValue& exclusions, const JsonValue& overrides,
                                              const std::vector<GameEntry>& existing);

}  // namespace ombra
