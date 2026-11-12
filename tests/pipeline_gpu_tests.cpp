// The real pipeline on the GPU with a synthetic frame source: PipelineSupervisor, the pacer, NVENC,
// the ring and DoSave all run for real, fed by SyntheticCapture instead of WGC, so nothing has to be
// on screen and nobody has to watch.
//
// "gpu" suite: runs only with `ombra-tests --gpu`.

#include <doctest/doctest.h>

#include "support/amf_caps.h"
#include "support/audio_analysis.h"
#include "support/media_probe.h"
#include "support/synthetic_capture.h"
#include "support/test_env.h"

#include "ombra/config.h"
#include "ombra/capture.h"
#include "ombra/clip_file.h"
#include "ombra/controller.h"
#include "ombra/encode_target.h"
#include "ombra/encoder.h"
#include "ombra/engine_pipe.h"
#include "ombra/json.h"
#include "ombra/audio_tracks.h"
#include "ombra/reconfig.h"
#include "ombra/ring.h"
#include "ombra/save.h"
#include "ombra/supervisor.h"
#include "ombra/util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iterator>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

namespace {

using namespace std::chrono_literals;

constexpr uint32_t kFps          = 60;
constexpr uint32_t kBitrateKbps  = 8000;
constexpr uint32_t kKeyframeSec  = 1;
constexpr uint32_t kRate         = 48000;
constexpr size_t   kPacketFrames = 480;

template <class Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

class PipelineRig {
public:
    explicit PipelineRig(const std::wstring& name, const EncoderOptions& encoder_opts = {})
        : scene(std::make_shared<SyntheticScene>()),
          supervisor(kFps, kBitrateKbps, kKeyframeSec, &ring, encoder_opts) {
        ring.Init(64u << 20, 16u << 20);
        audio.SetSources({{"Tone", "tone"}}, /*full_mix=*/false);
        audio.Start();  // real QPC here, so the hub encodes in the background as the engine does
        scene->present_fps = kFps;
        supervisor.SetCaptureFactory([s = scene] { return std::make_unique<SyntheticCapture>(s); });

        ctx.ring                = &ring;
        ctx.audio               = &audio;
        ctx.fps                 = kFps;
        ctx.buffer_seconds      = 3.0;
        ctx.out_path            = TestOutputPath(L"pipeline\\" + name + L".mp4");
        ctx.save_overlap_policy = "overlap";
        ctx.supervisor          = &supervisor;
    }

    ~PipelineRig() {
        audio_running_.store(false);
        if (audio_thread_.joinable()) audio_thread_.join();
        supervisor.Stop();
    }

    void Start() {
        supervisor.Start([this](const IVideoEncoder::AccessUnit& au, const Tick& tick) {
            ring.Append(au.data, au.size, tick.qpc, tick.index, au.is_keyframe);
        });
    }

    // Stands in for WASAPI: 10 ms packets stamped by QPC on one continuous timeline, silence until
    // the first white frame was presented, a 1 kHz tone from that exact QPC on. Runs 100 ms behind
    // real time so the flash QPC is always known before the packet covering it is built.
    void StartAudio() {
        audio_running_.store(true);
        audio_thread_ = std::thread([this] {
            const int64_t        freq   = QpcFrequency();
            const int64_t        origin = QpcNow();
            uint64_t             frame  = 0;
            std::vector<int16_t> packet(kPacketFrames * 2);
            while (audio_running_.load()) {
                const int64_t packet_qpc = origin + static_cast<int64_t>(frame) * freq / kRate;
                if (QpcNow() < packet_qpc + freq / 10) {
                    std::this_thread::sleep_for(2ms);
                    continue;
                }
                const int64_t onset = scene->first_flash_present_qpc.load();
                for (size_t i = 0; i < kPacketFrames; ++i) {
                    const int64_t sample_qpc = origin + static_cast<int64_t>(frame + i) * freq / kRate;
                    int16_t       s          = 0;
                    if (onset != 0 && sample_qpc >= onset) {
                        const double phase = 2.0 * std::numbers::pi * 1000.0 * double(frame + i) / kRate;
                        s = static_cast<int16_t>(std::lround(0.5 * 32767.0 * std::sin(phase)));
                    }
                    packet[i * 2]     = s;
                    packet[i * 2 + 1] = s;
                }
                audio.Append(0, packet.data(), packet.size(), packet_qpc);
                frame += kPacketFrames;
            }
        });
    }

    SaveOutcome Save(int64_t t_end) {
        ctx.save_in_flight.store(true);
        StdoutSilencer quiet;
        return DoSave(&ctx, t_end);
    }

    std::wstring SavedPath() {
        std::lock_guard<std::mutex> lock(ctx.last_save_mutex);
        return ctx.last_save_path;
    }

