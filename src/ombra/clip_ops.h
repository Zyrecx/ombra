// File operations on saved clips shared by ombra-ui's Clips screen and `ombra.exe export`: the
// Recycle Bin, replacing an original with its edit, and the lock an export holds on its source.
#pragma once

#include <windows.h>

#include <string>

namespace ombra {

// Moves `path` to the Recycle Bin, silently. An isolated instance (OMBRA_HOME: tests, --drive)
// deletes instead: the user's Recycle Bin is not the sandbox's. True only if the file is gone.
// Runs IFileOperation on its own STA thread, so any caller apartment works.
bool RecycleClip(const std::wstring& path, HWND owner = nullptr);

// The original to the Recycle Bin, then `edited` under its name with its creation and write
// times (it keeps its place in the grid and in Explorer). False with both files where they were
// if the original couldn't be recycled; if only the rename fails, the original is in the Recycle
// Bin and the edit keeps its own name. Nothing may hold `src` open.
bool ReplaceWithEdited(const std::wstring& src, const std::wstring& edited);

// Held by `ombra.exe export` for as long as it works on a clip, so an overlay opened meanwhile
// leaves that clip alone (no player holding it open, no second edit, no delete). Per instance
// (OMBRA_HOME) and per normalised path.
class ClipBusyLock {
public:
    explicit ClipBusyLock(const std::wstring& clip);
    ~ClipBusyLock();
    ClipBusyLock(const ClipBusyLock&) = delete;
    ClipBusyLock& operator=(const ClipBusyLock&) = delete;

private:
    HANDLE mutex_ = nullptr;
};

// Whether some process holds a ClipBusyLock on `clip`.
bool ClipBusy(const std::wstring& clip);

}  // namespace ombra
