// Small helpers shared by the engine and the test harness.
#pragma once

#include <windows.h>
#include <objbase.h>  // CoInitializeEx; WIN32_LEAN_AND_MEAN leaves it out

#include <cstdint>
#include <string>

namespace ombra {

std::string  NarrowUtf8(const std::wstring& w);
std::wstring WideFromUtf8(const std::string& s);

int64_t QpcNow();
int64_t QpcFrequency();

// Creates every missing directory in `path`'s parent chain. Returns false (and logs) instead of
// dying: a save target that can't be created is a failed save, never a dead engine.
bool EnsureParentDir(const std::wstring& path);
// Where EnsureParentDir's walk starts: past C: or \\server\share, npos when `dir` is a share's root.
size_t FirstCreatableDirOffset(const std::wstring& dir);

// Whether a file of about `bytes` can go at `path`: creates its folder, checks the disk's room and
// that the folder takes a new file. False is a removed drive, a full disk or a read-only folder.
bool FolderTakesFile(const std::wstring& path, uint64_t bytes);

// `path` if it can take the file, else `fallback` if that can (empty: no fallback), else empty.
std::wstring PickWritablePath(const std::wstring& path, const std::wstring& fallback, uint64_t bytes);

// Current refresh rate of `device` ("primary", or a `\\.\DISPLAYn` name as MonitorSpec carries
// it), rounded to the nearest Hz from the exact rate the display path runs at (143.98 -> 144,
// 59.94 -> 60). Falls back to DEVMODE's whole-Hz figure, which rounds down. 0 if neither reads.
int DisplayRefreshHz(const std::string& device);

// Windows' "Choose your app mode" (Settings > Personalisation > Colours): true for Light, and when
// the value can't be read (Windows' own default).
bool WindowsAppsUseLightTheme();

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

}  // namespace ombra
