// AMD AMF implementation of IVideoEncoder (AV1 or H.264). amfrt64.dll ships with the AMD driver and is loaded
// at runtime; only the vendored MIT headers in third_party/AMF are needed to build. Modelled on
// OBS's plugins/obs-ffmpeg/texture-amf.cpp (tag 30.2.3), trimmed to what the pacer's synchronous,
// one-frame-in-one-frame-out model needs.
#pragma once

#include "ombra/encoder.h"

#pragma warning(push, 0)
#include "AMF/components/VideoEncoderAV1.h"
#include "AMF/components/VideoEncoderVCE.h"
#include "AMF/core/Factory.h"
#pragma warning(pop)

#include <atomic>
#include <string>
#include <vector>

namespace ombra {

// The process's AMF runtime: amfrt64.dll, its factory and trace, loaded once and never unloaded
// (OBS does the same; unloading a driver runtime that may still own threads is not worth the risk).
// Only call after the out-of-process probe said AMF is usable (AmfUsable(), encode_target.h): AMF
// init can hang or crash on a bad driver, and in-process that would take the engine with it.
struct AmfRuntime {
    amf::AMFFactory* factory = nullptr;
    amf::AMFTrace*   trace   = nullptr;
    uint64_t         version = 0;  // AMFQueryVersion, AMF_FULL_VERSION layout
};
// nullptr with `error` set when the runtime is missing or refuses the header version.
const AmfRuntime* LoadAmfRuntime(std::string* error);
std::string AmfVersionString(uint64_t version);  // "1.4.36"

// Pure helpers, exposed for headless tests.
// Lowest H.264 level (AMF_H264_LEVEL__*) whose frame size and macroblock rate fit the stream.
int AmfH264Level(uint32_t width, uint32_t height, uint32_t fps);
// encode.preset (NVENC-style p1..p7) to AMF's H.264 quality preset.
int AmfQualityPresetFor(const std::string& preset);
// The same mapping onto AMF's AV1 quality presets.
int AmfAv1QualityPresetFor(const std::string& preset);
bool IsAnnexB(const std::vector<uint8_t>& bytes);
// The AV1 encoder's size alignment caps on `context`'s device (1x1 when it reports none). False when
// the device has no AV1 encoder. Read without Init, so it's cheap; the probe child calls it.
bool AmfAv1Alignment(const AmfRuntime& rt, amf::AMFContext* context, uint32_t& align_w, uint32_t& align_h);
// HeaderInsertionSpacing for a GOP of fps * keyframe_sec: the GOP, or one second of frames when the
// GOP exceeds AMF's 1000-frame cap (still a divisor of the GOP, so every IDR carries SPS/PPS).
int64_t AmfHeaderSpacing(uint32_t fps, uint32_t keyframe_sec);

class AmfEncoder : public IVideoEncoder {
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
    uint64_t LateFrames() const override { return late_frames_.load(std::memory_order_relaxed); }
    int64_t  LongestWaitMs() const override { return longest_wait_ms_.load(std::memory_order_relaxed); }

    ~AmfEncoder() override { Close(); }

private:
    bool OpenFailed(const std::string& what, AMF_RESULT res);  // die()s unless opened via TryOpen
    void ReportFault(const char* what, AMF_RESULT res);
    std::string ResultText(AMF_RESULT res) const;

    bool                          nonfatal_open_ = false;
    const AmfRuntime*             runtime_ = nullptr;
    amf::AMFContextPtr            context_;
    amf::AMFComponentPtr          encoder_;
    std::vector<ID3D11Texture2D*> slots_;  // not owned: the capture source's stage textures
    int64_t                       pts_per_frame_ = 0;  // AMF's 100 ns units
    amf::AMFBufferPtr             last_output_;        // the AU handed out until UnlockLast()
    VideoCodec                    codec_ = VideoCodec::H264;
    std::vector<uint8_t>          sequence_params_;

    std::atomic<bool>     faulted_{false};
    std::atomic<uint64_t> late_frames_{0};      // AUs that came back after the first QueryOutput timed out
    std::atomic<int64_t>  longest_wait_ms_{0};  // the longest of those waits
    std::string       fault_reason_;
    std::string       fault_code_;
};

}  // namespace ombra
