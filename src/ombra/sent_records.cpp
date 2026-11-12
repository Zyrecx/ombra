#include "ombra/sent_records.h"

#include "ombra/json.h"
#include "ombra/paths.h"

#include <windows.h>

#include <algorithm>
#include <string_view>

namespace ombra::sent_records {

namespace {

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string_view LineAt(std::string_view text, size_t pos) {
    if (pos >= text.size()) return {};
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

JsonValue JsonLineAt(std::string_view text, size_t pos) {
    try {
        return ParseJson(std::string(LineAt(text, pos)));
    } catch (const std::string&) {
        return {};
    }
}

// The line after `label` ("\nEvent:\n" and the like), parsed as JSON; Null when absent or malformed.
JsonValue JsonAfter(std::string_view text, std::string_view label) {
    const size_t at = text.find(label);
    return at == std::string_view::npos ? JsonValue{} : JsonLineAt(text, at + label.size());
}

// The session and usage records: the title, a blank line, then the JSON that was sent.
JsonValue JsonUnderTitle(std::string_view text) {
    const size_t first = text.find('\n');
    return first == std::string_view::npos ? JsonValue{} : JsonLineAt(text, first + 2);
}

std::string Str(const JsonValue& v, const char* a, const char* b = nullptr) {
    const JsonValue* x = v.Find(a);
    if (x && b) x = x->Find(b);
    return x && x->type == JsonValue::Type::String ? x->str : std::string();
}

// "20261110" -> "2026-11-10"; anything else as it came.
std::string DashedDay(std::string_view d) {
    if (d.size() != 8 || !std::all_of(d.begin(), d.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::string(d);
    return std::string(d.substr(0, 4)) + "-" + std::string(d.substr(4, 2)) + "-" + std::string(d.substr(6, 2));
}

std::string CrashSummary(std::string_view text) {
    const JsonValue e = JsonAfter(text, "\nEvent:\n");
    std::string reason = Str(e, "extra", "reason");
    // "access violation (0xC0000005) at 00007FF7..., writing 0000..." -> the part before the address.
    if (const size_t at = reason.find(" at 0"); at != std::string::npos) reason.resize(at);
    const std::string who = Str(e, "tags", "process") == "ombra-ui" ? "The overlay crashed" : "Ombra crashed";
    return reason.empty() ? who : who + ": " + reason;
}

std::string SessionSummary(std::string_view text) {
    const JsonValue  j   = JsonUnderTitle(text);
    const JsonValue* agg = j.Find("aggregates");
    if (agg && agg->type == JsonValue::Type::Array && !agg->arr.empty())
        for (const auto& [key, v] : agg->arr[0].obj) {
            if (key == "exited") return "Ran without crashing";
            if (key == "errored") return "Ran, with errors";
            if (key == "abnormal") return "Closed unexpectedly";
            if (key == "crashed") return "Crashed";
        }
    return "Whether Ombra ran and crashed";
}

std::string GamesSummary(std::string_view text) {
    const JsonValue  j     = JsonUnderTitle(text);
    const JsonValue* items = j.Find("items");
    if (!items || items->type != JsonValue::Type::Array || items->arr.empty()) return "Games played";
    std::vector<std::string> exes;
    for (const JsonValue& item : items->arr) {
        const JsonValue* attrs = item.Find("attributes");
        const std::string exe = attrs ? Str(*attrs, "ombra.game_exe", "value") : std::string();
        if (!exe.empty() && std::find(exes.begin(), exes.end(), exe) == exes.end()) exes.push_back(exe);
    }
    const size_t n = items->arr.size();
    std::string  s = std::to_string(n) + (n == 1 ? " game session" : " game sessions");
    for (size_t i = 0; i < exes.size(); ++i) s += (i ? ", " : ": ") + exes[i];
    return s;
}

}  // namespace

Record Parse(const std::wstring& file_name, const std::string& text) {
    Record r;
    r.file_name = file_name;
    r.bytes = text.size();
    const std::string_view t = text;
    const std::string_view title = LineAt(t, 0);

    if (const size_t at = t.rfind(kResultPrefix); at != std::string_view::npos) {
        r.result = std::string(LineAt(t, at + std::string_view(kResultPrefix).size()));
        r.status = StartsWith(r.result, "HTTP 2") ? Status::Sent : Status::Failed;
    }

    // The per-day kinds carry their UTC day in the file name: session-<day>-<start>, usage-*-<day>.
    const auto day_after = [&](std::wstring_view prefix) {
        const std::wstring_view n = file_name;
        if (n.substr(0, prefix.size()) != prefix || n.size() < prefix.size() + 8) return std::string();
        std::string d;
        for (wchar_t c : n.substr(prefix.size(), 8)) d += static_cast<char>(c);
        return DashedDay(d);
    };

    if (StartsWith(title, kCrashTitle)) {
        r.kind = Kind::Crash;
        r.summary = CrashSummary(t);
    } else if (StartsWith(title, kErrorTitle)) {
        r.kind = Kind::Error;
        r.summary = Str(JsonAfter(t, "\nEvent:\n"), "message");
        if (r.summary.empty()) r.summary = "A failure that didn't crash";
    } else if (StartsWith(title, kSessionTitle)) {
        r.kind = Kind::Session;
        r.day = day_after(L"session-");
        r.summary = SessionSummary(t);
    } else if (StartsWith(title, kGamesTitle)) {
        r.kind = Kind::GameSessions;
        r.day = day_after(L"usage-games-");
        r.summary = GamesSummary(t);
    } else if (StartsWith(title, kDailyTitle)) {
        r.kind = Kind::DailyUsage;
        r.day = day_after(L"usage-day-");
        r.summary = "Settings and performance for the day";
    } else if (StartsWith(title, kLogsTitle)) {
        r.kind = Kind::Logs;
        const size_t at = t.find(kLogsSizeLabel);
        r.summary = at == std::string_view::npos ? "Logs you sent"
                                                 : "Logs you sent: " + std::string(LineAt(t, at + std::string_view(kLogsSizeLabel).size()));
    } else {
        r.summary = std::string(title);
    }
    return r;
}

std::wstring Dir() { return Instance().data_dir + L"\\sent_reports"; }

std::vector<Record> List() {
    std::vector<Record> out;
    const std::wstring  dir = Dir();
    WIN32_FIND_DATAW    fd{};
    const HANDLE        h = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::string text(std::min<size_t>(fd.nFileSizeLow, 8u * 1024 * 1024), '\0');
        DWORD got = 0;
        if (!text.empty() && !ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        CloseHandle(f);
        text.resize(got);
        Record r = Parse(fd.cFileName, text);
        r.path = path;
        const ULONGLONG ft = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        r.written = static_cast<int64_t>((ft - 116444736000000000ull) / 10000000ull);
        out.push_back(std::move(r));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.written > b.written; });
    return out;
}

}  // namespace ombra::sent_records
