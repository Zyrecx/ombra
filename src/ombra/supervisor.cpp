#include "ombra/supervisor.h"

#include "ombra/av1.h"
#include "ombra/capture.h"
#include "ombra/crash_upload.h"
#include "ombra/die.h"
#include "ombra/log.h"
#include "ombra/telemetry.h"
#include "ombra/util.h"

#include <windows.h>

#include <winrt/base.h>

#include <chrono>

namespace ombra {

namespace {

// Backoff schedule: 1s, 2s, 4s, 8s, capped at 30s.
constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 16, 30};
constexpr int kBackoffSteps     = static_cast<int>(sizeof(kBackoffSeconds) / sizeof(kBackoffSeconds[0]));

// Reset the backoff after this long healthy, not on the first good watchdog poll — a fault that
// clears for one 200ms tick and immediately re-triggers shouldn't get a full reset.
constexpr auto kHealthyResetThreshold = std::chrono::seconds(10);

constexpr auto kWatchdogPollInterval = std::chrono::milliseconds(200);

// A resolution reopen must not fire until the pending size has held
// steady across at least this long -- see supervisor.h's resize_debounce_* comment. Two poll
// intervals: long enough that a mid-drag-resize (which reports a new ContentSize on nearly every
// WGC frame) never sees the same size twice in a row until the user actually stops moving the
// mouse, short enough that a single deliberate resize (maximize, snap) still reopens promptly.
constexpr auto kResizeSettleDelay = std::chrono::milliseconds(400);
// Window-mode targets: how often the watchdog re-resolves which window to record, and how long a
// different pick must hold before switching to it. Each switch is a rebuild and a barrier, so a
// focus flicker (a launcher popping up for a moment) must not cause one.
constexpr auto kWindowResolveInterval = std::chrono::seconds(1);
constexpr auto kWindowSwitchSettle    = std::chrono::seconds(2);
}  // namespace

PipelineSupervisor::PipelineSupervisor(uint32_t fps, uint32_t bitrate_kbps, uint32_t keyframe_sec,
                                        Ring* ring, const EncoderOptions& encoder_opts,
                                        bool capture_cursor, const CaptureTarget& target)
    : fps_(fps), bitrate_kbps_(bitrate_kbps), keyframe_sec_(keyframe_sec), ring_(ring),
      encoder_opts_(encoder_opts), capture_cursor_(capture_cursor),
      target_(target),
      // "DISPLAY1" from "\\.\DISPLAY1", or "primary".
      flight_(target.monitor_device.rfind("\\\\.\\", 0) == 0 ? target.monitor_device.c_str() + 4
                                                              : target.monitor_device.c_str()) {}

PipelineSupervisor::~PipelineSupervisor() {
    if (running_.load(std::memory_order_acquire)) Stop();
}

void PipelineSupervisor::Start(AuCallback cb, bool initial_armed) {
    cb_ = std::move(cb);
    // Fatal-at-startup validation always runs, armed or not (advisor review): a machine with no
    // usable NVENC should refuse to start loudly regardless of which mode it was
    // launched in, not wait for the first game to arm before finding out.
    BuildGeneration(/*fatal_open=*/true);
    if (!initial_armed) {
        // Nothing has been appended to this generation yet, so there is nothing to preserve —
        // tear it straight back down and leave the watchdog to build the real one on SetArmed(true).
        TeardownGeneration();
        pending_barrier_qpc_ = 0;
        armed_.store(false, std::memory_order_release);
    } else {
        armed_.store(true, std::memory_order_release);
    }
    running_.store(true, std::memory_order_release);
    watchdog_ = std::thread(&PipelineSupervisor::WatchdogLoop, this);
}

void PipelineSupervisor::SetArmed(bool want_armed) {
    bool expected = !want_armed;
    if (!armed_.compare_exchange_strong(expected, want_armed, std::memory_order_acq_rel)) return;
    if (want_armed && on_arm_) on_arm_();
    Log::Info("pipeline %s requested", want_armed ? "arm" : "disarm");
    // The actual teardown/build happens on the watchdog thread's next poll (see WatchdogLoop) —
    // it's the only thread allowed to touch capture_/encoder_ once Start() has returned, same
    // discipline as every other generation-lifecycle transition in this class.
}

void PipelineSupervisor::Stop() {
    // Stop the watchdog first: TeardownGeneration() below must not race a rebuild the watchdog
    // could otherwise start concurrently.
    running_.store(false, std::memory_order_release);
    if (watchdog_.joinable()) watchdog_.join();
    TeardownGeneration();
}

// Note: BuildGeneration() can leave *this* function throwing winrt::hresult_error to its caller
// (some WGC calls throw rather than returning an HRESULT) -- every caller below catches it.
//
// Half of the die()-on-rebuild gap is closed: Encoder::Open() now has a
// non-fatal counterpart (Encoder::TryOpen) and every rebuild uses it, so an encoder that will not
// initialize at the current size no longer exits the process -- it fails the build and the backoff
// loop retries. Capture now has the same split (Capture::TryOpen, capture.h/.cpp): a rebuild
// (fatal_open=false) uses it, so an adapter/driver fault during a fault rebuild retries on the
// same backoff instead of taking the process down. Startup (fatal_open=true) keeps Open()'s fatal
// behaviour -- a machine with no usable NVIDIA adapter should still refuse to start loudly.

void PipelineSupervisor::SetGameGpu(std::optional<LUID> luid) {
    std::lock_guard<std::mutex> lock(game_gpu_mutex_);
    const bool same = luid.has_value() == game_gpu_.has_value() && (!luid || SameLuid(*luid, *game_gpu_));
    if (same) return;
    if (!luid && game_gpu_) {
        game_gpu_held_        = game_gpu_;
        game_gpu_cleared_qpc_ = QpcNow();
    }
    game_gpu_ = luid;
}

std::optional<LUID> PipelineSupervisor::EffectiveGameGpu() const {
    std::lock_guard<std::mutex> lock(game_gpu_mutex_);
    if (game_gpu_) return game_gpu_;
    // Held for as long as a save can reach back: switching sooner cuts a barrier through the
    // game's last footage, and Ring::Pin() can't cross one. A disarmed ring rebuilds fresh anyway.
    if (game_gpu_held_ && ring_ && armed_.load(std::memory_order_acquire) &&
        QpcNow() - game_gpu_cleared_qpc_ < ring_->MaxAge())
        return game_gpu_held_;
    return std::nullopt;
}

