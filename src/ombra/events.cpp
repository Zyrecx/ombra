#include "ombra/events.h"

#include <algorithm>
#include <utility>

namespace ombra {

Subscription::Subscription(EventHub& hub, bool privileged)
    : hub_(hub), privileged_(privileged), wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    hub_.Add(this);
}

Subscription::~Subscription() {
    hub_.Remove(this);
    if (wake_) CloseHandle(wake_);
}

std::vector<std::string> Subscription::Set(const std::vector<std::string>& names) {
    std::set<std::string> wanted;
    std::vector<std::string> taken;
    for (const char* known : kEventNames) {
        if (std::find(names.begin(), names.end(), known) == names.end()) continue;
        wanted.insert(known);
        taken.emplace_back(known);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int delta = (wanted.empty() ? 0 : 1) - (wanted_.empty() ? 0 : 1);
    wanted_ = std::move(wanted);
    if (delta) hub_.Listening(delta);
    return taken;
}

bool Subscription::Take(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& line : queue_) out.push_back(std::move(line));
    queue_.clear();
    return !overflowed_;
}

void Subscription::Sent(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    unsent_ -= (std::min)(n, unsent_);
}

void Subscription::Queue(const std::string& name, const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (overflowed_ || !wanted_.count(name)) return;
    if (queue_.size() >= kMaxQueuedEvents) {
        overflowed_ = true;
        queue_.clear();
        unsent_ = 0;  // the connection closes: nothing more of it will be written
    } else {
        queue_.push_back(line);
        ++unsent_;
    }
    SetEvent(wake_);
}

void EventHub::Publish(const char* name, const JsonValue& fields, const JsonValue& private_fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || subs_.empty()) return;
    JsonValue event = JsonValue::MakeObject();
    event.Set("type", JsonValue::MakeStr("event"));
    event.Set("event", JsonValue::MakeStr(name));
    for (const auto& [key, value] : fields.obj) event.Set(key, value);
    const std::string line = WriteJsonCompact(event);
    std::string privileged_line;
    for (Subscription* s : subs_) {
        if (!s->privileged_ || private_fields.obj.empty()) {
            s->Queue(name, line);
            continue;
        }
        if (privileged_line.empty()) {
            JsonValue full = event;
            for (const auto& [key, value] : private_fields.obj) full.Set(key, value);
            privileged_line = WriteJsonCompact(full);
        }
        s->Queue(name, privileged_line);
    }
}

void EventHub::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}

bool EventHub::WaitDrained(DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        bool drained = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Subscription* s : subs_) {
                std::lock_guard<std::mutex> sub_lock(s->mutex_);
                drained &= s->unsent_ == 0;
            }
        }
        if (drained) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(10);
    }
}

void EventHub::Add(Subscription* s) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.push_back(s);
}

void EventHub::Remove(Subscription* s) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), s), subs_.end());
    std::lock_guard<std::mutex> sub_lock(s->mutex_);
    if (!s->wanted_.empty()) Listening(-1);
}

}  // namespace ombra
