// The one fatal path for ombra.exe and ombra-ui.exe.
//
// die(), an unhandled SEH exception, std::terminate, abort(), an invalid CRT parameter and a pure
// virtual call all end here: one ERROR line in the log, a minidump (thread stacks and the memory
// they point at, never the ring) in <data_dir>\crashes, then TerminateProcess. Not exit(): that runs
// static destructors under live threads.
//
// The faulting thread only records what happened and wakes a thread created at Install(); that
// thread logs, writes the dump and terminates, because the faulting thread may have no stack left
// (an overflow) and the heap may be corrupt. Nothing on the crash path allocates.
//
// Not caught: __fastfail (/GS cookie failures, some CRT checks) goes straight to the kernel and
// WER, past every in-process handler.
//
// Not installed under a debugger, under ASan (it has its own reports), in ombra-tests' own process
// (doctest has its handlers) or in the amfprobe child (its parent handles its crashes). Without
// Install(), Fatal() keeps the old behaviour: log, then exit(1).
#pragma once

#include <optional>
#include <string>

namespace ombra::crash {

// `exe` names the dumps and the pending marker: "ombra" or "ombra-ui". A string literal.
// Creates the crashes directory and loads dbghelp now, so the crash path has nothing to set up.
// Returns false (and installs nothing) under a debugger, ASan, or when no data_dir is resolvable.
bool Install(const char* exe);
bool Installed();

// die(): log `reason`, dump, terminate with exit code 1.
[[noreturn]] void Fatal(const char* reason);

// The previous run of `exe` crashed: what its fatal line said and the dump's file name. Cleared by
// reading, so each crash is reported by exactly one later start.
struct PreviousCrash {
    std::string  reason;
    std::wstring dump_name;
};
std::optional<PreviousCrash> TakePreviousCrash(const char* exe);

// Records diagnostics.sharing for the fatal path: a crash is only ever reported if this was on (and
// setup was finished) when it happened. The engine calls it at start and on every config change.
// `usage`: sharing is at the usage stop, which adds the install ID to what is sent.
void SetSharingEnabled(bool enabled, bool usage = false);

// Keeps the newest `keep` dumps in the crashes directory (every exe), with their .txt. The engine
// calls it at startup.
void PruneDumps(int keep);

// A crash before setup was finished is never sent on its own: its .txt says `share: ask`, and the
// overlay asks once whether to send it. The newest such dump nobody has answered for, at most
// kMaxAskAgeDays old (its base name: "<exe>-YYYYMMDD-HHMMSS-<pid>"), or empty.
std::wstring UnansweredCrash();
// The answer for `base`. Send leaves `<base>.asked` for the upload child (crash_upload.h), which
// sends it whatever the sharing setting is; Don't send marks it done. Either way older unanswered
// dumps are marked done too, so the overlay asks about one crash once.
void AnswerCrash(const std::wstring& base, bool send);
constexpr int kMaxAskAgeDays = 14;

// A random 32-hex id, the form Sentry wants for an event id: the overlay names a Send logs upload
// with one before the child starts, so the reference exists whatever happens to the post.
std::string NewReportId();
// The overlay's zip for one Send logs, in data_dir: this + the id's first 8 + ".zip". The child
// (crash_upload.h RunSendLogs) deletes it after posting.
inline constexpr wchar_t kSendLogsZipPrefix[] = L"send-logs-";

// Exit codes a crash ends with (an SEH crash exits with its exception code instead).
constexpr unsigned kExitDie   = 1;
constexpr unsigned kExitAbort = 3;  // terminate, abort, purecall, invalid parameter: abort()'s code

// The exception code a dump records for die() and for terminate/abort/purecall/invalid parameter,
// which have no real exception (customer-defined bit set).
constexpr unsigned long kDieExceptionCode   = 0xE04F0001;
constexpr unsigned long kAbortExceptionCode = 0xE04F0002;

// Re-installs the unhandled-exception filter, with a WARN if something (a driver or injected DLL
// loaded since Install) had replaced it. The engine calls it once the pipelines are up.
void RecheckFilter();

// Minidump user stream type holding one FlightRecorder object's bytes (flight_recorder.h layout).
constexpr unsigned kFlightRecorderStream = 0x4F4D0001;  // 'OM' 1; above LastReservedStream

// For the crash tests' child mode.
std::wstring CrashesDir();

}  // namespace ombra::crash
