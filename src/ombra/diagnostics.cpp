#include "ombra/diagnostics.h"

#include "ombra/brand.h"
#include "ombra/config.h"
#include "ombra/paths.h"
#include "ombra/util.h"
#include "ombra/version.h"
#include "ombra/zip_writer.h"

#include <windows.h>
#include <dxgi.h>
#include <lmcons.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace ombra {

namespace {

constexpr std::string_view kOpen   = "\xC2\xAB";  // «
constexpr std::string_view kClose  = "\xC2\xBB";  // »

bool IsSep(char c) { return c == '\\' || c == '/'; }

// A path starts with a drive (C:\ or C:/, not inside a word) or a UNC prefix (\\server), as written
// or JSON-escaped (C:\\, \\\\server: config.json, the settings: line). 0 = no path, 1 = a path,
// 2 = a JSON-escaped one.
int PathStartsAt(std::string_view s, size_t i) {
    auto alnum = [&](size_t k) { return k < s.size() && std::isalnum(static_cast<unsigned char>(s[k])); };
    auto bs    = [&](size_t k) { return k < s.size() && s[k] == '\\'; };
    if (i + 2 < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && s[i + 1] == ':' && IsSep(s[i + 2])) {
        if (i > 0 && alnum(i - 1)) return 0;
        return bs(i + 2) && bs(i + 3) ? 2 : 1;
    }
    // Not after a word, %VAR% or another backslash: "%USERPROFILE%\\Videos" is a JSON-escaped
    // relative path, not \\Videos.
    if (bs(i) && bs(i + 1) && (i == 0 || (!bs(i - 1) && !alnum(i - 1) && s[i - 1] != '%'))) {
        if (bs(i + 2) && bs(i + 3) && alnum(i + 4)) return 2;
        if (alnum(i + 2)) return 1;
    }
    return 0;
}

// Where a path that starts at `i` ends. Paths hold spaces, parentheses and apostrophes, so the end
// is a double quote, a line or field break, two spaces (log columns), or " key=" (the next pair).
size_t PathEnd(std::string_view s, size_t i) {
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\r' || c == '\n' || c == '\t' || c == ',' || c == '<' || c == '>' ||
            c == '|' || c == '\xC2' /* « » */)
            return i;
        if (c == ' ') {
            if (i + 1 < s.size() && (s[i + 1] == ' ' || PathStartsAt(s, i + 1))) return i;
            size_t j = i + 1;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.')) ++j;
            if (j > i + 1 && j < s.size() && s[j] == '=') return i;
        }
    }
    return s.size();
}

// One pass, longest name first at each position, so a replacement ("<user>") is never matched
// again by another name. ASCII case-insensitive; other bytes must match exactly.
std::string ReplaceNames(const std::string& text, const ScrubContext& ctx) {
    auto names = ctx.names;
    names.erase(std::remove_if(names.begin(), names.end(), [](const auto& n) { return n.first.size() < 3; }),
                names.end());
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    if (names.empty()) return text;
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        bool hit = false;
        for (const auto& [name, replacement] : names) {
            if (text.size() - i < name.size()) continue;
            size_t k = 0;
            while (k < name.size() && lower(text[i + k]) == lower(name[k])) ++k;
            if (k == name.size()) {
                out += replacement;
                i += name.size();
                hit = true;
                break;
            }
        }
        if (!hit) out.push_back(text[i++]);
    }
    return out;
}

std::wstring EnvOr(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::string ReadShared(const std::wstring& path) {
    // Share everything: the engine holds today's log open for append and may prune it.
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string data(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 256ll * 1024 * 1024)), '\0');
    DWORD       got = 0;
    if (!data.empty() && !ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
    CloseHandle(f);
    data.resize(got);
    return data;
}