std::optional<EncodeAdapter> PipelineSupervisor::ResolveAdapter(const CaptureTarget& target, const LUID* exclude) {
    const std::optional<LUID> game_gpu = EffectiveGameGpu();
    const LUID* game = game_gpu ? &*game_gpu : nullptr;
    if (adapter_resolver_) return adapter_resolver_(exclude, game);
    // A window's monitor is wherever the window Capture will pick is now; a move to a monitor on
    // another GPU is caught by the periodic recheck (ShouldReturnToPickedAdapter).
    HMONITOR monitor = nullptr;
    if (target.mode == CaptureTarget::Mode::Window) {
        const std::vector<WindowCandidate> windows = SnapshotTargetWindows();
        if (const int pick = ChooseTargetWindow(target.window, windows, target.preferred_window); pick >= 0)
            monitor = MonitorFromWindow(reinterpret_cast<HWND>(windows[static_cast<size_t>(pick)].id), MONITOR_DEFAULTTONULL);
    } else {
        monitor = MonitorForDevice(target.monitor_device);
    }
    return ResolveEncodeAdapter(encoder_opts_.adapter, monitor, exclude, game);
}

bool PipelineSupervisor::ShouldReturnToPickedAdapter() {
    const auto now = std::chrono::steady_clock::now();
    // A game starting or (after the hold) leaving changes auto's pick: check now and switch on the
    // first answer, or a save a minute into the game would reach back only to a late switch. The
    // twice-in-a-row rule below is for a driver still installing, which this isn't.
    const std::optional<LUID> game_gpu = encoder_opts_.adapter == "auto" ? EffectiveGameGpu() : std::nullopt;
    const bool game_changed = game_gpu.has_value() != game_gpu_applied_.has_value() ||
                              (game_gpu && !SameLuid(*game_gpu, *game_gpu_applied_));
    game_gpu_applied_ = game_gpu;
    if (game_changed && !game_gpu) {
        std::lock_guard<std::mutex> lock(game_gpu_mutex_);
        if (!game_gpu_) game_gpu_held_.reset();  // the hold is over; a later longer buffer can't revive it
    }
    if (!game_changed && now < next_adapter_check_) return false;
    // After a failed return, the backoff still decides when to try again.
    if (game_changed && adapter_return_failures_ > 0 && now < next_adapter_check_) return false;
    next_adapter_check_ = now + adapter_recheck_;

    EncodeAdapter current;
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        current = encode_adapter_;
    }
    // Auto's pick follows the recorded monitor's GPU, so any adapter can be the wrong one. An explicit
    // nvidia on NVENC is where it should be; an explicit AMD or CPU too. After a hardware encoder
    // wouldn't open, retrying here would cut the ring every check.
    const bool can_improve = !on_cpu_fallback_ &&
                             (encoder_opts_.adapter == "auto" ||
                              (encoder_opts_.adapter == "nvidia" && current.backend != EncoderBackend::Nvenc));
    if (!can_improve) {
        adapter_differed_ = false;
        return false;
    }
    const std::optional<EncodeAdapter> picked = ResolveAdapter(EffectiveTarget());
    const bool differs = picked && (!SameLuid(picked->luid, current.luid) || picked->backend != current.backend);
    // Twice in a row: a driver still installing can list its adapter before it can encode.
    const bool settled = differs && (adapter_differed_ || game_changed);
    return_for_game_ = settled && game_changed;
    adapter_differed_ = differs;
    return settled;
}

bool PipelineSupervisor::ReturnToPickedAdapter(int& backoff_index) {
    if (!return_for_game_) {
        Log::Info("encode adapter    the GPU encode.adapter wants is back -- rebuilding on it");
        Telemetry::Event("encode_adapter_return", "");
    } else if (game_gpu_applied_) {
        Log::Info("encode adapter    the game renders on another GPU -- recording there");
        Telemetry::Event("encode_adapter_game", "to=game");
    } else {
        Log::Info("encode adapter    the game's footage aged out -- back to the display's GPU");
        Telemetry::Event("encode_adapter_game", "to=display");
    }
    return_for_game_ = false;
    adapter_differed_ = false;
    EncodeAdapter left;
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        left = encode_adapter_;
    }
    rebuilding_.store(true, std::memory_order_release);
    TeardownGeneration();
    pending_barrier_qpc_ = QpcNow();  // another codec or GPU can't share a segment

    const auto try_build = [this] {
        try {
            return BuildGeneration(/*fatal_open=*/false);
        } catch (winrt::hresult_error const& e) {
            Log::Warn("encode adapter    build threw 0x%08lX %ls", static_cast<unsigned long>(static_cast<HRESULT>(e.code())),
                       e.message().c_str());
            return false;
        }
    };
    const std::optional<LUID> refused = std::exchange(refused_gpu_, std::nullopt);  // this try is for it
    bool built = try_build();
    if (built) {
        adapter_return_failures_ = 0;
    } else {
        // Each failed try cuts the ring, so they get rarer: 5 minutes, doubling to an hour at the real
        // 15 s recheck. These failures say nothing about the GPU's own fallback count either.
        const int factor = (std::min)(240, 20 << (std::min)(adapter_return_failures_, 4));
        ++adapter_return_failures_;
        next_adapter_check_ = std::chrono::steady_clock::now() + adapter_recheck_ * factor;
        hw_open_failures_ = 0;
        next_build_adapter_ = left;
        if (!refused_gpu_) refused_gpu_ = refused;
        Log::Info("encode adapter    it can't encode yet -- staying on the previous GPU, trying again later");
        built = try_build();
    }
    if (built) {
        rebuilding_.store(false, std::memory_order_release);
        return true;
    }
    const int attempt = static_cast<int>(rebuild_count_.load(std::memory_order_acquire)) + 1;
    return RebuildWithBackoff(attempt, backoff_index);
}

