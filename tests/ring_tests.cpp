// Ring (encoded-AU ring) unit tests, independent of capture/encode: byte-budget eviction (not just
// GOP-boundary trimming, which the real capture path rarely forces since screen content is small),
// pin protection against eviction (controlled overshoot), barrier T_start clamping, and per-segment
// SPS/PPS storage across resolution changes. Run against both RamArena and FileArena so every
// assertion exercises both backends identically. Headless, no GPU. Ported from main.cpp's
// RunRingTest/RunRingTestPass (was `ombra.exe ringtest`) — same cases, doctest assertions instead
// of printf/++fails.

#include <doctest/doctest.h>

#include "support/test_env.h"

#include "ombra/ring.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

namespace {

void InitBackend(Ring& r, bool use_file, size_t cap, size_t overshoot, const wchar_t* name) {
    if (use_file) r.Init(cap, overshoot, TestOutputPath(std::wstring(name) + L".ringtest.dat"));
    else r.Init(cap, overshoot);
}

std::vector<uint8_t> PayloadFor(int64_t q, size_t frame_bytes) {
    return std::vector<uint8_t>(frame_bytes, static_cast<uint8_t>(q));
}

}  // namespace

TEST_CASE("ring: a file it can't create falls back to RAM and keeps recording") {
    Ring ring;
    // '?' is reserved on NTFS whichever drives exist.
    CHECK_FALSE(ring.Init(4000, 1000, TestOutputPath(L"bad?folder\\ring.dat")));
    CHECK(ring.TakeStorageLost());  // so the user hears about it
    CHECK_FALSE(ring.TakeStorageLost());
    const std::vector<uint8_t> frame(100, 7);
    ring.Append(frame.data(), frame.size(), 10, 0, true);
    auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() == 1);
    std::vector<uint8_t> out;
    CHECK(ring.ReadInto(pinned[0], out));
    CHECK(out == frame);
    ring.Unpin();

    // A good path after a bad one really moves it back to a file.
    CHECK(ring.Init(4000, 1000, TestOutputPath(L"fallback.ringtest.dat")));
}

