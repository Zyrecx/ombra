// Live reconfigure paths that don't need the GPU: a disarmed supervisor re-Inits its rings on a
// Realloc without building a pipeline generation.

#include <doctest/doctest.h>

#include "support/test_env.h"

#include "ombra/config.h"
#include "ombra/paths.h"
#include "ombra/controller.h"
#include "ombra/audio_tracks.h"
#include "ombra/reconfig.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"
#include "ombra/util.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace ombra::test {

TEST_CASE("capture: a frame back at the capture size withdraws a pending resize") {
    using Step = PendingResize::Step;
    PendingResize r;
    CHECK(r.OnFrame(1920, 1080, 1920, 1080) == Step::None);
    CHECK_FALSE(r.Changed());
    // No size yet (a window's degenerate frame) says nothing, in either direction.
    CHECK(r.OnFrame(0, 0, 1920, 1080) == Step::None);

    CHECK(r.OnFrame(2560, 1440, 1920, 1080) == Step::Started);
    CHECK(r.OnFrame(1280, 720, 1920, 1080) == Step::None);  // still moving: reported once, newest size
    CHECK(r.Changed());
    CHECK(r.Width() == 1280);
    CHECK(r.Height() == 720);
    CHECK(r.OnFrame(0, 0, 1920, 1080) == Step::None);
    CHECK(r.Changed());

    CHECK(r.OnFrame(1920, 1080, 1920, 1080) == Step::Reverted);
    CHECK_FALSE(r.Changed());

    const uint32_t revision = r.Revision();
    CHECK(r.OnFrame(2560, 1440, 1920, 1080) == Step::Started);
    CHECK(r.Revision() == revision + 1);  // the same size as before, but a new change
    CHECK(r.OnFrame(1280, 720, 1920, 1080) == Step::None);
    CHECK(r.Revision() == revision + 1);
    r.Clear();
    CHECK_FALSE(r.Changed());
}

TEST_CASE("controller: the worker brings a supervisor back in step with its decision") {
    // The switch off and on between two passes leaves ArmState unchanged with the immediate disarm
    // still standing; here the same disagreement is set up directly. No game, so the answer is off.
    constexpr size_t kMb = 1024 * 1024;
    Ring video;
    video.Init(1 * kMb, 64 * 1024);
    PipelineSupervisor sup(60, 10000, 2, &video);
    sup.SetArmed(true);
    BufferController controller("game", "monitor", "", {&sup});
    controller.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sup.Armed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(sup.Armed());
    controller.Stop();
}

TEST_CASE("reconfig: a live buffer.seconds change resizes the audio hub's track rings") {
    // Audio used to be resized by the supervisor, one PcmRing per source; now it's the hub's job
    // and ApplyLiveConfig's to ask for it. A missed resize leaves audio evicting early on a longer
    // buffer, so a long save's audio starts late with no error.
    constexpr size_t kMb = 1024 * 1024;
    Ring video;
    video.Init(1 * kMb, 64 * 1024);
    PipelineSupervisor sup(60, 10000, 2, &video);
    sup.SetArmed(false);  // no generation to build: nothing touches the GPU
    BufferController controller(&sup);

    AudioTrackHub hub(QpcNow(), QpcFrequency(), 192, AudioTrackHub::SizeFor(10.0, 192, 0));
    hub.SetSources({{"Desktop", "desktop"}}, false);

    Config cfg{};
    cfg.buffer_seconds = 10.0;
    SaveContext save_ctx;
    save_ctx.ring  = &video;
    save_ctx.audio = &hub;

    JsonValue patch = JsonValue::MakeObject();
    patch.Set("buffer.seconds", JsonValue::MakeNum(120));
    const LiveApplyResult res = ApplyLiveConfig(cfg, patch, sup, controller, save_ctx, video);
    REQUIRE(res.ok);
    CHECK(res.tier == ApplyTier::Realloc);
    CHECK(hub.TrackRingCapacity() == AudioTrackHub::SizeFor(120.0, 192, 0).ring_capacity);
}

