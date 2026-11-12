#include "ombra/frame_scaler.h"

#include "ombra/shaders/scale_ps.h"
#include "ombra/shaders/scale_vs.h"

#include <cstdio>

namespace ombra {

namespace {
bool Failed(HRESULT hr, const char* what, std::string& error) {
    if (SUCCEEDED(hr)) return false;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "FrameScaler: %s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    error = buf;
    return true;
}
}  // namespace

bool FrameScaler::Open(ID3D11Device* device, const std::vector<ID3D11Texture2D*>& sources,
                       uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                       std::string& error) {
    device->GetImmediateContext(context_.put());
    multithread_ = context_.try_as<ID3D11Multithread>();

    if (Failed(device->CreateVertexShader(kScaleVS, sizeof(kScaleVS), nullptr, vs_.put()), "CreateVertexShader", error) ||
        Failed(device->CreatePixelShader(kScalePS, sizeof(kScalePS), nullptr, ps_.put()), "CreatePixelShader", error)) {
        return false;
    }

    const float params[4] = {static_cast<float>(src_w), static_cast<float>(src_h),
                             static_cast<float>(src_w) / static_cast<float>(dst_w),
                             static_cast<float>(src_h) / static_cast<float>(dst_h)};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(params);
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{params, 0, 0};
    if (Failed(device->CreateBuffer(&bd, &init, params_.put()), "CreateBuffer", error)) return false;

    srvs_.clear();
    for (ID3D11Texture2D* tex : sources) {
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        if (Failed(device->CreateShaderResourceView(tex, nullptr, srv.put()), "CreateShaderResourceView", error))
            return false;
        srvs_.push_back(std::move(srv));
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = dst_w;
    td.Height           = dst_h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // as the capture slots
    if (Failed(device->CreateTexture2D(&td, nullptr, output_.put()), "CreateTexture2D", error) ||
        Failed(device->CreateRenderTargetView(output_.get(), nullptr, rtv_.put()), "CreateRenderTargetView", error)) {
        return false;
    }

    viewport_ = D3D11_VIEWPORT{0.0f, 0.0f, static_cast<float>(dst_w), static_cast<float>(dst_h), 0.0f, 1.0f};
    dst_w_    = dst_w;
    dst_h_    = dst_h;
    return true;
}

void FrameScaler::Scale(uint32_t slot) {
    ID3D11DeviceContext* ctx = context_.get();
    if (multithread_) multithread_->Enter();
    ID3D11RenderTargetView*   rtv    = rtv_.get();
    ID3D11ShaderResourceView* srv    = srvs_[slot].get();
    ID3D11Buffer*             params = params_.get();
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &params);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &viewport_);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);
    // Unbound so neither the capture's next copy into this slot nor the encoder's read of the
    // output finds it still bound to the pipeline.
    ID3D11ShaderResourceView* no_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (multithread_) multithread_->Leave();
}

}  // namespace ombra