bool PipelineSupervisor::BuildGeneration(bool fatal_open) {
    last_build_target_missing_ = false;
    // Re-resolved every build: an adapter that disappeared since the last one (MUX switch, driver
    // reset) falls back to auto here rather than failing forever.
    // Auto and the CPU target follow the monitor being recorded (ResolveAdapter).
    const CaptureTarget target = EffectiveTarget();
    const LUID* skip = refused_gpu_ && encoder_opts_.adapter == "auto" ? &*refused_gpu_ : nullptr;
    const std::optional<EncodeAdapter> adapter =
        next_build_adapter_ ? std::exchange(next_build_adapter_, std::nullopt) : ResolveAdapter(target, skip);
    if (!adapter) {
        // Not even a software adapter: nothing can capture, so the CPU encoder can't help either.
        if (fatal_open) die("no display adapter to capture on");
        Log::Warn("pipeline build: no display adapter to capture on");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        if (!SameLuid(encode_adapter_.luid, adapter->luid) || encode_adapter_.backend != adapter->backend)
            Log::InfoW(L"encode adapter    %s  (%hs)", adapter->description.c_str(),
                       BackendName(adapter->backend));
        encode_adapter_ = *adapter;
    }

    std::unique_ptr<ICaptureSource> capture =
        capture_factory_ ? capture_factory_() : std::make_unique<Capture>();
    const bool opened_with_cursor = capture_cursor_.load();
    const bool capture_opened =
        fatal_open ? capture->Open(opened_with_cursor, EffectiveTarget(), adapter->luid)
                   : capture->TryOpen(opened_with_cursor, EffectiveTarget(), adapter->luid);
    if (!capture_opened) {
        if (!capture->FaultReason().empty()) {
            // A real fault (TryOpen only -- Open() would have died instead): log it the same way
            // the encoder-open-failed branch below does, and let the caller's backoff retry.
            Log::Warn("pipeline build: capture open failed -- %s", capture->FaultReason().c_str());
            return false;
        }
        // A strict Mode::Window with no match returns false with FaultReason() left empty —
        // that's the only other reason Open()/TryOpen() returns false, a routine
        // condition worth one log line per transition, not a flood on every RebuildWithBackoff
        // attempt (up to every 30s, forever, until the target appears).
        if (!target_missing_logged_) {
            Log::Info("pipeline build: capture target not running -- checking every second");
            target_missing_logged_ = true;
        }
        last_build_target_missing_ = true;
        return false;
    }
    target_missing_logged_ = false;

    std::unique_ptr<IVideoEncoder> encoder;
    std::unique_ptr<FrameScaler>   scaler;
    std::optional<EncodeAdapter>   other_gpu;
    if (!OpenEncodeStage(*capture, fatal_open, "pipeline build", encoder, scaler,
                         trying_other_gpu_ ? nullptr : &other_gpu)) {
        capture->Close();
        if (!other_gpu) return false;
        // Once per build: the other GPU's own failure goes to x264, not back here.
        next_build_adapter_ = std::move(other_gpu);
        trying_other_gpu_   = true;
        const bool built    = BuildGeneration(fatal_open);
        trying_other_gpu_   = false;
        return built;
    }

    capture->StartCapture();

    ICaptureSource* cap = capture.get();
    IVideoEncoder* enc = encoder.get();
    FrameScaler*   scl = scaler.get();
    {
        std::lock_guard<std::mutex> lock(gen_mutex_);
        // A cursor change that landed during the open found no capture_ to apply to. Its writer
        // stores before taking this lock, so either it sees capture_ or this sees its value.
        if (const bool cursor = capture_cursor_.load(); cursor != opened_with_cursor)
            capture->SetCursorCaptureEnabled(cursor);
        capture_ = std::move(capture);
        scaler_  = std::move(scaler);
        encoder_ = std::move(encoder);
    }
    // Raw references are fine here: StartSegment starts the Pacer on these exact objects, and
    // TeardownGeneration always stops (joins) that Pacer before the unique_ptrs above are allowed
    // to go -- see the header's gen_mutex_ comment.
    StartSegment(*cap, *enc, scl);
    return true;
}

bool PipelineSupervisor::OpenEncodeStage(ICaptureSource& capture, bool fatal_open, const char* context,
                                         std::unique_ptr<IVideoEncoder>& encoder,
                                         std::unique_ptr<FrameScaler>& scaler,
                                         std::optional<EncodeAdapter>* other_gpu) {
    const uint32_t      src_w   = capture.Width(), src_h = capture.Height();
    const EncodeAdapter adapter = CurrentEncodeAdapter();
    const VideoSize     wanted  = ResolveOutputSize(src_w, src_h, capture_resolution_);
    EncoderOptions      opts    = encoder_opts_;
    const EncodePlan    plan    = PlanEncode(wanted, opts.codec, adapter.av1_align_w, adapter.av1_align_h,
                                             opts.av1_unaligned);
    const VideoSize     out     = plan.size;
    if (plan.codec != opts.codec || !(out == wanted))
        Log::Info("%s: AV1 codes %ux%u only padded (aligns to %ux%u); encode.amf_av1_unaligned=%s -> %s %ux%u",
                  context, wanted.width, wanted.height, adapter.av1_align_w, adapter.av1_align_h,
                  opts.av1_unaligned.c_str(), VideoCodecName(plan.codec), out.width, out.height);
    opts.codec = plan.codec;
    if (out.width != src_w || out.height != src_h) {
        std::vector<ID3D11Texture2D*> slots;
        for (uint32_t slot = 0; slot < ICaptureSource::kSlots; ++slot) slots.push_back(capture.SlotTexture(slot));
        scaler = std::make_unique<FrameScaler>();
        std::string error;
        if (!scaler->Open(capture.Device(), slots, src_w, src_h, out.width, out.height, error)) {
            Log::Warn("%s: %s", context, error.c_str());
            scaler.reset();
            return false;
        }
        Log::Info("scaler  %ux%u -> %ux%u  (capture.resolution %s)", src_w, src_h, out.width, out.height,
                  capture_resolution_.c_str());
    }

    // A hardware encoder always opens non-fatally: when it won't open, the CPU encoder takes over on
    // the same capture device (at startup at once, on a rebuild after kHwOpenFailuresBeforeCpu
    // failed builds, so a driver reset that clears in a few seconds gets the GPU back). Only the CPU
    // encoder opens fatally at startup: with that failing, nothing can record.
    const bool hardware = adapter.backend != EncoderBackend::X264;
    auto make_encoder = [this](EncoderBackend b) {
        return encoder_factory_ ? encoder_factory_(b) : CreateVideoEncoder(b);
    };
    // x264 is H.264 whatever it's asked for. A GPU that lacks AV1 outright also opens H.264, inside Open,
    // and keeps AV1's bitrate: EncodeAdapter doesn't say which GPUs those are.
    const uint32_t kbps = BitrateForOpenedCodec(bitrate_kbps_, opts.preset_bitrate_codec,
                                                hardware ? plan.codec : VideoCodec::H264);
    if (kbps != bitrate_kbps_) Log::Info("%s: %u kbps for the codec it opened (preset gave %u)", context, kbps, bitrate_kbps_);
    encoder = make_encoder(adapter.backend);
    bool opened =
        (fatal_open && !hardware)
            ? encoder->Open(capture.Device(), out.width, out.height, fps_, kbps, keyframe_sec_, opts)
            : encoder->TryOpen(capture.Device(), out.width, out.height, fps_, kbps, keyframe_sec_, opts);
    if (hardware && opened) {
        hw_open_failures_ = 0;
        on_cpu_fallback_  = false;
    }
    if (hardware && !opened && (fatal_open || ++hw_open_failures_ >= kHwOpenFailuresBeforeCpu)) {
        const std::string reason = std::string(BackendName(adapter.backend)) + " would not open: " +
                                   encoder->FaultReason();
        // Auto picked the display's GPU over another with a hardware encoder: that one comes before
        // the CPU. The recheck tries this GPU again later (ReturnToPickedAdapter's backoff).
        if (other_gpu && encoder_opts_.adapter == "auto") {
            std::optional<EncodeAdapter> next = ResolveAdapter(EffectiveTarget(), &adapter.luid);
            if (next && next->backend != EncoderBackend::X264) {
                Log::Warn("%s: %s -- trying %s on the other GPU", context, reason.c_str(), BackendName(next->backend));
                Telemetry::Event("encoder_other_gpu", "from=%s to=%s reason=\"%s\"", BackendName(adapter.backend),
                                 BackendName(next->backend), encoder->FaultReason().c_str());
                encoder.reset();
                scaler.reset();
                *other_gpu   = std::move(next);
                refused_gpu_ = adapter.luid;
                return false;
            }
        }
        Log::Warn("%s: %s -- recording on the CPU (x264) instead", context, reason.c_str());
        Telemetry::Event("encoder_fallback", "from=%s reason=\"%s\"", BackendName(adapter.backend),
                         encoder->FaultReason().c_str());
        const std::string code = encoder->FaultCode();
        crash_upload::ReportError(crash_upload::ErrorKind::Encoder, "fallback:" + (code.empty() ? "unknown" : code),
                                  BackendName(adapter.backend), "hardware encoder would not open, recording on the CPU");
        encoder = make_encoder(EncoderBackend::X264);
        const uint32_t x264_kbps = BitrateForOpenedCodec(bitrate_kbps_, opts.preset_bitrate_codec, VideoCodec::H264);
        opened  = fatal_open ? encoder->Open(capture.Device(), out.width, out.height, fps_, x264_kbps,
                                             keyframe_sec_, opts)
                             : encoder->TryOpen(capture.Device(), out.width, out.height, fps_, x264_kbps,
                                                keyframe_sec_, opts);
        if (opened) {
            {
                std::lock_guard<std::mutex> lock(params_mutex_);
                encode_adapter_.backend = EncoderBackend::X264;
            }
            // Once per fallback, not on every rebuild that tries the GPU again and falls back again.
            if (!on_cpu_fallback_ && encoder_fallback_cb_) encoder_fallback_cb_(reason);
            on_cpu_fallback_ = true;
        }
    }
    if (!opened) {
        Log::Warn("%s: encoder open failed at %ux%u -- %s", context, out.width, out.height,
                   encoder->FaultReason().c_str());
        encoder.reset();
        scaler.reset();
        return false;
    }
    // Scaled: the encoder's one input is the scaler's output (slot 0, pacer.h). Native: the
    // capture's stage slots, in slot order.
    const uint32_t inputs = scaler ? 1 : ICaptureSource::kSlots;
    for (uint32_t slot = 0; slot < inputs; ++slot) {
        ID3D11Texture2D* tex = scaler ? scaler->Output() : capture.SlotTexture(slot);
        if (!encoder->RegisterSlot(tex, out.width, out.height)) {
            Log::Warn("%s: RegisterSlot(%u) failed -- %s", context, slot, encoder->FaultReason().c_str());
            encoder.reset();
            scaler.reset();
            return false;
        }
    }
    return true;
}

