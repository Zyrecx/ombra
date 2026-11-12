// NVENC AV1/H.264 encoder. Loaded at runtime via LoadLibrary — no import lib,
// no SDK install, only the vendored third_party/ffnvcodec/nvEncodeAPI.h.
//
// Synchronous mode, one registered D3D11 input texture, one bitstream buffer, since the driver
// caps the NVENC API version.
#pragma once

#include <windows.h>
#include <d3d11.h>

#pragma warning(push, 0)
#include "ffnvcodec/nvEncodeAPI.h"
#pragma warning(pop)

#include "ombra/encode_target.h"
#include "ombra/video_codec.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ombra {

struct EncoderOptions {
    std::string preset = "p4";
    std::string rate_control = "vbr";
    uint32_t    cq = 20;
    uint32_t    max_bitrate_kbps = 0;
    // encode.adapter: "auto" | "nvidia" | "amd". Resolved to an adapter and backend on every
    // pipeline build (encode_target.h), not by the encoder itself.
    std::string adapter = "auto";
    // encode.codec. AV1 falls back to H.264 inside Open() when the GPU can't encode it; Codec()
    // says what was opened.
    VideoCodec  codec = VideoCodec::H264;
    // encode.amf_av1_unaligned. Not read by the encoders: the supervisor's PlanEncode turns it into
    // the codec and size it opens them with.
    std::string av1_unaligned = "h264";
    // The codec the bitrate was derived for while a quality preset owns it; unset for Custom. Not read
    // by the encoders: the supervisor rescales when it opens another one (BitrateForOpenedCodec).
    std::optional<VideoCodec> preset_bitrate_codec;
};

// Test seam: makes every encoder treat AV1 as unsupported, so the H.264 fallback can be exercised
// on a GPU that has AV1.
void SetAv1UnsupportedForTest(bool unsupported);
bool Av1UnsupportedForTest();

// Vendor-neutral seam: PipelineSupervisor talks to this
// interface only, never to a concrete vendor encoder. Vendor-specific values (NVENC's p1-p7
// presets, AMF's usage/quality enums) stay as strings resolved *inside* an implementation, never
// leaked here or into config plumbing as vendor concepts — EncoderOptions above is that string
// carrier and does not change shape for this seam.
class IVideoEncoder {
public:
    // NV_ENC_BUFFER_FORMAT_ARGB pairs with DXGI_FORMAT_B8G8R8A8_UNORM (NVENC's names are
    // word-ordered, DXGI's byte-ordered).
    // Returns true on success. Failures are fatal (die()/DieNv, the "fatal at startup"
    // bucket) unless TryOpen() was used — see below.
    virtual bool Open(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
                       uint32_t bitrate_kbps, uint32_t keyframe_sec,
                       const EncoderOptions& options = {}) = 0;

    // Non-fatal Open, for the two paths that reopen an encoder while the engine is already running:
    // PipelineSupervisor's fault rebuild and its resolution-change reopen.
    // Returns false with Faulted()/FaultReason() set instead of exiting the process.
    //
    // Why this exists: before resize handling, a failing Open() could only happen at startup, where dying
    // loudly is correct. Resize handling puts Open() on a path users hit routinely — every resolution
    // change, every alt-tab into a game that changes mode — and "the new size exceeds what this
    // NVENC session will initialize" must not take down an always-resident recorder. Startup keeps
    // the fatal behaviour: a machine with no usable NVENC should still refuse to start.
    virtual bool TryOpen(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
                          uint32_t bitrate_kbps, uint32_t keyframe_sec,
                          const EncoderOptions& options = {}) = 0;

    // The codec Open() actually opened: options.codec, or H.264 when that was AV1 and the GPU
    // can't encode it.
    virtual VideoCodec Codec() const = 0;

    // The codec header pulled once at Open(): H.264 SPS/PPS in Annex-B (start-code-prefixed)
    // form, or the AV1 sequence header OBU. Every key frame also carries it in-band, but the
    // muxer's avcC/av1C needs it regardless of what the ring currently holds (the key frame that
    // started the buffer is the first thing GOP eviction drops once the ring wraps), so pulling it
    // once here avoids depending on the ring still having it at mux time.
    virtual const std::vector<uint8_t>& SequenceParams() const = 0;

    // Registers one texture as an NVENC input resource; call once per Capture stage slot, in slot
    // order, before EncodeSlot(). Slot index == registration order.
    // Returns false (rather than dying) when this Encoder was opened via TryOpen — registration
    // failures on a rebuild belong in the same recoverable bucket as the reopen itself.
    virtual bool RegisterSlot(ID3D11Texture2D* tex, uint32_t width, uint32_t height) = 0;

    // Encodes one frame from the texture registered at `slot`. Returns the access unit's byte
    // span; valid only until UnlockLast() (owned by NVENC's locked bitstream buffer).
    //
    // `ok` is false when an NVENC call inside this function failed — the "NVENC
    // session lost" bucket, recoverable at runtime, not `die()`. This function
    // used to die() here; it now reports the failure so PipelineSupervisor can tear down and
    // rebuild instead of taking the whole process with it. A false `ok` carries no valid `data`;
    // the caller must not touch it. Open()'s own failures stay fatal (the "fatal at
    // startup" bucket) — this change is scoped to the runtime-reachable per-tick path only.
    struct AccessUnit {
        const uint8_t* data = nullptr;
        size_t         size = 0;
        bool           is_keyframe = false;
        bool           ok          = true;
    };
    virtual AccessUnit EncodeSlot(uint32_t slot, uint32_t width, uint32_t height,
                                   uint64_t timestamp) = 0;
    // Must be called after the caller is done reading the AccessUnit's data. A no-op if the AU
    // that produced the current lock already failed (`ok == false`) — nothing to unlock.
    virtual void UnlockLast() = 0;

