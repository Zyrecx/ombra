// The records of what left the PC: crash_upload.cpp writes one text file per upload to
// <data_dir>\sent_reports\ before posting it and appends the result after; Settings lists them.
// The titles below are each record's first line, shared so the writer and this reader can't drift.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ombra::sent_records {

inline constexpr char kCrashTitle[]   = "Crash report ";    // + event id
inline constexpr char kErrorTitle[]   = "Error report ";    // + event id
inline constexpr char kSessionTitle[] = "Session report ";  // + UTC day " (UTC)"
inline constexpr char kGamesTitle[]   = "Game sessions (UTC day in the file name)";
inline constexpr char kDailyTitle[]   = "Daily usage (UTC day in the file name)";
inline constexpr char kLogsTitle[]    = "Logs sent by you ";  // + event id
inline constexpr char kLogsSizeLabel[] = "\nZip: ";           // + "<n> files, <size>"
inline constexpr char kResultPrefix[] = "\nResult: ";       // appended once the post returned

// PruneRecords keeps this many, newest first.
inline constexpr size_t kKeep = 20;

enum class Kind { Crash, Error, Session, GameSessions, DailyUsage, Logs, Unknown };
// Sent: HTTP 2xx. Failed: any other answer, or no connection. Unconfirmed: written but no result,
// so the post was cut off (or is running now) and may or may not have arrived.
enum class Status { Sent, Failed, Unconfirmed };

struct Record {
    Kind         kind = Kind::Unknown;
    Status       status = Status::Unconfirmed;
    std::string  result;     // "HTTP 200", "WinHTTP error 12007"; empty when Unconfirmed
    std::string  summary;    // one line in plain words: what it was about
    std::string  day;        // YYYY-MM-DD for the per-day kinds, else empty
    std::wstring file_name;  // in RecordsDir()
    std::wstring path;
    int64_t      written = 0;  // unix seconds, the file's last write (the post's end when it has a result)
    size_t       bytes = 0;
};

// Pure: what one record file says. `file_name` without a directory.
Record Parse(const std::wstring& file_name, const std::string& text);

// <data_dir>\sent_reports
std::wstring Dir();

// Every record in Dir(), newest first. Reads each file once (they're a few KB to ~100 KB).
std::vector<Record> List();

}  // namespace ombra::sent_records