// The one place a new generation's parameters are published, its barrier is dropped and its pacer
// is started -- in that order, and deliberately not duplicated between the two build paths (full
// rebuild and resolution reopen). Getting this order wrong in one of two copies is exactly how the
// one-avcC-per-track bug class comes back: the barrier has to carry the params of the segment it
// starts, and it has to be in the ring before the first AU of that segment is appended.
void PipelineSupervisor::StartSegment(ICaptureSource& capture, IVideoEncoder& encoder, FrameScaler* scaler) {
    // The coded size: the mux writes avcC/av1C/tkhd from it, so it must match the stream. H.264
    // codes the size it was opened at (its SPS crops any padding); AV1 may pad past it
    // (encode.amf_av1_unaligned=pad), and its sequence header says by how much.
    auto seg          = std::make_shared<SegmentParams>();
    seg->codec        = encoder.Codec();
    seg->codec_header = encoder.SequenceParams();
    seg->width        = scaler ? scaler->Width() : capture.Width();
    seg->height       = scaler ? scaler->Height() : capture.Height();
    seg->picture_width  = seg->width;
    seg->picture_height = seg->height;
    seg->fps          = fps_;
    if (seg->codec == VideoCodec::AV1)
        Av1CodedSize(seg->codec_header.data(), seg->codec_header.size(), seg->width, seg->height);
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        params_.codec_header  = seg->codec_header;
        params_.codec         = seg->codec;
        params_.width         = seg->width;
        params_.height        = seg->height;
        params_.source_width  = capture.Width();
        params_.source_height = capture.Height();
        params_.picture_width  = seg->picture_width;
        params_.picture_height = seg->picture_height;
        params_.fps           = fps_;
    }

    if (pending_barrier_qpc_ != 0) {
        // Latched at teardown (when the last AU of the previous segment had already been appended,
        // since the pacer was joined first) and dropped here, before this generation's pacer
        // produces its first AU.
        if (ring_) ring_->Barrier(pending_barrier_qpc_, seg);
        Log::Info("segment barrier  qpc=%lld  %ux%u  ring_barriers=%zu",
                  static_cast<long long>(pending_barrier_qpc_), seg->width, seg->height,
                  ring_ ? ring_->BarrierCount() : size_t{0});
        pending_barrier_qpc_ = 0;
    } else if (ring_) {
        // First generation of the process: no cut to mark, but the ring still needs to know what
        // the data arriving now was encoded with, for a save that happens before any barrier.
        ring_->SetBaseSegmentParams(seg);
    }

    auto pacer = std::make_unique<Pacer>();
    pacer->SetFlightRecorder(&flight_);
    pacer->Start(capture, encoder, fps_, cb_, pre_encode_cb_, scaler);
    std::lock_guard<std::mutex> lock(gen_mutex_);
    pacer_ = std::move(pacer);
}

// The monitor changed resolution: the frame pool has to be recreated at the new size and the
// encoder reopened (NVENC cannot change resolution in place), but the *capture device and WGC
// item survive* -- and so does everything already in the ring. Losing the user's buffer on every
// alt-tab is the wrong trade; the barrier is what keeps a save from straddling the two segments,
// not a flush.
//
// Returns false if the reopen failed, in which case the caller falls back to a full backoff
// rebuild -- pending_barrier_qpc_ is already latched by then, so that rebuild drops the same
// barrier this one would have.
bool PipelineSupervisor::ReopenForResolution(uint32_t w, uint32_t h) {
    std::unique_ptr<IVideoEncoder> old_encoder;
    std::unique_ptr<FrameScaler>   old_scaler;
    {
        std::lock_guard<std::mutex> lock(gen_mutex_);
        old_encoder = std::move(encoder_);
        old_scaler  = std::move(scaler_);
    }
    DetachAndStopPacer();  // must be first: the pacer thread may be inside EncodeSlot right now
    if (old_encoder) old_encoder->Close();
    old_scaler.reset();  // after the encoder that had its output registered

    ICaptureSource* capture = nullptr;
    {
        std::lock_guard<std::mutex> lock(gen_mutex_);
        capture = capture_.get();
    }
    if (!capture) return false;

    // Latched between the last AU of the old segment (the pacer is joined) and the first of the
    // new one (this generation's pacer has not started yet) -- the cut is genuinely empty. Before
    // Recreate, so the fallback rebuild after a failed one still drops it.
    pending_barrier_qpc_ = QpcNow();
    if (!capture->Recreate(w, h)) return false;

    // Same adapter as the capture device that survives this reopen (CurrentEncodeAdapter), and a
    // new scaler: Recreate replaced the slot textures its views pointed at.
    std::unique_ptr<IVideoEncoder> encoder;
    std::unique_ptr<FrameScaler>   scaler;
    if (!OpenEncodeStage(*capture, /*fatal_open=*/false, "resolution reopen", encoder, scaler)) return false;

    IVideoEncoder* enc = encoder.get();
    FrameScaler*   scl = scaler.get();
    {
        std::lock_guard<std::mutex> lock(gen_mutex_);
        scaler_  = std::move(scaler);
        encoder_ = std::move(encoder);
    }
    StartSegment(*capture, *enc, scl);
    resolution_change_count_.fetch_add(1, std::memory_order_release);
    return true;
}

