#include "ombra/update.h"

#include "ombra/crash_upload.h"
#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/util.h"
#include "ombra/version.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace ombra {

// ---- Versions ----

std::optional<SemVer> ParseSemVer(const std::string& text) {
    std::string s = text;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    if (const size_t plus = s.find('+'); plus != std::string::npos) s.resize(plus);
    SemVer v;
    if (const size_t dash = s.find('-'); dash != std::string::npos) {
        v.pre = s.substr(dash + 1);
        s.resize(dash);
        if (v.pre.empty()) return std::nullopt;
    }
    int*   parts[3] = {&v.major, &v.minor, &v.patch};
    size_t pos      = 0;
    for (int i = 0; i < 3; ++i) {
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return std::nullopt;
        long long n = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            n = n * 10 + (s[pos++] - '0');
            if (n > 1'000'000) return std::nullopt;
        }
        *parts[i] = static_cast<int>(n);
        if (i < 2) {
            if (pos >= s.size() || s[pos] != '.') return std::nullopt;
            ++pos;
        }
    }
    if (pos != s.size()) return std::nullopt;
    return v;
}

namespace {

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

std::vector<std::string> SplitDots(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t dot = s.find('.', start);
        out.push_back(s.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) return out;
        start = dot + 1;
    }
}

}  // namespace

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) {
        if (a.pre.empty() && b.pre.empty()) return 0;
        return a.pre.empty() ? 1 : -1;  // 1.0.0-rc < 1.0.0
    }
    const auto pa = SplitDots(a.pre), pb = SplitDots(b.pre);
    for (size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
        const bool na = AllDigits(pa[i]), nb = AllDigits(pb[i]);
        if (na && nb) {
            if (pa[i].size() != pb[i].size()) return pa[i].size() < pb[i].size() ? -1 : 1;
            if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;  // numeric identifiers sort before alphanumeric ones
        } else if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    if (pa.size() != pb.size()) return pa.size() < pb.size() ? -1 : 1;
    return 0;
}

// ---- The release feed ----

const ReleaseAsset* ReleaseInfo::FindAsset(const std::string& name) const {
    for (const auto& a : assets)
        if (a.name == name) return &a;
    return nullptr;
}

namespace {

std::string StrField(const JsonValue& obj, const char* key) {
    const JsonValue* v = obj.Find(key);
    return v && v->type == JsonValue::Type::String ? v->str : std::string();
}

bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

std::optional<ReleaseInfo> ParseReleaseJson(const std::string& text, std::string* error) {
    const auto fail = [&](const std::string& why) -> std::optional<ReleaseInfo> {
        if (error) *error = why;
        return std::nullopt;
    };
    JsonValue root;
    try {
        root = ParseJson(text);
    } catch (const std::string& e) {
        return fail("feed isn't JSON: " + e);
    }
    if (root.type != JsonValue::Type::Object) return fail("feed isn't a release object");
    ReleaseInfo r;
    r.tag      = StrField(root, "tag_name");
    r.page_url = StrField(root, "html_url");
    const auto ver = ParseSemVer(r.tag);
    if (!ver) return fail("release tag isn't a version: \"" + r.tag + "\"");
    r.version = r.tag[0] == 'v' || r.tag[0] == 'V' ? r.tag.substr(1) : r.tag;
    if (const JsonValue* d = root.Find("draft"); d && d->type == JsonValue::Type::Bool && d->b)
        return fail("release is a draft");
    if (const JsonValue* p = root.Find("prerelease"); p && p->type == JsonValue::Type::Bool)
        r.prerelease = p->b;
    if (const JsonValue* assets = root.Find("assets"); assets && assets->type == JsonValue::Type::Array) {
        for (const JsonValue& a : assets->arr) {
            if (a.type != JsonValue::Type::Object) continue;
            ReleaseAsset asset;
            asset.name = StrField(a, "name");
            asset.url  = StrField(a, "browser_download_url");
            const std::string digest = Lower(StrField(a, "digest"));
            if (digest.rfind("sha256:", 0) == 0 && IsHex64(digest.substr(7))) asset.sha256 = digest.substr(7);
            if (const JsonValue* sz = a.Find("size"); sz && sz->type == JsonValue::Type::Number && sz->num > 0)
                asset.size = static_cast<uint64_t>(sz->num);
            if (!asset.name.empty() && asset.url.rfind("https://", 0) == 0) r.assets.push_back(std::move(asset));
        }
    }
    return r;
}

// ---- Install kind and the apply gate ----

const char* InstallKindName(InstallKind kind) {
    switch (kind) {
        case InstallKind::Installer: return "installer";
        case InstallKind::Scoop:     return "scoop";
        case InstallKind::Portable:  return "portable";
    }
    return "portable";
}

InstallKind DetectInstallKind(const std::wstring& exe_dir, bool has_uninstaller, bool dir_writable) {
    std::wstring lower = exe_dir;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    if (lower.find(L"\\scoop\\apps\\") != std::wstring::npos) return InstallKind::Scoop;
    if (has_uninstaller && dir_writable) return InstallKind::Installer;
    return InstallKind::Portable;
}

bool SafeToApplyNow(const ApplyGate& g) {
    return g.idle_ms >= kApplyIdleMs && !g.busy_fullscreen && !g.game_running && !g.save_in_flight &&
           !g.other_ombra_processes;
}

UpdateCheckResult ParseUpdateCheckOutput(const std::string& text) {
    UpdateCheckResult r;
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "ok") r.ok = value == "1";
        else if (key == "error") r.error = value;
        else if (key == "latest") r.latest = value;
        else if (key == "page") r.page_url = value;
        else if (key == "newer") r.newer = value == "1";
        else if (key == "installer") r.installer_path = value;
        else if (key == "installer_sha256") r.installer_sha256 = value;
    }
    return r;
}

