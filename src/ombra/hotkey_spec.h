#pragma once

// "Mod+Mod+Key" hotkey strings (config's hotkeys.*): parsing, the one canonical spelling, and the
// rule for what may be bound at all. Shared by the engine (registration, set_config validation) and
// ombra-ui (the capture control, conflict checks) so the two can never disagree on what a string
// means. No engine or UI dependencies: it builds into ombra_core and the ombra-ui exe alike.

#include <windows.h>

#include <string>
#include <string_view>

namespace ombra {

struct HotkeyCombo {
    UINT mods = 0;  // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN, never MOD_NOREPEAT
    UINT vk   = 0;
    bool operator==(const HotkeyCombo&) const = default;
};

// Case-insensitive, any modifier order ("alt+f10", "Shift+Ctrl+Home"). False on anything
// unrecognised; callers fall back to a known default rather than guess.
bool ParseHotkeySpec(std::string_view spec, HotkeyCombo& out);

// Canonical spelling: Ctrl+Alt+Shift+Win+Key. Empty if `vk` isn't a bindable key.
std::string FormatHotkeySpec(const HotkeyCombo& combo);

// The key's own name ("F10", "Z", "PageUp"); empty if it can't be bound.
std::string HotkeyKeyName(UINT vk);

// Modifier keys themselves (either side), which a capture control shows live but never commits.
bool IsModifierVk(UINT vk);

enum class HotkeyProblem {
    None,
    Unparseable,    // not a key this build can bind
    NeedsModifier,  // a bare key, or Shift alone: it would fire while typing or playing
    Reserved,       // Alt+F4
};

// What's wrong with binding `combo` globally, if anything. F13-F24 may be bound bare (no keyboard
// types them; they exist for macro keys). Shift alone doesn't count as a modifier: under
// RegisterHotKey, Shift+A would eat every capital A typed anywhere.
HotkeyProblem ValidateHotkeyCombo(const HotkeyCombo& combo);
HotkeyProblem ValidateHotkeySpec(std::string_view spec);

// One plain sentence for a problem, for set_config's rejection and the capture control.
const char* HotkeyProblemText(HotkeyProblem problem);

}  // namespace ombra