TEST_CASE("CheckRingFile: says no to a bad folder or a disk that's too small, and leaves nothing") {
    const std::wstring good = TestOutputPath(L"check\\ring.dat");
    std::string why;
    CHECK(CheckRingFile(good, 1024, &why));
    CHECK(GetFileAttributesW(good.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((good + L".probe").c_str()) == INVALID_FILE_ATTRIBUTES);

    CHECK_FALSE(CheckRingFile(TestOutputPath(L"bad?folder\\ring.dat"), 1024, &why));
    CHECK_FALSE(why.empty());

    why.clear();
    CHECK_FALSE(CheckRingFile(good, size_t{1} << 62, &why));
    CHECK(why.find("free") != std::string::npos);
}

TEST_CASE("ring: a barrier arriving late still sorts by time") {
    Ring ring;
    ring.Init(100000, 1000);
    const std::vector<uint8_t> frame(100, 1);
    for (int64_t q = 0; q < 10; ++q) ring.Append(frame.data(), frame.size(), q * 100, q, true);

    ring.Barrier(800);  // a rebuild's barrier during a save...
    ring.Barrier(300);  // ...then that save's cut at its older press
    auto pinned = ring.Pin(0);
    REQUIRE_FALSE(pinned.empty());
    CHECK(pinned.front().qpc == 800);  // the rebuild's still bounds the next save
    ring.Unpin();
}

// use_file is the RamArena-vs-FileArena axis; every case below runs once per backend.
TEST_CASE("ring: eviction, pin protection, and raw-dump fidelity") {
    const bool use_file = GENERATE(false, true);
    CAPTURE(use_file);

    const size_t kFrameBytes = 1000;
    Ring ring;
    // 10 "frames" of 1000 bytes each, IDR every 3rd frame (a 3-frame GOP) -> capacity for ~3.5
    // GOPs so eviction is forced well before the test ends.
    InitBackend(ring, use_file, kFrameBytes * 10, kFrameBytes * 30, L"ring");

    // Each AU's payload is filled with its own qpc byte so a readback can be checked against what
    // was actually written, not just its length — a same-length garbage read must fail this test.
    int64_t qpc = 0;
    auto append_gop = [&](int frame_count) {
        for (int i = 0; i < frame_count; ++i, ++qpc) {
            std::vector<uint8_t> payload = PayloadFor(qpc, kFrameBytes);
            ring.Append(payload.data(), payload.size(), qpc, static_cast<uint64_t>(qpc), /*is_keyframe=*/i == 0);
        }
    };

    // Push 8 GOPs of 3 frames (24 frames, 24000 bytes) through a 10000-byte ring: eviction must
    // have run, and whatever remains must start on a keyframe.
    for (int gop = 0; gop < 8; ++gop) append_gop(3);
    auto all = ring.Pin(0);  // min_qpc=0: everything still live
    REQUIRE(!all.empty());
    CHECK(all.front().is_keyframe);
    ring.Unpin();

    // Pin an old range, then append past capacity: the pin must protect its data (ring overshoots
    // its byte budget) rather than evicting into a range a save is reading.
    auto old_range = ring.Pin(0);
    REQUIRE(!old_range.empty());
    const int64_t pinned_qpc = old_range.front().qpc;
    for (int gop = 0; gop < 5; ++gop) append_gop(3);  // well past capacity again, while pinned
    bool pinned_still_readable = true;
    std::vector<uint8_t> readback;
    for (const auto& d : old_range) {
        ring.ReadInto(d, readback);
        if (readback != PayloadFor(d.qpc, kFrameBytes)) { pinned_still_readable = false; break; }
    }
    CHECK(pinned_still_readable);

    // FileArena-specific: prove the file itself holds what the descriptors claim, not just that
    // Ring's API returns the right bytes back. Reads the mapped file directly, outside the
    // Ring/IRingArena API, and reassembles wrap the same way FileArena::ReadInto does —
    // descriptors here are known to straddle the file end (24 frames through a 10-frame-capacity
    // ring), so a reader that assumes no wrap would false-fail here.
    if (use_file) {
        const std::wstring path = TestOutputPath(L"ring.ringtest.dat");
        HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(fh != INVALID_HANDLE_VALUE);
        const size_t physical_bytes = kFrameBytes * 10 + kFrameBytes * 30;
        std::vector<uint8_t> dump(physical_bytes);
        DWORD read_bytes = 0;
        const bool dump_ok = ReadFile(fh, dump.data(), static_cast<DWORD>(dump.size()), &read_bytes, nullptr) &&
                              read_bytes == dump.size();
        CloseHandle(fh);
        REQUIRE(dump_ok);

        bool raw_matches = true;
        for (const auto& d : old_range) {
            std::vector<uint8_t> reassembled(d.length);
            const size_t first_chunk = (std::min)(d.length, physical_bytes - d.offset);
            std::memcpy(reassembled.data(), dump.data() + d.offset, first_chunk);
            if (first_chunk < d.length)
                std::memcpy(reassembled.data() + first_chunk, dump.data(), d.length - first_chunk);
            if (reassembled != PayloadFor(d.qpc, kFrameBytes)) { raw_matches = false; break; }
        }
        CHECK(raw_matches);
    }
    ring.Unpin();

    // After unpin, the same old range must eventually be evictable again (no permanent leak).
    for (int gop = 0; gop < 8; ++gop) append_gop(3);
    auto post = ring.Pin(0);
    const bool old_still_present = !post.empty() && post.front().qpc <= pinned_qpc;
    ring.Unpin();
    CHECK(!old_still_present);
}

// Barrier tests.
TEST_CASE("ring: barrier clamps T_start to the newest barrier on record") {
    const bool use_file = GENERATE(false, true);
    CAPTURE(use_file);

    const size_t kFrameBytes = 1000;
    Ring bring;
    InitBackend(bring, use_file, kFrameBytes * 20, kFrameBytes * 10, L"bring");
    int64_t bqpc = 0;
    auto append_gop_b = [&](int frame_count) {
        for (int i = 0; i < frame_count; ++i, ++bqpc) {
            std::vector<uint8_t> payload = PayloadFor(bqpc, kFrameBytes);
            bring.Append(payload.data(), payload.size(), bqpc, static_cast<uint64_t>(bqpc), /*is_keyframe=*/i == 0);
        }
    };

    // 5 GOPs (15 frames, qpc 0..14), then a barrier right after frame 6 (qpc=6, a keyframe —
    // frame_count=3 per GOP so keyframes land at qpc 0,3,6,9,12), then one more GOP (qpc 15..17).
    // Total 18 frames = 18000 bytes, under the ring's 20000-byte soft budget, so nothing evicts —
    // qpc 0..5 stay physically present specifically so this test exercises the barrier's clamp,
    // not eviction coincidentally producing the same answer.
    for (int g = 0; g < 5; ++g) append_gop_b(3);
    bring.Barrier(6);
    append_gop_b(3);

    // A request for min_qpc=0 (everything) must clamp to the barrier: T_start must be qpc=6, not
    // qpc=0, even though qpc=0..5 are still physically present and unevicted.
    auto clamped = bring.Pin(0);
    REQUIRE(!clamped.empty());
    CHECK(clamped.front().qpc == 6);
    bring.Unpin();

    // A barrier newer than every surviving descriptor must yield an empty window, not a
    // clamp-to-nearest-available or a crash.
    bring.Barrier(1000);
    auto past_everything = bring.Pin(0);
    CHECK(past_everything.empty());
    bring.Unpin();
}

TEST_CASE("ring: two barriers on record clamp to the newest, not the oldest") {
    const bool use_file = GENERATE(false, true);
    CAPTURE(use_file);

    const size_t kFrameBytes = 1000;
    Ring bring2;
    InitBackend(bring2, use_file, kFrameBytes * 20, kFrameBytes * 10, L"bring2");
    int64_t b2qpc = 0;
    auto append_gop_b2 = [&](int frame_count) {
        for (int i = 0; i < frame_count; ++i, ++b2qpc) {
            std::vector<uint8_t> payload = PayloadFor(b2qpc, kFrameBytes);
            bring2.Append(payload.data(), payload.size(), b2qpc, static_cast<uint64_t>(b2qpc), /*is_keyframe=*/i == 0);
        }
    };
    for (int g = 0; g < 3; ++g) append_gop_b2(3);  // qpc 0..8, keyframes at 0,3,6
    bring2.Barrier(3);
    for (int g = 0; g < 3; ++g) append_gop_b2(3);  // qpc 9..17, keyframes at 9,12,15
    bring2.Barrier(9);
    auto newest_clamp = bring2.Pin(0);
    REQUIRE(!newest_clamp.empty());
    CHECK(newest_clamp.front().qpc == 9);
    bring2.Unpin();
}

// Per-segment SPS/PPS storage. The bug this exists to
// prevent: after two resolution changes the encoder's cached SequenceParams() describes the
// *newest* segment, so a save of older footage muxes an avcC that does not match its own
// bitstream. Content-addressable here too — each segment's fake "SPS/PPS" blob is its own marker
// byte, so a wrong answer names the wrong segment rather than just being the wrong length.
TEST_CASE("ring: per-segment SPS/PPS survives two resolution changes") {
    const bool use_file = GENERATE(false, true);
    CAPTURE(use_file);

    const size_t kFrameBytes = 1000;
    auto seg = [](uint8_t marker, uint32_t w, uint32_t h) {
        auto p = std::make_shared<SegmentParams>();
        p->codec_header.assign(8, marker);
        p->width  = w;
        p->height = h;
        return p;
    };
    auto pinned_marker = [&](Ring& r, int64_t min_qpc) {
        r.Pin(min_qpc);
        auto params = r.PinnedSegmentParams();
        r.Unpin();
        return params ? std::pair<int, uint32_t>{params->codec_header.empty() ? -1 : params->codec_header[0], params->width}
                      : std::pair<int, uint32_t>{-1, 0};
    };

    SUBCASE("two barriers deep, the pinned window reports its own segment") {
        Ring sring;
        InitBackend(sring, use_file, kFrameBytes * 20, kFrameBytes * 10, L"sring");
        int64_t sqpc = 0;
        auto append_gop_s = [&](int frame_count) {
            for (int i = 0; i < frame_count; ++i, ++sqpc) {
                std::vector<uint8_t> payload = PayloadFor(sqpc, kFrameBytes);
                sring.Append(payload.data(), payload.size(), sqpc, static_cast<uint64_t>(sqpc), /*is_keyframe=*/i == 0);
            }
        };
        // Segment A (1920x1080), then a resolution change to 1280x720 (segment B), then another to
        // 2560x1440 (segment C) — the two-changes case a single resolution change cannot catch.
        sring.SetBaseSegmentParams(seg('A', 1920, 1080));
        for (int g = 0; g < 2; ++g) append_gop_s(3);  // qpc 0..5
        sring.Barrier(sqpc, seg('B', 1280, 720));
        for (int g = 0; g < 2; ++g) append_gop_s(3);  // qpc 6..11
        sring.Barrier(sqpc, seg('C', 2560, 1440));
        for (int g = 0; g < 2; ++g) append_gop_s(3);  // qpc 12..17

        auto [marker, width] = pinned_marker(sring, 0);
        CHECK(marker == 'C');
        CHECK(width == 2560);
    }

    SUBCASE("base params before any barrier exists") {
        // A save in the first seconds of a run has no barrier to read params off, and must still
        // get the parameters it was encoded with.
        Ring base_ring;
        InitBackend(base_ring, use_file, kFrameBytes * 20, kFrameBytes * 10, L"base_ring");
        base_ring.SetBaseSegmentParams(seg('A', 1920, 1080));
        int64_t nqpc = 0;
        for (int i = 0; i < 6; ++i, ++nqpc) {
            std::vector<uint8_t> payload = PayloadFor(nqpc, kFrameBytes);
            base_ring.Append(payload.data(), payload.size(), nqpc, static_cast<uint64_t>(nqpc), /*is_keyframe=*/i % 3 == 0);
        }
        auto [bmarker, bwidth] = pinned_marker(base_ring, 0);
        CHECK(bmarker == 'A');
        CHECK(bwidth == 1920);
    }

    SUBCASE("eviction promotes a pruned barrier's params to the base") {
        // Without this, a long-running ring that has wrapped past its last resolution change would
        // fall back to the *previous* segment's SPS/PPS — the same class of bug, just delayed by
        // an hour of runtime.
        Ring evict_ring;
        InitBackend(evict_ring, use_file, kFrameBytes * 6, kFrameBytes * 6, L"evict_ring");  // small budget: eviction is the point
        evict_ring.SetBaseSegmentParams(seg('A', 1920, 1080));
        int64_t eqpc = 0;
        auto append_gop_e = [&](int frame_count) {
            for (int i = 0; i < frame_count; ++i, ++eqpc) {
                std::vector<uint8_t> payload = PayloadFor(eqpc, kFrameBytes);
                evict_ring.Append(payload.data(), payload.size(), eqpc, static_cast<uint64_t>(eqpc), /*is_keyframe=*/i == 0);
            }
        };
        append_gop_e(3);
        evict_ring.Barrier(eqpc, seg('B', 1280, 720));
        for (int g = 0; g < 4; ++g) append_gop_e(3);  // pushes the pre-barrier GOP out entirely
        CHECK(evict_ring.BarrierCount() == 0);
        auto [emarker, ewidth] = pinned_marker(evict_ring, 0);
        CHECK(emarker == 'B');
        CHECK(ewidth == 1280);
    }
}

// The idle-desktop growth: a byte budget far bigger than tiny AUs will ever fill kept hours of
// footage (and a descriptor per frame) that no save window reaches. SetMaxAge bounds it by time.
TEST_CASE("ring: max age evicts whole GOPs no save can reach, and keeps the save window whole") {
    constexpr int64_t kGop = 30;       // frames per GOP; qpc is in frames here
    constexpr int64_t kMaxAge = 300;   // 10 GOPs
    Ring ring;
    ring.Init(64 * 1024 * 1024, 1024 * 1024);  // never reached: 8-byte AUs
    ring.SetMaxAge(kMaxAge);
    const std::vector<uint8_t> payload(8, 0x42);
    int64_t q = 0;
    for (; q < 30 * kGop; ++q) {
        ring.Append(payload.data(), payload.size(), q, static_cast<uint64_t>(q), q % kGop == 0);
    }
    // Bytes held stay about max-age worth, not the 900 frames appended.
    CHECK(ring.LiveBytes() <= static_cast<size_t>(kMaxAge + 2 * kGop) * payload.size());
    // A save of exactly max-age seconds still starts at the first keyframe inside the window.
    const int64_t t_end = q;
    auto aus = ring.Pin(t_end - kMaxAge, t_end);
    REQUIRE_FALSE(aus.empty());
    CHECK(aus.front().qpc == t_end - kMaxAge);
    CHECK(aus.front().is_keyframe);
    ring.Unpin();
}

// A 120 s buffer saved 118-120 s: Pin() starts at the first keyframe inside the window. The save
// uses PinCovering(), which starts at the keyframe before it, so the window is covered in full.
TEST_CASE("ring: PinCovering starts at the keyframe at or before the floor, never across a barrier") {
    constexpr int64_t kGop = 30;
    constexpr int64_t kMaxAge = 300 + 2 * kGop;  // buffer + slack, as RingMaxAgeQpc
    Ring ring;
    ring.Init(64 * 1024 * 1024, 1024 * 1024);
    ring.SetMaxAge(kMaxAge);
    const std::vector<uint8_t> payload(8, 0x42);
    int64_t q = 0;
    for (; q < 30 * kGop + 7; ++q) {
        ring.Append(payload.data(), payload.size(), q, static_cast<uint64_t>(q), q % kGop == 0);
    }
    const int64_t floor = q - 300;  // 607: mid-GOP

    auto plain = ring.Pin(floor, q);
    REQUIRE_FALSE(plain.empty());
    CHECK(plain.front().qpc == 630);  // loses 23 frames
    ring.Unpin();

    auto covering = ring.PinCovering(floor, q);
    REQUIRE_FALSE(covering.empty());
    CHECK(covering.front().qpc == 600);
    CHECK(covering.front().is_keyframe);
    ring.Unpin();

    // A keyframe exactly on the floor is the start, not the one before it.
    auto exact = ring.PinCovering(600, q);
    REQUIRE_FALSE(exact.empty());
    CHECK(exact.front().qpc == 600);
    ring.Unpin();

    // A barrier after the covering keyframe wins: the save never spans a segment change.
    ring.Barrier(615, nullptr);
    auto barred = ring.PinCovering(floor, q);
    REQUIRE_FALSE(barred.empty());
    CHECK(barred.front().qpc == 630);
    ring.Unpin();
}

TEST_CASE("ring: max age never evicts under a pin") {
    constexpr int64_t kGop = 10;
    Ring ring;
    ring.Init(1024 * 1024, 1024 * 1024);
    ring.SetMaxAge(50);
    const std::vector<uint8_t> payload(8, 1);
    int64_t q = 0;
    for (; q < 100; ++q) ring.Append(payload.data(), payload.size(), q, static_cast<uint64_t>(q), q % kGop == 0);
    auto aus = ring.Pin(q - 50, q);
    REQUIRE_FALSE(aus.empty());
    const int64_t pinned_from = aus.front().qpc;
    for (; q < 300; ++q) ring.Append(payload.data(), payload.size(), q, static_cast<uint64_t>(q), q % kGop == 0);
    std::vector<uint8_t> out;
    ring.ReadInto(aus.front(), out);  // still readable: the pin held eviction back
    CHECK(out.size() == payload.size());
    ring.Unpin();
    ring.Append(payload.data(), payload.size(), q, static_cast<uint64_t>(q), true);
    auto after = ring.Pin(0, q + 1);
    REQUIRE_FALSE(after.empty());
    CHECK(after.front().qpc > pinned_from);  // released: the aged GOPs went on the next keyframe
    ring.Unpin();
}

namespace {

size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

// A RamArena that refuses to commit past `limit` bytes: the system out of commit.
class RefusingArena : public IRingArena {
public:
    RefusingArena(size_t size, size_t limit) : inner_(size), limit_(limit) {}
    bool Write(size_t offset, const uint8_t* data, size_t size) override { return inner_.Write(offset, data, size); }
    bool ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const override {
        return inner_.ReadInto(offset, length, out);
    }
    size_t Size() const override { return inner_.Size(); }
    bool   EnsureCommitted(size_t offset, size_t size) override {
        if (inner_.CommittedBytes() + inner_.UncommittedBytesIn(offset, size) > limit_) return false;
        return inner_.EnsureCommitted(offset, size);
    }
    size_t CommittedBytes() const override { return inner_.CommittedBytes(); }
    size_t DecommitFree(size_t begin, size_t length, size_t max_chunks) override {
        return inner_.DecommitFree(begin, length, max_chunks);
    }
    void   Decommit() override { inner_.Decommit(); }

private:
    RamArena inner_;
    size_t   limit_;
};

// Each AU's bytes are its index, so a read proves which AU it came from.
void AppendTagged(Ring& ring, size_t bytes, int i, int gop) {
    const std::vector<uint8_t> au(bytes, static_cast<uint8_t>(i));
    ring.Append(au.data(), au.size(), 1000 + i, static_cast<uint64_t>(i), i % gop == 0);
}

// A storage whose drive was removed: every Write and ReadInto fails once `gone` is set.
class VanishingArena : public IRingArena {
public:
    explicit VanishingArena(size_t size, bool* gone) : inner_(size), gone_(gone) {}
    bool Write(size_t offset, const uint8_t* data, size_t size) override {
        if (*gone_) return false;
        inner_.EnsureCommitted(offset, size);
        return inner_.Write(offset, data, size);
    }
    bool ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const override {
        return !*gone_ && inner_.ReadInto(offset, length, out);
    }
    size_t Size() const override { return inner_.Size(); }

private:
    RamArena inner_;
    bool*    gone_;
};

// Rewrites an access violation at `page` into the code a removed drive raises, so the real __try in
// FileArena runs on a real fault. Proves the handler; only pulling a real drive proves Windows raises
// this code.
void*  g_fault_page = nullptr;
LONG CALLBACK FaultAsInPageError(EXCEPTION_POINTERS* info) {
    auto* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->ExceptionInformation[1] >= reinterpret_cast<ULONG_PTR>(g_fault_page) &&
        record->ExceptionInformation[1] < reinterpret_cast<ULONG_PTR>(g_fault_page) + 4096) {
        record->ExceptionCode = EXCEPTION_IN_PAGE_ERROR;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void CheckPinnedTagged(Ring& ring, const std::vector<AuDescriptor>& pinned, size_t bytes) {
    std::vector<uint8_t> out;
    for (const auto& d : pinned) {
        REQUIRE(ring.ReadInto(d, out));
        REQUIRE(out.size() == bytes);
        const auto tag = static_cast<uint8_t>(d.qpc - 1000);
        CHECK(std::all_of(out.begin(), out.end(), [tag](uint8_t b) { return b == tag; }));
    }
}

}  // namespace

TEST_CASE("ring: memory is committed as footage arrives, not at Init") {
    constexpr size_t kMB = 1u << 20;
    const size_t before = PrivateBytes();
    Ring ring;
    ring.Init(512 * kMB, 16 * kMB);
    CHECK(ring.CommittedBytes() == 0);
    CHECK(PrivateBytes() < before + 8 * kMB);  // was 528 MB, zero-filled

    const std::vector<uint8_t> au(256 * 1024, 0x5A);
    for (int i = 0; i < 20; ++i) ring.Append(au.data(), au.size(), 1000 + i, static_cast<uint64_t>(i), i % 4 == 0);
    CHECK(ring.CommittedBytes() == 8 * kMB);  // 5 MB of footage, in 4 MB steps
    CHECK(PrivateBytes() < before + 16 * kMB);
}

TEST_CASE("ring: released once idle for its max age; the next append starts it over") {
    Ring ring;
    ring.Init(64u << 20, 4u << 20);
    ring.SetMaxAge(1000);
    const std::vector<uint8_t> au(64 * 1024, 0x11);
    for (int i = 0; i < 40; ++i) ring.Append(au.data(), au.size(), 100 + i * 10, static_cast<uint64_t>(i), i % 10 == 0);
    ring.Barrier(300);
    const int64_t last = 100 + 39 * 10;

    CHECK_FALSE(ring.ReleaseIfIdle(last + 999));  // a save could still reach the newest frames
    auto pinned = ring.Pin(0);
    REQUIRE(!pinned.empty());
    CHECK_FALSE(ring.ReleaseIfIdle(last + 5000));  // never under a save's pin
    ring.Unpin();

    CHECK(ring.ReleaseIfIdle(last + 1000));
    CHECK(ring.Released());
    CHECK(ring.CommittedBytes() == 0);
    CHECK(ring.LiveBytes() == 0);
    CHECK(ring.BarrierCount() == 0);
    CHECK(ring.Pin(0).empty());
    ring.Unpin();
    CHECK_FALSE(ring.ReleaseIfIdle(last + 9000));  // once

    // Re-arm: the supervisor drops the barrier its disarm latched, with the new segment's params.
    auto seg     = std::make_shared<SegmentParams>();
    seg->width   = 1280;
    seg->height  = 720;
    ring.Barrier(last + 9000, seg);
    for (int i = 0; i < 20; ++i)
        ring.Append(au.data(), au.size(), last + 9100 + i * 10, static_cast<uint64_t>(i), i % 10 == 0);
    CHECK_FALSE(ring.Released());
    CHECK(ring.CommittedBytes() > 0);
    const auto after = ring.Pin(0);
    REQUIRE(after.size() == 20);
    CHECK(after.front().qpc == last + 9100);
    REQUIRE(static_cast<bool>(ring.PinnedSegmentParams()));
    CHECK(ring.PinnedSegmentParams()->width == 1280);
    std::vector<uint8_t> out;
    ring.ReadInto(after.back(), out);
    CHECK(out == au);
    ring.Unpin();
}

TEST_CASE("ring: a refused commit reuses the oldest footage's memory instead of killing the engine") {
    constexpr size_t kMB = 1u << 20;
    Ring ring;
    ring.InitWithArena(std::make_unique<RefusingArena>(66 * kMB, 8 * kMB), 64 * kMB);  // 2 MB overshoot
    const size_t kAu = 300 * 1024;
    for (int i = 0; i < 400; ++i) AppendTagged(ring, kAu, i, 5);  // ~117 MB: wraps the arena
    CHECK(ring.CommitCapped());
    CHECK(ring.CommittedBytes() <= 8 * kMB);
    CHECK(ring.LiveBytes() <= 8 * kMB);
    // What's held is the newest footage, whole and readable.
    const auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() >= 10);
    CHECK(pinned.front().is_keyframe);
    CHECK(pinned.back().qpc == 1000 + 399);
    CheckPinnedTagged(ring, pinned, kAu);
    ring.Unpin();

    // A release returns the memory and lifts the cap; the next pass commits afresh.
    ring.SetMaxAge(1);
    CHECK(ring.ReleaseIfIdle(5000));
    CHECK_FALSE(ring.CommitCapped());
    CHECK(ring.CommittedBytes() == 0);
}

TEST_CASE("ring: a capped arena too small for an access unit drops that GOP, never a fragment") {
    Ring ring;
    ring.InitWithArena(std::make_unique<RefusingArena>(64u << 20, 0), 60u << 20);  // commits nothing
    std::vector<uint8_t> au(1000, 1);
    for (int i = 0; i < 10; ++i) ring.Append(au.data(), au.size(), i, static_cast<uint64_t>(i), i % 5 == 0);
    CHECK(ring.LiveBytes() == 0);
    CHECK(ring.Pin(0).empty());
    ring.Unpin();
}

// The arena is sized for buffer.seconds at max bitrate; age eviction keeps far less live, and the
// write position still walks the whole arena before it wraps. Trimming keeps commit near live.
TEST_CASE("ring: trimming keeps commit near live footage across wraps, and every byte intact") {
    constexpr size_t kMB = 1u << 20;
    constexpr size_t kAu = 100 * 1024;
    constexpr int    kGop = 30;
    Ring ring;
    ring.Init(60 * kMB, 4 * kMB);
    ring.SetMaxAge(150);  // 5 GOPs, ~15 MB of a 64 MB arena
    size_t worst = 0;
    for (int i = 0; i < 3000; ++i) {  // ~293 MB: 4+ laps
        AppendTagged(ring, kAu, i, kGop);
        if (i % 10 == 0) ring.TrimCommitted();
        if (i > 300) worst = (std::max)(worst, ring.CommittedBytes() - (std::min)(ring.CommittedBytes(), ring.LiveBytes()));
    }
    // Live, plus the chunks it partly covers, the one kept ahead, and trimming's one-per-call lag.
    CHECK(worst <= 4 * RamArena::kCommitChunk);
    CHECK(ring.CommittedBytes() < 32 * kMB);
    const auto pinned = ring.Pin(0);
    REQUIRE(!pinned.empty());
    CHECK(pinned.back().qpc == 1000 + 2999);
    CheckPinnedTagged(ring, pinned, kAu);
    ring.Unpin();
}

TEST_CASE("ring: a write that wraps onto a trimmed chunk 0 commits it again") {
    constexpr size_t kMB = 1u << 20;
    constexpr size_t kAu = 700 * 1024;  // 24 MB arena / 700 KB: the 36th AU straddles the end
    Ring ring;
    ring.Init(20 * kMB, 4 * kMB);
    ring.SetMaxAge(10);
    for (int i = 0; i < 34; ++i) {
        AppendTagged(ring, kAu, i, 2);
        while (ring.TrimCommitted() > 0) {}
    }
    for (int i = 34; i < 60; ++i) {  // across the end of the arena and over chunk 0 again
        AppendTagged(ring, kAu, i, 2);
        while (ring.TrimCommitted() > 0) {}
        const auto pinned = ring.Pin(0);
        REQUIRE(!pinned.empty());
        CHECK(pinned.back().qpc == 1000 + i);
        CheckPinnedTagged(ring, pinned, kAu);
        ring.Unpin();
    }
    CHECK(ring.CommittedBytes() <= 16 * kMB);
}

TEST_CASE("ring: trimming never touches what a pin holds") {
    constexpr size_t kAu = 64 * 1024;
    Ring ring;
    ring.Init(40u << 20, 4u << 20);
    ring.SetMaxAge(60);
    int i = 0;
    for (; i < 200; ++i) AppendTagged(ring, kAu, i, 10);
    const auto pinned = ring.Pin(1000 + i - 60);
    REQUIRE(!pinned.empty());
    for (; i < 700; ++i) {  // ~31 MB more: past the 44 MB arena's end while the pin holds its range
        AppendTagged(ring, kAu, i, 10);
        ring.TrimCommitted();
    }
    CheckPinnedTagged(ring, pinned, kAu);
    ring.Unpin();
}

TEST_CASE("ring: a trim's hold on the ring mutex") {
    // TrimCommitted runs on the watchdog, but Append waits on the same mutex while it runs: one
    // VirtualFree(MEM_DECOMMIT) of a touched 4 MB chunk.
    constexpr size_t kAu = 83 * 1024;
    Ring ring;
    ring.Init(256u << 20, 16u << 20);
    ring.SetMaxAge(120);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    std::vector<double> us;
    for (int i = 0; i < 6000; ++i) {
        AppendTagged(ring, kAu, i, 60);
        if (i % 12 != 0) continue;
        QueryPerformanceCounter(&a);
        const size_t freed = ring.TrimCommitted();
        QueryPerformanceCounter(&b);
        if (freed > 0) us.push_back(1e6 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart));
    }
    REQUIRE(!us.empty());
    std::sort(us.begin(), us.end());
    MESSAGE("TrimCommitted of a 4 MB chunk (" << us.size() << " trims): p50 " << us[us.size() / 2] << " us, worst "
                                              << us.back() << " us");
    CHECK(us[us.size() / 2] < 2000.0);
}

