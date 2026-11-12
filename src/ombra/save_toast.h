// What a save tells its toast (engine.cpp's SpawnSaveToast -> ombra-ui --toast save): the engine
// writes this into a small inheritable file mapping before it signals the result's event, and the
// toast reads it once that event fires. Header-only: ombra_core and ombra_ui_core both use it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ombra {

struct SaveToastInfo {
    uint32_t clips = 0;     // clips written (one per monitor saved)
    float    seconds = 0;   // the longest clip's length; 0 = unknown
};

// "30 s", "2 min", "1 min 30 s": rounded to the second.
inline std::wstring FormatClipLength(double seconds) {
    const long long total = (std::max)(1LL, std::llround(seconds));
    if (total < 60) return std::to_wstring(total) + L" s";
    const long long m = total / 60, s = total % 60;
    return std::to_wstring(m) + L" min" + (s ? L" " + std::to_wstring(s) + L" s" : L"");
}

// The toast's title for a saved action: "Saved last 30 s", "Saved 2 clips, last 30 s".
inline std::wstring SavedToastTitle(const SaveToastInfo& info) {
    if (info.seconds <= 0) return info.clips > 1 ? L"Saved " + std::to_wstring(info.clips) + L" clips" : L"Clip saved";
    const std::wstring last = L"last " + FormatClipLength(info.seconds);
    if (info.clips > 1) return L"Saved " + std::to_wstring(info.clips) + L" clips, " + last;
    return L"Saved " + last;
}

}  // namespace ombra