// Moves the current Pacer out, stops (joins) it and records its per-generation counters. Both
// teardown paths go through here so the stats snapshot cannot drift between them.
std::unique_ptr<Pacer> PipelineSupervisor::DetachAndStopPacer() {
    std::unique_ptr<Pacer> pacer;
    {
        std::lock_guard<std::mutex> lock(gen_mutex_);
        pacer = std::move(pacer_);
    }
    if (!pacer) return pacer;
    pacer->Stop();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    last_stats_.ticks_submitted = pacer->TicksSubmitted();
    last_stats_.duplicate_ticks = pacer->DuplicateTicks();
    last_stats_.late_ticks      = pacer->LateTicks();
    last_stats_.resync_count    = pacer->ResyncCount();
    last_stats_.timer_backend   = pacer->TimerBackend();
    last_stats_.latency         = pacer->ComputeLatencyStats();
    retired_counters_.Add(pacer->ReadCounters());
    return pacer;
}

void PipelineSupervisor::TeardownGeneration(FaultInfo* out_fault) {
    // This ordering is NOT wmain's final-shutdown ordering (capture.Close(); pacer.Stop();
    // encoder.Close(); -- safe there only because the pacer is known-idle by that point in a
    // single-shot run). Mid-session, the pacer thread may be inside ClaimSlotForEncoding/
    // EncodeSlot this instant; capture.Close() or encoder.Close() first would be a use-after-free
    // on the pointers that thread is holding. Stop (join) the pacer first, always.
    DetachAndStopPacer();

    std::unique_ptr<ICaptureSource> capture;
    std::unique_ptr<FrameScaler>    scaler;
    std::unique_ptr<IVideoEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(gen_mutex_);
        capture = std::move(capture_);
        scaler  = std::move(scaler_);
        encoder = std::move(encoder_);
    }
    // Read the fault reason here, with the pacer thread already joined above -- encoder.h's
    // FaultReason() contract requires that (a torn std::string read otherwise, since the pacer
    // thread is the only writer and Pacer::Stop() just guaranteed it is no longer running). Must
    // happen before `encoder`'s unique_ptr goes out of scope at the end of this function.
    if (out_fault) {
        if (capture && !capture->IsHealthy()) {
            out_fault->reason  = capture->HealthIssue();
            out_fault->code    = out_fault->reason.find("capture item closed") != std::string::npos ? "item_closed"
                                                                                                    : "device_removed";
            out_fault->capture = true;
        } else if (encoder && encoder->Faulted()) {
            out_fault->reason = encoder->FaultReason();
            out_fault->code   = encoder->FaultCode();
        }
        if (out_fault->code.empty()) out_fault->code = "unknown";
    }
    if (encoder) encoder->Close();
    scaler.reset();  // after the encoder (its output was registered), before the capture slots it reads
    if (capture) capture->Close();
}

PipelineSupervisor::RunStats PipelineSupervisor::LastStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return last_stats_;
}

TickCounters PipelineSupervisor::LifetimeCounters() const {
    TickCounters total;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        total = retired_counters_;
    }
    std::lock_guard<std::mutex> lock(gen_mutex_);
    if (pacer_) total.Add(pacer_->ReadCounters());
    return total;
}

uint64_t PipelineSupervisor::CurrentTicksSubmitted() const {
    std::lock_guard<std::mutex> lock(gen_mutex_);
    return pacer_ ? pacer_->TicksSubmitted() : 0;
}

EncodeAdapter PipelineSupervisor::CurrentEncodeAdapter() const {
    std::lock_guard<std::mutex> lock(params_mutex_);
    return encode_adapter_;
}

PipelineSupervisor::PipelineParams PipelineSupervisor::CurrentParams() const {
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_;
}

void PipelineSupervisor::TestInjectItemClosed() {
    std::lock_guard<std::mutex> lock(gen_mutex_);
    if (capture_) capture_->TestInjectItemClosed();
}

void PipelineSupervisor::TestInjectDeviceRemoved() {
    std::lock_guard<std::mutex> lock(gen_mutex_);
    if (capture_) capture_->TestInjectDeviceRemoved();
}

void PipelineSupervisor::TestForceContentSize(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lock(gen_mutex_);
    if (capture_) capture_->TestForceContentSize(w, h);
}