TEST_CASE("ring: Append with commits stays far inside a tick's budget") {
    // Writing footage into fresh address space: every 4 MB one VirtualAlloc(MEM_COMMIT), and the
    // first write to each page takes a demand-zero fault. That cost used to be paid once at Init.
    // 40 Mbps at 60 fps is ~83 KB a frame.
    constexpr size_t kAu = 83 * 1024;
    const std::vector<uint8_t> au(kAu, 0x42);
    Ring ring;
    ring.Init(256u << 20, 16u << 20);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    std::vector<double> us;
    const int frames = static_cast<int>((256u << 20) / kAu);
    us.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        QueryPerformanceCounter(&a);
        ring.Append(au.data(), au.size(), i, static_cast<uint64_t>(i), i % 120 == 0);
        QueryPerformanceCounter(&b);
        us.push_back(1e6 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart));
    }
    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    const double p50 = sorted[sorted.size() / 2], p999 = sorted[sorted.size() * 999 / 1000], worst = sorted.back();
    // The same writes into already-committed, already-touched memory: the steady state after a wrap.
    double warm_worst = 0.0;
    for (int i = 0; i < 2000; ++i) {
        QueryPerformanceCounter(&a);
        ring.Append(au.data(), au.size(), frames + i, static_cast<uint64_t>(frames + i), i % 120 == 0);
        QueryPerformanceCounter(&b);
        warm_worst = (std::max)(warm_worst, 1e6 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart));
    }
    MESSAGE("Append 83 KB into fresh memory: p50 " << p50 << " us, p99.9 " << p999 << " us, worst " << worst
                                                   << " us; into warm memory, worst " << warm_worst << " us");
    CHECK(p999 < 2000.0);
}

