#include "ombra/open_recording.h"

#include "ombra/crash_upload.h"
#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/util.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace ombra {

namespace {

std::string Hex(const uint8_t* p, size_t n) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += kDigits[p[i] >> 4];
        s += kDigits[p[i] & 15];
    }
    return s;
}

bool Unhex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    const auto digit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = digit(s[2 * i]), lo = digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

// "<volume serial, 16 hex>:<file id, 32 hex>"
std::string FileIdOf(HANDLE h) {
    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)) return {};
    uint8_t serial[8];
    for (int i = 0; i < 8; ++i) serial[i] = static_cast<uint8_t>(id.VolumeSerialNumber >> (56 - 8 * i));
    return Hex(serial, 8) + ":" + Hex(id.FileId.Identifier, sizeof id.FileId.Identifier);
}

bool ParseFileId(const std::string& s, uint64_t& serial, FILE_ID_128& id) {
    uint8_t b[8];
    if (s.size() != 16 + 1 + 32 || s[16] != ':' || !Unhex(s.substr(0, 16), b, 8) ||
        !Unhex(s.substr(17), id.Identifier, sizeof id.Identifier))
        return false;
    serial = 0;
    for (uint8_t c : b) serial = serial << 8 | c;
    return true;
}

// Attributes only: works beside a writer that shares nothing but reads.
HANDLE OpenForId(const std::wstring& path) {
    return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

bool ReadText(const std::wstring& path, std::string& out) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// On the disk whole or not at all: a temporary file, synced, renamed over the record.
bool WriteDurably(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      wrote = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) &&
                    wrote == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

bool SameMarkers(const MuxMarkers& a, const MuxMarkers& b) {
    if (a.json != b.json || a.chapters.size() != b.chapters.size()) return false;
    for (size_t i = 0; i < a.chapters.size(); ++i)
        if (a.chapters[i].ms != b.chapters[i].ms || a.chapters[i].title != b.chapters[i].title) return false;
    return true;
}

// This engine's record names: its pid and start time, then a count.
std::wstring NextRecordName() {
    static const unsigned long long started = GetTickCount64();
    static std::atomic<unsigned>    count{0};
    wchar_t name[64];
    swprintf_s(name, L"%lu-%llx-%u.json", GetCurrentProcessId(), started, ++count);
    return name;
}

}  // namespace

std::wstring OpenRecordingsDir() {
    const std::wstring& data = Instance().data_dir;
    return data.empty() ? std::wstring{} : data + L"\\recordings\\open";
}

std::string FileIdAt(const std::wstring& path) {
    const HANDLE h = OpenForId(path);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string id = FileIdOf(h);
    CloseHandle(h);
    return id;
}

std::wstring PathOfFileId(const std::wstring& on_volume_of, const std::string& file_id) {
    uint64_t    serial = 0;
    FILE_ID_128 id{};
    if (!ParseFileId(file_id, serial, id)) return {};
    wchar_t root[MAX_PATH];
    if (!GetVolumePathNameW(on_volume_of.c_str(), root, MAX_PATH)) return {};
    const HANDLE volume = OpenForId(root);
    if (volume == INVALID_HANDLE_VALUE) return {};
    FILE_ID_DESCRIPTOR d{};
    d.dwSize         = sizeof d;
    d.Type           = ExtendedFileIdType;
    d.ExtendedFileId = id;
    const HANDLE h = OpenFileById(volume, &d, FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, 0);
    CloseHandle(volume);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    if (FileIdOf(h) == file_id) {  // the same volume, not one that took the drive letter since
        wchar_t     buf[32768];
        const DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n > 0 && n < std::size(buf)) path.assign(buf, n);
    }
    CloseHandle(h);
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0) path = path.substr(4);
    std::wstring lower = path;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    // Deleted: in the Recycle Bin, or still held open by someone after a POSIX delete, which moves
    // it into \$Extend\$Deleted until the last handle closes. Leave it be.
    if (lower.find(L"\\$recycle.bin\\") != std::wstring::npos || lower.find(L"\\$extend\\") != std::wstring::npos)
        return {};
    return path;
}