// ---- Hashing and HTTP (child side, plus a re-hash before an apply) ----

namespace {

struct Sha256 {
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool               ok   = false;
    Sha256() {
        ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
             BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Sha256() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Update(const void* data, size_t n) {
        if (ok && BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0) < 0)
            ok = false;
    }
    std::string Finish() {
        unsigned char digest[32];
        if (!ok || BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) return {};
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        for (unsigned char b : digest) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 15]);
        }
        return out;
    }
};

}  // namespace

std::string Sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Sha256               h;
    std::vector<uint8_t> buf(1 << 16);
    DWORD                got = 0;
    bool                 ok  = true;
    for (;;) {
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        h.Update(buf.data(), got);
    }
    CloseHandle(f);
    return ok ? h.Finish() : std::string();
}

namespace {

std::string DescribeWinHttpError(DWORD code);

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET x) : h(x) {}
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle&)            = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

// GETs `url` (https only; WinHTTP follows GitHub's redirect to its download host) and hands each
// chunk to `sink`. `sink` returning false stops the transfer. false + *error on any failure.
bool HttpGet(const std::wstring& url, const wchar_t* accept, uint64_t max_bytes,
             const std::function<bool(const char*, size_t)>& sink, std::string* error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize      = sizeof(parts);
    wchar_t host[256]       = {};
    wchar_t path[2048]      = {};
    parts.lpszHostName      = host;
    parts.dwHostNameLength  = ARRAYSIZE(host);
    parts.lpszUrlPath       = path;
    parts.dwUrlPathLength   = ARRAYSIZE(path);
    wchar_t extra[2048]     = {};
    parts.lpszExtraInfo     = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        *error = "not an https URL";
        return false;
    }
    // The User-Agent GitHub requires: the product and version only.
    const std::wstring agent = L"Ombra/" + WideFromUtf8(AppVersion());
    WinHttpHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        *error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    WinHttpHandle connect(WinHttpConnect(session.h, host, parts.nPort, 0));
    const std::wstring object = std::wstring(path) + extra;
    WinHttpHandle request(connect.h ? WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr,
                                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         WINHTTP_FLAG_SECURE)
                                    : nullptr);
    if (!request.h) {
        *error = "couldn't open the request (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    const std::wstring headers = std::wstring(L"Accept: ") + accept;
    if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        *error = DescribeWinHttpError(GetLastError());
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        // releases/latest answers 404 until a release is published (drafts and prereleases don't count).
        *error = status == 404 ? "no release is published yet"
               : status == 403 || status == 429 ? "GitHub is limiting requests (HTTP " + std::to_string(status) + ")"
                                                : "HTTP " + std::to_string(status);
        return false;
    }
    std::vector<char> buf(1 << 16);
    uint64_t          total = 0;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            *error = "read failed: " + DescribeWinHttpError(GetLastError());
            return false;
        }
        if (got == 0) return true;
        total += got;
        if (total > max_bytes) {
            *error = "response larger than expected";
            return false;
        }
        if (!sink(buf.data(), got)) {
            *error = "write failed";
            return false;
        }
    }
}