TEST_CASE("ring: a reader releasing behind itself lets eviction follow it") {
    // 10 AUs of budget, 5 of headroom: a pin of 10 that never released would break by the 6th
    // append. Releasing as it reads, the reader only ever holds back what it hasn't read yet.
    constexpr size_t kAu = 1000;
    constexpr int    kGop = 2;
    Ring ring;
    ring.Init(kAu * 10, kAu * 5);
    int i = 0;
    for (; i < 10; ++i) AppendTagged(ring, kAu, i, kGop);
    const auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() == 10);
    std::vector<uint8_t> out;
    for (size_t k = 0; k < pinned.size(); ++k) {
        REQUIRE(ring.ReadInto(pinned[k], out));
        CHECK(out.front() == static_cast<uint8_t>(k));
        ring.ReleaseThrough(pinned[k]);
        AppendTagged(ring, kAu, i++, kGop);  // the encoder keeps going while the save writes
    }
    CHECK(ring.LiveBytes() <= kAu * 15);
    ring.Unpin();
}

TEST_CASE("ring: a pin held past the headroom fails the read, not the process") {
    constexpr size_t kAu = 1000;
    Ring ring;
    ring.Init(kAu * 10, kAu * 5);
    int i = 0;
    for (; i < 10; ++i) AppendTagged(ring, kAu, i, 5);
    const auto pinned = ring.Pin(0);
    REQUIRE_FALSE(pinned.empty());
    std::vector<uint8_t> out;
    CHECK(ring.ReadInto(pinned.front(), out));
    for (; i < 40; ++i) AppendTagged(ring, kAu, i, 5);  // a stalled writer: the pin can't hold
    CHECK(ring.LiveBytes() <= kAu * 15);
    CHECK_FALSE(ring.ReadInto(pinned.back(), out));
    ring.Unpin();
    const auto next = ring.Pin(0);  // the next save works normally
    REQUIRE_FALSE(next.empty());
    CHECK(next.front().is_keyframe);
    CheckPinnedTagged(ring, next, kAu);
    ring.Unpin();
}

