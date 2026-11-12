// Lock-free latency histogram and tick counters, shared by the pacer (writer, once per tick) and
// the telemetry sampler (reader, any thread, while the pacer keeps running).
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ombra {

// Fixed 0.25 ms buckets covering 0-100 ms, then doubling ones to 3.2 s, so a 100 ms stall and a
// 2 s one read differently; the last bucket absorbs everything slower. Recording is one relaxed
// atomic increment, no allocation and no lock, so it is safe on the pacer's tick path.
// This replaced per-tick std::vector::push_back, which grew without bound in the resident engine
// and copied megabytes on the tick thread whenever the vector reallocated.
class LatencyHistogram {
public:
    static constexpr double kBucketMs    = 0.25;
    static constexpr size_t kLinearBuckets = 400;
    static constexpr size_t kBucketCount   = kLinearBuckets + 5;  // 200, 400, 800, 1600, 3200 ms

    // A bucket's upper edge.
    static constexpr double EdgeMs(size_t i) {
        if (i < kLinearBuckets) return static_cast<double>(i + 1) * kBucketMs;
        return 100.0 * static_cast<double>(uint64_t{2} << (i - kLinearBuckets));
    }

    struct Snapshot {
        std::array<uint64_t, kBucketCount> counts{};

        uint64_t Total() const {
            uint64_t total = 0;
            for (uint64_t c : counts) total += c;
            return total;
        }

        // Upper edge of the bucket holding the pct-th sample (pct in [0, 1]), 0 when empty. The last
        // bucket reports 3200 ms, meaning "1600 ms or slower".
        double Percentile(double pct) const {
            const uint64_t total = Total();
            if (total == 0) return 0.0;
            uint64_t rank = static_cast<uint64_t>(std::ceil(pct * static_cast<double>(total)));
            if (rank == 0) rank = 1;
            uint64_t seen = 0;
            for (size_t i = 0; i < kBucketCount; ++i) {
                seen += counts[i];
                if (seen >= rank) return EdgeMs(i);
            }
            return EdgeMs(kBucketCount - 1);
        }

        double Max() const {
            for (size_t i = kBucketCount; i-- > 0;) {
                if (counts[i] != 0) return EdgeMs(i);
            }
            return 0.0;
        }

        void Add(const Snapshot& other) {
            for (size_t i = 0; i < kBucketCount; ++i) counts[i] += other.counts[i];
        }

        // Per-bucket saturating difference. Saturating because a reader can briefly see a torn-down
        // generation's counts disappear before the supervisor folds them into its retired total.
        Snapshot Since(const Snapshot& earlier) const {
            Snapshot out;
            for (size_t i = 0; i < kBucketCount; ++i)
                out.counts[i] = counts[i] > earlier.counts[i] ? counts[i] - earlier.counts[i] : 0;
            return out;
        }

        void KeepMax(const Snapshot& other) {
            for (size_t i = 0; i < kBucketCount; ++i)
                if (other.counts[i] > counts[i]) counts[i] = other.counts[i];
        }
    };

    void Record(double ms) noexcept {
        size_t i = ms <= 0.0 ? 0 : static_cast<size_t>(ms / kBucketMs);
        if (i >= kLinearBuckets) {
            i = kLinearBuckets;
            while (i < kBucketCount - 1 && ms >= EdgeMs(i)) ++i;
        }
        buckets_[i].fetch_add(1, std::memory_order_relaxed);
    }

    Snapshot Read() const noexcept {
        Snapshot out;
        for (size_t i = 0; i < kBucketCount; ++i)
            out.counts[i] = buckets_[i].load(std::memory_order_relaxed);
        return out;
    }

private:
    std::array<std::atomic<uint64_t>, kBucketCount> buckets_{};
};

// Everything the pacer counts, as one value type: the supervisor accumulates it across pipeline
// generations, and a periodic reader diffs two readings to get per-interval numbers.
struct TickCounters {
    uint64_t ticks      = 0;
    uint64_t duplicates = 0;
    uint64_t late       = 0;
    uint64_t resyncs    = 0;
    LatencyHistogram::Snapshot wait_overshoot;
    LatencyHistogram::Snapshot encode;

    void Add(const TickCounters& o) {
        ticks += o.ticks;
        duplicates += o.duplicates;
        late += o.late;
        resyncs += o.resyncs;
        wait_overshoot.Add(o.wait_overshoot);
        encode.Add(o.encode);
    }

    // Same saturating semantics as Snapshot::Since.
    TickCounters Since(const TickCounters& earlier) const {
        auto sub = [](uint64_t a, uint64_t b) { return a > b ? a - b : 0; };
        TickCounters out;
        out.ticks          = sub(ticks, earlier.ticks);
        out.duplicates     = sub(duplicates, earlier.duplicates);
        out.late           = sub(late, earlier.late);
        out.resyncs        = sub(resyncs, earlier.resyncs);
        out.wait_overshoot = wait_overshoot.Since(earlier.wait_overshoot);
        out.encode         = encode.Since(earlier.encode);
        return out;
    }

    // Pairs with Since: keeping the per-field maximum as the new baseline means a transient dip
    // (see Snapshot::Since) is neither counted twice nor lost once the counts reappear.
    void KeepMax(const TickCounters& o) {
        if (o.ticks > ticks) ticks = o.ticks;
        if (o.duplicates > duplicates) duplicates = o.duplicates;
        if (o.late > late) late = o.late;
        if (o.resyncs > resyncs) resyncs = o.resyncs;
        wait_overshoot.KeepMax(o.wait_overshoot);
        encode.KeepMax(o.encode);
    }
};

}  // namespace ombra
