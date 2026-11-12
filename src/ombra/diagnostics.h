// "Get logs": one .zip of the last days' logs, telemetry, crash reports and config, scrubbed, for a
// GitHub issue or (later) the "Send logs to developer" upload.
//
// Scrubbing happens here, on export, never on write:
//   - every «...» span (log.h: window titles, device names) becomes «redacted»;
//   - an absolute path keeps only its last part: C:\Users\Ana\Videos\Ombra\Game 2026.mp4 ->
//     <path>\Game 2026.mp4, so exe and clip names survive and folders don't;
//   - the Windows user name and the computer name become <user> and <computer> wherever they appear.
// Crash dumps are memory snapshots and can't be scrubbed; the bundle's README says so.
//
// Runs in the overlay as well as the engine: a user whose engine won't start still needs it.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ombra {

struct ScrubContext {
    // Replaced case-insensitively, longest first. Entries shorter than 3 characters are ignored
    // (a one-letter user name would eat every word).
    std::vector<std::pair<std::string, std::string>> names;
};

// The user name, the profile folder's name (they can differ) and the computer name.
ScrubContext MachineScrubContext();

std::string ScrubText(std::string_view text, const ScrubContext& ctx);

// Ombra's version, the Windows build and each GPU with its driver.
std::string SystemInfo();
// SystemInfo without Ombra's version: a crash report names the crashed build in its release, and
// the process sending it may be a newer one.
std::string MachineInfo();

struct BundleOptions {
    int         days  = 3;   // today and the days before it, by the date in each file's name
    std::string today;       // YYYYMMDD; empty = the local date now (tests pin it)
};

struct BundleResult {
    bool        ok    = false;
    size_t      files = 0;
    size_t      bytes = 0;  // the zip's size
    std::string error;
};

// <Downloads>\Ombra logs YYYY-MM-DD HH-MM.zip ("-2", "-3" ... if taken); empty if there's no
// Downloads folder.
std::wstring DefaultBundlePath();

// Writes the zip to `zip_path` (its folder must exist). Reads this instance's data dir (paths.h)
// and config.
BundleResult WriteDiagnosticsBundle(const std::wstring& zip_path, const BundleOptions& options = {});

// A new GitHub issue on the bug report form (.github/ISSUE_TEMPLATE/bug_report.yml), its version,
// Windows and GPU fields filled from `machine_info` (MachineInfo's text) and, with a `reference`
// (a Send logs event id), the logs field saying so. Nothing else: the user writes the rest.
std::string BugReportUrl(const std::string& machine_info, const std::string& reference);

}  // namespace ombra
