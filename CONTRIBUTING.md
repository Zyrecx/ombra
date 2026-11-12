# Contributing to Ombra

Thanks for helping. Bug reports and suggestions go in [issues](../../issues/new/choose), each with a
short form. For code, open an issue first for anything bigger than a small fix, so we can agree on
the shape before you spend time on it.

## Building

Needs Windows 11, Visual Studio Build Tools (MSVC and the Windows SDK), CMake and Ninja.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1   # -> build\Release
build\Release\ombra-tests.exe                        # headless tests, about 20 s (needs ffmpeg on PATH)
build\Release\ombra-tests.exe --gpu                  # adds the encoders and real capture
```

`build.ps1` downloads a pinned, hash-checked nasm for x264. Tests run in an isolated home under
`%TEMP%\ombra-tests\` and never touch your own config, clips or running Ombra.

## How the code is laid out

- `ombra.exe` (`src/ombra/`) is the engine: capture, encoders, the replay buffer, audio, saving, the
  tray and hotkeys. It runs all the time, so it stays small and cheap.
- `ombra-ui.exe` (`src/ombra-ui/`) is the Alt+Z overlay. It starts when opened, closes when hidden,
  and talks to the engine over a named pipe.
- `tests/` holds the doctest suite. New tests go there.

## Rules that keep Ombra working

- **Nothing touches the game process.** Capture is Windows Graphics Capture only: no hooks, no
  injected DLLs. Anti-cheat compatibility depends on it.
- **Windows 11 only.** Check capabilities at runtime; no Windows 10 code paths.
- **Small and cheap.** The engine runs all the time. Say what a change costs in memory and CPU.
- **UI changes need screenshots.** `ombra-ui.exe --drive SCRIPT` renders the overlay headlessly,
  without an engine, to PNGs; attach before and after to the pull request. The command list is at
  the top of `src/ombra-ui/driver.cpp`.
- Copied or ported code keeps a header comment naming its source and licence, and gets an entry in
  `THIRD-PARTY-NOTICES.txt`.

## Licence

Ombra is GPL-3.0-or-later. By contributing you agree your contribution is released under the same
licence.
