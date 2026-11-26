// What the engine tells subscribed pipe connections as it happens (protocol.h "subscribe").
//
// Costs nothing unless used: with no subscriber, Publish is one atomic load and builds nothing.
// Raised only from rare moments (a switch, a game, a save, exit), never from the pacer or audio
// paths. Each connection's Subscription queues its own lines; the connection's pipe thread waits
// on Wake() beside its read and writes them out (pipe.cpp), so the hub adds no thread.
#pragma once

#include "ombra/json.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ombra {

// The events this engine raises, in the order `subscribe` lists them.
inline constexpr const char* kEventNames[] = {"replay", "buffering", "game_start", "game_end",
                                              "clip_saved", "exiting"};
// A connection this far behind is dropped (it catches up with `status` when it comes back).
inline constexpr size_t kMaxQueuedEvents = 64;

class EventHub;

// One connection's subscription. Lives on the connection's thread; the hub only queues into it.
class Subscription {
public:
    // `privileged`: sees the fields only Ombra's own command line may (a clip's paths).
    Subscription(EventHub& hub, bool privileged);
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    // Replaces what this connection hears; returns the names taken, in kEventNames order. Names
    // this engine doesn't raise are left out, not refused: a newer client may ask for more.
    std::vector<std::string> Set(const std::vector<std::string>& names);

    // Auto-reset; signalled when a line is queued or the queue overflowed.
    HANDLE Wake() const { return wake_; }
    // Moves the queued lines into `out` (each one JSON line, no newline). False: the queue
    // overflowed and the connection should close.
    bool Take(std::vector<std::string>& out);
    // `n` taken lines are written (or dropped with the connection): WaitDrained's count.
    void Sent(size_t n);

private:
    friend class EventHub;
    // Under the hub's mutex.
    void Queue(const std::string& name, const std::string& line);

    EventHub&             hub_;
    const bool            privileged_;
    HANDLE                wake_;
    std::mutex            mutex_;
    std::set<std::string> wanted_;
    std::deque<std::string> queue_;
    bool                  overflowed_ = false;
    size_t                unsent_     = 0;  // queued or taken, not yet Sent
};

class EventHub {
public:
    // True while any connection hears anything: callers guard payload building on it.
    bool Active() const { return listening_.load(std::memory_order_acquire) > 0; }

    // Raises `name` with `fields` for everyone and `private_fields` added for privileged
    // subscriptions. No-op after Close(). Prefer PublishIfActive, which builds nothing unheard.
    void Publish(const char* name, const JsonValue& fields, const JsonValue& private_fields = {});
    // `build(JsonValue& fields, JsonValue& private_fields)` runs only when someone listens.
    template <typename Build>
    void PublishIfActive(const char* name, Build&& build) {
        if (!Active()) return;
        JsonValue fields = JsonValue::MakeObject(), private_fields = JsonValue::MakeObject();
        build(fields, private_fields);
        Publish(name, fields, private_fields);
    }

    // Nothing is raised after this (a destructor's late change, say).
    void Close();
    // Waits up to `timeout_ms` for every queued line to be written; false on timeout.
    bool WaitDrained(DWORD timeout_ms);

private:
    friend class Subscription;
    void Add(Subscription* s);
    void Remove(Subscription* s);
    void Listening(int delta) { listening_.fetch_add(delta, std::memory_order_acq_rel); }

    std::mutex                 mutex_;
    std::vector<Subscription*> subs_;
    std::atomic<int>           listening_{0};  // subscriptions with at least one name
    bool                       closed_ = false;
};

}  // namespace ombra
