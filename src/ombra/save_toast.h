// What a save tells its toast (engine.cpp's SpawnSaveToast -> ombra-ui --toast save): the engine
// writes this into a small inheritable file mapping before it signals the result's event, and the
// toast reads it once that event fires. Header-only: ombra_core and ombra_ui_core both use it.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ombra {

// An API app name is 1-64 bytes of UTF-8 (app_name.h ValidAppName): at most 64 UTF-16 units.
inline constexpr size_t kToastAppUnits = 64;

struct SaveToastInfo {
    uint32_t clips = 0;     // clips written (one per monitor saved)
    float    seconds = 0;   // the longest clip's length; 0 = unknown
    // An app's clip still taking footage (its `after`): the QPC its footage ends at, 0 when it's
    // all in already. QPC is one clock across processes. An extension moves it on.
    int64_t  end_qpc = 0;
    // Odd while the engine rewrites end_qpc and app with the toast already running (an extension):
    // write through SetToastLive, read through ReadToastLive.
    uint32_t version = 0;
    // The API app whose action this is, written before the toast starts so every state names it;
    // empty for the user's own save. NUL-terminated; read it through ReadToastLive or ToastApp.
    wchar_t  app[kToastAppUnits + 1] = {};
    // The game the clip is of (its folder name, SanitizeFolderName: at most 64), written with the
    // result before its event is signalled; empty off a game. Read it through ToastGame.
    wchar_t  game[kToastAppUnits + 1] = {};
};

// Copies a game name in, cut to the buffer on a code-point boundary. Engine side, before the signal.
inline void SetToastGame(SaveToastInfo& info, std::wstring_view name) {
    size_t n = (std::min)(name.size(), kToastAppUnits);
    if (n < name.size() && n > 0 && name[n - 1] >= 0xD800 && name[n - 1] <= 0xDBFF) --n;  // a split pair
    std::copy_n(name.data(), n, info.game);
    info.game[n] = L'\0';
}

// The game from a mapping another process wrote: bounded even if it isn't terminated.
inline std::wstring ToastGame(const SaveToastInfo& info) {
    size_t n = 0;
    while (n < kToastAppUnits && info.game[n]) ++n;
    return std::wstring(info.game, n);
}

// A field another process may be writing (atomic_ref<const T> is C++26): loads never write, so a
// read-only view is fine.
template <typename T>
T LoadRelaxed(const T& field) {
    return std::atomic_ref<T>(const_cast<T&>(field)).load(std::memory_order_relaxed);
}

// Copies `name` in, cut to the buffer on a code-point boundary.
inline void SetToastApp(SaveToastInfo& info, std::wstring_view name) {
    size_t n = (std::min)(name.size(), kToastAppUnits);
    if (n < name.size() && n > 0 && name[n - 1] >= 0xD800 && name[n - 1] <= 0xDBFF) --n;  // a split pair
    for (size_t i = 0; i < n; ++i) std::atomic_ref<wchar_t>(info.app[i]).store(name[i], std::memory_order_relaxed);
    std::atomic_ref<wchar_t>(info.app[n]).store(L'\0', std::memory_order_relaxed);
}

// The name from a mapping another process wrote: bounded even if it isn't terminated.
inline std::wstring ToastApp(const SaveToastInfo& info) {
    std::wstring out;
    for (size_t n = 0; n < kToastAppUnits; ++n) {
        const wchar_t c = LoadRelaxed(info.app[n]);
        if (!c) break;
        out += c;
    }
    return out;
}

// The names of every app that asked for one clip, as its toast shows them: "A", "A, B"; past the
// buffer, "A and 2 more" (cut to it if even that doesn't fit).
inline std::wstring ToastAppNames(const std::vector<std::wstring>& apps) {
    if (apps.empty()) return {};
    std::wstring all = apps.front();
    for (size_t i = 1; i < apps.size(); ++i) all += L", " + apps[i];
    if (all.size() <= kToastAppUnits) return all;
    const std::wstring more = L" and " + std::to_wstring(apps.size() - 1) + L" more";
    std::wstring first = apps.front();
    if (first.size() + more.size() > kToastAppUnits) {
        size_t n = kToastAppUnits > more.size() ? kToastAppUnits - more.size() : 0;
        if (n > 0 && first[n - 1] >= 0xD800 && first[n - 1] <= 0xDBFF) --n;  // a split pair
        first.resize(n);
    }
    return first + more;
}

// The engine's one writer per mapping: the end and the name together, as a seqlock.
inline void SetToastLive(SaveToastInfo& info, int64_t end_qpc, std::wstring_view app) {
    std::atomic_ref<uint32_t> version(info.version);
    version.fetch_add(1, std::memory_order_relaxed);  // odd: a write is on
    std::atomic_thread_fence(std::memory_order_release);  // the odd version before any data
    std::atomic_ref<int64_t>(info.end_qpc).store(end_qpc, std::memory_order_relaxed);
    SetToastApp(info, app);
    version.fetch_add(1, std::memory_order_release);
}