TEST_CASE("reconfig: a live change reaches every monitor's pipeline, or none") {
    constexpr size_t kMb = 1024 * 1024;
    Ring ring_a, ring_b;
    ring_a.Init(1 * kMb, 64 * 1024);
    ring_b.Init(1 * kMb, 64 * 1024);
    PipelineSupervisor sup_a(60, 10000, 2, &ring_a), sup_b(60, 10000, 2, &ring_b);
    sup_a.SetArmed(false);
    sup_b.SetArmed(false);
    BufferController controller(&sup_a);

    Config cfg{};
    cfg.buffer_seconds = 10.0;
    SaveContext ctx_a, ctx_b;
    ctx_a.ring = &ring_a;
    ctx_b.ring = &ring_b;
    const std::vector<LivePipeline> both = {{sup_a, ctx_a, ring_a, ""}, {sup_b, ctx_b, ring_b, " - DISPLAY2"}};

    // Free tier: one split the next two-monitor save between the old folder and the new one.
    JsonValue folder = JsonValue::MakeObject();
    folder.Set("output.root", JsonValue::MakeStr("D:\\Clips"));
    REQUIRE(ApplyLiveConfig(cfg, folder, both, controller).ok);
    CHECK(ctx_a.output_root == "D:\\Clips");
    CHECK(ctx_b.output_root == "D:\\Clips");

    // Realloc tier: both rings, the save windows and each one's own file.
    const std::wstring base = TestOutputPath(L"reconfig\\ring.dat");
    const std::wstring second = TestOutputPath(L"reconfig\\ring - DISPLAY2.dat");
    JsonValue disk = JsonValue::MakeObject();
    disk.Set("buffer.seconds", JsonValue::MakeNum(30));
    disk.Set("buffer.storage", JsonValue::MakeStr("file"));
    disk.Set("buffer.file_path", JsonValue::MakeStr(NarrowUtf8(base)));
    const LiveApplyResult res = ApplyLiveConfig(cfg, disk, both, controller);
    REQUIRE(res.ok);
    CHECK(res.tier == ApplyTier::Realloc);
    CHECK(ring_a.Capacity() == ring_b.Capacity());
    CHECK(ring_b.Capacity() != 1 * kMb);
    CHECK(ctx_a.buffer_seconds == 30.0);
    CHECK(ctx_b.buffer_seconds == 30.0);
    CHECK_FALSE(ctx_a.save_in_flight.load());  // the save slots it held are free again
    CHECK_FALSE(ctx_b.save_in_flight.load());
    CHECK(GetFileAttributesW(base.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(second.c_str()) != INVALID_FILE_ATTRIBUTES);
    ring_a.Init(1 * kMb, 64 * 1024);  // lets go of the files
    ring_b.Init(1 * kMb, 64 * 1024);

    // A save on the second monitor refuses the whole change, the first keeps its ring.
    ctx_b.save_in_flight.store(true);
    JsonValue longer = JsonValue::MakeObject();
    longer.Set("buffer.seconds", JsonValue::MakeNum(60));
    CHECK_FALSE(ApplyLiveConfig(cfg, longer, both, controller).ok);
    CHECK(ring_a.Capacity() == 1 * kMb);
    CHECK(cfg.buffer_seconds == 30.0);
    CHECK_FALSE(ctx_a.save_in_flight.load());  // the refused change gave back what it had claimed
    CHECK(ctx_b.save_in_flight.load());        // and left the running save's alone
}

TEST_CASE("reconfig: a config.json that can't be written is reported, and the change still applies") {
    constexpr size_t kMb = 1024 * 1024;
    Ring video;
    video.Init(1 * kMb, 64 * 1024);
    PipelineSupervisor sup(60, 10000, 2, &video);
    sup.SetArmed(false);
    BufferController controller(&sup);
    Config cfg{};
    SaveContext save_ctx;
    save_ctx.ring = &video;

    // A folder where the atomic write puts its temp file.
    const std::wstring blocker = Instance().config_dir + L"\\config.json.tmp";
    CreateDirectoryW(blocker.c_str(), nullptr);
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("output.root", JsonValue::MakeStr("D:\\Clips"));
    const LiveApplyResult res = ApplyLiveConfig(cfg, patch, sup, controller, save_ctx, video);
    RemoveDirectoryW(blocker.c_str());

    CHECK(res.ok);
    CHECK(res.error.find("config.json") != std::string::npos);
    CHECK(save_ctx.output_root == "D:\\Clips");
}

TEST_CASE("reconfig: a disk buffer it can't create is refused, and nothing changes") {
    constexpr size_t kMb = 1024 * 1024;
    Ring video;
    video.Init(1 * kMb, 64 * 1024);
    PipelineSupervisor sup(60, 10000, 2, &video);
    sup.SetArmed(false);
    BufferController controller(&sup);

    Config cfg{};
    cfg.buffer_seconds = 10.0;
    SaveContext save_ctx;
    save_ctx.ring = &video;

    JsonValue patch = JsonValue::MakeObject();
    patch.Set("buffer.storage", JsonValue::MakeStr("file"));
    patch.Set("buffer.file_path", JsonValue::MakeStr("C:\\bad?folder\\ring.dat"));
    const LiveApplyResult res = ApplyLiveConfig(cfg, patch, sup, controller, save_ctx, video);
    CHECK_FALSE(res.ok);
    CHECK(res.error.find("disk") != std::string::npos);
    CHECK(cfg.buffer_storage != "file");
    CHECK(video.Capacity() == 1 * kMb);
}

TEST_CASE("reconfig: set_config commits the bitrate a preset-only patch derives") {
    // Only preview_config used to derive it, so a Save of the preset alone (the UI's own bitrate
    // sync still in flight) committed the new preset with the old bitrate.
    constexpr size_t kMb = 1024 * 1024;
    Ring video;
    video.Init(1 * kMb, 64 * 1024);
    PipelineSupervisor sup(60, 10000, 2, &video);
    sup.SetArmed(false);
    BufferController controller(&sup);

    Config cfg{};
    cfg.capture_framerate     = 60;
    cfg.encode_quality_preset = "medium";
    cfg.encode_bitrate_kbps   = QualityPresetToBitrate("medium", 1920, 1080, 60, VideoCodec::AV1);
    SaveContext save_ctx;
    save_ctx.ring = &video;

    JsonValue patch = JsonValue::MakeObject();
    patch.Set("encode.quality_preset", JsonValue::MakeStr("high"));
    // A disarmed supervisor reports 0x0, so ApplyLiveConfig derives at its 1920x1080 fallback.
    const LiveApplyResult res = ApplyLiveConfig(cfg, patch, sup, controller, save_ctx, video);
    REQUIRE(res.ok);
    CHECK(cfg.encode_quality_preset == "high");
    CHECK(cfg.encode_bitrate_kbps == QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1));

    JsonValue bitrate_only = JsonValue::MakeObject();
    bitrate_only.Set("encode.bitrate_kbps", JsonValue::MakeNum(12345));
    REQUIRE(ApplyLiveConfig(cfg, bitrate_only, sup, controller, save_ctx, video).ok);
    CHECK(cfg.encode_bitrate_kbps == 12345);
    CHECK(cfg.encode_quality_preset == "custom");

    // The UI's framerate sync stages {framerate, derived bitrate}: the engine must read that
    // bitrate back as the same preset, not flip it to custom.
    cfg.encode_quality_preset = "high";
    cfg.encode_bitrate_kbps   = QualityPresetToBitrate("high", 1920, 1080, 60, VideoCodec::AV1);
    JsonValue fps_sync = JsonValue::MakeObject();
    fps_sync.Set("capture.framerate", JsonValue::MakeNum(120));
    fps_sync.Set("encode.bitrate_kbps", JsonValue::MakeNum(QualityPresetToBitrate("high", 1920, 1080, 120, VideoCodec::AV1)));
    REQUIRE(ApplyLiveConfig(cfg, fps_sync, sup, controller, save_ctx, video).ok);
    CHECK(cfg.encode_quality_preset == "high");
    CHECK(cfg.encode_bitrate_kbps == QualityPresetToBitrate("high", 1920, 1080, 120, VideoCodec::AV1));
}