    Ring                            ring;
    AudioTrackHub audio{QpcNow(), QpcFrequency(), 128, AudioTrackHub::SizeFor(30.0, 128, 0)};
    std::shared_ptr<SyntheticScene> scene;
    PipelineSupervisor              supervisor;
    SaveContext                     ctx;

private:
    std::atomic<bool> audio_running_{false};
    std::thread       audio_thread_;
};

// Mean of B, G and R over a centred 16x16 block.
double CentreLevel(const VideoBgra& video, size_t frame) {
    const uint32_t x0 = video.width / 2 - 8, y0 = video.height / 2 - 8;
    const uint8_t* px = video.Frame(frame);
    double         sum = 0;
    for (uint32_t y = y0; y < y0 + 16; ++y)
        for (uint32_t x = x0; x < x0 + 16; ++x) {
            const size_t o = (size_t(y) * video.width + x) * 4;
            sum += px[o] + px[o + 1] + px[o + 2];
        }
    return sum / (16.0 * 16.0 * 3.0);
}

// Fresh slots are this blue. The pacer holds a segment's first encode until the source's first
// present, so a blue frame in a clip means a never-drawn slot reached the encoder.
constexpr float kFreshSlotBlue[4] = {0.0f, 0.0f, 1.0f, 1.0f};

void UseFreshSlotMarker(SyntheticScene& scene) {
    std::copy(std::begin(kFreshSlotBlue), std::end(kFreshSlotBlue), scene.fresh_slot_rgba);
}

bool IsMarkerBlue(const uint8_t* bgra) { return bgra[0] > 150 && bgra[1] < 100 && bgra[2] < 100; }
bool IsWhite(const uint8_t* bgra) { return bgra[0] > 200 && bgra[1] > 200 && bgra[2] > 200; }

// No marker frame may lead the clip (see kFreshSlotBlue), and every frame must be white at the
// centre and the bottom-right corner: a black one means the encoder read a frame the scaler hadn't
// drawn.
void CheckAllWhite(const VideoBgra& video) {
    auto centre = [&](size_t f) {
        return video.Frame(f) + ((size_t(video.height) / 2) * video.width + video.width / 2) * 4;
    };
    auto corner = [&](size_t f) { return video.Frame(f) + (size_t(video.width) * video.height - 1) * 4; };
    size_t lead = 0;
    while (lead < video.FrameCount() && IsMarkerBlue(centre(lead)) && IsMarkerBlue(corner(lead))) ++lead;
    CHECK_MESSAGE(lead == 0, "clip opens on " << lead << " never-drawn frames");
    REQUIRE(lead < video.FrameCount());
    for (size_t f = lead; f < video.FrameCount(); ++f) {
        CAPTURE(f);
        CHECK(IsWhite(centre(f)));
        CHECK(IsWhite(corner(f)));
    }
}

void CheckCleanVideo(const std::wstring& file, uint32_t width, uint32_t height, const char* codec_name) {
    const ProbeInfo probe = Probe(file);
    REQUIRE_MESSAGE(probe.ok, probe.error);
    const auto video = probe.Of("video");
    REQUIRE(video.size() == 1);
    CHECK(video[0].codec_name == codec_name);
    CHECK(video[0].width == width);
    CHECK(video[0].height == height);
    CHECK(video[0].color_range == "tv");
    CHECK(video[0].color_primaries == "bt709");
    const std::string errors = DecodeErrors(file);
    CHECK_MESSAGE(errors.empty(), "ffmpeg decode errors: " << errors);
}

void CheckCleanH264(const std::wstring& file, uint32_t width, uint32_t height) {
    CheckCleanVideo(file, width, height, "h264");
}

// The ring, the save and the av01 mux on real encoder output: a 3 s window of AV1 asked of `adapter`
// (with encode.amf_av1_unaligned = `unaligned`) saves as a clean constant-rate clip in `codec` at
// `size`, starting on a key frame, whose image is `picture` (the clap; 0: all of `size`).
void SaveAv1Through(const char* adapter, const std::wstring& name, const char* unaligned = "h264",
                    VideoCodec codec = VideoCodec::AV1, VideoSize size = {320, 180}, VideoSize picture = {}) {
    EncoderOptions opts;
    opts.adapter       = adapter;
    opts.codec         = VideoCodec::AV1;
    opts.av1_unaligned = unaligned;
    PipelineRig rig(name, opts);
    rig.Start();
    std::this_thread::sleep_for(3500ms);
    const auto params = rig.supervisor.CurrentParams();
    CHECK(params.codec == codec);
    CHECK(params.width == size.width);
    CHECK(params.height == size.height);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    CheckCleanVideo(file, size.width, size.height, codec == VideoCodec::AV1 ? "av1" : "h264");
    ClipInfo info;
    REQUIRE(ReadClipInfo(file, info));
    CHECK(info.picture_width == (picture.width ? picture.width : size.width));
    CHECK(info.picture_height == (picture.height ? picture.height : size.height));
    const VideoFrames frames = ProbeVideoFrames(file);
    MESSAGE("frames " << frames.count << ", pts " << frames.first_pts_s << ".." << frames.last_pts_s);
    CHECK(frames.first_is_key);
    CHECK(frames.count >= 3 * kFps - 2);
    CHECK(frames.count <= 4 * kFps + 1);
    CHECK(frames.last_pts_s - frames.first_pts_s ==
          doctest::Approx(double(frames.count - 1) / kFps).epsilon(0.01));
    CHECK(rig.supervisor.RebuildCount() == 0);
}

}  // namespace

TEST_CASE("gpu: NVENC AV1 through the real pipeline saves a clean av01 clip" * doctest::test_suite("gpu")) {
    SaveAv1Through("nvidia", L"av1-nvenc");
}

// The synthetic source is 320x180, off RDNA3's 64x16 AV1 grid, so each encode.amf_av1_unaligned
// mode gives its own result. Sizes assume that grid (MESSAGEd); another AMD GPU may differ.
TEST_CASE("gpu: AMF AV1 through the real pipeline, per encode.amf_av1_unaligned mode" * doctest::test_suite("gpu")) {
    bool any_amd = false;
    for (const auto& a : EnumerateAdapters(nullptr)) any_amd |= a.vendor_id == kVendorAmd && !a.software;
    if (!any_amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }
    SeedAmfCapsInProcess();  // what the engine's startup probe does, in-process
    const auto amd = ResolveEncodeAdapter("amd");
    REQUIRE(amd);
    MESSAGE("AV1 alignment " << amd->av1_align_w << "x" << amd->av1_align_h);
    REQUIRE((180 % amd->av1_align_h) != 0);
    SUBCASE("h264") { SaveAv1Through("amd", L"av1-amf-h264", "h264", VideoCodec::H264, {320, 180}); }
    SUBCASE("pad") {
        SaveAv1Through("amd", L"av1-amf-pad", "pad", VideoCodec::AV1,
                       PredictPaddedSize({320, 180}, amd->av1_align_w, amd->av1_align_h), {320, 180});
    }
    SUBCASE("scale") {
        SaveAv1Through("amd", L"av1-amf-scale", "scale", VideoCodec::AV1,
                       PlanEncode({320, 180}, VideoCodec::AV1, amd->av1_align_w, amd->av1_align_h, "scale").size);
    }
    SetAmfAdapterCaps({});
}

// AV1 asked for, H.264 opened: the segment records what was opened, so the save is a valid H.264
// file rather than H.264 bytes in an av01 box.
TEST_CASE("gpu: an AV1 pipeline on a GPU without AV1 saves H.264" * doctest::test_suite("gpu")) {
    SetAv1UnsupportedForTest(true);
    EncoderOptions opts;
    opts.codec = VideoCodec::AV1;
    PipelineRig rig(L"av1-fallback", opts);
    rig.Start();
    std::this_thread::sleep_for(3500ms);
    CHECK(rig.supervisor.CurrentParams().codec == VideoCodec::H264);
    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
    SetAv1UnsupportedForTest(false);
}

TEST_CASE("gpu: a synthetic source through the real pipeline saves a clean constant-rate clip" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"basic");
    rig.Start();
    std::this_thread::sleep_for(3500ms);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    CheckCleanH264(file, 320, 180);

    // 3 s window, IDR every second: the clip starts on the IDR at or before its start.
    const VideoFrames frames = ProbeVideoFrames(file);
    MESSAGE("frames " << frames.count << ", pts " << frames.first_pts_s << ".." << frames.last_pts_s);
    CHECK(frames.first_is_key);
    CHECK(frames.count >= 3 * kFps - 2);
    CHECK(frames.count <= 4 * kFps + 1);
    CHECK(frames.last_pts_s - frames.first_pts_s ==
          doctest::Approx(double(frames.count - 1) / kFps).epsilon(0.01));
    CHECK(rig.scene->opens.load() == 1);
}

