#include "ombra/flight_recorder.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ombra {

namespace {

constexpr size_t                  kMaxRecorders = 8;  // one per monitor pipeline
std::atomic<FlightRecorder*>      g_registry[kMaxRecorders];

int64_t QpcFrequency() {
    static const int64_t f = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return li.QuadPart;
    }();
    return f;
}

}  // namespace

FlightRecorder::FlightRecorder(const char* name) {
    std::snprintf(name_, sizeof(name_), "%s", name ? name : "");
    for (auto& r : g_registry) {
        FlightRecorder* expected = nullptr;
        if (r.compare_exchange_strong(expected, this)) break;
    }
}

FlightRecorder::~FlightRecorder() {
    for (auto& r : g_registry) {
        FlightRecorder* expected = this;
        if (r.compare_exchange_strong(expected, nullptr)) break;
    }
}

void FlightRecorder::Push(const FlightRecord& r) {
    // Single writer: head_ is only stored here. A seqlock per slot: 0 marks it torn while written.
    const uint64_t index = head_.load(std::memory_order_relaxed);
    Slot&          slot  = slots_[index % kCapacity];
    slot.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.rec = r;
    slot.seq.store(index + 1, std::memory_order_release);
    head_.store(index + 1, std::memory_order_release);
}

bool FlightRecorder::Read(uint64_t index, FlightRecord& out) const {
    const Slot& slot = slots_[index % kCapacity];
    if (slot.seq.load(std::memory_order_acquire) != index + 1) return false;
    out = slot.rec;
    std::atomic_thread_fence(std::memory_order_acquire);
    return slot.seq.load(std::memory_order_relaxed) == index + 1;
}

size_t FlightRecorder::FormatSummary(char* out, size_t cap, double seconds) const {
    if (cap == 0) return 0;
    out[0] = 0;
    const uint64_t head = head_.load(std::memory_order_acquire);
    FlightRecord   newest;
    if (head == 0 || !Read(head - 1, newest)) {
        const int n = _snprintf_s(out, cap, _TRUNCATE, "flight recorder %s: no ticks recorded", name_);
        return n < 0 ? std::strlen(out) : static_cast<size_t>(n);
    }
    const int64_t from_qpc = newest.qpc - static_cast<int64_t>(seconds * static_cast<double>(QpcFrequency()));

    // Oldest index in the window. Records keep their order; a torn one (being overwritten as the
    // oldest) ends the walk.
    uint64_t first = head - 1;
    for (uint64_t i = head - 1; i > 0 && head - i < kCapacity; --i) {
        FlightRecord r;
        if (!Read(i - 1, r) || r.qpc < from_qpc) break;
        first = i - 1;
    }

    uint32_t encode[kCapacity];
    size_t   n = 0, late = 0, dup = 0, resyncs = 0, skipped = 0, keys = 0;
    uint64_t bytes = 0;
    uint32_t max_bytes = 0, max_encode = 0;
    int64_t  max_encode_qpc = 0;
    for (uint64_t i = first; i < head; ++i) {
        FlightRecord r;
        if (!Read(i, r)) continue;
        encode[n++] = r.encode_us;
        late += (r.flags & FlightRecord::kLate) != 0;
        dup += (r.flags & FlightRecord::kDuplicate) != 0;
        keys += (r.flags & FlightRecord::kKeyframe) != 0;
        if (r.flags & FlightRecord::kResync) {
            ++resyncs;
            skipped += r.skipped;
        }
        bytes += r.au_bytes;
        max_bytes = std::max(max_bytes, r.au_bytes);
        if (r.encode_us >= max_encode) {
            max_encode     = r.encode_us;
            max_encode_qpc = r.qpc;
        }
    }
    if (n == 0) return 0;
    const double span = static_cast<double>(newest.qpc - from_qpc) / static_cast<double>(QpcFrequency());
    uint32_t     sorted[kCapacity];
    std::copy(encode, encode + n, sorted);
    std::sort(sorted, sorted + n);
    const double p50 = static_cast<double>(sorted[n / 2]) / 1000.0;
    const double p99 = static_cast<double>(sorted[std::min(n - 1, (n * 99) / 100)]) / 1000.0;
    const double max_at =
        static_cast<double>(max_encode_qpc - newest.qpc) / static_cast<double>(QpcFrequency());

    size_t len = 0;
    auto   put = [&](const char* fmt, auto... args) {
        if (len + 1 >= cap) return;
        const int w = _snprintf_s(out + len, cap - len, _TRUNCATE, fmt, args...);
        len         = w < 0 ? cap - 1 : len + static_cast<size_t>(w);
    };
    put("flight recorder %s, last %.1f s: %zu ticks, %zu late, %zu duplicate, %zu resyncs (%zu ticks dropped), "
        "%zu keyframes; encode ms p50 %.1f p99 %.1f max %.1f at %.2f s; AU avg %.1f KB max %.1f KB; encode ms per tick:",
        name_, span, n, late, dup, resyncs, skipped, keys, p50, p99, max_encode / 1000.0, max_at,
        static_cast<double>(bytes) / static_cast<double>(n) / 1024.0, max_bytes / 1024.0);
    for (uint64_t i = first; i < head && len + 8 < cap; ++i) {
        FlightRecord r;
        if (!Read(i, r)) continue;
        char marks[8];
        int  m = 0;
        if (r.flags & FlightRecord::kFirst) marks[m++] = 'F';
        if (r.flags & FlightRecord::kLate) marks[m++] = 'L';
        if (r.flags & FlightRecord::kDuplicate) marks[m++] = 'D';
        if (r.flags & FlightRecord::kResync) marks[m++] = 'R';
        if (r.flags & FlightRecord::kKeyframe) marks[m++] = 'K';
        if (r.flags & FlightRecord::kEncodeFail) marks[m++] = 'X';
        marks[m] = 0;
        put(" %u%s", (r.encode_us + 500) / 1000, marks);
    }
    return len;
}

std::string FlightRecorder::Summary(double seconds) const {
    std::string s(4000, '\0');
    s.resize(FormatSummary(s.data(), s.size() + 1, seconds));
    return s;
}

size_t FlightRecorder::RawViews(RawView* out, size_t cap) {
    size_t n = 0;
    for (auto& r : g_registry) {
        FlightRecorder* rec = r.load(std::memory_order_acquire);
        if (rec && n < cap) out[n++] = {rec, sizeof(FlightRecorder)};
    }
    return n;
}

void FlightRecorder::ForEachSummary(char* buf, size_t cap, double seconds, void (*emit)(const char* line)) {
    for (auto& r : g_registry) {
        FlightRecorder* rec = r.load(std::memory_order_acquire);
        if (rec && rec->FormatSummary(buf, cap, seconds) > 0) emit(buf);
    }
}

}  // namespace ombra
