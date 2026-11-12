// Ombra engine.
//
// Build: powershell -ExecutionPolicy Bypass -File build.ps1
// Run:   build\Release\ombra.exe                                    (the real default: resident
//          forever, no argv. Creates a hidden window the tray routes to, starts PipeServer on
//          \\.\pipe\OmbraEngine, and runs the capture/encode/pace/hotkey machinery. Ring length is
//          config's buffer.seconds (60s). Exit via the window's close or Ctrl+C in the console. No
//          auto-save on exit — that behavior is timedtest's, not the resident engine's.)
//        build\Release\ombra.exe timedtest [total_seconds] [buffer_seconds] [output.mp4]
//          (an explicit scriptable test harness. default: 15s captured; buffer_seconds defaults to
//          config's buffer.seconds unless overridden here; Alt+F10 (or config's
//          hotkeys.save_replay) saves; a final auto-save also fires at total_seconds if the hotkey
//          was never pressed)
//        build\Release\ombra.exe gputest [--pid <n>]                 (GPU topology)
//        build\Release\ombra.exe capturebench                        (draws a small window on
//          screen for a few seconds to measure capture latency; run this yourself, it is never
//          launched automatically, see capturebench.cpp's safety contract)
//        build\Release\ombra.exe amfprobe                            (the out-of-process AMF
//          probe the engine spawns, amf_probe.h)
//        build\Release\ombra.exe --help                              (every config key)
// Tests live in ombra-tests.exe (tests/).

#include "ombra/engine.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>

// ombra.exe is WIN32-subsystem so the resident engine (Run-key autostart, ombra-ui's
// CREATE_NO_WINDOW spawn) never flashes or holds open a console window — the thing this change
// exists to fix. Mirrors ombra-ui/main.cpp's RunUi(): only rebind to the console screen buffer
// when stdout/stderr aren't already pointing somewhere (freopen to CONOUT$ unconditionally would
// silently discard output redirected to a file by a test script), so `timedtest`/`gputest`/etc.
// invoked from an interactive terminal still print into it.
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }

    wchar_t*  cmdline = GetCommandLineW();
    int       argc    = 0;
    wchar_t** argv    = CommandLineToArgvW(cmdline, &argc);
    const int rc      = RunEngine(argc, argv);
    if (argv) LocalFree(argv);
    return rc;
}