// A driver update: the NVIDIA adapter vanishes, a rebuild lands on AMD, and NVIDIA comes back.
TEST_CASE("gpu: a pipeline that fell to AMD returns to the GPU encode.adapter wants once it's back" *
          doctest::test_suite("gpu")) {
    SetAmfAdapterCaps(AllAmdH264Caps());
    const auto adapters = EnumerateAdapters(nullptr);
    const auto amd = PickEncodeAdapter(adapters, "amd", AmfAdapterCapsSnapshot());
    const auto nvidia = PickEncodeAdapter(adapters, "nvidia", {});
    if (!amd || amd->backend != EncoderBackend::Amf || !nvidia || nvidia->backend != EncoderBackend::Nvenc) {
        MESSAGE("needs both an AMD and an NVIDIA GPU: skipped");
        return;
    }
    std::string preference = "auto";
    bool moves = true;
    SUBCASE("auto moves") {}
    SUBCASE("nvidia moves") { preference = "nvidia"; }
    SUBCASE("an explicit AMD stays") { preference = "amd"; moves = false; }

    EncoderOptions opts;
    opts.adapter = preference;
    // Declared before the rig: its destructor joins the watchdog that calls the lambdas reading them.
    std::atomic<bool> nvidia_back{false};
    PipelineRig rig(L"adapter-return", opts);
    rig.supervisor.SetAdapterResolver(
        [&](const LUID*, const LUID*) -> std::optional<EncodeAdapter> { return nvidia_back.load() ? *nvidia : *amd; }, 200ms);
    rig.Start();
    REQUIRE(WaitFor([&] { return rig.scene->opens.load() == 1; }, 5000ms));
    REQUIRE(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf);
    // Nothing better yet: a healthy pipeline stays put through several checks.
    std::this_thread::sleep_for(1000ms);
    CHECK(rig.supervisor.RebuildCount() == 0);
    CHECK(rig.scene->opens.load() == 1);

    nvidia_back.store(true);
    if (moves) {
        CHECK(WaitFor([&] { return rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Nvenc; }, 8000ms));
        // And it stays, and records: NVENC is first in the order, nothing rebuilds it again, and a
        // save holds the new segment.
        std::this_thread::sleep_for(2000ms);
        CHECK(rig.scene->opens.load() == 2);
        CHECK(rig.ring.BarrierCount() == 1);  // the move's cut; the first segment drops none
        CHECK(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    } else {
        std::this_thread::sleep_for(1500ms);
        CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf);
        CHECK(rig.scene->opens.load() == 1);
    }
}

// A game starts on the NVIDIA GPU while the display's AMD GPU records: auto moves at once (not on
// the slow recheck), and after the game closes it stays until the game's footage has aged out.
TEST_CASE("gpu: auto moves to the NVIDIA GPU a game renders on at once, and back after the hold" *
          doctest::test_suite("gpu")) {
    SetAmfAdapterCaps(AllAmdH264Caps());
    const auto adapters = EnumerateAdapters(nullptr);
    const auto amd = PickEncodeAdapter(adapters, "amd", AmfAdapterCapsSnapshot());
    const auto nvidia = PickEncodeAdapter(adapters, "nvidia", {});
    if (!amd || amd->backend != EncoderBackend::Amf || !nvidia || nvidia->backend != EncoderBackend::Nvenc) {
        MESSAGE("needs both an AMD and an NVIDIA GPU: skipped");
        return;
    }
    EncoderOptions opts;
    opts.adapter = "auto";
    PipelineRig rig(L"game-gpu", opts);
    rig.ring.SetMaxAge(RingMaxAgeQpc(1.0));  // a 3 s hold
    rig.supervisor.SetAdapterResolver(
        [&](const LUID*, const LUID* game) -> std::optional<EncodeAdapter> {
            return game && SameLuid(*game, nvidia->luid) ? *nvidia : *amd;
        },
        60000ms);
    rig.Start();
    REQUIRE(WaitFor([&] { return rig.scene->opens.load() == 1; }, 5000ms));
    REQUIRE(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf);

    // A game on the display's own GPU changes nothing.
    rig.supervisor.SetGameGpu(amd->luid);
    std::this_thread::sleep_for(1000ms);
    CHECK(rig.scene->opens.load() == 1);

    const auto set_at = std::chrono::steady_clock::now();
    rig.supervisor.SetGameGpu(nvidia->luid);
    REQUIRE(WaitFor([&] { return rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Nvenc; }, 5000ms));
    CHECK(std::chrono::steady_clock::now() - set_at < 3000ms);
    CHECK(WaitFor([&] { return rig.scene->opens.load() == 2; }, 2000ms));

    const auto cleared_at = std::chrono::steady_clock::now();
    rig.supervisor.SetGameGpu(std::nullopt);
    std::this_thread::sleep_for(1500ms);
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Nvenc);
    CHECK(rig.Save(QpcNow()) == SaveOutcome::kSaved);  // the game's footage is still there
    REQUIRE(WaitFor([&] { return rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf; }, 6000ms));
    CHECK(std::chrono::steady_clock::now() - cleared_at >= 2500ms);
    std::this_thread::sleep_for(500ms);
    CHECK(rig.scene->opens.load() == 3);
}

// encode.adapter=amd end to end: the supervisor resolves the AMD adapter, opens the synthetic
// source's device on it, and AmfEncoder feeds the same ring and save path NVENC does.
TEST_CASE("gpu: encode.adapter=amd records through AMF and saves a clean constant-rate clip" *
          doctest::test_suite("gpu")) {
    bool any_amd = false;
    for (const auto& a : EnumerateAdapters(nullptr)) any_amd |= a.vendor_id == kVendorAmd && !a.software;
    if (!any_amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }
    SetAmfAdapterCaps(AllAmdH264Caps());  // in-process: see gpu_encode_tests.cpp's AmdAdapter()
    EncoderOptions opts;
    opts.adapter = "amd";
    PipelineRig rig(L"amf-basic", opts);
    rig.Start();
    std::this_thread::sleep_for(3500ms);
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    CheckCleanH264(file, 320, 180);
    const VideoFrames frames = ProbeVideoFrames(file);
    MESSAGE("frames " << frames.count << ", pts " << frames.first_pts_s << ".." << frames.last_pts_s);
    CHECK(frames.first_is_key);
    CHECK(frames.count >= 3 * kFps - 2);
    CHECK(frames.count <= 4 * kFps + 1);
    CHECK(frames.last_pts_s - frames.first_pts_s ==
          doctest::Approx(double(frames.count - 1) / kFps).epsilon(0.01));
    CHECK(rig.supervisor.RebuildCount() == 0);  // no encoder faults (a lagging AU would fault)
    SetAmfAdapterCaps({});
}

// encode.adapter=cpu end to end: x264 on the adapter that drives a display, through the same ring
// and save path as the hardware encoders.
TEST_CASE("gpu: encode.adapter=cpu records through x264 and saves a clean constant-rate clip" *
          doctest::test_suite("gpu")) {
    EncoderOptions opts;
    opts.adapter = "cpu";
    opts.codec   = VideoCodec::AV1;  // the default config: the CPU records H.264 instead
    PipelineRig rig(L"x264-basic", opts);
    rig.Start();
    std::this_thread::sleep_for(3500ms);
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::X264);
    CHECK(rig.supervisor.CurrentParams().codec == VideoCodec::H264);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    CheckCleanH264(file, 320, 180);
    const VideoFrames frames = ProbeVideoFrames(file);
    MESSAGE("frames " << frames.count << ", pts " << frames.first_pts_s << ".." << frames.last_pts_s);
    CHECK(frames.first_is_key);
    CHECK(frames.count >= 3 * kFps - 2);
    CHECK(frames.count <= 4 * kFps + 1);
    CHECK(frames.last_pts_s - frames.first_pts_s ==
          doctest::Approx(double(frames.count - 1) / kFps).epsilon(0.01));
    CHECK(rig.supervisor.RebuildCount() == 0);  // a held-back frame would fault and rebuild
}

