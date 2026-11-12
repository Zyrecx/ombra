#include "ombra-ui/ui/disk_location.h"

#include "ombra-ui/ui/widget.h"  // FormatMemory

namespace ombra_ui {

std::wstring DiskLocation::Caption() const {
    std::wstring s = is_default ? L"Default folder · " : L"";
    const std::wstring on = drive.empty() ? L"" : L" on " + drive;
    if (free_bytes < 0.0) return s + L"Free space unknown" + on;
    if (file_bytes < 0.0) return s + FormatMemory(free_bytes) + L" free" + on;
    if (!Fits())
        return s + L"Needs " + FormatMemory(file_bytes) + L", only " + FormatMemory(AvailableBytes()) + L" free" + on;
    return s + L"Buffer file " + FormatMemory(file_bytes) + L" · " + FormatMemory(free_bytes) + L" free" + on;
}

}  // namespace ombra_ui
