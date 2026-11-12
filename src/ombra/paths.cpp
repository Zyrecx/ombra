#include "ombra/paths.h"
#include "ombra/brand.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cstdint>
#include <cstdio>
#include <cwctype>

namespace ombra {

namespace {

constexpr wchar_t kInstalledPipeName[]  = L"\\\\.\\pipe\\" OMBRA_INSTALL_ID "Engine";
constexpr wchar_t kInstalledMutexName[] = L"Local\\" OMBRA_INSTALL_ID "EnginePipeInstance";

std::wstring KnownFolderOmbraDir(REFKNOWNFOLDERID id) {
    PWSTR base = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, 0, nullptr, &base);
    std::wstring dir = (SUCCEEDED(hr) && base) ? std::wstring(base) + L"\\" OMBRA_INSTALL_ID : std::wstring();
    CoTaskMemFree(base);
    return dir;
}

std::wstring FullPathWithoutTrailingSlash(const std::wstring& path) {
    std::wstring out = path;
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed > 0) {
        std::wstring buf(needed, L'\0');
        const DWORD n = GetFullPathNameW(path.c_str(), needed, buf.data(), nullptr);
        if (n > 0 && n < needed) out.assign(buf.data(), n);
    }
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

// NTFS paths are case-insensitive, so the instance id is too.
uint64_t Fnv1a64CaseInsensitive(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        const uint16_t u = static_cast<uint16_t>(std::towlower(c));
        h = (h ^ (u & 0xFF)) * 1099511628211ull;
        h = (h ^ (u >> 8)) * 1099511628211ull;
    }
    return h;
}

}  // namespace

InstancePaths ResolveInstancePaths(const std::wstring& home) {
    InstancePaths p;
    if (home.empty()) {
        p.config_dir     = KnownFolderOmbraDir(FOLDERID_RoamingAppData);
        p.data_dir       = KnownFolderOmbraDir(FOLDERID_LocalAppData);
        p.pipe_name      = kInstalledPipeName;
        p.instance_mutex = kInstalledMutexName;
        return p;
    }

    const std::wstring dir = FullPathWithoutTrailingSlash(home);
    wchar_t suffix[24];
    swprintf_s(suffix, L"-%016llx", static_cast<unsigned long long>(Fnv1a64CaseInsensitive(dir)));
    p.config_dir     = dir;
    p.data_dir       = dir;
    p.pipe_name      = std::wstring(kInstalledPipeName) + suffix;
    p.instance_mutex = std::wstring(kInstalledMutexName) + suffix;
    p.isolated       = true;
    return p;
}

const InstancePaths& Instance() {
    static const InstancePaths instance = [] {
        std::wstring home;
        const DWORD needed = GetEnvironmentVariableW(L"OMBRA_HOME", nullptr, 0);
        if (needed > 1) {
            home.resize(needed);
            const DWORD n = GetEnvironmentVariableW(L"OMBRA_HOME", home.data(), needed);
            home.resize(n < needed ? n : 0);
        }
        return ResolveInstancePaths(home);
    }();
    return instance;
}

std::wstring SetupCompletedPath() {
    const std::wstring& dir = Instance().config_dir;
    return dir.empty() ? L"" : dir + L"\\setup_completed";
}

bool SetupCompleted() {
    const std::wstring path = SetupCompletedPath();
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring FallbackClipsDir() {
    if (Instance().isolated) return Instance().data_dir + L"\\fallback-clips";
    PWSTR base = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &base);
    std::wstring dir = (SUCCEEDED(hr) && base) ? std::wstring(base) + L"\\" OMBRA_PRODUCT_NAME : std::wstring();
    CoTaskMemFree(base);
    return dir;
}

std::wstring ThumbsDir() {
    const std::wstring& base = Instance().data_dir;
    return base.empty() ? std::wstring() : base + L"\\thumbs";
}

std::wstring MonitorThumbPath(const std::string& device) {
    const std::wstring dir = ThumbsDir();
    if (dir.empty()) return {};
    std::wstring name(device.begin(), device.end());  // "primary" or "\\.\DISPLAYn": ASCII
    for (wchar_t& c : name) {
        if (c == L'\\' || c == L':' || c == L'.' || c == L'/') c = L'_';
    }
    return dir + L"\\mon_" + name + L".png";
}

std::wstring WindowThumbPath(uint64_t hwnd) {
    const std::wstring dir = ThumbsDir();
    if (dir.empty()) return {};
    wchar_t name[40];
    swprintf_s(name, L"\\win_%016llX.png", static_cast<unsigned long long>(hwnd));
    return dir + name;
}

}  // namespace ombra