std::string WriteOpenRecordingJson(const OpenRecordingInfo& info) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("v", JsonValue::MakeNum(1));
    root.Set("path", JsonValue::MakeStr(NarrowUtf8(info.path)));
    root.Set("file_id", JsonValue::MakeStr(info.file_id));
    root.Set("app", JsonValue::MakeStr(info.app));
    root.Set("title", JsonValue::MakeStr(info.title));
    root.Set("tries", JsonValue::MakeNum(info.tries));
    JsonValue chapters = JsonValue::MakeArray();
    for (const MuxChapter& c : info.markers.chapters) {
        JsonValue chapter = JsonValue::MakeObject();
        chapter.Set("ms", JsonValue::MakeNum(static_cast<double>(c.ms)));
        chapter.Set("title", JsonValue::MakeStr(c.title));
        chapters.arr.push_back(std::move(chapter));
    }
    root.Set("chapters", std::move(chapters));
    root.Set("markers", JsonValue::MakeStr(info.markers.json));
    return WriteJsonCompact(root);
}

bool ParseOpenRecordingJson(const std::string& text, OpenRecordingInfo& out) {
    JsonValue root;
    try {
        root = ParseJson(text);
    } catch (const std::string&) {
        return false;
    }
    const auto str = [&](const char* key) -> const std::string* {
        const JsonValue* v = root.Find(key);
        return v && v->type == JsonValue::Type::String ? &v->str : nullptr;
    };
    const JsonValue* v     = root.Find("v");
    const std::string* path = str("path");
    if (!v || v->type != JsonValue::Type::Number || v->num != 1 || !path || path->empty()) return false;
    out      = {};
    out.path = WideFromUtf8(*path);
    if (const std::string* s = str("file_id")) out.file_id = *s;
    if (const std::string* s = str("app")) out.app = *s;
    if (const std::string* s = str("title")) out.title = *s;
    if (const std::string* s = str("markers")) out.markers.json = *s;
    if (const JsonValue* t = root.Find("tries"); t && t->type == JsonValue::Type::Number)
        out.tries = static_cast<int>(t->num);
    if (const JsonValue* c = root.Find("chapters"); c && c->type == JsonValue::Type::Array) {
        for (const JsonValue& e : c->arr) {
            const JsonValue* ms    = e.Find("ms");
            const JsonValue* title = e.Find("title");
            if (!ms || ms->type != JsonValue::Type::Number || ms->num < 0 || !title ||
                title->type != JsonValue::Type::String)
                continue;
            out.markers.chapters.push_back({static_cast<uint64_t>(ms->num), title->str});
        }
    }
    return true;
}

bool OpenRecording::Begin(const std::wstring& dir, const std::wstring& path, const std::string& app,
                          const std::string& title) {
    record_.clear();  // a part whose Finish failed leaves its record for the next start
    if (dir.empty()) return false;
    info_       = {};
    info_.path  = path;
    info_.app   = app;
    info_.title = title;
    record_     = dir + L"\\" + NextRecordName();
    if (!EnsureParentDir(record_) || !Write()) {
        Log::Warn("recording: can't write its crash record \"%ls\"; a crash would leave it fragmented",
                  record_.c_str());
        record_.clear();
        return false;
    }
    return true;
}

void OpenRecording::Opened() {
    if (!Active()) return;
    info_.file_id = FileIdAt(info_.path);
    Write();
}

void OpenRecording::Moved(const std::wstring& path) {
    if (!Active()) return;
    info_.path = path;
    Write();
}

void OpenRecording::SetMarkers(const MuxMarkers& markers) {
    if (!Active() || SameMarkers(markers, info_.markers)) return;
    info_.markers = markers;
    Write();
}