// Runs BuildGeneration() on the capped backoff until it succeeds or shutdown is requested. A
// first attempt can fail even for a transient fault -- e.g. the NVIDIA adapter takes a moment to
// reappear after a TDR -- so one failed attempt must not give up; the retry policy says "capped at
// 30s," not "give up after N tries." Returns false only when shutdown was requested mid-backoff,
// in which case no barrier is dropped at all (StartSegment never runs) -- harmless, since nothing
// was ever appended after the cut it would have marked.
bool PipelineSupervisor::RebuildWithBackoff(int attempt, int& backoff_index) {
    // Each failed attempt is a WARN; recording that stays down this long is footage the user loses,
    // so it is reported once as an ERROR.
    constexpr auto kDownIsLoss  = std::chrono::seconds(10);
    const auto     down_since   = std::chrono::steady_clock::now();
    bool           reported_down = false;
    auto down_seconds = [&] {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - down_since).count());
    };
    for (;;) {
        // A strict window target that isn't running is polled, not backed off (see
        // last_build_target_missing_), and quietly: BuildGeneration logged it once.
        const bool polling_for_target = last_build_target_missing_;
        // Waiting for a window that isn't open is the user's choice of target, not a failure.
        if (!reported_down && !polling_for_target && std::chrono::steady_clock::now() - down_since >= kDownIsLoss) {
            Log::Error("pipeline  recording has been down %llds and rebuilds keep failing -- still retrying",
                       down_seconds());
            reported_down = true;
            crash_upload::ReportError(crash_upload::ErrorKind::Capture, "rebuilds_failing",
                                      BackendName(CurrentEncodeAdapter().backend), "recording down, rebuilds failing");
        }
        const int  backoff_sec        = polling_for_target ? 1 : kBackoffSeconds[backoff_index];
        if (!polling_for_target) Log::Info("pipeline rebuild attempt=%d backing off %ds", attempt, backoff_sec);
        // Slept in 1s chunks (not one this_thread::sleep_for(backoff_sec)) so a pending
        // RequestReconfigure -- waiting on reconfig_mutex_, which this loop holds via WatchdogLoop's
        // reconfig_lock for its entire duration -- is noticed within ~1s instead of blocking behind
        // up to the full 30s-capped backoff. See reconfig_requested_'s declaration.
        for (int slept = 0; slept < backoff_sec; ++slept) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running_.load(std::memory_order_acquire) ||
                reconfig_requested_.load(std::memory_order_acquire) ||
                !armed_.load(std::memory_order_acquire)) {
                break;
            }
        }
        if (!polling_for_target && backoff_index + 1 < kBackoffSteps) ++backoff_index;

        if (!running_.load(std::memory_order_acquire)) {
            rebuilding_.store(false, std::memory_order_release);
            return false;
        }
        if (!armed_.load(std::memory_order_acquire)) {
            // Disarmed mid-retry (BufferController: the One window program stayed closed, the game
            // is gone, or the switch went off). Stop chasing the target; the watchdog's disarmed
            // branch takes over, so the ring can be released and the audio hub can sleep. The cut
            // stays latched in pending_barrier_qpc_ for the next arm.
            rebuilding_.store(false, std::memory_order_release);
            return false;
        }
        if (reconfig_requested_.load(std::memory_order_acquire)) {
            // Abandon this attempt -- a fresh RequestReconfigure call supersedes whatever target
            // this retry loop was still chasing (e.g. a closed window) and is waiting on
            // reconfig_mutex_ right now. The generation is already torn down at every call site
            // that reaches here, so there is nothing to clean up before backing off.
            rebuilding_.store(false, std::memory_order_release);
            return false;
        }

        // Waiting on a window: look before building, so a target that stays closed costs one
        // window snapshot a second, not an adapter enumeration.
        if (polling_for_target &&
            ChooseTargetWindow(EffectiveTarget().window, SnapshotTargetWindows(), 0) < 0) {
            continue;
        }

        bool built = false;
        try {
            built = BuildGeneration(/*fatal_open=*/false);
        } catch (winrt::hresult_error const& e) {
            Log::Warn("pipeline rebuild attempt=%d threw 0x%08lX %ls -- retrying", attempt,
                      static_cast<unsigned long>(static_cast<HRESULT>(e.code())),
                      e.message().c_str());
            continue;
        }
        if (!built) continue;  // already logged by BuildGeneration; retry on the same backoff

        rebuild_count_.fetch_add(1, std::memory_order_release);
        rebuilding_.store(false, std::memory_order_release);
        Log::Info("pipeline rebuilt  attempt=%d  ring_barriers=%zu", attempt,
                  ring_ ? ring_->BarrierCount() : size_t{0});
        if (reported_down) Log::Info("pipeline  recording is back after %llds", down_seconds());
        return true;
    }
}

CaptureTarget PipelineSupervisor::EffectiveTarget() const {
    CaptureTarget t = target_;
    if (t.mode == CaptureTarget::Mode::WindowWithFallback) {
        t.window.match.clear();  // capture.target_window is One window's, never the game's
        t.window.is_game = games_ ? games_->GameFilter() : [](const std::wstring&) { return false; };
        if (!game_fallback_.load(std::memory_order_acquire)) t.mode = CaptureTarget::Mode::Window;
    }
    return t;
}

uint64_t PipelineSupervisor::RecordedWindowId() const {
    std::lock_guard<std::mutex> lock(gen_mutex_);
    return capture_ ? capture_->TargetWindowId() : 0;
}

bool PipelineSupervisor::MaybeSwitchWindow(ICaptureSource& capture, int& backoff_index) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_window_check_) return false;
    next_window_check_ = now + kWindowResolveInterval;

    const uint64_t                     current = capture.TargetWindowId();
    const std::vector<WindowCandidate> windows = SnapshotTargetWindows();
    const int      pick = ChooseTargetWindow(EffectiveTarget().window, windows, current);
    const uint64_t want = pick >= 0 ? windows[static_cast<size_t>(pick)].id : 0;

    // want == 0 with a window recorded that still exists: the target changed and it no longer
    // counts, so switch to the monitor fallback or to waiting.
    const auto ms = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    };
    const bool current_exists = current != 0 && IsWindow(reinterpret_cast<HWND>(current));
    if (StepWindowSwitch(window_switch_, want, current, current_exists, ms(now),
                         std::chrono::duration_cast<std::chrono::milliseconds>(kWindowSwitchSettle).count()) !=
        WindowSwitchAction::Switch) {
        return false;
    }

    if (pick >= 0) {
        const WindowCandidate& w = windows[static_cast<size_t>(pick)];
        Log::InfoW(L"capture target  switching window -> %s  «%s»", w.image_path.c_str(), w.title.c_str());
    } else {
        Log::Info("capture target  the recorded window no longer matches the target -- rebuilding");
    }
    rebuilding_.store(true, std::memory_order_release);
    TeardownGeneration();
    pending_barrier_qpc_     = QpcNow();
    target_.preferred_window = want;
    bool built = false;
    try {
        built = BuildGeneration(/*fatal_open=*/false);
    } catch (winrt::hresult_error const& e) {
        Log::Warn("window switch build threw 0x%08lX %ls -- retrying on backoff",
                   static_cast<unsigned long>(static_cast<HRESULT>(e.code())), e.message().c_str());
    }
    if (built) {
        rebuild_count_.fetch_add(1, std::memory_order_release);
        rebuilding_.store(false, std::memory_order_release);
        return true;
    }
    const int attempt = static_cast<int>(rebuild_count_.load(std::memory_order_acquire)) + 1;
    RebuildWithBackoff(attempt, backoff_index);
    return true;
}

