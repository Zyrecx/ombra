// ombra.com: the console twin of ombra.exe. A bare `ombra` in cmd, PowerShell or a .bat runs it
// (.COM comes before .EXE in PATHEXT), and a console program is waited for and its output captured,
// which ombra.exe (GUI subsystem, so the resident engine never holds a console) can't give.
// Only the command line (cli.h), none of the engine.

#include "ombra/cli.h"

int wmain(int argc, wchar_t** argv) {
    return ombra::RunCli(argc, argv);
}