void OpenRecording::End() {
    if (!Active()) return;
    if (!DeleteFileW(record_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        Log::Warn("recording: can't delete its crash record \"%ls\" (error %lu)", record_.c_str(), GetLastError());
    record_.clear();
}

bool OpenRecording::Write() {
    if (WriteDurably(record_, WriteOpenRecordingJson(info_))) return true;
    Log::Warn("recording: can't update its crash record \"%ls\" (error %lu)", record_.c_str(), GetLastError());
    return false;
}

std::vector<LeftRecording> ListLeftRecordings(const std::wstring& dir) {
    std::vector<LeftRecording> left;
    if (dir.empty()) return left;
    WIN32_FIND_DATAW fd;
    const HANDLE     find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return left;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name   = fd.cFileName;
        const std::wstring record = dir + L"\\" + name;
        const bool json = name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".json") == 0;
        if (!json) {
            DeleteFileW(record.c_str());  // a .tmp a crash left mid-write
            continue;
        }
        LeftRecording r;
        r.record = record;
        std::string text;
        if (!ReadText(record, text) || !ParseOpenRecordingJson(text, r.info)) {
            Log::Warn("recording: crash record \"%ls\" doesn't read; dropped", record.c_str());
            DeleteFileW(record.c_str());
            continue;
        }
        left.push_back(std::move(r));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return left;
}

std::vector<RecoveredRecording> RecoverLeftRecordings(const std::vector<LeftRecording>& left,
                                                      const std::stop_token& stop) {
    using Recovery = Mp4FragmentedWriter::Recovery;
    std::vector<RecoveredRecording> done;
    for (const LeftRecording& l : left) {
        if (stop.stop_requested()) break;  // the engine is exiting: the rest wait for the next start
        const auto drop = [&] { DeleteFileW(l.record.c_str()); };
        OpenRecordingInfo info = l.info;
        if (info.tries >= kMaxRecoverTries) {
            // Each start failed, or began on it and never got to the end (Recover itself crashed).
            Log::Error("recording: gave up making \"%ls\" plain after %d tries; it stays fragmented",
                       info.path.c_str(), info.tries);
            crash_upload::ReportError(crash_upload::ErrorKind::Save, "recording_recover_gave_up", "",
                                      "a crashed recording couldn't be made plain");
            drop();
            continue;
        }
        // The file the record names: by its id, never another file now at its path.
        std::wstring path = info.path;
        if (!info.file_id.empty() && FileIdAt(path) != info.file_id) {
            path = PathOfFileId(info.path, info.file_id);
            if (path.empty()) {
                Log::Info("recording: \"%ls\" is gone; its crash record dropped", info.path.c_str());
                drop();
                continue;
            }
            Log::Info("recording: \"%ls\" moved to \"%ls\"", info.path.c_str(), path.c_str());
        }
        info.path = path;
        ++info.tries;
        if (!WriteDurably(l.record, WriteOpenRecordingJson(info))) {
            Log::Warn("recording: can't update the crash record of \"%ls\"; it waits for the next start",
                      path.c_str());
            continue;
        }

        const Mp4FragmentedWriter::Recovered r = Mp4FragmentedWriter::Recover(path, info.markers);
        Log::Info("recording: recover \"%ls\": %s (%s), %.1f s, %llu bytes cut", path.c_str(),
                  Mp4FragmentedWriter::RecoveryName(r.result), r.detail.c_str(), r.video_ms / 1000.0,
                  static_cast<unsigned long long>(r.cut_bytes));
        switch (r.result) {
            case Recovery::kRecovered:
                done.push_back({path, r.video_ms / 1000.0, info.app});
                drop();
                break;
            case Recovery::kInUse:
                --info.tries;  // not tried: the next start does
                WriteDurably(l.record, WriteOpenRecordingJson(info));
                break;
            case Recovery::kFailed:
                // Perhaps for now only (a full disk, a file it can't open): the record stays, and
                // the next start tries again until kMaxRecoverTries.
                break;
            case Recovery::kAlreadyPlain:
            case Recovery::kNoVideo:
            case Recovery::kNotOurs:
            case Recovery::kMissing:
                drop();
                break;
        }
    }
    return done;
}

}  // namespace ombra