std::vector<std::wstring> List(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW          fd{};
    const HANDLE              h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

// "ombra-20261106.log", "samples-20261106.csv", "samples-20261106-1.csv" (a second file the same
// day) -> "20261106": the first 8 digits after a '-' that end at '.' or '-'. "" if none.
std::string DateOf(const std::wstring& name) {
    for (size_t i = name.find(L'-'); i != std::wstring::npos; i = name.find(L'-', i + 1)) {
        if (i + 9 >= name.size() || (name[i + 9] != L'.' && name[i + 9] != L'-')) continue;
        std::string d;
        for (size_t k = i + 1; k <= i + 8 && name[k] >= L'0' && name[k] <= L'9'; ++k)
            d.push_back(static_cast<char>(name[k]));
        if (d.size() == 8) return d;
    }
    return {};
}

// YYYYMMDD `days - 1` days before `today`.
std::string FirstDay(const std::string& today, int days) {
    SYSTEMTIME st{};
    st.wYear  = static_cast<WORD>(std::stoi(today.substr(0, 4)));
    st.wMonth = static_cast<WORD>(std::stoi(today.substr(4, 2)));
    st.wDay   = static_cast<WORD>(std::stoi(today.substr(6, 2)));
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= static_cast<ULONGLONG>(std::max(days - 1, 0)) * 24ull * 3600ull * 10000000ull;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::string OsBuild() {
    wchar_t build[32] = L"?";
    DWORD   size      = sizeof(build);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
                 RRF_RT_REG_SZ, nullptr, build, &size);
    DWORD ubr = 0, ubr_size = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                 nullptr, &ubr, &ubr_size);
    return NarrowUtf8(build) + "." + std::to_string(ubr);
}

constexpr const char* kReadme =
    "Ombra diagnostics bundle\r\n"
    "\r\n"
    "logs/, telemetry/, crashes/*.txt, config.json and system.txt are scrubbed: folders in paths,\r\n"
    "window titles, device names and your user and computer names are replaced. Exe, clip and GPU\r\n"
    "names are kept.\r\n"
    "\r\n"
    "crashes/*.dmp are memory snapshots of a crashed Ombra process. They can't be scrubbed and may\r\n"
    "hold paths or window titles. Delete them before sharing if that matters to you.\r\n";

}  // namespace

ScrubContext MachineScrubContext() {
    ScrubContext ctx;
    wchar_t      user[UNLEN + 1];
    DWORD        n = UNLEN + 1;
    if (GetUserNameW(user, &n)) ctx.names.push_back({NarrowUtf8(user), "<user>"});
    const std::wstring profile = EnvOr(L"USERPROFILE");
    const size_t       slash   = profile.find_last_of(L"\\/");
    if (slash != std::wstring::npos) ctx.names.push_back({NarrowUtf8(profile.substr(slash + 1)), "<user>"});
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computer, &n)) ctx.names.push_back({NarrowUtf8(computer), "<computer>"});
    return ctx;
}

std::string ScrubText(std::string_view text, const ScrubContext& ctx) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, kOpen.size(), kOpen) == 0) {
            // The last » on the line, not the first: titles hold » themselves ("Forum » Thread").
            // Over-redacting the rest of a line is the safe way to be wrong.
            const size_t eol   = std::min(text.find('\n', i), text.size());
            const size_t close = text.substr(0, eol).rfind(kClose);
            if (close != std::string_view::npos && close >= i + kOpen.size()) {
                out += kOpen;
                out += "redacted";
                out += kClose;
                i = close + kClose.size();
                continue;
            }
        }
        if (const int kind = PathStartsAt(text, i)) {
            size_t end = PathEnd(text, i);
            while (end > i && (text[end - 1] == ' ' || text[end - 1] == '.')) --end;  // "... in C:\x." prose
            size_t last = end;
            while (last > i && !IsSep(text[last - 1])) --last;
            // A JSON-escaped path keeps its escaping, so config.json stays valid JSON.
            out += kind == 2 ? "<path>\\\\" : "<path>\\";
            out.append(text.substr(last, end - last));
            i = end;
            continue;
        }
        out.push_back(text[i++]);
    }
    return ReplaceNames(out, ctx);
}

std::string SystemInfo() { return std::string("Ombra ") + AppVersion() + " (" + BuildCommit() + ")\n" + MachineInfo(); }

std::string MachineInfo() {
    std::string s = "Windows build " + OsBuild() + "\n";
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            LARGE_INTEGER umd{};
            char          driver[48] = "?";
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                std::snprintf(driver, sizeof(driver), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                              HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            char line[400];
            std::snprintf(line, sizeof(line), "GPU %u: %s  vendor 0x%04X  driver %s  %llu MB\n", i,
                          NarrowUtf8(d.Description).c_str(), d.VendorId, driver,
                          static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024)));
            s += line;
            adapter->Release();
        }
        factory->Release();
    }
    return s;
}

