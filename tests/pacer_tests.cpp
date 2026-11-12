// The pacer's first-frame hold, headless: a fake source and a fake encoder, no device.
#include "ombra/latency.h"
#include "ombra/pacer.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace ombra;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

namespace {

// Publishes slot 1 (frame 1) on Publish(); until then the latest slot is 0, the never-drawn one.
class FakeSource final : public ICaptureSource {
public:
    void Publish() {
        latest_.store(1);
        has_frame_.store(true);
    }

    std::atomic<bool> healthy{true};

    bool Open(bool, const CaptureTarget&, const LUID&) override { return true; }
    bool TryOpen(bool, const CaptureTarget&, const LUID&) override { return true; }
    const std::string& FaultReason() const override { return reason_; }
    void StartCapture() override {}
    void Close() override {}
    void SetCursorCaptureEnabled(bool) override {}
    ID3D11Device* Device() const override { return nullptr; }
    ID3D11Texture2D* SlotTexture(uint32_t) const override { return nullptr; }
    uint32_t Width() const override { return 64; }
    uint32_t Height() const override { return 64; }
    SlotClaim ClaimSlotForEncoding() override {
        claims.fetch_add(1);
        if (!has_frame_.load()) return {latest_.load(), 0};
        return {latest_.load(), fresh_each_claim.load() ? frame_.fetch_add(1) + 1 : 1u};
    }
    void ReleaseEncodingSlot() override {}
    bool HasFrame() const override { return has_frame_.load(); }
    bool SizeChanged() const override { return false; }
    uint32_t PendingWidth() const override { return 0; }
    uint32_t PendingHeight() const override { return 0; }
    uint32_t PendingRevision() const override { return 0; }
    bool IsTargetMinimized() const override { return false; }
    bool Recreate(uint32_t, uint32_t) override { return false; }
    bool IsHealthy() const override { return healthy.load(); }
    const char* HealthIssue() const override { return healthy.load() ? "healthy" : "fake fault"; }
    void ReadSlotRegionBGRA(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::vector<uint8_t>&, uint32_t&) override {}
    void ReleaseReadbackStaging() override {}
    void TestInjectItemClosed() override {}
    void TestInjectDeviceRemoved() override {}
    void TestForceContentSize(uint32_t, uint32_t) override {}

    std::atomic<int> claims{0};
    // Every claim sees a newer frame, still in slot 1: what two slots look like when the source
    // presents faster than the pacer ticks.
    std::atomic<bool> fresh_each_claim{false};

private:
    std::atomic<uint32_t> latest_{0};
    std::atomic<bool>     has_frame_{false};
    std::atomic<uint64_t> frame_{1};
    std::string           reason_;
};

struct Encoded {
    uint32_t              slot;
    uint64_t              timestamp;
    clock_type::time_point at;
};

class FakeEncoder final : public IVideoEncoder {
public:
    bool Open(ID3D11Device*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const EncoderOptions&) override { return true; }
    bool TryOpen(ID3D11Device*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const EncoderOptions&) override { return true; }
    VideoCodec Codec() const override { return VideoCodec::H264; }
    const std::vector<uint8_t>& SequenceParams() const override { return params_; }
    bool RegisterSlot(ID3D11Texture2D*, uint32_t, uint32_t) override { return true; }
    AccessUnit EncodeSlot(uint32_t slot, uint32_t, uint32_t, uint64_t timestamp) override {
        if (encode_time.count() > 0) std::this_thread::sleep_for(encode_time);
        std::lock_guard<std::mutex> lock(mutex_);
        encoded_.push_back({slot, timestamp, clock_type::now()});
        return AccessUnit{byte_, 1, encoded_.size() == 1, true};
    }
    void UnlockLast() override {}
    bool Faulted() const override { return false; }
    const std::string& FaultReason() const override { return reason_; }
    void Close() override {}

    std::vector<Encoded> Encodes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return encoded_;
    }

    std::chrono::milliseconds encode_time{0};

private:
    std::mutex           mutex_;
    std::vector<Encoded> encoded_;
    std::vector<uint8_t> params_;
    std::string          reason_;
    uint8_t              byte_[1] = {0};
};

constexpr uint32_t kFps = 60;

double MsSince(clock_type::time_point from, clock_type::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

}  // namespace

TEST_CASE("pacer: the first encode waits for the source's first frame") {
    FakeSource  source;
    FakeEncoder encoder;
    Pacer       pacer;
    std::vector<Tick> ticks;
    std::mutex        ticks_mutex;
    pacer.Start(source, encoder, kFps, [&](const IVideoEncoder::AccessUnit&, const Tick& t) {
        std::lock_guard<std::mutex> lock(ticks_mutex);
        ticks.push_back(t);
    });

    std::this_thread::sleep_for(150ms);
    CHECK(encoder.Encodes().empty());
    CHECK(source.claims.load() == 0);  // a held tick claims no slot
    const auto published = clock_type::now();
    source.Publish();
    std::this_thread::sleep_for(200ms);
    pacer.Stop();

    const auto encodes = encoder.Encodes();
    REQUIRE(!encodes.empty());
    CHECK(encodes.front().at >= published);
    for (const auto& e : encodes) CHECK(e.slot == 1);  // never the undrawn slot 0
    CHECK(encodes.front().timestamp == 0);
    REQUIRE(!ticks.empty());
    CHECK(ticks.front().index == 0);
    CHECK(pacer.FirstFrameWaitTicks() >= 5);
    CHECK(pacer.TicksSubmitted() == encodes.size());
    CHECK(pacer.DuplicateTicks() < encodes.size());  // the held ticks don't count as duplicates
}

