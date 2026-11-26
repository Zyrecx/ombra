#include "ombra-ui/engine_events.h"

#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"

#include <optional>

namespace ombra_ui {

using ombra::JsonValue;

namespace {

constexpr DWORD kReplyTimeoutMs = 3000;
constexpr DWORD kRetryMs = 1000;
constexpr size_t kMaxLineBytes = 64 * 1024;  // an event line is a few hundred bytes

// What the overlay hears: the switch, footage, recordings and games. clip_saved is left out: no
// screen shows anything it would change while the overlay is open.
constexpr const char* kSubscribed[] = {"replay", "buffering", "game_start", "game_end",
                                       "recording_started", "recording_stopped"};

// One overlapped connection whose every wait also ends on `stop`.
class Connection {
public:
    explicit Connection(HANDLE stop) : stop_(stop), io_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Connection() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        if (io_) CloseHandle(io_);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool Open() {
        if (!io_) return false;
        pipe_ = CreateFileW(ombra::Instance().pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    bool Send(const JsonValue& msg) {
        std::string line = ombra::WriteJsonCompact(msg) + "\n";
        DWORD done = 0;
        return Io(true, line.data(), static_cast<DWORD>(line.size()), &done, kReplyTimeoutMs) && done == line.size();
    }

    // The next line, waiting up to `timeout_ms` (INFINITE: until it comes, the pipe drops or stop).
    std::optional<JsonValue> Read(DWORD timeout_ms) {
        const ULONGLONG deadline = timeout_ms == INFINITE ? 0 : GetTickCount64() + timeout_ms;
        for (;;) {
            // Events come in batches: whatever follows the first line stays for the next Read.
            if (const size_t nl = buffer_.find('\n'); nl != std::string::npos) {
                const std::string text = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                if (text.empty()) continue;
                try {
                    return ombra::ParseJson(text);
                } catch (const std::string& err) {
                    ombra::Log::Warn("engine_events: unreadable line: %s", err.c_str());
                    return std::nullopt;  // framing is suspect: reconnect
                }
            }
            if (buffer_.size() > kMaxLineBytes) return std::nullopt;
            DWORD wait = INFINITE;
            if (deadline) {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) return std::nullopt;
                wait = static_cast<DWORD>(deadline - now);
            }
            char chunk[4096];
            DWORD done = 0;
            if (!Io(false, chunk, sizeof(chunk), &done, wait) || done == 0) return std::nullopt;
            buffer_.append(chunk, done);
        }
    }

private:
    bool Io(bool write, char* data, DWORD size, DWORD* done, DWORD timeout_ms) {
        OVERLAPPED ov{};
        ov.hEvent = io_;
        ResetEvent(io_);
        const BOOL started = write ? WriteFile(pipe_, data, size, nullptr, &ov) : ReadFile(pipe_, data, size, nullptr, &ov);
        if (!started && GetLastError() != ERROR_IO_PENDING) return false;
        const HANDLE waits[] = {io_, stop_};
        if (WaitForMultipleObjects(2, waits, FALSE, timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &ov);
            GetOverlappedResult(pipe_, &ov, done, TRUE);  // the buffer stays ours until the cancel lands
            return false;
        }
        return GetOverlappedResult(pipe_, &ov, done, FALSE) != FALSE;
    }

    HANDLE      stop_;
    HANDLE      io_;
    HANDLE      pipe_ = INVALID_HANDLE_VALUE;
    std::string buffer_;
};

bool IsOk(const std::optional<JsonValue>& reply) {
    const JsonValue* ok = reply ? reply->Find("ok") : nullptr;
    return ok && ok->type == JsonValue::Type::Bool && ok->b;
}

JsonValue Message(const char* type, int id) {
    JsonValue m = JsonValue::MakeObject();
    m.Set("type", JsonValue::MakeStr(type));
    m.Set("id", JsonValue::MakeNum(id));
    return m;
}

}  // namespace

bool EngineEvents::Classify(const std::string& name, EngineEvent& out) {
    if (name == "replay") out = EngineEvent::kReplay;
    else if (name == "buffering") out = EngineEvent::kBuffering;
    else if (name == "recording_started" || name == "recording_stopped") out = EngineEvent::kRecording;
    else if (name == "game_start" || name == "game_end") out = EngineEvent::kGame;
    else return false;
    return true;
}

void EngineEvents::Start(HWND hwnd, UINT msg) {
    if (worker_.joinable()) return;
    if (!stop_) stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return;
    ResetEvent(stop_);
    worker_ = std::thread([this, hwnd, msg] { Run(hwnd, msg); });
}

void EngineEvents::Stop() {
    if (worker_.joinable()) {
        SetEvent(stop_);
        worker_.join();
    }
    if (stop_) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

void EngineEvents::Run(HWND hwnd, UINT msg) {
    ombra::Log::SetThreadName("engine-events");
    auto post = [&](EngineEvent e) { PostMessageW(hwnd, msg, static_cast<WPARAM>(e), 0); };
    bool was_connected = false;
    for (;;) {
        bool subscribed = false;
        {
            Connection c(stop_);
            if (c.Open()) {
                JsonValue hello = Message("hello", 1);
                hello.Set("v", JsonValue::MakeNum(ombra::kProtocolVersion));
                JsonValue sub = Message("subscribe", 2);
                JsonValue names = JsonValue::MakeArray();
                for (const char* n : kSubscribed) names.arr.push_back(JsonValue::MakeStr(n));
                sub.Set("events", std::move(names));
                // Nothing is subscribed before its reply, so the reply is the next line.
                subscribed = c.Send(hello) && IsOk(c.Read(kReplyTimeoutMs)) && c.Send(sub) &&
                             IsOk(c.Read(kReplyTimeoutMs));
            }
            if (subscribed) {
                was_connected = true;
                post(EngineEvent::kConnected);
                while (const std::optional<JsonValue> line = c.Read(INFINITE)) {
                    const JsonValue* type = line->Find("type");
                    const JsonValue* name = line->Find("event");
                    EngineEvent e;
                    if (type && type->type == JsonValue::Type::String && type->str == "event" && name &&
                        name->type == JsonValue::Type::String && Classify(name->str, e))
                        post(e);
                }
            }
        }
        if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) return;
        // Said once per drop, not on every failed retry while the engine stays away.
        if (was_connected) {
            post(EngineEvent::kLost);
            was_connected = false;
        }
        if (WaitForSingleObject(stop_, kRetryMs) == WAIT_OBJECT_0) return;
    }
}

}  // namespace ombra_ui