namespace {
// A hardware encoder that never opens: a broken driver, an exhausted NVENC session limit.
class UnopenableEncoder final : public IVideoEncoder {
public:
    bool Open(ID3D11Device*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const EncoderOptions&) override {
        FAIL("a hardware encoder must open non-fatally");
        return false;
    }
    bool TryOpen(ID3D11Device*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const EncoderOptions&) override {
        faulted_ = true;
        return false;
    }
    VideoCodec                  Codec() const override { return VideoCodec::H264; }
    const std::vector<uint8_t>& SequenceParams() const override { return none_; }
    bool                        RegisterSlot(ID3D11Texture2D*, uint32_t, uint32_t) override { return false; }
    AccessUnit EncodeSlot(uint32_t, uint32_t, uint32_t, uint64_t) override { return AccessUnit{.ok = false}; }
    void               UnlockLast() override {}
    bool               Faulted() const override { return faulted_; }
    const std::string& FaultReason() const override { return reason_; }
    void               Close() override {}

private:
    bool                 faulted_ = false;
    std::string          reason_  = "injected: session limit";
    std::vector<uint8_t> none_;
};
}  // namespace

// The fallback that makes "works on every PC" true: a hardware encoder that won't open is replaced
// by x264 on the same capture device -- at startup at once, on a rebuild after
// kHwOpenFailuresBeforeCpu failed builds -- and the engine is told once, not on every rebuild.
TEST_CASE("gpu: a hardware encoder that won't open falls back to x264, once per fallback" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"x264-fallback");
    std::atomic<int> fallbacks{0};
    rig.supervisor.SetEncoderFactory([](EncoderBackend b) -> std::unique_ptr<IVideoEncoder> {
        if (b == EncoderBackend::X264) return CreateVideoEncoder(b);
        return std::make_unique<UnopenableEncoder>();
    });
    rig.supervisor.SetEncoderFallbackCallback([&](const std::string& reason) {
        CHECK(reason.find("injected") != std::string::npos);
        ++fallbacks;
    });
    rig.Start();  // startup: falls back on the first failure instead of dying
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::X264);
    CHECK(fallbacks.load() == 1);
    std::this_thread::sleep_for(3500ms);
    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);

    // A fault rebuild tries the GPU again first, fails kHwOpenFailuresBeforeCpu builds on the
    // backoff (1 + 2 + 4 s), then records on the CPU again without a second notice.
    const auto rebuilds = rig.supervisor.RebuildCount();
    rig.supervisor.TestInjectDeviceRemoved();
    REQUIRE(WaitFor([&] { return rig.supervisor.RebuildCount() > rebuilds; }, 15000ms));
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::X264);
    CHECK(fallbacks.load() == 1);
    std::this_thread::sleep_for(2500ms);
    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
}

namespace {
// A real encoder that notes the bitrate it was opened with.
class BitrateSpyEncoder final : public IVideoEncoder {
public:
    BitrateSpyEncoder(std::unique_ptr<IVideoEncoder> inner, std::atomic<uint32_t>& seen) : inner_(std::move(inner)), seen_(seen) {}
    bool Open(ID3D11Device* d, uint32_t w, uint32_t h, uint32_t fps, uint32_t kbps, uint32_t key, const EncoderOptions& o) override {
        seen_.store(kbps);
        return inner_->Open(d, w, h, fps, kbps, key, o);
    }
    bool TryOpen(ID3D11Device* d, uint32_t w, uint32_t h, uint32_t fps, uint32_t kbps, uint32_t key, const EncoderOptions& o) override {
        seen_.store(kbps);
        return inner_->TryOpen(d, w, h, fps, kbps, key, o);
    }
    VideoCodec                  Codec() const override { return inner_->Codec(); }
    const std::vector<uint8_t>& SequenceParams() const override { return inner_->SequenceParams(); }
    bool RegisterSlot(ID3D11Texture2D* t, uint32_t w, uint32_t h) override { return inner_->RegisterSlot(t, w, h); }
    AccessUnit EncodeSlot(uint32_t a, uint32_t b, uint32_t c, uint64_t d) override { return inner_->EncodeSlot(a, b, c, d); }
    void               UnlockLast() override { inner_->UnlockLast(); }
    bool               Faulted() const override { return inner_->Faulted(); }
    const std::string& FaultReason() const override { return inner_->FaultReason(); }
    void               Close() override { inner_->Close(); }

private:
    std::unique_ptr<IVideoEncoder> inner_;
    std::atomic<uint32_t>&         seen_;
};
}  // namespace

// A preset derived for AV1 on NVIDIA, which won't open: x264 records H.264 at H.264's share, and a
// Custom bitrate stays the user's number.
TEST_CASE("gpu: a fallback that changes the codec opens at that codec's preset bitrate" * doctest::test_suite("gpu")) {
    EncoderOptions opts;
    opts.codec = VideoCodec::AV1;
    uint32_t expect = 0;
    SUBCASE("preset") {
        opts.preset_bitrate_codec = VideoCodec::AV1;
        expect = BitrateForOpenedCodec(kBitrateKbps, VideoCodec::AV1, VideoCodec::H264);
        CHECK(expect > kBitrateKbps);
    }
    SUBCASE("custom") { expect = kBitrateKbps; }
    std::atomic<uint32_t> x264_kbps{0};  // before the rig: its watchdog reads it through the factory
    PipelineRig rig(L"fallback-bitrate", opts);
    rig.supervisor.SetEncoderFactory([&](EncoderBackend b) -> std::unique_ptr<IVideoEncoder> {
        if (b == EncoderBackend::X264) return std::make_unique<BitrateSpyEncoder>(CreateVideoEncoder(b), x264_kbps);
        return std::make_unique<UnopenableEncoder>();
    });
    rig.Start();
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::X264);
    CHECK(x264_kbps.load() == expect);
}

// Mid driver install: NVIDIA is listed but can't encode. Stays on a working AMD, not x264.
TEST_CASE("gpu: a return to a GPU that can't encode yet goes back to the adapter that worked" *
          doctest::test_suite("gpu")) {
    SetAmfAdapterCaps(AllAmdH264Caps());
    const auto adapters = EnumerateAdapters(nullptr);
    const auto amd = PickEncodeAdapter(adapters, "amd", AmfAdapterCapsSnapshot());
    const auto nvidia = PickEncodeAdapter(adapters, "nvidia", {});
    if (!amd || amd->backend != EncoderBackend::Amf || !nvidia || nvidia->backend != EncoderBackend::Nvenc) {
        MESSAGE("needs both an AMD and an NVIDIA GPU: skipped");
        return;
    }
    EncoderOptions opts;
    opts.adapter = "auto";
    // Declared before the rig: its destructor joins the watchdog that calls the lambdas reading them.
    std::atomic<bool> nvidia_listed{false}, nvenc_refuses{true};
    std::atomic<int> fallbacks{0};
    PipelineRig rig(L"adapter-return-refused", opts);
    rig.supervisor.SetAdapterResolver(
        [&](const LUID*, const LUID*) -> std::optional<EncodeAdapter> { return nvidia_listed.load() ? *nvidia : *amd; }, 200ms);
    rig.supervisor.SetEncoderFactory([&](EncoderBackend b) -> std::unique_ptr<IVideoEncoder> {
        if (b == EncoderBackend::Nvenc && nvenc_refuses.load()) return std::make_unique<UnopenableEncoder>();
        return CreateVideoEncoder(b);
    });
    rig.supervisor.SetEncoderFallbackCallback([&](const std::string&) { ++fallbacks; });
    rig.Start();
    REQUIRE(WaitFor([&] { return rig.scene->opens.load() == 1; }, 5000ms));

    nvidia_listed.store(true);
    // The try on NVIDIA, then straight back on AMD, with no backoff through a recording gap.
    REQUIRE(WaitFor([&] { return rig.scene->opens.load() == 3; }, 5000ms));
    std::this_thread::sleep_for(2000ms);
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf);
    CHECK(rig.scene->opens.load() == 3);  // the next try waits: each one cuts the ring
    CHECK(fallbacks.load() == 0);
    CHECK(rig.Save(QpcNow()) == SaveOutcome::kSaved);

    // Once it can encode, a later check moves there.
    nvenc_refuses.store(false);
    CHECK(WaitFor([&] { return rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Nvenc; }, 8000ms));
    CHECK(fallbacks.load() == 0);
}

