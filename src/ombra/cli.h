// Ombra's command line: `ombra clip`, `ombra marker`... A thin client of the public pipe API
// (protocol.h), the same commands any app can send, built into two binaries: the console twin
// ombra.com (what a bare `ombra` runs from a shell) and ombra.exe (for callers that find only
// .exe, such as Python's subprocess without a shell).
//
// Exit codes are a public contract: 0 ok, 1 error, 2 usage, 3 Ombra isn't running, 4 not allowed,
// 5 Instant Replay off, 6 nothing new to save.
#pragma once

namespace ombra {

// `arg` (argv[1]) names a command-line command: the engine hands it to RunCli before anything of
// its own (log, crash handler, instance lock) starts.
bool IsCliCommand(const wchar_t* arg);

// argv[1] is the command. Never starts the engine, except `start`.
int RunCli(int argc, wchar_t** argv);

// The usage text, to stdout (exit 0) or, for a usage error, stderr (exit 2).
int PrintCliUsage(bool error);

}  // namespace ombra
