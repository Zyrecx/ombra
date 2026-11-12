#include "ombra/log.h"

#include "ombra/paths.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ombra {

namespace {

constexpr size_t kMaxLineBytes = 4096;  // truncated beyond this, not fatal

std::mutex            g_mutex;
HANDLE                g_file      = INVALID_HANDLE_VALUE;
bool                  g_opened    = false;  // an open was attempted for g_file_day
WORD                  g_file_day  = 0;
size_t                g_prune_max = 0;      // 0: this process doesn't prune
std::atomic<int>      g_level{static_cast<int>(LogLevel::Info)};
std::atomic<bool>     g_echo{true};
char                  g_process   = 'E';
thread_local const char* t_thread_name = nullptr;

struct Repeat {
    ULONGLONG   window_start = 0;
    int         in_window    = 0;
    int         suppressed   = 0;
    const char* level_tag    = "";
    std::string sample;  // the last suppressed line's text, for Shutdown's summary
};
std::unordered_map<uint64_t, Repeat> g_repeat;

// The repeat key: the call site plus the text with digit runs left out, so one site logging the
// same thing in a loop collapses while distinct lines from one format (per-adapter, per-stage) don't.
uint64_t RepeatKey(const void* site, const char* text) {
    uint64_t h = 1469598103934665603ull ^ reinterpret_cast<uintptr_t>(site);
    for (const char* p = text; *p; ++p) {
        if (*p >= '0' && *p <= '9') continue;
        h = (h ^ static_cast<unsigned char>(*p)) * 1099511628211ull;
    }
    return h;
}

std::wstring LogsDir() {
    const std::wstring& base = Instance().data_dir;
    return base.empty() ? std::wstring() : base + L"\\logs";
}

std::wstring FileNameFor(const SYSTEMTIME& st) {
    wchar_t buf[32];
    swprintf_s(buf, L"ombra-%04u%02u%02u.log", st.wYear, st.wMonth, st.wDay);
    return buf;
}

void CloseLocked() {
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
}

void OpenLocked(const SYSTEMTIME& st) {
    CloseLocked();
    g_opened   = true;
    g_file_day = st.wDay;
    const std::wstring dir = LogsDir();
    if (dir.empty()) return;  // no file logging; the stdout echo still works
    // Not EnsureParentDir: it logs on failure, and this runs under g_mutex. data_dir and logs are
    // the two levels that may be missing.
    CreateDirectoryW(Instance().data_dir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring name = FileNameFor(st);
    if (g_prune_max) log_detail::PruneDir(dir, name, g_prune_max);
    g_file = log_detail::OpenAppend(dir + L"\\" + name);
}

void EnsureOpenLocked(const SYSTEMTIME& st) {
    // A resident engine runs for days: roll to the new day's file (and prune) at midnight.
    if (!g_opened || st.wDay != g_file_day) OpenLocked(st);
}

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "INFO ";
    }
}

void EmitLocked(const SYSTEMTIME& st, const char* level_tag, const char* text) {
    char thread[24];
    if (t_thread_name) {
        _snprintf_s(thread, sizeof(thread), _TRUNCATE, "%s", t_thread_name);
    } else {
        _snprintf_s(thread, sizeof(thread), _TRUNCATE, "%lu", GetCurrentThreadId());
    }
    char line[kMaxLineBytes + 96];
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] [%s] [%c %lu %s] %s\n",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level_tag, g_process,
                              GetCurrentProcessId(), thread, text);
    const size_t len = n < 0 ? std::strlen(line) : static_cast<size_t>(n);
    if (g_echo.load(std::memory_order_relaxed)) std::fwrite(line, 1, len, stdout);
    if (g_file != INVALID_HANDLE_VALUE) log_detail::AppendLine(g_file, std::string_view(line, len));
}

void WriteLine(LogLevel level, const void* site, const char* text) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureOpenLocked(st);

    Repeat& r = g_repeat[RepeatKey(site, text)];
    if (r.window_start == 0 || now - r.window_start >= static_cast<ULONGLONG>(log_detail::kRepeatWindowMs)) {
        r.window_start = now;
        r.in_window    = 0;
    }
    if (++r.in_window > log_detail::kBurst) {
        ++r.suppressed;
        r.level_tag = LevelTag(level);
        r.sample    = text;
        return;
    }
    if (r.suppressed) {
        char buf[kMaxLineBytes + 64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (and %d more like it suppressed)", text, r.suppressed);
        r.suppressed = 0;
        r.sample.clear();
        EmitLocked(st, LevelTag(level), buf);
        return;
    }
    EmitLocked(st, LevelTag(level), text);
}

void LogV(LogLevel level, const char* fmt, va_list args) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;
    char text[kMaxLineBytes];
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, args);
    WriteLine(level, fmt, text);
}

void LogVW(LogLevel level, const wchar_t* fmt, va_list args) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;
    wchar_t wtext[kMaxLineBytes];
    _vsnwprintf_s(wtext, sizeof(wtext) / sizeof(wchar_t), _TRUNCATE, fmt, args);
    char text[kMaxLineBytes];
    const int n = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, text, sizeof(text), nullptr, nullptr);
    WriteLine(level, fmt, n > 0 ? text : "<unencodable log message>");
}

}  // namespace

LogLevel ParseLogLevel(std::string_view s) { return s == "debug" ? LogLevel::Debug : LogLevel::Info; }