// Auto's pick (the display's GPU, AMD here) won't open its encoder: it records on the other GPU's
// hardware encoder, not x264, and a later rebuild goes straight there instead of retrying it.
TEST_CASE("gpu: auto records on the other GPU when the display GPU's encoder won't open" *
          doctest::test_suite("gpu")) {
    SetAmfAdapterCaps(AllAmdH264Caps());
    const auto adapters = EnumerateAdapters(nullptr);
    const auto amd = PickEncodeAdapter(adapters, "amd", AmfAdapterCapsSnapshot());
    const auto nvidia = PickEncodeAdapter(adapters, "nvidia", {});
    if (!amd || amd->backend != EncoderBackend::Amf || !nvidia || nvidia->backend != EncoderBackend::Nvenc) {
        MESSAGE("needs both an AMD and an NVIDIA GPU: skipped");
        return;
    }
    EncoderOptions opts;
    opts.adapter = "auto";
    // Declared before the rig: its destructor joins the watchdog that calls the lambdas reading them.
    std::atomic<int> fallbacks{0}, amf_tries{0};
    PipelineRig rig(L"adapter-other-gpu", opts);
    // No return check during the test: it would try AMD again on purpose.
    rig.supervisor.SetAdapterResolver(
        [&](const LUID* exclude, const LUID*) -> std::optional<EncodeAdapter> {
            return exclude && SameLuid(*exclude, amd->luid) ? *nvidia : *amd;
        },
        600000ms);
    rig.supervisor.SetEncoderFactory([&](EncoderBackend b) -> std::unique_ptr<IVideoEncoder> {
        if (b == EncoderBackend::Amf) {
            ++amf_tries;
            return std::make_unique<UnopenableEncoder>();
        }
        return CreateVideoEncoder(b);
    });
    rig.supervisor.SetEncoderFallbackCallback([&](const std::string&) { ++fallbacks; });
    rig.Start();
    REQUIRE(WaitFor([&] { return rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Nvenc; }, 5000ms));
    CHECK(SameLuid(rig.supervisor.CurrentEncodeAdapter().luid, nvidia->luid));
    CHECK(amf_tries.load() == 1);
    CHECK(fallbacks.load() == 0);
    std::this_thread::sleep_for(2000ms);
    CHECK(rig.Save(QpcNow()) == SaveOutcome::kSaved);

    PipelineSupervisor::SupervisorReconfigParams req;
    req.tier         = ApplyTier::Rebuild;
    req.fps          = kFps;
    req.bitrate_kbps = kBitrateKbps;
    req.keyframe_sec = kKeyframeSec;
    req.encoder_opts = opts;
    const int opens = rig.scene->opens.load();  // the AMD try and the NVIDIA one
    REQUIRE(rig.supervisor.RequestReconfigure(req));
    CHECK(rig.scene->opens.load() == opens + 1);
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Nvenc);
    CHECK(amf_tries.load() == 1);
    CHECK(fallbacks.load() == 0);
}

TEST_CASE("gpu: audio placed by QPC lines up with video through the real pacer" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"av-sync");
    rig.Start();
    rig.StartAudio();

    const int64_t freq = QpcFrequency();
    const int64_t flash = QpcNow() + freq * 3 / 2;
    rig.scene->flash_end_qpc.store(flash + freq * 3 / 10);
    rig.scene->flash_start_qpc.store(flash);
    std::this_thread::sleep_for(3200ms);
    REQUIRE(rig.scene->first_flash_present_qpc.load() != 0);

    REQUIRE(rig.Save(QpcNow() - freq / 5) == SaveOutcome::kSaved);  // audio runs 100 ms behind
    const std::wstring file = rig.SavedPath();
    CheckCleanH264(file, 320, 180);

    const VideoBgra video = DecodeVideoBgra(file);
    REQUIRE(video.FrameCount() > 0);
    size_t white = video.FrameCount();
    for (size_t i = 0; i < video.FrameCount(); ++i) {
        if (CentreLevel(video, i) > 200.0) {
            white = i;
            break;
        }
    }
    REQUIRE_MESSAGE(white < video.FrameCount(), "no white frame in the saved clip");

    const Pcm  pcm   = DecodeAudio(file, 0);
    const auto onset = FirstFrameAbove(pcm, 0.1);
    REQUIRE_MESSAGE(onset.has_value(), "no tone in the saved clip");

    const double video_s = double(white) / kFps;
    const double audio_s = double(*onset) / kRate;
    MESSAGE("first white frame " << video_s * 1000.0 << " ms, tone onset " << audio_s * 1000.0
                                 << " ms, video - audio " << (video_s - audio_s) * 1000.0 << " ms");
    // The pacer picks a presented frame up on its next tick, so video trails by up to one period.
    CHECK(video_s - audio_s >= -0.010);
    CHECK(video_s - audio_s <= 1.0 / kFps + 0.010);
}

// Two changes back to back: with one, "the newest parameters" and "this segment's parameters" are
// the same answer, so a save muxing a stale SPS/PPS would still pass.
TEST_CASE("gpu: a resolution change reopens the encoder and the next save is at the new size" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"resolution");
    rig.Start();
    std::this_thread::sleep_for(1500ms);

    rig.scene->width.store(480);
    rig.scene->height.store(270);
    REQUIRE(WaitFor([&] { return rig.supervisor.ResolutionChangeCount() >= 1; }, 5000ms));
    std::this_thread::sleep_for(1500ms);
    rig.scene->width.store(256);
    rig.scene->height.store(144);
    REQUIRE(WaitFor([&] { return rig.supervisor.ResolutionChangeCount() >= 2; }, 5000ms));
    std::this_thread::sleep_for(2000ms);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    // The 3 s window reaches back past the second change; Pin() stops at its barrier.
    CheckCleanH264(file, 256, 144);
    const VideoFrames frames = ProbeVideoFrames(file);
    MESSAGE("frames after the second change " << frames.count);
    CHECK(frames.first_is_key);
    CHECK(frames.count >= kFps);
    CHECK(frames.count <= 5 * kFps / 2);
    CHECK(rig.ring.BarrierCount() == 2);
    CHECK(rig.scene->opens.load() == 1);  // a reopen recreates the source in place, not a rebuild
    CHECK(rig.supervisor.RebuildCount() == 0);
}