namespace {
// A One window target whose program is closed: every open reports "not found" (false, no fault
// reason), the routine case PipelineSupervisor polls for. Creates no device, so nothing touches the GPU.
class ClosedWindowCapture final : public ICaptureSource {
public:
    bool Open(bool, const CaptureTarget&, const LUID&) override { return false; }
    bool TryOpen(bool, const CaptureTarget&, const LUID&) override { return false; }
    const std::string& FaultReason() const override { return reason_; }
    void StartCapture() override {}
    void Close() override {}
    void SetCursorCaptureEnabled(bool) override {}
    ID3D11Device* Device() const override { return nullptr; }
    ID3D11Texture2D* SlotTexture(uint32_t) const override { return nullptr; }
    uint32_t Width() const override { return 0; }
    uint32_t Height() const override { return 0; }
    SlotClaim ClaimSlotForEncoding() override { return {kNoSlot, 0}; }
    void ReleaseEncodingSlot() override {}
    bool HasFrame() const override { return false; }
    bool SizeChanged() const override { return false; }
    uint32_t PendingWidth() const override { return 0; }
    uint32_t PendingHeight() const override { return 0; }
    uint32_t PendingRevision() const override { return 0; }
    bool IsTargetMinimized() const override { return false; }
    bool Recreate(uint32_t, uint32_t) override { return false; }
    bool IsHealthy() const override { return true; }
    const char* HealthIssue() const override { return ""; }
    void ReadSlotRegionBGRA(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::vector<uint8_t>&, uint32_t&) override {}
    void ReleaseReadbackStaging() override {}
    void TestInjectItemClosed() override {}
    void TestInjectDeviceRemoved() override {}
    void TestForceContentSize(uint32_t, uint32_t) override {}

private:
    std::string reason_;
};

template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}
}  // namespace

