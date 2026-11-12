// Logging for the engine, the overlay and the toasts.
//
// File: <data_dir>\logs\ombra-YYYYMMDD.log, one per local day, shared by every Ombra process. Each
// line is one WriteFile on a FILE_APPEND_DATA handle, so lines from different processes interleave
// whole, never torn. Line format (scripts/telemetry.py parses it):
//   [HH:MM:SS.mmm] [LEVEL] [P pid thread] message
// P is the process: E engine, U overlay, T toast. thread is the name given by SetThreadName, or the
// thread id. Every line is also echoed to stdout unless SetEcho(false).
//
// Levels say who has to act: ERROR = the user lost something (a save, footage, the engine); WARN =
// degraded but recovered, or a fallback was taken; INFO = a state change, logged once; DEBUG =
// detail, off unless log.level is debug.
//
// Private text: a window title, a device's friendly name or anything else a user typed or named
// goes inside « » (U+00AB/U+00BB). Logs are written as is, for local debugging; an export
// (diagnostics.h) replaces each «...» span, user paths and the user's and computer's names.
//
// Repeat control: per call site (the format string's address) and text with digits ignored, at most
// kBurst lines per 30 s; the next line after the window says how many were suppressed. Shutdown()
// prints what's pending.
//
// Only the engine prunes (EnablePruning): every *.log in the logs directory, oldest first, today's
// never, at enable time and at each day rollover.
//
// HARD CONSTRAINT, already paid for once: nothing on the pacer's per-tick path (Pacer::Run's loop)
// may call any of these. They take a mutex and do file I/O; a single printf there caused a measured
// 461 ms stall. Accumulate per-tick data in atomics and log a summary from elsewhere.
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

#if defined(__clang__)
#define OMBRA_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define OMBRA_PRINTF(fmt_idx, arg_idx)
#endif

namespace ombra {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// "debug" -> Debug, anything else -> Info (log.level only offers info|debug).
LogLevel ParseLogLevel(std::string_view s);

class Log {
public:
    // Opens today's file for append (creating the logs directory). Returns its path, or empty when
    // there is no file logging. Safe to call more than once; logging before Init() opens the file.
    static std::wstring Init();

    // E (engine, the default), U (overlay) or T (toast). Set before the first line.
    static void SetProcessTag(char tag);

    // Names the calling thread in log lines and, via SetThreadDescription, in debuggers and dumps.
    // `name` must outlive the thread (a string literal).
    static void SetThreadName(const char* name);

    // Lines below `level` are dropped. Live; the engine applies log.level on Save.
    static void SetLevel(LogLevel level);
    static LogLevel Level();
    static bool DebugEnabled() { return Level() <= LogLevel::Debug; }

    // Engine only: prune the logs directory to `max_total_bytes` now and at each day rollover.
    static void EnablePruning(size_t max_total_bytes);

    // Turns the stdout echo off or on (on by default); the file still gets every line.
    static void SetEcho(bool on);

    // Prints pending suppression counts and closes the file. Logging afterwards reopens it.
    static void Shutdown();

    static void Debug(const char* fmt, ...) OMBRA_PRINTF(1, 2);
    static void Info(const char* fmt, ...) OMBRA_PRINTF(1, 2);
    static void Warn(const char* fmt, ...) OMBRA_PRINTF(1, 2);
    static void Error(const char* fmt, ...) OMBRA_PRINTF(1, 2);

    // The fatal handler's line (crash.cpp): an ERROR written as `thread`, from any thread, without
    // waiting on the log mutex for more than ~200 ms (the crashed thread may hold it) and without
    // heap allocation. Appends are atomic per WriteFile, so writing unlocked can't tear a line.
    static void Emergency(const char* thread, const char* text);
    // The calling thread's SetThreadName name, or nullptr.
    static const char* CurrentThreadName();

    // Wide-format variants for call sites that hold wchar_t text (adapter names, paths).
    static void DebugW(const wchar_t* fmt, ...);
    static void InfoW(const wchar_t* fmt, ...);
    static void WarnW(const wchar_t* fmt, ...);
    static void ErrorW(const wchar_t* fmt, ...);
};

// Exposed for tests.
namespace log_detail {
constexpr int kBurst          = 5;
constexpr int kRepeatWindowMs = 30000;
HANDLE OpenAppend(const std::wstring& path);
bool   AppendLine(HANDLE file, std::string_view line);
// Deletes the oldest *.log files in `dir` (never `keep_name`) until the total is <= max_bytes.
void   PruneDir(const std::wstring& dir, const std::wstring& keep_name, size_t max_bytes);
// Clears the repeat-control table (tests run many lines through one call site).
void   ResetRepeatState();
}  // namespace log_detail

}  // namespace ombra