// A size that comes back inside the debounce must not cost the buffer: a reopen would cut a barrier
// and a save can't reach past one. Then the same size again settles from zero and does reopen.
TEST_CASE("gpu: a resolution change that reverses inside the debounce keeps the buffer" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"resolution-reverted");
    rig.Start();
    std::this_thread::sleep_for(1000ms);
    const size_t barriers = rig.ring.BarrierCount();

    rig.scene->width.store(480);
    rig.scene->height.store(270);
    std::this_thread::sleep_for(100ms);  // kResizeSettleDelay is 400 ms
    rig.scene->width.store(320);
    rig.scene->height.store(180);
    std::this_thread::sleep_for(1500ms);
    CHECK(rig.supervisor.ResolutionChangeCount() == 0);
    CHECK(rig.ring.BarrierCount() == barriers);

    rig.scene->width.store(480);
    rig.scene->height.store(270);
    CHECK(WaitFor([&] { return rig.supervisor.ResolutionChangeCount() == 1; }, 5000ms));
}

// A size that flaps faster than the watchdog polls: most polls see the same pending size, but it
// never held still, so nothing has settled.
TEST_CASE("gpu: a resolution that flaps between watchdog polls never settles" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"resolution-flap");
    rig.Start();
    std::this_thread::sleep_for(1000ms);
    const size_t barriers = rig.ring.BarrierCount();

    const auto until = std::chrono::steady_clock::now() + 4000ms;
    while (std::chrono::steady_clock::now() < until) {
        rig.scene->width.store(480);
        rig.scene->height.store(270);
        std::this_thread::sleep_for(130ms);
        rig.scene->width.store(320);
        rig.scene->height.store(180);
        std::this_thread::sleep_for(50ms);
    }
    std::this_thread::sleep_for(600ms);
    CHECK(rig.supervisor.ResolutionChangeCount() == 0);
    CHECK(rig.ring.BarrierCount() == barriers);
}

// The cursor setting changing while a build has the source open but hasn't published it: the
// reconfigure finds no capture to apply to, so the build has to pick the value up.
TEST_CASE("gpu: a cursor change during a pipeline build reaches the new capture" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"cursor-during-build");
    rig.scene->on_opened = [&rig] {
        PipelineSupervisor::SupervisorReconfigParams req{};
        req.tier           = ApplyTier::Free;
        req.capture_cursor = false;
        rig.supervisor.RequestReconfigure(req);
    };
    rig.Start();
    CHECK(rig.scene->opens.load() >= 1);
    CHECK_FALSE(rig.scene->cursor.load());
}

// capture.resolution end to end: FrameScaler between the source and the encoder, through a source
// resize (a new scaler on the reopen) and a live reconfigure. The scene stays white, so a frame the
// encoder read before the scale pass landed, or a draw that missed the edges, decodes dark.
TEST_CASE("gpu: capture.resolution scales the encoded size, keeps the aspect, and follows resizes" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"scaled");
    rig.scene->width.store(2560);
    rig.scene->height.store(1600);
    rig.scene->flash_start_qpc.store(1);
    rig.scene->flash_end_qpc.store(INT64_MAX);
    UseFreshSlotMarker(*rig.scene);
    rig.supervisor.SetCaptureResolution("1080p");
    rig.Start();

    auto check_save = [&](uint32_t w, uint32_t h) {
        std::this_thread::sleep_for(2500ms);
        REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
        const std::wstring file = rig.SavedPath();
        CheckCleanH264(file, w, h);
        CHECK(rig.supervisor.CurrentParams().width == w);
        CHECK(rig.supervisor.CurrentParams().height == h);
        const VideoBgra video = DecodeVideoBgra(file, 8);
        REQUIRE(video.FrameCount() == 8);
        CheckAllWhite(video);
    };

    check_save(1728, 1080);  // 16:10 at 1080p: no bars, no squash
    CHECK(rig.supervisor.CurrentParams().source_width == 2560);

    rig.scene->width.store(2560);
    rig.scene->height.store(1440);
    REQUIRE(WaitFor([&] { return rig.supervisor.ResolutionChangeCount() >= 1; }, 5000ms));
    check_save(1920, 1080);
    CHECK(rig.supervisor.RebuildCount() == 0);

    PipelineSupervisor::SupervisorReconfigParams req;
    req.tier               = ApplyTier::Rebuild;
    req.fps                = kFps;
    req.bitrate_kbps       = kBitrateKbps;
    req.keyframe_sec       = kKeyframeSec;
    req.capture_resolution = "720p";
    REQUIRE(rig.supervisor.RequestReconfigure(req));
    check_save(1280, 720);

    req.capture_resolution = "native";  // back to no scaler at all
    REQUIRE(rig.supervisor.RequestReconfigure(req));
    check_save(2560, 1440);
}

TEST_CASE("gpu: capture.resolution through AMF encodes the scaled frame" * doctest::test_suite("gpu")) {
    bool any_amd = false;
    for (const auto& a : EnumerateAdapters(nullptr)) any_amd |= a.vendor_id == kVendorAmd && !a.software;
    if (!any_amd) {
        MESSAGE("no AMD adapter: skipped");
        return;
    }
    SetAmfAdapterCaps(AllAmdH264Caps());
    EncoderOptions opts;
    opts.adapter = "amd";
    PipelineRig rig(L"amf-scaled", opts);
    rig.scene->width.store(2560);
    rig.scene->height.store(1600);
    rig.scene->flash_start_qpc.store(1);
    rig.scene->flash_end_qpc.store(INT64_MAX);
    UseFreshSlotMarker(*rig.scene);
    rig.supervisor.SetCaptureResolution("1080p");
    rig.Start();
    std::this_thread::sleep_for(2500ms);
    CHECK(rig.supervisor.CurrentEncodeAdapter().backend == EncoderBackend::Amf);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    CheckCleanH264(file, 1728, 1080);
    const VideoBgra video = DecodeVideoBgra(file, 8);
    REQUIRE(video.FrameCount() == 8);
    CheckAllWhite(video);
    CHECK(rig.supervisor.RebuildCount() == 0);
    SetAmfAdapterCaps({});
}

