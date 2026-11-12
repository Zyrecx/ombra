// The last few seconds of per-tick encode facts, kept in memory and written out only when something
// goes wrong: a pipeline fault logs a summary, a crash logs it
// and puts the raw records in the minidump. Nothing is logged while things work.
//
// One recorder per pipeline, owned by its PipelineSupervisor and outliving every pacer generation, so
// the ticks before a fault are still there after the faulted pacer is joined and destroyed. The
// pacer is the only writer; Push() is a handful of plain stores and two release stores, no lock and
// no syscall, so it is allowed on the tick path. Readers may run on any thread at
// any time and skip a record the writer is in the middle of.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ombra {

struct FlightRecord {
    enum Flag : uint8_t {
        kLate       = 1,   // already past its deadline when the pacer woke
        kDuplicate  = 2,   // the source had nothing new: the previous frame again
        kResync     = 4,   // the pacer fell a full period behind and dropped `skipped` ticks first
        kKeyframe   = 8,
        kFirst      = 16,  // the first tick of a pacer generation (after a start, rebuild, resize)
        kEncodeFail = 32,  // the encoder faulted on this tick
    };
    int64_t  qpc          = 0;
    uint32_t encode_us    = 0;  // EncodeSlot (+ scale), capped at ~71 min
    uint32_t au_bytes     = 0;
    uint16_t overshoot_us = 0;  // how late the wait returned, capped at 65.5 ms
    uint8_t  flags        = 0;
    uint8_t  skipped      = 0;  // ticks dropped by a resync just before this one, capped at 255
};

class FlightRecorder {
public:
    static constexpr size_t kCapacity = 1024;  // 17 s at 60 fps, 4.3 s at 240; 32 KB with the sequence words

    // `name` labels the summary ("DISPLAY1"); copied. Registers the recorder for crash dumps.
    explicit FlightRecorder(const char* name);
    ~FlightRecorder();
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    // The pacer thread only.
    void Push(const FlightRecord& r);

    // One line for the log: counts, encode p50/p99/max and every tick's encode ms over the last
    // `seconds` before the newest record (L late, D duplicate, R resync, K keyframe, F first, X
    // failed). No allocation, so the crash path can call it; returns the length written.
    size_t FormatSummary(char* out, size_t cap, double seconds) const;
    std::string Summary(double seconds) const;

    // The crash path (crash.cpp): every live recorder's raw ring, for a minidump user stream, without
    // allocating. Returns how many were written to `out`.
    struct RawView {
        const void* data;
        size_t      bytes;
    };
    static size_t RawViews(RawView* out, size_t cap);
    // Calls FormatSummary for every live recorder into `buf` and hands each line to `emit`.
    static void ForEachSummary(char* buf, size_t cap, double seconds, void (*emit)(const char* line));

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};  // index + 1 once written, 0 while being written
        FlightRecord          rec;
    };
    bool Read(uint64_t index, FlightRecord& out) const;

    char                              name_[32] = {};
    std::array<Slot, kCapacity>       slots_;
    std::atomic<uint64_t>             head_{0};  // records ever pushed
};

}  // namespace ombra