void PipelineSupervisor::WatchdogLoop() {
    Log::SetThreadName("watchdog");
    int backoff_index = 0;
    auto healthy_since = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kWatchdogPollInterval);
        if (!running_.load(std::memory_order_acquire)) break;

        std::unique_lock<std::mutex> reconfig_lock(reconfig_mutex_, std::defer_lock);
        if (!reconfig_lock.try_lock()) {
            continue;
        }

        ICaptureSource* capture = nullptr;
        IVideoEncoder* encoder = nullptr;
        {
            std::lock_guard<std::mutex> lock(gen_mutex_);
            capture = capture_.get();
            encoder = encoder_.get();
        }

        const bool want_armed = armed_.load(std::memory_order_acquire);
        if (!want_armed) {
            if (capture || encoder) {
                // SetArmed(false) just landed with a live generation — tear it down and stay
                // down. This is deliberately not TeardownGeneration()'s caller elsewhere (fault,
                // resolution reopen): no backoff, no rebuild attempt follows, per SetArmed's own
                // comment on why the watchdog and not Stop()/Start() owns this transition.
                Log::Info("pipeline disarm  tearing down, buffer left intact, no rebuild until rearmed");
                rebuilding_.store(true, std::memory_order_release);
                TeardownGeneration();
                pending_barrier_qpc_ = QpcNow();
                rebuilding_.store(false, std::memory_order_release);
            }
            if (ring_ && ring_->ReleaseIfIdle(QpcNow())) {
                Log::Info("pipeline disarmed for a whole buffer length -- ring memory released");
                if (on_ring_released_) on_ring_released_();
            }
            continue;  // nothing else to watch while disarmed
        }
        if (!capture || !encoder) {
            // Armed but no generation exists — either the very first arm-on-demand (game mode
            // booted disarmed) or the far side of a disarm/rearm cycle. Build one on
            // the same capped backoff a fault rebuild uses; pending_barrier_qpc_ is
            // already latched from the disarm (or 0 for a first arm-on-demand), and StartSegment
            // drops it once this generation's SPS/PPS exist to attach to it.
            Log::Info("pipeline arm  building generation");
            rebuilding_.store(true, std::memory_order_release);
            bool built = false;
            try {
                built = BuildGeneration(/*fatal_open=*/false);
            } catch (winrt::hresult_error const& e) {
                Log::Warn("pipeline arm build threw 0x%08lX %ls -- retrying on backoff",
                          static_cast<unsigned long>(static_cast<HRESULT>(e.code())),
                          e.message().c_str());
            }
            if (!built) {
                const int attempt =
                    static_cast<int>(rebuild_count_.load(std::memory_order_acquire)) + 1;
                if (!RebuildWithBackoff(attempt, backoff_index)) {
                    // false means either shutdown (running_ false -- stop the thread) or a pending
                    // RequestReconfigure preempted the retry (running_ still true -- drop
                    // reconfig_lock via `continue` and let it through; it will build fresh).
                    if (!running_.load(std::memory_order_acquire)) return;
                    continue;
                }
            } else {
                rebuilding_.store(false, std::memory_order_release);
            }
            healthy_since = std::chrono::steady_clock::now();
            continue;
        }

        // Here, not in Append: that runs on the pacer's per-tick path.
        if (ring_) ring_->TrimCommitted();
        if (const bool capped = ring_ && ring_->CommitCapped(); capped != logged_commit_cap_) {
            if (capped)
                Log::Warn("ring  the system refused more memory -- buffer holds what %.0f MB fits until the ring is "
                          "released or rebuilt", static_cast<double>(ring_->CommittedBytes()) / (1024.0 * 1024.0));
            logged_commit_cap_ = capped;
        }
        if (ring_ && ring_->TakeStorageLost()) {
            Log::Error("ring  buffer.file_path isn't usable (its drive may be gone) -- the buffer is in RAM until "
                       "the next start or settings change");
            if (Telemetry::Enabled()) Telemetry::Event("ring_storage_lost", "");
            if (ring_storage_lost_cb_) ring_storage_lost_cb_();
        }
        if (encoder != late_encoder_) late_encoder_ = encoder, late_logged_ = 0;
        // A rebuilt encoder can reuse the old one's address; its count starts lower, so resync quietly.
        if (const uint64_t late = encoder->LateFrames(); late < late_logged_) {
            late_logged_ = late;
        } else if (late > late_logged_) {
            Log::Warn("encoder late  %llu frame(s) needed more than one wait, longest %lld ms -- the GPU is busy",
                      static_cast<unsigned long long>(late), static_cast<long long>(encoder->LongestWaitMs()));
            if (Telemetry::Enabled())
                Telemetry::Event("encoder_late", "late=%llu longest_ms=%lld", static_cast<unsigned long long>(late),
                                 static_cast<long long>(encoder->LongestWaitMs()));
            late_logged_ = late;
        }

        const bool capture_ok = capture->IsHealthy();
        const bool encoder_ok = !encoder->Faulted();  // atomic read -- see encoder.h's contract
        if (capture_ok && encoder_ok) {
            // A resolution change is not a fault: the pipeline is healthy, it is just producing
            // frames at a size this generation's encoder cannot accept. Handled without
            // any backoff -- the user changed a display mode, there is nothing to wait for -- and
            // without tearing down the capture device, so the ring keeps every AU it already has.
            if (capture->SizeChanged()) {
                // A minimized window target is not a resolution change
                // to react to -- whatever (possibly degenerate) ContentSize WGC reports while
                // iconic isn't the size the user cares about, and reopening on it would cut a ring
                // barrier for no real content, same buffer-reset symptom as an unde-bounced resize.
                // Leave the generation alone; the real restored size, once real frames resume, goes
                // through the same debounce path below like any other change.
                if (capture->IsTargetMinimized()) continue;

                const uint32_t revision = capture->PendingRevision();
                const uint32_t nw = capture->PendingWidth();
                const uint32_t nh = capture->PendingHeight();
                if (nw != resize_debounce_w_ || nh != resize_debounce_h_ || revision != resize_debounce_revision_) {
                    // Pending size moved since the last poll -- still settling (e.g. mid-drag).
                    // Reset the debounce clock and wait rather than reopening on this intermediate
                    // size: each reopen cuts a ring segment barrier, and Ring::Pin() can't cross
                    // one, so reopening on every mid-resize frame was collapsing the replayable
                    // buffer to "since the last barrier" for the whole duration of a drag-resize.
                    resize_debounce_w_ = nw;
                    resize_debounce_h_ = nh;
                    resize_debounce_revision_ = revision;
                    resize_debounce_since_ = std::chrono::steady_clock::now();
                    continue;
                }
                if (std::chrono::steady_clock::now() - resize_debounce_since_ < kResizeSettleDelay) {
                    continue;  // same size as last poll, but not stable long enough yet
                }
                if (!capture->SizeChanged()) continue;  // withdrawn since the check above

                Log::Info("resolution change  %ux%u -> %ux%u  reopening encoder, ring preserved",
                          capture->Width(), capture->Height(), nw, nh);
                resize_debounce_w_ = 0;
                resize_debounce_h_ = 0;
                if (ReopenForResolution(nw, nh)) {
                    healthy_since = std::chrono::steady_clock::now();
                    continue;
                }
                // The reopen failed at the new size (pending_barrier_qpc_ is already latched, so
                // the rebuild below drops the same cut this one would have). Fall through to a
                // full teardown + backoff rebuild rather than leaving a generation half-built.
                Log::Warn("resolution reopen failed -- falling back to a full pipeline rebuild");
                rebuilding_.store(true, std::memory_order_release);
                TeardownGeneration();
                const int attempt =
                    static_cast<int>(rebuild_count_.load(std::memory_order_acquire)) + 1;
                if (!RebuildWithBackoff(attempt, backoff_index)) {
                    if (!running_.load(std::memory_order_acquire)) return;
                    continue;
                }
                healthy_since = std::chrono::steady_clock::now();
                continue;
            }
            // Withdrawn (PendingResize) or none: the same size coming back later settles from zero.
            resize_debounce_w_ = 0;
            resize_debounce_h_ = 0;
            if (target_.mode != CaptureTarget::Mode::Monitor && MaybeSwitchWindow(*capture, backoff_index)) {
                if (!running_.load(std::memory_order_acquire)) return;
                healthy_since = std::chrono::steady_clock::now();
                continue;
            }
            if (ShouldReturnToPickedAdapter()) {
                if (!ReturnToPickedAdapter(backoff_index)) {
                    if (!running_.load(std::memory_order_acquire)) return;
                    continue;
                }
                healthy_since = std::chrono::steady_clock::now();
                continue;
            }
            if (std::chrono::steady_clock::now() - healthy_since > kHealthyResetThreshold)
                backoff_index = 0;
            continue;
        }

        const int attempt = static_cast<int>(rebuild_count_.load(std::memory_order_acquire)) + 1;
        rebuilding_.store(true, std::memory_order_release);

        // TeardownGeneration joins the pacer thread before reading `reason` from encoder.h's
        // FaultReason() -- the only point that read is safe (see its own comment).
        FaultInfo fault;
        TeardownGeneration(&fault);
        const std::string& reason = fault.reason;
        // A monitor unplug or mode change closes the capture item: expected, and the rebuild below
        // recovers it. Anything else is a fault the rebuild recovers from (WARN); if it doesn't,
        // RebuildWithBackoff reports the outage as an ERROR.
        if (reason.find("capture item closed") != std::string::npos)
            Log::Info("pipeline  capture closed (%s), rebuilding  attempt=%d", reason.c_str(), attempt);
        else
        {
            Log::Warn("pipeline fault  reason=\"%s\"  attempt=%d, rebuilding", reason.c_str(), attempt);
            // What the encoder did just before: the pacer is joined, its ticks are all in.
            Log::Warn("%s", flight_.Summary(2.0).c_str());
            crash_upload::ReportError(fault.capture ? crash_upload::ErrorKind::Capture : crash_upload::ErrorKind::Encoder,
                                      fault.code, BackendName(CurrentEncodeAdapter().backend), "pipeline fault, rebuilding");
        }

        // Latch the cut here, with the pacer joined and nothing able to append; StartSegment drops
        // it on the far side, once the new generation's SPS/PPS exist to attach to it (rather than
        // dropping a bare, params-less barrier at this point). Latched once
        // per fault, not once per retry: a barrier just marks "don't read past here," and retrying
        // BuildGeneration() does not create a second cut to mark.
        pending_barrier_qpc_ = QpcNow();

        if (!RebuildWithBackoff(attempt, backoff_index)) {
            if (!running_.load(std::memory_order_acquire)) return;
            continue;
        }
        healthy_since = std::chrono::steady_clock::now();
    }
}