TEST_CASE("ring: Init waits for a pinned save to finish") {
    Ring ring;
    ring.Init(1u << 20, 1u << 20);
    for (int i = 0; i < 10; ++i) AppendTagged(ring, 100, i, 5);
    const auto pinned = ring.Pin(0);
    REQUIRE_FALSE(pinned.empty());
    std::atomic<bool> reinit_done{false};
    std::thread realloc([&] {
        ring.Init(2u << 20, 1u << 20);
        reinit_done = true;
    });
    Sleep(100);
    CHECK_FALSE(reinit_done.load());  // the save's bytes are still there to read
    std::vector<uint8_t> out;
    CHECK(ring.ReadInto(pinned.back(), out));
    CHECK(out.front() == 9);
    ring.Unpin();
    realloc.join();
    CHECK(reinit_done.load());
    CHECK(ring.Capacity() == (2u << 20));
}

TEST_CASE("ring: losing its storage moves to RAM, restarts at a keyframe and reports once") {
    bool gone = false;
    Ring ring;
    ring.InitWithArena(std::make_unique<VanishingArena>(1u << 20, &gone), 1u << 19);
    auto params = std::make_shared<SegmentParams>();
    params->width = 1234;
    for (int i = 0; i < 25; ++i) AppendTagged(ring, 1000, i, 10);
    ring.Barrier(1000 + 20, params);
    CHECK_FALSE(ring.TakeStorageLost());

    gone = true;
    AppendTagged(ring, 1000, 25, 10);  // mid-GOP: it references footage that's gone
    CHECK(ring.LiveBytes() == 0);
    CHECK(ring.TakeStorageLost());
    CHECK_FALSE(ring.TakeStorageLost());
    CHECK(ring.BarrierCount() == 0);

    for (int i = 26; i < 30; ++i) AppendTagged(ring, 1000, i, 10);  // the rest of that GOP: dropped too
    CHECK(ring.LiveBytes() == 0);
    for (int i = 30; i < 36; ++i) AppendTagged(ring, 1000, i, 10);

    const auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() == 6);
    CHECK(pinned.front().is_keyframe);
    CHECK(pinned.front().index == 30);
    REQUIRE(ring.PinnedSegmentParams().get() != nullptr);
    CHECK(ring.PinnedSegmentParams()->width == 1234);  // the segment goes on; its params stay
    CheckPinnedTagged(ring, pinned, 1000);
    ring.Unpin();
}

