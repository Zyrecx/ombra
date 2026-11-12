#include "ombra/hotkey_spec.h"
#include "ombra/brand.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace ombra {

namespace {

struct NamedKey {
    const char* name;
    UINT        vk;
};

// Layout-independent keys only: OEM punctuation (`, [, ;) moves between keyboard layouts, so a
// string naming it would mean a different physical key on another machine.
constexpr NamedKey kNamedKeys[] = {
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME},     {"End", VK_END},
    {"PageUp", VK_PRIOR},  {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE},   {"ScrollLock", VK_SCROLL},
};

std::string Upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool ParseKey(const std::string& upper, UINT& vk) {
    if (upper.size() == 1) {
        const char c = upper[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { vk = static_cast<UINT>(c); return true; }
        return false;
    }
    if (upper.size() >= 2 && upper.size() <= 3 && upper[0] == 'F' && std::isdigit(static_cast<unsigned char>(upper[1]))) {
        const int n = std::atoi(upper.c_str() + 1);
        if (n >= 1 && n <= 24) { vk = VK_F1 + static_cast<UINT>(n - 1); return true; }
        return false;
    }
    if (upper.size() == 4 && upper.compare(0, 3, "NUM") == 0 && upper[3] >= '0' && upper[3] <= '9') {
        vk = VK_NUMPAD0 + static_cast<UINT>(upper[3] - '0');
        return true;
    }
    for (const auto& k : kNamedKeys) {
        if (upper == Upper(k.name)) { vk = k.vk; return true; }
    }
    return false;
}

}  // namespace

std::string HotkeyKeyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    for (const auto& k : kNamedKeys) {
        if (k.vk == vk) return k.name;
    }
    return {};
}

bool IsModifierVk(UINT vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

bool ParseHotkeySpec(std::string_view spec, HotkeyCombo& out) {
    out = {};
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        const size_t plus = spec.find('+', start);
        parts.push_back(spec.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    if (parts.back().empty()) return false;

    HotkeyCombo combo;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string p = Upper(parts[i]);
        UINT bit = 0;
        if (p == "ALT") bit = MOD_ALT;
        else if (p == "CTRL" || p == "CONTROL") bit = MOD_CONTROL;
        else if (p == "SHIFT") bit = MOD_SHIFT;
        else if (p == "WIN") bit = MOD_WIN;
        else return false;
        if (combo.mods & bit) return false;  // "Alt+Alt+X"
        combo.mods |= bit;
    }
    if (!ParseKey(Upper(parts.back()), combo.vk)) return false;
    out = combo;
    return true;
}

std::string FormatHotkeySpec(const HotkeyCombo& combo) {
    const std::string key = HotkeyKeyName(combo.vk);
    if (key.empty()) return {};
    std::string s;
    if (combo.mods & MOD_CONTROL) s += "Ctrl+";
    if (combo.mods & MOD_ALT)     s += "Alt+";
    if (combo.mods & MOD_SHIFT)   s += "Shift+";
    if (combo.mods & MOD_WIN)     s += "Win+";
    return s + key;
}

HotkeyProblem ValidateHotkeyCombo(const HotkeyCombo& combo) {
    if (HotkeyKeyName(combo.vk).empty()) return HotkeyProblem::Unparseable;
    if (combo.vk == VK_F4 && combo.mods == MOD_ALT) return HotkeyProblem::Reserved;
    const bool macro_key = combo.vk >= VK_F13 && combo.vk <= VK_F24;
    if (!macro_key && !(combo.mods & (MOD_ALT | MOD_CONTROL | MOD_WIN))) return HotkeyProblem::NeedsModifier;
    return HotkeyProblem::None;
}

HotkeyProblem ValidateHotkeySpec(std::string_view spec) {
    HotkeyCombo combo;
    if (!ParseHotkeySpec(spec, combo)) return HotkeyProblem::Unparseable;
    return ValidateHotkeyCombo(combo);
}

const char* HotkeyProblemText(HotkeyProblem problem) {
    switch (problem) {
        case HotkeyProblem::None:          return "";
        case HotkeyProblem::Unparseable:   return OMBRA_PRODUCT_NAME " can't bind that key.";
        case HotkeyProblem::NeedsModifier: return "Add Ctrl, Alt or Win, or it fires while you type.";
        case HotkeyProblem::Reserved:      return "Alt+F4 closes windows.";
    }
    return "";
}

}  // namespace ombra