bool PipelineSupervisor::RequestReconfigure(const SupervisorReconfigParams& req) {
    if (req.tier == ApplyTier::Free) {
        capture_cursor_ = req.capture_cursor;
        std::lock_guard<std::mutex> lock(gen_mutex_);
        if (capture_) {
            capture_->SetCursorCaptureEnabled(capture_cursor_);
        }
        return true;
    }

    uint64_t my_realloc_seq = 0;
    if (req.tier == ApplyTier::Realloc) {
        if (ring_ && ring_->IsPinned()) {
            Log::Warn("pipeline reconfigure: Ring is currently pinned, refusing realloc");
            return false;
        }
        my_realloc_seq = realloc_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    // Tell RebuildWithBackoff (running on the watchdog thread, possibly holding reconfig_mutex_ for
    // up to its full 30s-capped backoff sleep while it retries a now-stale target) to abandon that
    // attempt instead of making this call block behind it. Cleared on every return path below so a
    // genuine future fault still gets its normal backoff.
    reconfig_requested_.store(true, std::memory_order_release);
    struct ReconfigRequestedGuard {
        std::atomic<bool>& flag;
        ~ReconfigRequestedGuard() { flag.store(false, std::memory_order_release); }
    } reconfig_requested_guard{reconfig_requested_};

    std::unique_lock<std::mutex> lock(reconfig_mutex_);

    if (req.tier == ApplyTier::Realloc && realloc_seq_.load(std::memory_order_acquire) != my_realloc_seq) {
        // A fresher Realloc request has already been queued behind this one -- e.g. the user
        // dragged the buffer.seconds slider through several values -- so this call's ring reinit
        // would only be immediately discarded once the fresher call runs. Skip it; the fresher
        // call is the one that actually reallocates.
        Log::Info("pipeline reconfigure: superseded by a newer realloc request, skipping");
        return true;
    }

    rebuilding_.store(true, std::memory_order_release);

    TeardownGeneration();

    if (req.tier == ApplyTier::Realloc) {
        if (ring_ && req.ring_capacity > 0) {
            // Threaded through the request rather than left sticky on Ring,
            // so both directions of a live buffer.storage change (ram->file and file->ram) do the
            // right thing instead of one direction silently no-oping — see reconfig.cpp, which
            // populates these two fields from the live Config on every Realloc-tier request, not
            // just ones that actually touch buffer.storage/buffer.file_path.
            if (req.ring_file_backed) {
                ring_->Init(req.ring_capacity, req.ring_overshoot, req.ring_file_path);
            } else {
                ring_->Init(req.ring_capacity, req.ring_overshoot);
            }
        }
        pending_barrier_qpc_ = 0;
    } else {
        pending_barrier_qpc_ = QpcNow();
    }

    fps_ = req.fps;
    bitrate_kbps_ = req.bitrate_kbps;
    keyframe_sec_ = req.keyframe_sec;
    encoder_opts_ = req.encoder_opts;
    capture_cursor_ = req.capture_cursor;
    // Only mode/window.match come from a live patch — target_.monitor_device is
    // deliberately left untouched. This supervisor's own monitor never changes at runtime (only
    // capture.monitors' pipeline *count* can, and that's a Rebuild-the-whole-vector-of-pipelines
    // change wmain does, not something RequestReconfigure's single-supervisor scope reaches); if
    // this branch also overwrote monitor_device from a request that left it at its CaptureTarget{}
    // default ("primary"), every unrelated Realloc/Rebuild reconfigure (e.g. buffer.seconds alone)
    // would silently reset a non-primary monitor target back to primary.
    target_.mode         = CaptureTarget::ParseMode(req.capture_target_mode);
    target_.window.match = WideFromUtf8(req.capture_target_window);
    capture_resolution_  = req.capture_resolution;

    bool built = false;
    if (armed_.load(std::memory_order_acquire)) {
        try {
            built = BuildGeneration(/*fatal_open=*/false);
        } catch (winrt::hresult_error const& e) {
            Log::Warn("pipeline reconfigure build threw 0x%08lX %ls",
                       static_cast<unsigned long>(static_cast<HRESULT>(e.code())),
                       e.message().c_str());
        }
    } else {
        built = true;
    }

    rebuild_count_.fetch_add(1, std::memory_order_relaxed);
    rebuilding_.store(false, std::memory_order_release);
    return built;
}

}  // namespace ombra