TEST_CASE("ring: a keyframe arriving as the storage goes starts the RAM arena") {
    bool gone = false;
    Ring ring;
    ring.InitWithArena(std::make_unique<VanishingArena>(1u << 20, &gone), 1u << 19);
    for (int i = 0; i < 5; ++i) AppendTagged(ring, 1000, i, 10);
    gone = true;
    AppendTagged(ring, 1000, 10, 10);
    CHECK(ring.TakeStorageLost());
    const auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() == 1);
    CheckPinnedTagged(ring, pinned, 1000);
    ring.Unpin();
}

TEST_CASE("ring: a save in progress when the storage goes fails instead of reading the new arena") {
    bool gone = false;
    Ring ring;
    ring.InitWithArena(std::make_unique<VanishingArena>(1u << 20, &gone), 1u << 19);
    for (int i = 0; i < 5; ++i) AppendTagged(ring, 1000, i, 10);
    const auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() == 5);
    std::vector<uint8_t> out;
    CHECK(ring.ReadInto(pinned[0], out));

    gone = true;
    SUBCASE("the writer finds out first") {
        AppendTagged(ring, 1000, 10, 10);  // into the new arena, at the offset pinned[0] names
        gone = false;                       // the old arena is out of the picture either way
        CHECK_FALSE(ring.ReadInto(pinned[0], out));
    }
    SUBCASE("the save finds out first") {
        CHECK_FALSE(ring.ReadInto(pinned[1], out));
        AppendTagged(ring, 1000, 5, 10);  // moves to RAM; mid-GOP, so dropped
        CHECK(ring.LiveBytes() == 0);
        CHECK_FALSE(ring.ReadInto(pinned[1], out));
    }
    ring.Unpin();
    CHECK(ring.TakeStorageLost());

    AppendTagged(ring, 1000, 20, 10);
    const auto next = ring.Pin(0);
    REQUIRE_FALSE(next.empty());
    CHECK(next.back().index == 20);
    CHECK(ring.ReadInto(next.back(), out));
    ring.Unpin();
}