namespace log_detail {

HANDLE OpenAppend(const std::wstring& path) {
    // FILE_APPEND_DATA without FILE_WRITE_DATA: every WriteFile goes to the current end of file,
    // atomically against other appenders, so lines from the engine, overlay and toasts never tear
    // (the CRT's "a" mode seeks then writes: not atomic across processes). FILE_SHARE_DELETE lets
    // the engine prune a file another process still holds open.
    return CreateFileW(path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool AppendLine(HANDLE file, std::string_view line) {
    // Unbuffered: this process can be killed at any time, and a logged line must survive it.
    DWORD written = 0;
    return WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
           written == line.size();
}

void PruneDir(const std::wstring& dir, const std::wstring& keep_name, size_t max_bytes) {
    struct Entry { std::wstring name; uint64_t size; };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        entries.push_back({fd.cFileName, (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Oldest first by the YYYYMMDD in the name, whatever the prefix (ombra-, and umbra- from before
    // the rename); a name without a date sorts first.
    auto date_of = [](const std::wstring& name) {
        const size_t dash = name.rfind(L'-');
        return dash == std::wstring::npos ? std::wstring() : name.substr(dash + 1);
    };
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        const std::wstring da = date_of(a.name), db = date_of(b.name);
        return da != db ? da < db : a.name < b.name;
    });

    uint64_t total = 0;
    for (const auto& e : entries) total += e.size;
    for (const auto& e : entries) {
        if (total <= max_bytes) break;
        if (_wcsicmp(e.name.c_str(), keep_name.c_str()) == 0) continue;
        if (DeleteFileW((dir + L"\\" + e.name).c_str())) total -= e.size;
    }
}

void ResetRepeatState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_repeat.clear();
}

}  // namespace log_detail

std::wstring Log::Init() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureOpenLocked(st);
    if (g_file == INVALID_HANDLE_VALUE) return L"";
    return LogsDir() + L"\\" + FileNameFor(st);
}

void Log::SetProcessTag(char tag) { g_process = tag; }

void Log::SetThreadName(const char* name) {
    t_thread_name = name;
    wchar_t wide[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64) > 0) SetThreadDescription(GetCurrentThread(), wide);
}

const char* Log::CurrentThreadName() { return t_thread_name; }

void Log::Emergency(const char* thread, const char* text) {
    // Static, not stack: this runs on the crash thread (crash.cpp), but keep it frame-light anyway.
    static char line[kMaxLineBytes + 96];
    SYSTEMTIME  st{};
    GetLocalTime(&st);
    bool locked = false;
    for (int i = 0; i < 20 && !(locked = g_mutex.try_lock()); ++i) Sleep(10);
    if (locked && (!g_opened || st.wDay != g_file_day)) OpenLocked(st);  // only when the mutex is ours
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] [ERROR] [%c %lu %s] %s\n",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_process,
                              GetCurrentProcessId(), thread, text);
    const size_t len = n < 0 ? std::strlen(line) : static_cast<size_t>(n);
    // _nolock: the crashed thread may hold the stdout stream lock.
    _fwrite_nolock(line, 1, len, stdout);
    _fflush_nolock(stdout);
    const HANDLE file = g_file;
    if (file != INVALID_HANDLE_VALUE) log_detail::AppendLine(file, std::string_view(line, len));
    if (locked) g_mutex.unlock();
}

void Log::SetEcho(bool on) { g_echo.store(on, std::memory_order_relaxed); }

void Log::SetLevel(LogLevel level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

LogLevel Log::Level() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

void Log::EnablePruning(size_t max_total_bytes) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_prune_max = max_total_bytes;
    const std::wstring dir = LogsDir();
    if (!dir.empty() && max_total_bytes) log_detail::PruneDir(dir, FileNameFor(st), max_total_bytes);
}

void Log::Shutdown() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [site, r] : g_repeat) {
        if (!r.suppressed) continue;
        char buf[kMaxLineBytes + 64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d more suppressed, last: %s", r.suppressed, r.sample.c_str());
        r.suppressed = 0;
        EnsureOpenLocked(st);
        EmitLocked(st, r.level_tag, buf);
    }
    CloseLocked();
    g_opened = false;
}

void Log::Debug(const char* fmt, ...) { va_list a; va_start(a, fmt); LogV(LogLevel::Debug, fmt, a); va_end(a); }
void Log::Info(const char* fmt, ...)  { va_list a; va_start(a, fmt); LogV(LogLevel::Info, fmt, a); va_end(a); }
void Log::Warn(const char* fmt, ...)  { va_list a; va_start(a, fmt); LogV(LogLevel::Warn, fmt, a); va_end(a); }
void Log::Error(const char* fmt, ...) { va_list a; va_start(a, fmt); LogV(LogLevel::Error, fmt, a); va_end(a); }

void Log::DebugW(const wchar_t* fmt, ...) { va_list a; va_start(a, fmt); LogVW(LogLevel::Debug, fmt, a); va_end(a); }
void Log::InfoW(const wchar_t* fmt, ...)  { va_list a; va_start(a, fmt); LogVW(LogLevel::Info, fmt, a); va_end(a); }
void Log::WarnW(const wchar_t* fmt, ...)  { va_list a; va_start(a, fmt); LogVW(LogLevel::Warn, fmt, a); va_end(a); }
void Log::ErrorW(const wchar_t* fmt, ...) { va_list a; va_start(a, fmt); LogVW(LogLevel::Error, fmt, a); va_end(a); }

}  // namespace ombra