std::string BugReportUrl(const std::string& machine_info, const std::string& reference) {
    const auto enc = [](std::string_view v) {
        std::string out;
        for (unsigned char ch : v) {
            if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                out += static_cast<char>(ch);
            } else {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", ch);
                out += hex;
            }
        }
        return out;
    };
    std::string windows, gpus;
    std::istringstream in(machine_info);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Windows build ", 0) == 0) windows = "Windows 11 (build " + line.substr(14) + ")";
        if (line.rfind("GPU ", 0) != 0) continue;
        const size_t colon = line.find(": "), end = line.find("  vendor 0x");
        if (colon == std::string::npos || end == std::string::npos || end <= colon) continue;
        if (line.compare(end + 11, 4, "1414") == 0) continue;  // Microsoft Basic Render Driver
        gpus += (gpus.empty() ? "" : " + ") + line.substr(colon + 2, end - colon - 2);
    }
    std::string url = std::string(kRepoUrl) + "/issues/new?template=bug_report.yml&version=" + enc(AppVersion());
    if (!gpus.empty()) url += "&gpu=" + enc(gpus.substr(0, 200));
    if (!windows.empty()) url += "&windows=" + enc(windows);
    if (!reference.empty()) url += "&logs=" + enc("Sent from Settings, reference " + reference);
    return url;
}

std::wstring DefaultBundlePath() {
    std::wstring dir;
    if (Instance().isolated) {
        dir = Instance().data_dir;  // tests and --drive never write into the user's Downloads
    } else {
        PWSTR downloads = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &downloads))) dir = downloads;
        CoTaskMemFree(downloads);
    }
    if (dir.empty()) return {};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t when[32];
    swprintf_s(when, L"%04u-%02u-%02u %02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    const std::wstring stem = dir + L"\\" OMBRA_PRODUCT_NAME L" logs " + when;
    std::wstring       path = stem + L".zip";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n)
        path = stem + L"-" + std::to_wstring(n) + L".zip";
    return path;
}

BundleResult WriteDiagnosticsBundle(const std::wstring& zip_path, const BundleOptions& options) {
    BundleResult       result;
    const std::wstring data = Instance().data_dir;
    if (data.empty()) {
        result.error = "no data folder";
        return result;
    }
    std::string today = options.today;
    if (today.empty()) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
        today = buf;
    }
    const std::string  first = FirstDay(today, options.days);
    const ScrubContext ctx   = MachineScrubContext();
    ZipWriter          zip;

    zip.Add("README.txt", kReadme);
    zip.Add("system.txt", ScrubText(SystemInfo(), ctx));
    auto add_dated = [&](const wchar_t* sub, const wchar_t* pattern, const char* folder) {
        const std::wstring dir = data + L"\\" + sub;
        for (const std::wstring& name : List(dir, pattern)) {
            const std::string date = DateOf(name);
            if (date.empty() || date < first || date > today) continue;
            zip.Add(std::string(folder) + "/" + NarrowUtf8(name), ScrubText(ReadShared(dir + L"\\" + name), ctx));
        }
    };
    add_dated(L"logs", L"*.log", "logs");
    add_dated(L"telemetry", L"*.csv", "telemetry");
    // Crash reports: every one kept (crash.h prunes to 5), whatever their age.
    const std::wstring crashes = data + L"\\crashes";
    for (const std::wstring& name : List(crashes, L"*.txt"))
        zip.Add("crashes/" + NarrowUtf8(name), ScrubText(ReadShared(crashes + L"\\" + name), ctx));
    for (const std::wstring& name : List(crashes, L"*.dmp"))
        zip.Add("crashes/" + NarrowUtf8(name), ReadShared(crashes + L"\\" + name));
    const std::wstring config = ConfigDirW() + L"\\config.json";
    if (GetFileAttributesW(config.c_str()) != INVALID_FILE_ATTRIBUTES)
        zip.Add("config.json", ScrubText(ReadShared(config), ctx));

    zip.Finish();
    result.files = zip.Count();
    result.bytes = zip.Bytes().size();
    if (result.bytes > 0xFFFFFFFFull) {  // no ZIP64 (zip_writer.h); offsets would wrap
        result.error = "the bundle is over 4 GB";
        return result;
    }
    result.ok    = zip.Write(zip_path);
    if (!result.ok) result.error = "could not write the zip (error " + std::to_string(GetLastError()) + ")";
    return result;
}

}  // namespace ombra
