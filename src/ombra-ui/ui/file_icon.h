#pragma once

// A program's icon as premultiplied BGRA, for widgets that draw a real app icon (the Games list).
// The source is a file on disk: an .exe/.dll/.ico gives its first icon resource at the size asked
// for (SHDefExtractIconW picks the closest image the file has); anything else (Steam's cached
// .jpg) is decoded by WIC at its own size. Decoding happens off the UI thread
// (ScreenHost::RequestFileIconsAsync); the widget turns the bytes into a D2D bitmap in Draw.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ombra_ui {

struct FileIcon {
    std::wstring         path;
    UINT                 w = 0, h = 0;
    std::vector<uint8_t> bgra;  // premultiplied, w*4 stride; empty = no icon (failure or none in the file)
};

// Needs COM initialised on the calling thread (WIC). `px` is the wanted edge in device pixels.
FileIcon LoadFileIcon(const std::wstring& path, UINT px);

}  // namespace ombra_ui
