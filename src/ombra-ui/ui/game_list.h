#pragma once

// GameRow: one entry of the Games page's list, as data -- the page turns each into a SettingRow in
// a RowList (only the list scrolls) and tests read these. A `decide` row is an app Game mode armed
// for on its own that the list doesn't answer for yet: It's a game / Not a game instead of the
// switch, tinted, at the top.

#include "ombra-ui/ui/file_icon.h"

#include <functional>
#include <memory>
#include <string>

namespace ombra_ui {

struct GameRow {
    std::wstring name;
    std::wstring source;   // "Riot" / "Steam" / "Epic" / "Added by you"
    std::wstring detail;   // the path, as shown
    std::wstring warning;  // what's risky about the path, under it in the warning colour
    bool is_game   = true;
    bool removable = false;               // gets a × in Edit list (on_remove)
    bool decide    = false;               // It's a game / Not a game instead of the switch
    std::shared_ptr<const FileIcon> icon; // null or empty: the controller glyph
    std::function<void(bool)> on_toggle;
    std::function<void()>     on_remove;
    std::function<void(bool)> on_decide;  // true = "It's a game"
};

}  // namespace ombra_ui