TEST_CASE("gpu: a save racing a pipeline rebuild still writes a clean clip" * doctest::test_suite("gpu")) {
    PipelineRig rig(L"save-vs-rebuild");
    rig.Start();
    std::this_thread::sleep_for(2500ms);

    PipelineSupervisor::SupervisorReconfigParams req;
    req.tier         = ApplyTier::Rebuild;
    req.fps          = kFps;
    req.bitrate_kbps = kBitrateKbps;
    req.keyframe_sec = kKeyframeSec;

    const int64_t t_end = QpcNow();
    auto          save  = std::async(std::launch::async, [&] { return rig.Save(t_end); });
    const bool    rebuilt = rig.supervisor.RequestReconfigure(req);
    const SaveOutcome outcome = save.get();

    CHECK(rebuilt);
    REQUIRE(outcome == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
    CHECK(ProbeVideoFrames(rig.SavedPath()).first_is_key);
    CHECK(rig.scene->opens.load() == 2);

    // The rebuilt generation records and saves normally.
    std::this_thread::sleep_for(2000ms);
    rig.ctx.out_path = TestOutputPath(L"pipeline\\save-vs-rebuild-after.mp4");
    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
}

TEST_CASE("gpu: a real frame-pool recreate failure rebuilds once and saves keep working" *
          doctest::test_suite("gpu")) {
    struct State {
        std::atomic<bool> resize{false};
        std::atomic<int> opens{0}, failures{0};
    };
    // Schedule a stable resize without desktop motion. Open, slots, the frame pool, exception
    // handling, teardown and rebuilt capture are all real; only the resize notification is fake.
    class ResizeCapture : public Capture {
    public:
        explicit ResizeCapture(std::shared_ptr<State> state) : state_(std::move(state)) {
            if (state_->opens.fetch_add(1) == 0) TestFailNextFramePoolRecreate();
        }
        bool SizeChanged() const override { return state_->resize.load(); }
        uint32_t PendingWidth() const override { return 640; }
        uint32_t PendingHeight() const override { return 360; }
        uint32_t PendingRevision() const override { return 1; }
        bool Recreate(uint32_t w, uint32_t h) override {
            const bool ok = Capture::Recreate(w, h);
            if (!ok) state_->failures.fetch_add(1);
            state_->resize.store(false);
            return ok;
        }
    private:
        std::shared_ptr<State> state_;
    };
    auto state = std::make_shared<State>();
    PipelineRig rig(L"frame-pool-failure");
    rig.supervisor.SetCaptureFactory([state] { return std::make_unique<ResizeCapture>(state); });
    rig.Start();
    REQUIRE(WaitFor([&] { return rig.supervisor.CurrentTicksSubmitted() >= kFps; }, 10000ms));
    const auto before = rig.supervisor.CurrentParams();
    state->resize.store(true);
    REQUIRE(WaitFor([&] { return rig.supervisor.RebuildCount() == 1; }, 10000ms));
    CHECK(state->failures.load() == 1);
    CHECK(state->opens.load() == 2);
    CHECK(rig.supervisor.ResolutionChangeCount() == 0);
    CHECK(rig.ring.BarrierCount() == 1);
    REQUIRE(WaitFor([&] { return rig.supervisor.CurrentTicksSubmitted() >= 2 * kFps; }, 10000ms));
    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), before.width, before.height);
    const auto frames = ProbeVideoFrames(rig.SavedPath());
    CHECK(frames.first_is_key);
    CHECK(frames.count >= kFps);
    CHECK(frames.count <= 5 * kFps / 2);  // the save cannot reach across the rebuild barrier
    CHECK(rig.supervisor.RebuildCount() == 1);
}

TEST_CASE("gpu: a device fault rebuilds the pipeline and saves keep working" * doctest::test_suite("gpu")) {
    PipelineRig rig(L"fault");
    rig.Start();
    std::this_thread::sleep_for(1500ms);

    rig.supervisor.TestInjectDeviceRemoved();
    REQUIRE(WaitFor([&] { return rig.supervisor.RebuildCount() >= 1; }, 10000ms));
    std::this_thread::sleep_for(2000ms);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
    CHECK(ProbeVideoFrames(rig.SavedPath()).first_is_key);
    CHECK(rig.scene->opens.load() == 2);
    CHECK(rig.ring.BarrierCount() >= 1);
}

// The capture item closing (window gone, display detached) takes the same rebuild path as a device
// fault, and each rebuild drops exactly one barrier.
TEST_CASE("gpu: a closed capture item rebuilds, then a device fault rebuilds again" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"item-closed");
    rig.Start();
    std::this_thread::sleep_for(1500ms);

    rig.supervisor.TestInjectItemClosed();
    REQUIRE(WaitFor([&] { return rig.supervisor.RebuildCount() >= 1; }, 10000ms));
    CHECK(rig.ring.BarrierCount() == 1);
    const uint64_t ticks = rig.supervisor.CurrentTicksSubmitted();
    std::this_thread::sleep_for(800ms);
    CHECK(rig.supervisor.CurrentTicksSubmitted() > ticks);

    rig.supervisor.TestInjectDeviceRemoved();
    REQUIRE(WaitFor([&] { return rig.supervisor.RebuildCount() >= 2; }, 10000ms));
    CHECK(rig.ring.BarrierCount() == 2);
    std::this_thread::sleep_for(2000ms);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
    CHECK(rig.scene->opens.load() == 3);
}

// Pins PipelineSupervisor::BuildGeneration's contract that a capture-open failure during a rebuild
// (fatal_open=false) retries on the capped backoff instead of taking the process down, now that
// Capture has a TryOpen() counterpart to Encoder::TryOpen (capture.h/.cpp). SyntheticCapture never
// died even before this fix -- what this proves is the supervisor's plumbing (BuildGeneration
// calling TryOpen, reading FaultReason(), and RebuildWithBackoff retrying on a real fault, not just
// the routine "target not found" case it already handled). The real Capture::Open() die() paths
// (no NVIDIA adapter, a failing D3D11CreateDevice) stay unverified here by construction -- a
// synthetic source can't reproduce them without a real adapter loss.

TEST_CASE("gpu: a capture-open fault during rebuild backs off and retries instead of dying" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"capture-open-fault");
    rig.Start();
    std::this_thread::sleep_for(1500ms);

    rig.scene->fail_next_opens.store(2);
    rig.supervisor.TestInjectDeviceRemoved();
    REQUIRE(WaitFor([&] { return rig.scene->fail_next_opens.load() == 0; }, 10000ms));
    REQUIRE(WaitFor([&] { return rig.supervisor.RebuildCount() >= 1; }, 15000ms));
    std::this_thread::sleep_for(2000ms);

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    CheckCleanH264(rig.SavedPath(), 320, 180);
    // opens only counts successful Open()s (SyntheticScene's fetch_add sits past the injected
    // early-return) -- the initial open plus the rebuild's third, finally-successful attempt.
    CHECK(rig.scene->opens.load() == 2);
}