TEST_CASE("supervisor: a disarm ends the wait for a closed One window program, so the ring is released") {
    // BufferController disarms when the program stays closed. The supervisor's retry loop used to
    // check only shutdown and reconfigure, so it kept polling for the window while disarmed: the
    // ring was never released and the shared audio hub never slept.
    Ring video;
    video.Init(1024 * 1024, 64 * 1024);
    video.SetMaxAge(QpcFrequency() / 10);
    CaptureTarget target;
    target.mode         = CaptureTarget::Mode::Window;
    target.window.match = L"ombra-test-no-such-program.exe";
    PipelineSupervisor sup(60, 10000, 2, &video, EncoderOptions{}, false, target);
    sup.SetCaptureFactory([] { return std::make_unique<ClosedWindowCapture>(); });
    std::atomic<bool> released{false};
    sup.SetRingReleaseCallbacks([&] { released = true; }, [] {});

    sup.Start([](const IVideoEncoder::AccessUnit&, const Tick&) {}, /*initial_armed=*/true);
    REQUIRE(WaitFor([&] { return sup.Rebuilding(); }, std::chrono::seconds(3)));  // waiting for the window
    CHECK_FALSE(released.load());

    sup.SetArmed(false);
    CHECK(WaitFor([&] { return released.load(); }, std::chrono::seconds(4)));
    CHECK_FALSE(sup.Rebuilding());
    sup.Stop();
}

}  // namespace ombra::test