TEST_CASE("ring: a failed page-in on a real mapped file doesn't kill the process") {
    const std::wstring path = TestOutputPath(L"page-fault.ringtest.dat");
    Ring ring;
    REQUIRE(ring.Init(1u << 19, 1u << 18, path));
    CHECK_FALSE(ring.TakeStorageLost());
    for (int i = 0; i < 5; ++i) AppendTagged(ring, 1000, i, 10);

    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(page);
    DWORD old_protect = 0;
    REQUIRE(VirtualProtect(page, 4096, PAGE_NOACCESS, &old_protect));
    g_fault_page = page;
    void* handler = AddVectoredExceptionHandler(1, FaultAsInPageError);
    REQUIRE(handler);
    ring.Append(static_cast<const uint8_t*>(page), 1000, 1005, 5, false);
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(page, 0, MEM_RELEASE);

    CHECK(ring.TakeStorageLost());
    CHECK(ring.LiveBytes() == 0);
    AppendTagged(ring, 1000, 10, 10);
    const auto pinned = ring.Pin(0);
    REQUIRE(pinned.size() == 1);
    CheckPinnedTagged(ring, pinned, 1000);
    ring.Unpin();
    ring.Init(1u << 19, 1u << 18);  // lets go of nothing on disk: TakeStorageLost closed the file
    DeleteFileW(path.c_str());
}

}  // namespace ombra::test