std::wstring UpdatesDir() {
    return Instance().data_dir.empty() ? L"" : Instance().data_dir + L"\\updates";
}

std::wstring EnvOr(const wchar_t* name, const wchar_t* fallback) {
    wchar_t     buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n > 0 && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : std::wstring(fallback);
}

// Manual tests against another repo's release: OMBRA_UPDATE_FEED and OMBRA_UPDATE_ASSET.
std::wstring FeedUrl() { return EnvOr(L"OMBRA_UPDATE_FEED", kDefaultUpdateFeed); }
std::string  InstallerAssetName() {
    return NarrowUtf8(EnvOr(L"OMBRA_UPDATE_ASSET", WideFromUtf8(kInstallerAssetName).c_str()));
}

// WinHTTP's codes for "this PC isn't online", which is most failed checks.
std::string DescribeWinHttpError(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR: return "no connection (" + std::to_string(code) + ")";
        case ERROR_WINHTTP_TIMEOUT:          return "timed out";
        case ERROR_WINHTTP_SECURE_FAILURE:   return "the secure connection failed (" + std::to_string(code) + ")";
        default:                             return "error " + std::to_string(code);
    }
}

// Values go one per line; a stray newline would forge a key.
std::string OneLine(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
}

}  // namespace

int RunUpdateCheckChild(int argc, wchar_t** argv) {
    bool download = false;
    for (int i = 2; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--download") download = true;
    const auto report_error = [](const std::string& why) {
        std::printf("ok=0\nerror=%s\n", OneLine(why).c_str());
        std::fflush(stdout);
        return 1;
    };

    std::string body, error;
    if (!HttpGet(FeedUrl(), L"application/vnd.github+json", 4 << 20,
                 [&](const char* p, size_t n) { body.append(p, n); return true; }, &error))
        return report_error(error);
    const auto release = ParseReleaseJson(body, &error);
    if (!release) return report_error(error);
    const auto latest  = ParseSemVer(release->version);
    const auto current = ParseSemVer(AppVersion());
    const bool newer   = latest && current && CompareSemVer(*latest, *current) > 0;
    std::printf("ok=1\nlatest=%s\npage=%s\nnewer=%d\n", OneLine(release->version).c_str(),
                OneLine(release->page_url).c_str(), newer ? 1 : 0);
    std::fflush(stdout);
    if (!download || !newer) return 0;

    const std::string   asset_name = InstallerAssetName();
    const ReleaseAsset* asset      = release->FindAsset(asset_name);
    if (!asset) return report_error("release has no " + asset_name);
    if (asset->sha256.empty()) return report_error("release lists no SHA-256 for the installer");
    const std::wstring dir = UpdatesDir();
    if (dir.empty()) return report_error("no data folder");
    const std::wstring final_path = dir + L"\\Ombra-Setup-" + WideFromUtf8(release->version) + L".exe";
    if (Sha256File(final_path) == asset->sha256) {  // already fetched by an earlier check
        std::printf("installer=%s\ninstaller_sha256=%s\n", NarrowUtf8(final_path).c_str(), asset->sha256.c_str());
        return 0;
    }
    const std::wstring part = final_path + L".part";
    if (!EnsureParentDir(part)) return report_error("couldn't create the updates folder");
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return report_error("couldn't write the download");
    Sha256         hash;
    const uint64_t cap = asset->size ? asset->size : (256ull << 20);
    const bool     got = HttpGet(WideFromUtf8(asset->url), L"application/octet-stream", cap,
                                 [&](const char* p, size_t n) {
                                 hash.Update(p, n);
                                 DWORD wrote = 0;
                                 return WriteFile(f, p, static_cast<DWORD>(n), &wrote, nullptr) && wrote == n;
                             },
                                 &error);
    CloseHandle(f);
    const std::string digest = got ? hash.Finish() : std::string();
    if (!got || digest != asset->sha256) {
        DeleteFileW(part.c_str());
        return report_error(got ? "download doesn't match the release's SHA-256" : "download: " + error);
    }
    if (!MoveFileExW(part.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part.c_str());
        return report_error("couldn't move the download into place");
    }
    std::printf("installer=%s\ninstaller_sha256=%s\n", NarrowUtf8(final_path).c_str(), digest.c_str());
    std::fflush(stdout);
    return 0;
}

// ---- Engine side ----

std::atomic<Updater*> g_updater_for_engine{nullptr};

namespace {

constexpr auto     kFirstCheckDelay = std::chrono::minutes(3);
constexpr int64_t  kCheckIntervalS  = 12 * 3600;
constexpr int64_t  kRetryIntervalS  = 3600;
constexpr int64_t  kReapplyAfterS   = 24 * 3600;
constexpr auto     kGatePoll        = std::chrono::minutes(1);
constexpr DWORD    kChildTimeoutMs  = 10 * 60 * 1000;

std::wstring ExeDir() {
    wchar_t     exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

bool DirWritable(const std::wstring& dir) {
    const std::wstring probe = dir + L"\\.ombra-write-test";
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

bool SetupFinished() {
    const std::wstring& dir = Instance().config_dir;
    return !dir.empty() && GetFileAttributesW((dir + L"\\first_run_done").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring StatePath() {
    const std::wstring dir = UpdatesDir();
    return dir.empty() ? L"" : dir + L"\\state.json";
}

// updates\state.json: survives the engine's restarts.
struct PersistedState {
    int64_t     last_check_unix = 0;
    std::string attempted_version;  // the last update handed to setup, and when
    int64_t     attempted_unix = 0;
    bool        attempt_checked = false;  // a later start compared it with the running version
};

PersistedState ReadState() {
    PersistedState st;
    std::ifstream  in(StatePath(), std::ios::binary);
    if (!in) return st;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        const JsonValue v   = ParseJson(ss.str());
        const auto      num = [&](const char* key) {
            const JsonValue* t = v.Find(key);
            return t && t->type == JsonValue::Type::Number ? static_cast<int64_t>(t->num) : 0;
        };
        st.last_check_unix = num("last_check_unix");
        st.attempted_unix  = num("attempted_unix");
        if (const JsonValue* a = v.Find("attempted_version"); a && a->type == JsonValue::Type::String)
            st.attempted_version = a->str;
        if (const JsonValue* c = v.Find("attempt_checked"); c && c->type == JsonValue::Type::Bool)
            st.attempt_checked = c->b;
    } catch (const std::string&) {
    }
    return st;
}

void WriteState(const PersistedState& st) {
    const std::wstring path = StatePath();
    if (path.empty() || !EnsureParentDir(path)) return;
    JsonValue v = JsonValue::MakeObject();
    v.Set("last_check_unix", JsonValue::MakeNum(static_cast<double>(st.last_check_unix)));
    v.Set("attempted_version", JsonValue::MakeStr(st.attempted_version));
    v.Set("attempted_unix", JsonValue::MakeNum(static_cast<double>(st.attempted_unix)));
    v.Set("attempt_checked", JsonValue::MakeBool(st.attempt_checked));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << WriteJsonCompact(v);
}

// Runs `ombra.exe update-check [--download]` and returns its stdout. Gives up after
// kChildTimeoutMs or when `stop` is signalled.
std::string RunCheckChild(bool download, HANDLE stop, std::string* error) {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        *error = "GetModuleFileNameW failed";
        return {};
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE              read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 1 << 16)) {
        *error = "CreatePipe failed";
        return {};
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write_end, sizeof(write_end),
                              nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb         = sizeof(si);
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError  = write_end;
    si.lpAttributeList        = attrs;
    std::wstring        cmd   = L"\"" + std::wstring(exe) + L"\" update-check" + (download ? L" --download" : L"");
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS,
                                        nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        *error = "couldn't start the update check";
        return {};
    }
    // The child prints a few short lines, far below the pipe's buffer, so reading after it exits
    // can't deadlock.
    HANDLE      waits[2] = {pi.hProcess, stop};
    const DWORD w        = WaitForMultipleObjects(2, waits, FALSE, kChildTimeoutMs);
    if (w != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        *error = w == WAIT_OBJECT_0 + 1 ? "stopped" : "update check timed out";
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::string output;
    for (;;) {
        char  buf[1024];
        DWORD got = 0;
        if (!ReadFile(read_end, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        output.append(buf, got);
    }
    CloseHandle(read_end);
    return error->empty() ? output : std::string();
}

HANDLE g_updater_stop_event = nullptr;  // owned by the one Updater, for RunCheckChild

}  // namespace

void CleanupStaleInstallers(const std::wstring& dir, const std::string& current_version) {
    const auto current = ParseSemVer(current_version);
    if (dir.empty() || !current) return;
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW((dir + L"\\Ombra-Setup-*.exe").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring name = entry.cFileName;
        const std::wstring prefix = L"Ombra-Setup-", suffix = L".exe";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) continue;
        const auto version = ParseSemVer(NarrowUtf8(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size())));
        if (!version || CompareSemVer(*version, *current) > 0) continue;

        // An exclusive handle refuses a setup that is open/running. Delete by that same handle,
        // so no other process can open it for a handoff between the check and the deletion.
        const HANDLE file = CreateFileW((dir + L"\\" + name).c_str(), DELETE | FILE_READ_ATTRIBUTES, 0,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(file, &info) &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        CloseHandle(file);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

Updater::Updater(Hooks hooks) : hooks_(std::move(hooks)) {
    const std::wstring dir = ExeDir();
    const bool uninstaller = !dir.empty() &&
                             GetFileAttributesW((dir + L"\\unins000.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    kind_                = DetectInstallKind(dir, uninstaller, uninstaller && DirWritable(dir));
    updates_dir_         = UpdatesDir();
    status_.current      = AppVersion();
    status_.install_kind = InstallKindName(kind_);
    status_.last_check_unix = ReadState().last_check_unix;
}

Updater::~Updater() { Stop(); }

void Updater::Start() {
    if (worker_.joinable() || Instance().isolated) return;
    CleanupStaleInstallers(updates_dir_, AppVersion());
    // Setup restarts the engine whether or not the install worked (Apply).
    if (PersistedState st = ReadState(); !st.attempted_version.empty() && !st.attempt_checked) {
        if (st.attempted_version != AppVersion()) {
            Log::Warn("update            setup for %s left %s installed", st.attempted_version.c_str(), AppVersion());
            crash_upload::ReportError(crash_upload::ErrorKind::Update, "setup_left_old_version", "",
                                      "update setup ran, the old version is still installed");
        }
        st.attempt_checked = true;
        WriteState(st);
    }
    if (!g_updater_stop_event) g_updater_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_ = std::thread([this] { WorkerLoop(); });
}

void Updater::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    if (g_updater_stop_event) SetEvent(g_updater_stop_event);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

Updater::Status Updater::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Updater::RequestCheck() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        check_requested_ = true;
    }
    cv_.notify_all();
}

bool Updater::RequestInstall(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != "ready") {
            if (error) *error = kind_ == InstallKind::Installer ? "no update is ready to install"
                                                                 : "this copy is updated outside Ombra";
            return false;
        }
        install_requested_ = true;
    }
    cv_.notify_all();
    return true;
}

void Updater::SetState(const char* state, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    status_.error = error;
}

void Updater::RunCheck(bool download) {
    SetState(download ? "downloading" : "checking");
    std::string error;
    const std::string out = RunCheckChild(download, g_updater_stop_event, &error);
    if (error == "stopped") return;
    const UpdateCheckResult r = ParseUpdateCheckOutput(out);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::string ready_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (r.ok) status_.last_check_unix = now;
        if (!r.latest.empty()) status_.latest = r.latest;
        if (!r.page_url.empty()) status_.page_url = r.page_url;
        if (!error.empty() || (!r.ok && r.latest.empty())) {
            status_.state = "failed";
            status_.error = !error.empty() ? error : r.error;
        } else if (!r.newer) {
            status_.state = "up_to_date";
            status_.error.clear();
        } else if (!r.installer_path.empty() && !r.installer_sha256.empty()) {
            status_.state     = "ready";
            status_.error.clear();
            ready_installer_  = WideFromUtf8(r.installer_path);
            ready_sha256_     = r.installer_sha256;
            ready_version     = r.latest;
        } else {
            // Newer, but not downloaded: this kind doesn't self-update, or the download failed.
            status_.state = "available";
            status_.error = r.error;
        }
    }
    if (r.ok) {
        PersistedState st  = ReadState();
        st.last_check_unix = now;
        WriteState(st);
    }
    const Status s = GetStatus();
    Log::Info("update check      %s (this %s, latest %s, %s)%s%s", s.state.c_str(), s.current.c_str(),
              s.latest.empty() ? "?" : s.latest.c_str(), s.install_kind.c_str(), s.error.empty() ? "" : ": ",
              s.error.c_str());
    if (!ready_version.empty() && hooks_.on_ready) hooks_.on_ready(ready_version);
}

bool Updater::Apply(std::string* error) {
    std::wstring path;
    std::string  sha;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = ready_installer_;
        sha  = ready_sha256_;
    }
    // Re-hashed right before it runs: the file sat on disk since the download.
    if (path.empty() || Sha256File(path) != sha) {
        *error = "the downloaded installer changed or is gone";
        crash_upload::ReportError(crash_upload::ErrorKind::Update, "installer_changed", "", "downloaded installer changed");
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = "failed";
        status_.error = *error;
        ready_installer_.clear();
        return false;
    }
    // /UPDATE tells setup (installer/ombra.iss) to wait for this engine (/ENGINEPID) to exit and
    // start it again afterwards, whether or not the install worked. A silent setup shows nothing,
    // so its log goes beside the download.
    std::wstring cmd = L"\"" + path + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE /ENGINEPID=" +
                       std::to_wstring(GetCurrentProcessId());
    if (!updates_dir_.empty()) cmd += L" /LOG=\"" + updates_dir_ + L"\\setup.log\"";
    STARTUPINFOW        si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        const DWORD start_error = GetLastError();
        *error = "couldn't start the installer (" + std::to_string(start_error) + ")";
        crash_upload::ReportError(crash_upload::ErrorKind::Update, "installer_start:" + std::to_string(start_error), "",
                                  "couldn't start the installer");
        SetState("failed", *error);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    {
        // If setup fails and Ombra comes back on the old version, the same update isn't
        // auto-applied again for a day (Install now still can), so a broken setup can't loop.
        PersistedState st    = ReadState();
        st.attempted_version = GetStatus().latest;
        st.attempted_unix    = static_cast<int64_t>(std::time(nullptr));
        st.attempt_checked   = false;
        WriteState(st);
    }
    SetState("installing");
    Log::Info("update            installing %s, engine exits for setup", NarrowUtf8(path).c_str());
    if (hooks_.request_exit) hooks_.request_exit();
    return true;
}

void Updater::WorkerLoop() {
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        const bool requested_check   = check_requested_;
        const bool requested_install = install_requested_;
        check_requested_   = false;
        install_requested_ = false;
        const std::string state = status_.state;
        const int64_t     last  = status_.last_check_unix;
        lock.unlock();

        const bool may_check = SetupFinished() && (requested_check || !hooks_.check_enabled || hooks_.check_enabled());
        const bool installs  = kind_ == InstallKind::Installer;
        const int64_t now  = static_cast<int64_t>(std::time(nullptr));
        // A failed check (offline, GitHub down) retries hourly, not every poll.
        const bool due = std::chrono::steady_clock::now() - started >= kFirstCheckDelay &&
                         now - last >= kCheckIntervalS && now - last_attempt_unix_ >= kRetryIntervalS;
        if (state == "ready") {
            const PersistedState st = ReadState();
            std::string latest;
            {
                std::lock_guard<std::mutex> l(mutex_);
                latest = status_.latest;
            }
            const bool retried_lately = st.attempted_version == latest && now - st.attempted_unix < kReapplyAfterS;
            const bool auto_ok = !retried_lately && hooks_.auto_install && hooks_.auto_install() && hooks_.gate &&
                                 SafeToApplyNow(hooks_.gate());
            if (requested_install || auto_ok) {
                std::string error;
                if (Apply(&error)) return;
                Log::Warn("update            %s", error.c_str());
            } else if (requested_check) {
                RunCheck(installs);
            }
        } else if (may_check && (requested_check || due)) {
            // Downloads only where an apply can follow; "Install now" waits for this to finish.
            last_attempt_unix_ = now;
            RunCheck(installs);
        }

        lock.lock();
        if (stop_) break;
        cv_.wait_for(lock, kGatePoll, [this] { return stop_ || check_requested_ || install_requested_; });
    }
}

}  // namespace ombra
