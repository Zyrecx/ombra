// What a save tells its toast (engine.cpp's SpawnSaveToast -> ombra-ui --toast save): the engine
// writes this into a small inheritable file mapping before it signals the result's event, and the
// toast reads it once that event fires. Header-only: ombra_core and ombra_ui_core both use it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace ombra {

// An API app name is 1-64 bytes of UTF-8 (pipe.cpp ValidAppName): at most 64 UTF-16 units.
inline constexpr size_t kToastAppUnits = 64;

struct SaveToastInfo {
    uint32_t clips = 0;     // clips written (one per monitor saved)
    float    seconds = 0;   // the longest clip's length; 0 = unknown
    // The API app whose action this is, written before the toast starts so every state names it;
    // empty for the user's own save. NUL-terminated; read it through ToastApp.
    wchar_t  app[kToastAppUnits + 1] = {};
};

// Copies `name` in, cut to the buffer on a code-point boundary.
inline void SetToastApp(SaveToastInfo& info, std::wstring_view name) {
    size_t n = (std::min)(name.size(), kToastAppUnits);
    if (n < name.size() && n > 0 && name[n - 1] >= 0xD800 && name[n - 1] <= 0xDBFF) --n;  // a split pair
    std::copy_n(name.data(), n, info.app);
    info.app[n] = L'\0';
}

// The name from a mapping another process wrote: bounded even if it isn't terminated.
inline std::wstring ToastApp(const SaveToastInfo& info) {
    size_t n = 0;
    while (n < kToastAppUnits && info.app[n]) ++n;
    return std::wstring(info.app, n);
}

// "30 s", "2 min", "1 min 30 s": rounded to the second.
inline std::wstring FormatClipLength(double seconds) {
    const long long total = (std::max)(1LL, std::llround(seconds));
    if (total < 60) return std::to_wstring(total) + L" s";
    const long long m = total / 60, s = total % 60;
    return std::to_wstring(m) + L" min" + (s ? L" " + std::to_wstring(s) + L" s" : L"");
}

// A saved recording's toast title: "Recording saved, 12 min 5 s", "..., 1 h 20 min"; without a
// length, "Recording saved".
inline std::wstring RecordingSavedTitle(double seconds) {
    if (seconds <= 0) return L"Recording saved";
    const long long total = std::llround(seconds);
    if (total < 3600) return L"Recording saved, " + FormatClipLength(seconds);
    const long long h = total / 3600, m = total % 3600 / 60;
    return L"Recording saved, " + std::to_wstring(h) + L" h" + (m ? L" " + std::to_wstring(m) + L" min" : L"");
}

// Recordings a crash left fragmented, made plain at engine start: "Recording recovered, 12 min 5 s",
// or "3 recordings recovered".
inline std::wstring RecordingRecoveredTitle(double seconds, uint32_t count) {
    if (count > 1) return std::to_wstring(count) + L" recordings recovered";
    const std::wstring saved = RecordingSavedTitle(seconds);  // "Recording saved[, length]"
    return L"Recording recovered" + saved.substr(std::wstring_view(L"Recording saved").size());
}

// The toast's title for a saved action: "Saved last 30 s", "Saved 2 clips, last 30 s".
inline std::wstring SavedToastTitle(const SaveToastInfo& info) {
    if (info.seconds <= 0) return info.clips > 1 ? L"Saved " + std::to_wstring(info.clips) + L" clips" : L"Clip saved";
    const std::wstring last = L"last " + FormatClipLength(info.seconds);
    if (info.clips > 1) return L"Saved " + std::to_wstring(info.clips) + L" clips, " + last;
    return L"Saved " + last;
}

}  // namespace ombra