struct ToastLive {
    int64_t      end_qpc = 0;
    std::wstring app;
};
// A consistent end and name, though the engine may be rewriting them; nothing when the engine stayed
// mid-write through every try (preempted, or died: the toast also waits on its process). The caller
// keeps what it had.
inline std::optional<ToastLive> ReadToastLive(const SaveToastInfo& info) {
    // Loads only: the toast maps the info read-only.
    std::atomic_ref<uint32_t> version(const_cast<uint32_t&>(info.version));
    for (int tries = 0; tries < 1000; ++tries) {
        const uint32_t before = version.load(std::memory_order_acquire);
        if (before & 1) continue;
        ToastLive live;
        live.end_qpc = LoadRelaxed(info.end_qpc);
        live.app     = ToastApp(info);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (version.load(std::memory_order_relaxed) == before) return live;
    }
    return std::nullopt;
}

// How long a pending save's toast waits past the later of its start and its clip's end before it
// takes the save for lost (an engine hung mid-save; one that exits resolves it at once).
inline constexpr double kPendingSaveMaxSeconds = 120.0;

// Whole seconds left of a clip still taking footage, rounded up; 0 once its end is reached.
inline int64_t ClipSecondsLeft(int64_t end_qpc, int64_t now_qpc, int64_t freq) {
    if (end_qpc <= now_qpc || freq <= 0) return 0;
    return (end_qpc - now_qpc + freq - 1) / freq;
}

// What a toast says: a headline, the detail line under it ("Last 30 s · VALORANT"), and the one
// number a compact pill keeps on its chip ("30 s"). Either of the last two may be empty.
struct ToastText {
    std::wstring title, detail, stat;
};

// Parts of a detail line, joined with a middle dot; empty parts drop out.
inline std::wstring JoinToastDetail(std::initializer_list<std::wstring_view> parts) {
    std::wstring out;
    for (std::wstring_view p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += L" · ";
        out += p;
    }
    return out;
}

// A pending save: an app's clip still taking footage counts down ("Recording clip", "25 s left"),
// then "Saving clip…" while the file is finished.
inline ToastText PendingSaveText(int64_t end_qpc, int64_t now_qpc, int64_t freq) {
    const int64_t left = ClipSecondsLeft(end_qpc, now_qpc, freq);
    if (left <= 0) return {L"Saving clip…", {}, {}};
    const std::wstring n = std::to_wstring(left) + L" s";
    return {L"Recording clip", n + L" left", n};
}

// When that text next changes: the next whole second of the countdown, or the end; 0 for none.
inline int64_t PendingTitleChangeQpc(int64_t end_qpc, int64_t now_qpc, int64_t freq) {
    const int64_t left = ClipSecondsLeft(end_qpc, now_qpc, freq);
    return left <= 0 ? 0 : end_qpc - (left - 1) * freq;
}

// When the toast gives up on a pending save (kPendingSaveMaxSeconds past the later of the two).
inline int64_t PendingGiveUpQpc(int64_t start_qpc, int64_t end_qpc, int64_t freq) {
    return (std::max)(start_qpc, end_qpc) + static_cast<int64_t>(kPendingSaveMaxSeconds * static_cast<double>(freq));
}

// "30 s", "2 min", "1 min 30 s": rounded to the second.
inline std::wstring FormatClipLength(double seconds) {
    const long long total = (std::max)(1LL, std::llround(seconds));
    if (total < 60) return std::to_wstring(total) + L" s";
    const long long m = total / 60, s = total % 60;
    return std::to_wstring(m) + L" min" + (s ? L" " + std::to_wstring(s) + L" s" : L"");
}

// A recording's length: as a clip's under an hour, then "1 h 20 min".
inline std::wstring FormatRecordingLength(double seconds) {
    const long long total = std::llround(seconds);
    if (total < 3600) return FormatClipLength(seconds);
    const long long h = total / 3600, m = total % 3600 / 60;
    return std::to_wstring(h) + L" h" + (m ? L" " + std::to_wstring(m) + L" min" : L"");
}

// A saved recording: "Recording saved", "12 min 5 s"; no detail without a length.
inline ToastText RecordingSavedText(double seconds) {
    if (seconds <= 0) return {L"Recording saved", {}, {}};
    const std::wstring len = FormatRecordingLength(seconds);
    return {L"Recording saved", len, len};
}

// Recordings a crash left fragmented, made plain at engine start: "Recording recovered" with its
// length, or "3 recordings recovered".
inline ToastText RecordingRecoveredText(double seconds, uint32_t count) {
    if (count > 1) return {std::to_wstring(count) + L" recordings recovered", {}, {}};
    ToastText t = RecordingSavedText(seconds);
    t.title = L"Recording recovered";
    return t;
}

// A saved clip: "Clip saved" (or "2 clips saved"), "Last 30 s · VALORANT".
inline ToastText SavedToastText(const SaveToastInfo& info) {
    ToastText t;
    t.title = info.clips > 1 ? std::to_wstring(info.clips) + L" clips saved" : L"Clip saved";
    if (info.seconds > 0) t.stat = FormatClipLength(info.seconds);
    t.detail = JoinToastDetail({t.stat.empty() ? std::wstring{} : L"Last " + t.stat, ToastGame(info)});
    return t;
}

// The detail an app's action shows: the app first, so a tool's action is never anonymous, even
// with the detail line turned off (`details` false keeps only the app).
inline std::wstring AppToastDetail(std::wstring_view app, std::wstring_view detail, bool details) {
    return JoinToastDetail({app, details ? detail : std::wstring_view{}});
}

}  // namespace ombra
