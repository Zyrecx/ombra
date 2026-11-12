// The CPU encoder: libx264 (third_party/x264, GPL-2.0+), H.264 only. Used when the user picks
// "CPU" as the recording GPU, or when no hardware encoder opens (encode_target.h,
// PipelineSupervisor). It costs far more than NVENC/AMF: ~1-1.6 cores of a Ryzen 7 250 at 1080p
// veryfast on a synthetic pattern, more on real footage and on a weaker CPU.
//
// Per frame: two shader passes convert the BGRA slot to NV12 on the capture device (shaders/nv12.hlsl,
// BT.709 limited), the planes are copied to staging textures and mapped (~3 MB at 1080p instead of
// 8 MB of BGRA), and x264 encodes from the mapped memory. Synchronous like the hardware encoders:
// tune=zerolatency means no lookahead, no B-frames and sliced threads, so every frame in returns its
// own AU (the pacer's one-in-one-out pairing).
#pragma once

#include "ombra/encoder.h"

#include <d3d11_4.h>

#include <winrt/base.h>

#include <atomic>
#include <string>
#include <vector>

typedef struct x264_t x264_t;

namespace ombra {

// encode.preset (p1 fastest .. p7 slowest) onto x264's presets. p4, the default, is veryfast, OBS's
// x264 default; x264's own default (medium) costs several times as much CPU.
const char* X264PresetFor(const std::string& preset);

class X264Encoder : public IVideoEncoder {
public:
    bool Open(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
              uint32_t bitrate_kbps, uint32_t keyframe_sec,
              const EncoderOptions& options = {}) override;
    bool TryOpen(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps,
                 uint32_t bitrate_kbps, uint32_t keyframe_sec,
                 const EncoderOptions& options = {}) override;
    // Always H.264: an AV1 request records H.264, the same fallback as a GPU without AV1.
    VideoCodec Codec() const override { return VideoCodec::H264; }
    const std::vector<uint8_t>& SequenceParams() const override { return sequence_params_; }
    bool RegisterSlot(ID3D11Texture2D* tex, uint32_t width, uint32_t height) override;
    AccessUnit EncodeSlot(uint32_t slot, uint32_t width, uint32_t height,
                          uint64_t timestamp) override;
    // No-op: the AU points into x264's own output buffer, valid until the next EncodeSlot.
    void UnlockLast() override {}
    bool               Faulted() const override { return faulted_.load(std::memory_order_acquire); }
    const std::string& FaultReason() const override { return fault_reason_; }
    std::string        FaultCode() const override { return fault_code_; }
    void Close() override;

    ~X264Encoder() override { Close(); }

private:
    bool OpenFailed(const std::string& what, HRESULT hr = S_OK);  // die()s unless opened via TryOpen
    void ReportFault(const std::string& what);

    bool                 nonfatal_open_ = false;
    x264_t*              enc_ = nullptr;
    uint32_t             width_ = 0, height_ = 0;  // even (ResolveOutputSize)
    std::vector<uint8_t> sequence_params_;

    winrt::com_ptr<ID3D11Device>          device_;
    winrt::com_ptr<ID3D11DeviceContext>   context_;
    winrt::com_ptr<ID3D11Multithread>     multithread_;
    winrt::com_ptr<ID3D11VertexShader>    vs_;
    winrt::com_ptr<ID3D11PixelShader>     ps_luma_, ps_chroma_;
    winrt::com_ptr<ID3D11Buffer>          params_;
    winrt::com_ptr<ID3D11Texture2D>       luma_, chroma_;             // render targets
    winrt::com_ptr<ID3D11RenderTargetView> luma_rtv_, chroma_rtv_;
    winrt::com_ptr<ID3D11Texture2D>       luma_stage_, chroma_stage_;  // CPU-readable copies
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> srvs_;     // one per registered slot

    std::atomic<bool> faulted_{false};
    std::string       fault_reason_;
    std::string       fault_code_;
};

}  // namespace ombra
