#include "ombra-ui/pipe_client.h"

#include "ombra/log.h"
#include "ombra/config.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"

#include <algorithm>

namespace ombra_ui {

using ombra::JsonValue;
using ombra::ParseJson;
using ombra::WriteJsonCompact;

PipeClient::~PipeClient() { Close(); }

void PipeClient::Close() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool PipeClient::Connect(DWORD timeout_ms) {
    Close();
    const DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        HANDLE h = CreateFileW(ombra::Instance().pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pipe_ = h;
            return true;
        }
        // Same two retryable errors the pipe tests' client (tests/instance_tests.cpp) retries:
        // ERROR_FILE_NOT_FOUND means the engine's accept thread hasn't created its first pipe
        // instance yet (this is exactly the race ombra-ui hits spawning ombra.exe), ERROR_PIPE_BUSY
        // means an instance exists but every slot is serving another client right now.
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            ombra::Log::Warn("pipe_client: connect failed (err=%lu)", err);
            return false;
        }
        if (GetTickCount() >= deadline) return false;
        if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(ombra::Instance().pipe_name.c_str(), 200);
        else Sleep(20);
    }
}

namespace {

bool CanRetry(const JsonValue& req) {
    const auto* type = req.Find("type");
    if (!type || type->type != JsonValue::Type::String) return false;
    const auto& name = type->str;
    if (name == "set_config") {
        const auto* patch = req.Find("config");
        if (!patch || patch->type != JsonValue::Type::Object) return false;
        for (const auto& [key, value] : patch->obj) {
            const auto& settings = ombra::AllSettings();
            const auto setting = std::find_if(settings.begin(), settings.end(),
                [&](const auto& meta) { return key == meta.key; });
            if (setting == settings.end() || setting->kind == ombra::SettingKind::Action) return false;
        }
        return true;
    }
    return name == "get_schema" || name == "get_config" || name == "get_status" ||
           name == "preview_config" || name == "list_monitors" || name == "list_windows" ||
           name == "list_processes" || name == "list_audio_sessions" || name == "list_audio_devices" ||
           name == "audio_status" || name == "set_armed";
}

bool WriteLine(HANDLE h, const std::string& line) {
    const std::string data = line + "\n";
    DWORD             written = 0;
    return WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
           written == data.size();
}

// Polling read up to a deadline, same shape as the pipe tests' raw client — this client has
// no concurrent-request need (see pipe_client.h), so a simple blocking-with-timeout read is enough
// and doesn't need overlapped I/O.
bool ReadLine(HANDLE h, std::string& out, DWORD timeout_ms) {
    out.clear();
    const DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        char  buf[512];
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail == 0) {
            if (GetTickCount() >= deadline) return false;
            Sleep(10);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) return false;
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n') return true;
            out += buf[i];
        }
    }
}

}  // namespace

bool PipeClient::Hello(DWORD timeout_ms, std::string* reject_reason) {
    if (!Connected()) return false;
    JsonValue hello = JsonValue::MakeObject();
    hello.Set("type", JsonValue::MakeStr("hello"));
    hello.Set("id", JsonValue::MakeNum(next_id_++));
    hello.Set("v", JsonValue::MakeNum(ombra::kProtocolVersion));
    if (!WriteLine(pipe_, WriteJsonCompact(hello))) return false;
    std::string line;
    if (!ReadLine(pipe_, line, timeout_ms)) return false;
    try {
        JsonValue         resp = ParseJson(line);
        const JsonValue*  ok   = resp.Find("ok");
        if (ok && ok->type == JsonValue::Type::Bool && ok->b) return true;
        // An explicit response, just not a success one (e.g. a version mismatch) — distinct from
        // "nothing came back at all" so a caller knows retrying can't help.
        if (reject_reason) {
            const JsonValue* err = resp.Find("error");
            *reject_reason = (err && err->type == JsonValue::Type::String) ? err->str
                                                                            : "rejected (no reason given)";
        }
        return false;
    } catch (const std::string& err) {
        ombra::Log::Error("pipe_client: hello response failed to parse: %s", err.c_str());
        return false;
    }
}

bool PipeClient::Reconnect(DWORD timeout_ms) {
    return Connect(timeout_ms) && Hello(timeout_ms);
}

std::optional<JsonValue> PipeClient::Request(JsonValue req, DWORD timeout_ms) {
    if (!Connected() && !Reconnect(1500)) return std::nullopt;

    auto try_once = [&]() -> std::optional<JsonValue> {
        const int id = next_id_++;
        JsonValue r = req;
        r.Set("id", JsonValue::MakeNum(id));
        if (!WriteLine(pipe_, WriteJsonCompact(r))) return std::nullopt;
        std::string line;
        if (!ReadLine(pipe_, line, timeout_ms)) return std::nullopt;
        try {
            auto response = ParseJson(line);
            const auto* reply_id = response.Find("id");
            if (!reply_id || reply_id->type != JsonValue::Type::Number || reply_id->num != id)
                return std::nullopt;
            return response;
        } catch (const std::string& err) {
            ombra::Log::Error("pipe_client: response failed to parse: %s", err.c_str());
            return std::nullopt;
        }
    };

    if (auto result = try_once()) return result;

    // A missing reply cannot tell us whether an action happened. Never repeat saves,
    // screenshots or action settings. Close the handle so a late reply cannot answer the next
    // request; that request reconnects before sending anything.
    Close();
    if (!CanRetry(req)) return std::nullopt;
    ombra::Log::Info("pipe_client: request failed, reconnecting (engine may have restarted)");
    if (!Reconnect(1500)) {
        ombra::Log::Error("pipe_client: reconnect failed -- engine unreachable");
        return std::nullopt;
    }
    auto result = try_once();
    if (!result) Close();
    return result;
}

}  // namespace ombra_ui