    // Set the moment any NVENC call inside EncodeSlot/UnlockLast fails; never cleared, since a
    // faulted Encoder is rebuilt from scratch (a fresh object), not reused — see
    // PipelineSupervisor. `Faulted()` is atomic and safe to poll from any thread (PipelineSupervisor's
    // watchdog does, once per 200ms tick, concurrently with the pacer thread that might be setting
    // it). `FaultReason()` is NOT synchronized the same way — `fault_reason_` is a plain
    // std::string, and a concurrent read while EncodeSlot/UnlockLast is still writing it is a torn
    // read, not just a stale value. Only call it once the thread that could still be calling
    // EncodeSlot/UnlockLast (the pacer thread) is known-joined — PipelineSupervisor reads it from
    // inside TeardownGeneration(), after Pacer::Stop() has returned, never from the watchdog's
    // regular polling loop.
    virtual bool               Faulted() const = 0;
    virtual const std::string& FaultReason() const = 0;
    // The same fault as a stable token for error reports (the failing call and its status, no
    // sizes or driver text), under FaultReason()'s threading rule. Empty when not known.
    virtual std::string FaultCode() const { return {}; }
    // AUs that arrived only after the encoder's normal wait ran out, and the longest such wait.
    // Plain atomics: any thread may read them while the pacer runs.
    virtual uint64_t LateFrames() const { return 0; }
    virtual int64_t  LongestWaitMs() const { return 0; }

    // Idempotent (Close() no-ops once already closed), so calling it explicitly stays fine.
    virtual void Close() = 0;

    // Exists because of TryOpen: a failed Open() previously called exit(1), so a
    // half-opened Encoder could not outlive the process. Now three paths drop a
    // std::unique_ptr<IVideoEncoder> whose Open() got as far as creating the vendor session before
    // failing, and the supervisor's backoff loop retries forever — one leaked session per attempt
    // would eventually hit the vendor's per-process session limit and turn a recoverable fault into
    // a permanent one, defeating the exact property TryOpen was added to provide (advisor review).
    // A virtual destructor is required here precisely because callers hold this as
    // std::unique_ptr<IVideoEncoder>: a non-virtual base destructor would only run ~NvencEncoder's
    // *base-class* slice and leak the derived class's NVENC session on every teardown, silently
    // reintroducing the session-exhaustion bug this comment describes.
    virtual ~IVideoEncoder() = default;
};

// NVENC implementation of IVideoEncoder (AV1 or H.264). Loaded at runtime via LoadLibrary — no import lib,
// no SDK install, only the vendored third_party/ffnvcodec/nvEncodeAPI.h.
//
// Synchronous mode, one registered D3D11 input texture, one bitstream buffer, since the driver
// caps the NVENC API version.
class NvencEncoder : public IVideoEncoder {
public:
    bool Open(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
              uint32_t bitrate_kbps, uint32_t keyframe_sec,
              const EncoderOptions& options = {}) override;
    bool TryOpen(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
                 uint32_t bitrate_kbps, uint32_t keyframe_sec,
                 const EncoderOptions& options = {}) override;
    VideoCodec Codec() const override { return codec_; }
    const std::vector<uint8_t>& SequenceParams() const override { return sequence_params_; }
    bool RegisterSlot(ID3D11Texture2D* tex, uint32_t width, uint32_t height) override;
    AccessUnit EncodeSlot(uint32_t slot, uint32_t width, uint32_t height,
                           uint64_t timestamp) override;
    void UnlockLast() override;
    bool               Faulted() const override { return faulted_.load(std::memory_order_acquire); }
    const std::string& FaultReason() const override { return fault_reason_; }
    std::string        FaultCode() const override { return fault_code_; }
    void Close() override;

    ~NvencEncoder() override { Close(); }

private:
    bool SupportsCodec(const GUID& codec_guid);
    [[noreturn]] void DieNv(const char* what, NVENCSTATUS s);
    // Non-fatal counterpart to DieNv, for the runtime-reachable calls in EncodeSlot/UnlockLast.
    void ReportFault(const char* what, NVENCSTATUS s);

    bool                                 nonfatal_open_ = false;  // set by TryOpen
    HMODULE                              dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST          fn_{};
    void*                                enc_ = nullptr;
    std::vector<NV_ENC_REGISTERED_PTR>   reg_;
    NV_ENC_OUTPUT_PTR                    bs_  = nullptr;
    bool                                 locked_ = false;
    VideoCodec                           codec_ = VideoCodec::H264;
    std::vector<uint8_t>                 sequence_params_;

    std::atomic<bool> faulted_{false};
    std::string       fault_reason_;
    std::string       fault_code_;
};

// Factory: the one place a backend becomes a concrete encoder. Which backend (and which adapter)
// is decided by ResolveEncodeAdapter (encode_target.h); the device passed to Open() must be on that
// adapter.
std::unique_ptr<IVideoEncoder> CreateVideoEncoder(EncoderBackend backend);

}  // namespace ombra
