#pragma once

// DiskLocation: where a disk-backed replay buffer lives (buffer.storage=file), and whether the buffer
// file fits there. Replay & Quality's Folder row shows the folder in a PathControl and Caption() under
// it, red when it doesn't fit. The old Folder row showed only "Browse…" while buffer.file_path was
// empty (the default), so nobody could tell where the file went, which drive paid for it, or whether
// it fit.

#include <string>

namespace ombra_ui {

struct DiskLocation {
    std::wstring folder;             // the folder shown, the default one when buffer.file_path is empty
    bool         is_default = false; // folder is the default (buffer.file_path empty)
    std::wstring drive;              // "C:", or a share's root, for the caption
    double       free_bytes = -1.0;  // free on that drive for this user; < 0 unknown
    double       file_bytes = -1.0;  // the buffer file once allocated (staged config); < 0 unknown
    double       reusable_bytes = 0.0;  // an existing buffer file there, replaced on the next start

    // Space the file can take: what's free plus the old file it replaces.
    double AvailableBytes() const { return free_bytes + reusable_bytes; }
    bool   Fits() const { return file_bytes < 0.0 || free_bytes < 0.0 || file_bytes <= AvailableBytes(); }
    std::wstring Caption() const;
};

}  // namespace ombra_ui