// Game mode between games: a disarm that lasts a whole buffer length frees the ring (and the gate
// main.cpp builds suspends the audio hub); re-arming records into fresh memory and saves cleanly.
// The released ring has no barriers left, so this also checks the re-arm barrier carries the new
// segment's SPS/PPS.
TEST_CASE("gpu: a ring released after a long disarm records and saves cleanly after re-arm" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"release_rearm");
    rig.ring.SetMaxAge(RingMaxAgeQpc(rig.ctx.buffer_seconds));  // 3 s + 2 s, as the engine sets it
    std::atomic<int> released{0};
    rig.supervisor.SetRingReleaseCallbacks(
        [&] {
            released.fetch_add(1);
            rig.audio.SetSuspended(true);
        },
        [&] { rig.audio.SetSuspended(false); });
    rig.Start();
    std::this_thread::sleep_for(2000ms);
    CHECK(rig.ring.CommittedBytes() > 0);

    rig.supervisor.SetArmed(false);
    // Wait on the callback, not RingReleased(): the ring reads released before the watchdog has
    // run the callback (supervisor.cpp, ReleaseIfIdle then on_ring_released_).
    REQUIRE(WaitFor([&] { return released.load() == 1; }, 10000ms));
    CHECK(rig.supervisor.RingReleased());
    CHECK(rig.ring.CommittedBytes() == 0);
    CHECK(rig.audio.Suspended());
    CHECK(WaitFor([&] { return rig.audio.LiveBytes() == 0; }, 1000ms));

    rig.supervisor.SetArmed(true);
    CHECK_FALSE(rig.audio.Suspended());
    std::this_thread::sleep_for(3500ms);
    CHECK_FALSE(rig.ring.Released());

    REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
    const std::wstring file = rig.SavedPath();
    CheckCleanH264(file, 320, 180);
    const VideoFrames frames = ProbeVideoFrames(file);
    MESSAGE("after re-arm: frames " << frames.count);
    CHECK(frames.first_is_key);
    CHECK(frames.count >= 2 * kFps - 2);
    CHECK(released.load() == 1);  // not released again while armed
}

// The pipe's save_now against real footage: the handler's own save_in_flight claim, the path it
// reports, and the SaveContext fields get_status reads afterwards. The other request types are
// covered headless (engine_pipe_tests.cpp).
TEST_CASE("gpu: save_now through the pipe handler saves the footage and reports its path" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"pipe-save-now");
    BufferController controller(&rig.supervisor);
    rig.ctx.controller = &controller;
    rig.Start();
    std::this_thread::sleep_for(3500ms);

    Config    cfg{};
    JsonValue req = JsonValue::MakeObject();
    req.Set("type", JsonValue::MakeStr("save_now"));
    JsonValue resp;
    {
        StdoutSilencer quiet;
        resp = HandlePipeRequest(req, cfg, rig.supervisor, controller, rig.ctx);
    }
    const JsonValue* saved = resp.Find("saved");
    const JsonValue* path  = resp.Find("path");
    REQUIRE(saved);
    REQUIRE(saved->b);
    REQUIRE(path);
    REQUIRE(path->type == JsonValue::Type::String);
    const std::wstring file = rig.SavedPath();
    CHECK(path->str == NarrowUtf8(file));
    CHECK_FALSE(rig.ctx.save_in_flight.load());
    {
        std::lock_guard<std::mutex> lock(rig.ctx.last_save_mutex);
        CHECK(rig.ctx.last_save_qpc > 0);
    }
    CheckCleanH264(file, 320, 180);
}

// Each apply tier against a running pipeline, with a save after every change that moves footage:
// Free changes nothing in flight, Rebuild cuts a segment the save spans cleanly, Realloc is refused
// while a save holds a pin and drops the buffer otherwise, and buffer.storage switches the ring to a
// file and back (a Realloc once re-Inited a file-backed ring as RAM).
TEST_CASE("gpu: each live-apply tier on a running pipeline leaves saves working" *
          doctest::test_suite("gpu")) {
    PipelineRig rig(L"apply-tiers");
    BufferController controller(&rig.supervisor);
    rig.ctx.controller = &controller;
    rig.Start();
    std::this_thread::sleep_for(3000ms);

    Config cfg{};
    cfg.capture_framerate   = kFps;
    cfg.encode_bitrate_kbps = kBitrateKbps;
    cfg.encode_keyframe_sec = kKeyframeSec;
    cfg.buffer_seconds      = 3.0;
    auto apply = [&](JsonValue patch) {
        return ApplyLiveConfig(cfg, patch, rig.supervisor, controller, rig.ctx, rig.ring);
    };
    // The rig opens H.264; the first Rebuild-tier apply reopens with cfg's encode.video_codec
    // (AV1 by default), so the saves after it also cover a live codec switch.
    auto save_now = [&] {
        REQUIRE(rig.Save(QpcNow()) == SaveOutcome::kSaved);
        const bool av1 = rig.supervisor.CurrentParams().codec == VideoCodec::AV1;
        CheckCleanVideo(rig.SavedPath(), 320, 180, av1 ? "av1" : "h264");
    };

    {
        const uint64_t rebuilds = rig.supervisor.RebuildCount();
        JsonValue      patch    = JsonValue::MakeObject();
        patch.Set("output.filename", JsonValue::MakeStr("tiers_free_{game}.mp4"));
        const LiveApplyResult res = apply(std::move(patch));
        CHECK(res.ok);
        CHECK(res.tier == ApplyTier::Free);
        CHECK_FALSE(res.buffer_dropped);
        CHECK(rig.supervisor.RebuildCount() == rebuilds);
        CHECK(rig.ctx.output_filename == "tiers_free_{game}.mp4");
    }
    {
        const uint64_t rebuilds = rig.supervisor.RebuildCount();
        JsonValue      patch    = JsonValue::MakeObject();
        patch.Set("encode.preset", JsonValue::MakeStr("p3"));
        const LiveApplyResult res = apply(std::move(patch));
        CHECK(res.ok);
        CHECK(res.tier == ApplyTier::Rebuild);
        CHECK_FALSE(res.buffer_dropped);
        CHECK(rig.supervisor.RebuildCount() > rebuilds);
        std::this_thread::sleep_for(2000ms);
        save_now();
    }
    JsonValue longer = JsonValue::MakeObject();
    longer.Set("buffer.seconds", JsonValue::MakeNum(5.0));
    {
        std::this_thread::sleep_for(2500ms);
        REQUIRE_FALSE(rig.ring.Pin(0, QpcNow()).empty());
        const LiveApplyResult res = apply(longer);
        CHECK_FALSE(res.ok);
        CHECK_FALSE(res.error.empty());
        rig.ring.Unpin();
    }
    {
        const size_t          capacity = rig.ring.Capacity();
        const LiveApplyResult res      = apply(longer);
        CHECK(res.ok);
        CHECK(res.tier == ApplyTier::Realloc);
        CHECK(res.buffer_dropped);
        CHECK(rig.ring.Capacity() != capacity);
        std::this_thread::sleep_for(2500ms);
        save_now();
    }
    const std::wstring ring_file = TestOutputPath(L"pipeline\\apply-tiers-ring.dat");
    {
        DeleteFileW(ring_file.c_str());
        JsonValue patch = JsonValue::MakeObject();
        patch.Set("buffer.storage", JsonValue::MakeStr("file"));
        patch.Set("buffer.file_path", JsonValue::MakeStr(NarrowUtf8(ring_file)));
        const LiveApplyResult res = apply(std::move(patch));
        CHECK(res.ok);
        CHECK(cfg.buffer_storage == "file");
        CHECK(GetFileAttributesW(ring_file.c_str()) != INVALID_FILE_ATTRIBUTES);
        std::this_thread::sleep_for(2500ms);
        save_now();
    }
    {
        JsonValue patch = JsonValue::MakeObject();
        patch.Set("buffer.storage", JsonValue::MakeStr("ram"));
        const LiveApplyResult res = apply(std::move(patch));
        CHECK(res.ok);
        CHECK(cfg.buffer_storage == "ram");
        std::this_thread::sleep_for(2500ms);
        save_now();
    }
}

}  // namespace ombra::test