TEST_CASE("pacer: a duplicate tick is the same frame again, not the same slot") {
    FakeSource  source;
    source.Publish();
    FakeEncoder encoder;
    Pacer       pacer;
    pacer.Start(source, encoder, kFps, [](const IVideoEncoder::AccessUnit&, const Tick&) {});
    std::this_thread::sleep_for(100ms);
    const uint64_t held = pacer.DuplicateTicks();  // frame 1 claimed over and over
    source.fresh_each_claim.store(true);
    std::this_thread::sleep_for(150ms);
    pacer.Stop();

    REQUIRE(encoder.Encodes().size() > 6);
    for (const auto& e : encoder.Encodes()) CHECK(e.slot == 1);
    CHECK(held > 0);
    CHECK(pacer.DuplicateTicks() <= held + 1);  // the tick racing the switch may still repeat
}

TEST_CASE("pacer: an encoder too slow for the frame rate keeps video time on the wall clock") {
    FakeSource  source;
    source.Publish();
    FakeEncoder encoder;
    encoder.encode_time = 50ms;  // three 60 fps periods per frame
    Pacer pacer;
    std::vector<Tick> ticks;
    std::mutex        ticks_mutex;
    pacer.Start(source, encoder, kFps, [&](const IVideoEncoder::AccessUnit&, const Tick& t) {
        std::lock_guard<std::mutex> lock(ticks_mutex);
        ticks.push_back(t);
    });
    std::this_thread::sleep_for(1500ms);
    pacer.Stop();

    const auto encodes = encoder.Encodes();
    REQUIRE(encodes.size() >= 10);
    CHECK(pacer.ResyncCount() > 0);
    uint64_t previous = encodes.front().timestamp;
    for (size_t i = 1; i < encodes.size(); ++i) {
        CHECK(encodes[i].timestamp > previous);  // the encoders and the mux need them rising
        previous = encodes[i].timestamp;
    }

    // What the mux plays (index / fps) matches how long the frames really took. Submitting every
    // frame's index + 1 made this about a third of the real time.
    const double played_ms = (encodes.back().timestamp - encodes.front().timestamp) * 1000.0 / kFps;
    const double real_ms   = MsSince(encodes.front().at, encodes.back().at);
    MESSAGE(encodes.size() << " frames, played " << played_ms << " ms, real " << real_ms << " ms");
    CHECK(played_ms == doctest::Approx(real_ms).epsilon(0.1));
    CHECK(pacer.TicksSubmitted() == encodes.size());

    std::lock_guard<std::mutex> lock(ticks_mutex);
    REQUIRE(ticks.size() == encodes.size());
    CHECK(ticks.back().index == encodes.back().timestamp);  // ring and encoder agree
}

TEST_CASE("pacer: a source that never delivers still starts encoding after the bound") {
    FakeSource  source;
    FakeEncoder encoder;
    Pacer       pacer;
    const auto  start = clock_type::now();
    pacer.Start(source, encoder, kFps, [](const IVideoEncoder::AccessUnit&, const Tick&) {});
    std::this_thread::sleep_for(Pacer::kFirstFrameWait + 500ms);
    pacer.Stop();

    const auto encodes = encoder.Encodes();
    REQUIRE(!encodes.empty());
    const double first_ms = MsSince(start, encodes.front().at);
    MESSAGE("first encode after " << first_ms << " ms");
    CHECK(first_ms >= 400.0);
    CHECK(first_ms <= 1500.0);
    CHECK(encodes.front().timestamp == 0);
    CHECK(encodes.size() >= 10);  // and keeps going: the gate is passed once, not per tick
}

TEST_CASE("pacer: Stop returns promptly while waiting for the first frame") {
    FakeSource  source;
    FakeEncoder encoder;
    Pacer       pacer;
    pacer.Start(source, encoder, kFps, [](const IVideoEncoder::AccessUnit&, const Tick&) {});
    std::this_thread::sleep_for(100ms);
    const auto stop_at = clock_type::now();
    pacer.Stop();
    CHECK(MsSince(stop_at, clock_type::now()) < 200.0);
    CHECK(encoder.Encodes().empty());
}

TEST_CASE("pacer: a fault while waiting for the first frame still ends the loop") {
    FakeSource  source;
    FakeEncoder encoder;
    Pacer       pacer;
    pacer.Start(source, encoder, kFps, [](const IVideoEncoder::AccessUnit&, const Tick&) {});
    std::this_thread::sleep_for(100ms);
    source.healthy.store(false);
    std::this_thread::sleep_for(100ms);
    source.healthy.store(true);
    source.Publish();  // a loop still running would encode this
    std::this_thread::sleep_for(150ms);
    pacer.Stop();
    CHECK(encoder.Encodes().empty());
    CHECK(pacer.TicksSubmitted() == 0);
}

TEST_CASE("LatencyHistogram: stalls past 100 ms keep their size") {
    const auto max_of = [](double ms) {
        LatencyHistogram h;
        h.Record(ms);
        return h.Read().Max();
    };
    CHECK(max_of(0.1) == doctest::Approx(0.25));
    CHECK(max_of(99.9) == doctest::Approx(100.0));
    CHECK(max_of(100.0) == doctest::Approx(200.0));
    CHECK(max_of(199.0) == doctest::Approx(200.0));
    CHECK(max_of(200.0) == doctest::Approx(400.0));
    CHECK(max_of(1500.0) == doctest::Approx(1600.0));
    CHECK(max_of(60000.0) == doctest::Approx(3200.0));  // the last bucket takes the rest

    LatencyHistogram h;
    for (int i = 0; i < 99; ++i) h.Record(1.0);
    h.Record(500.0);
    CHECK(h.Read().Percentile(0.5) == doctest::Approx(1.25));
    CHECK(h.Read().Percentile(1.0) == doctest::Approx(800.0));
}
