#include "ombra/util.h"

#include "ombra/log.h"

#include <vector>

namespace ombra {

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

int64_t QpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t QpcFrequency() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
}

int DisplayRefreshHz(const std::string& device) {
    std::wstring name;
    if (device.empty() || device == "primary") {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(primary, &mi)) return 0;
        name = mi.szDevice;
    } else {
        name = WideFromUtf8(device);
    }
    // The active path's target refresh is a rational (143980/1000 on a 143.98 Hz panel), where
    // DEVMODE's dmDisplayFrequency truncates it to 143.
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                               nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size      = sizeof(source);
                source.header.adapterId = paths[i].sourceInfo.adapterId;
                source.header.id        = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
                if (_wcsicmp(source.viewGdiDeviceName, name.c_str()) != 0) continue;
                const DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (r.Numerator > 0 && r.Denominator > 0)
                    return static_cast<int>((static_cast<uint64_t>(r.Numerator) + r.Denominator / 2) / r.Denominator);
            }
        }
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return 0;
    // 0 and 1 both mean "hardware default" per the DEVMODE docs, not a real rate.
    return dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0;
}

size_t FirstCreatableDirOffset(const std::wstring& dir) {
    const size_t colon = dir.find(L':');
    if (colon != std::wstring::npos) return colon + 1;
    const bool long_unc = dir.rfind(L"\\\\?\\UNC\\", 0) == 0;
    if (!long_unc && dir.rfind(L"\\\\", 0) != 0 && dir.rfind(L"//", 0) != 0) return 0;
    // \\server\share is the root: neither part can be created, and both must be there already.
    const size_t server_end = dir.find_first_of(L"\\/", long_unc ? 8 : 2);
    if (server_end == std::wstring::npos) return std::wstring::npos;
    return dir.find_first_of(L"\\/", server_end + 1);
}

bool EnsureParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    const std::wstring dir = path.substr(0, slash);
    if (dir.empty()) return true;

    size_t pos = FirstCreatableDirOffset(dir);
    if (pos == std::wstring::npos) return true;  // a share's root: FolderTakesFile tests it by writing
    while (true) {
        pos = dir.find_first_of(L"\\/", pos + 1);
        const std::wstring prefix = (pos == std::wstring::npos) ? dir : dir.substr(0, pos);
        if (!prefix.empty() && !CreateDirectoryW(prefix.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            Log::Warn("could not create output directory for \"%ls\"", path.c_str());
            return false;
        }
        if (pos == std::wstring::npos) break;
    }
    return true;
}

bool FolderTakesFile(const std::wstring& path, uint64_t bytes) {
    if (!EnsureParentDir(path)) return false;
    const std::wstring folder = path.substr(0, path.find_last_of(L"\\/") + 1);
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(folder.empty() ? L"." : folder.c_str(), &free_bytes, nullptr, nullptr)) return false;
    if (free_bytes.QuadPart < bytes) return false;
    HANDLE probe = CreateFileW((path + L".probe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return false;
    CloseHandle(probe);
    return true;
}

std::wstring PickWritablePath(const std::wstring& path, const std::wstring& fallback, uint64_t bytes) {
    if (FolderTakesFile(path, bytes)) return path;
    if (!fallback.empty() && FolderTakesFile(fallback, bytes)) return fallback;
    return {};
}

bool WindowsAppsUseLightTheme() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light != 0;
}

}  // namespace ombra
