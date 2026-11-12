// The capture.resolution downscale: a GPU-local pass between a capture stage slot and the encoder's
// input, on the encode device. Owned by a pipeline generation next to its capture source and
// encoder, and only built when the encoded size differs from the captured one; at native the
// encoder reads the capture slots directly.
//
// It is not an encoder setting: NVENC's built-in rescale is gone from OBS's "NVENC (new)", and a
// shader pass serves NVENC and AMF alike. The area filter is in shaders/scale.hlsl.
#pragma once

#include <windows.h>
#include <d3d11_4.h>  // ID3D11Multithread

#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ombra {

class FrameScaler {
public:
    // `sources` are the capture source's stage slots, all src_w x src_h BGRA with
    // D3D11_BIND_SHADER_RESOURCE; the output is one dst_w x dst_h BGRA texture (one is enough: the
    // encode is synchronous, so it has been read before the next tick draws into it). Rebuilt
    // whenever the slots are (a capture Recreate). False with `error` set on any D3D failure.
    bool Open(ID3D11Device* device, const std::vector<ID3D11Texture2D*>& sources, uint32_t src_w,
              uint32_t src_h, uint32_t dst_w, uint32_t dst_h, std::string& error);

    // Draws source `slot` into Output(). Runs on the pacer's per-tick path: D3D calls only, no
    // logging, no allocation. Holds the device's multithread lock for the whole state-set-and-draw
    // sequence, since the capture thread uses the same immediate context.
    void Scale(uint32_t slot);

    ID3D11Texture2D* Output() const { return output_.get(); }
    uint32_t         Width() const  { return dst_w_; }
    uint32_t         Height() const { return dst_h_; }

private:
    winrt::com_ptr<ID3D11DeviceContext>                    context_;
    winrt::com_ptr<ID3D11Multithread>                      multithread_;
    winrt::com_ptr<ID3D11VertexShader>                     vs_;
    winrt::com_ptr<ID3D11PixelShader>                      ps_;
    winrt::com_ptr<ID3D11Buffer>                           params_;
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> srvs_;
    winrt::com_ptr<ID3D11Texture2D>                        output_;
    winrt::com_ptr<ID3D11RenderTargetView>                 rtv_;
    D3D11_VIEWPORT                                         viewport_{};
    uint32_t                                               dst_w_ = 0;
    uint32_t                                               dst_h_ = 0;
};

}  // namespace ombra
