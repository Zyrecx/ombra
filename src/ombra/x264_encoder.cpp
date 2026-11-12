#include "ombra/x264_encoder.h"

#include "ombra/die.h"
#include "ombra/log.h"
#include "ombra/shaders/nv12_Chroma.h"
#include "ombra/shaders/nv12_Luma.h"
#include "ombra/shaders/scale_vs.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include "x264.h"
}

namespace ombra {

namespace {

// x264 logs warnings and errors only (i_log_level below); everything else is noise. Runs inside
// x264_encoder_open on the watchdog thread, never per frame at this level.
void X264Log(void*, int level, const char* fmt, va_list args) {
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (level <= X264_LOG_ERROR) Log::Warn("x264  %s", buf);
    else Log::Debug("x264  %s", buf);
}

}  // namespace

const char* X264PresetFor(const std::string& preset) {
    if (preset == "p1") return "ultrafast";
    if (preset == "p2" || preset == "p3") return "superfast";
    if (preset == "p6") return "faster";
    if (preset == "p7") return "fast";
    return "veryfast";  // p4 (the default), p5, anything unknown
}

bool X264Encoder::OpenFailed(const std::string& what, HRESULT hr) {
    std::string reason = "x264: " + what;
    if (FAILED(hr)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (0x%08lX)", static_cast<unsigned long>(hr));
        reason += buf;
    }
    if (!nonfatal_open_) die(reason.c_str());
    faulted_      = true;
    fault_reason_ = reason;
    fault_code_   = reason.substr(6);  // without "x264: "
    Log::Warn("encoder open failed  %s", reason.c_str());
    return false;
}

void X264Encoder::ReportFault(const std::string& what) {
    // First fault wins: the reason is read once the pacer has stopped (encoder.h).
    if (faulted_.exchange(true, std::memory_order_acq_rel)) return;
    fault_reason_ = "x264: " + what;
    fault_code_   = what;
}

bool X264Encoder::TryOpen(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps,
                          uint32_t bitrate_kbps, uint32_t keyframe_sec, const EncoderOptions& options) {
    nonfatal_open_ = true;
    return Open(device, w, h, fps, bitrate_kbps, keyframe_sec, options);
}

bool X264Encoder::Open(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps,
                       uint32_t bitrate_kbps, uint32_t keyframe_sec, const EncoderOptions& options) {
    // ResolveOutputSize rounds the encode size down to even; 4:2:0 can't code (or crop) odd sizes.
    if ((w & 1) || (h & 1) || w == 0 || h == 0) return OpenFailed("odd or empty size");
    width_  = w;
    height_ = h;

    // --- GPU side: NV12 conversion targets and their staging copies ---------------------------
    device_.copy_from(device);
    device->GetImmediateContext(context_.put());
    multithread_ = context_.try_as<ID3D11Multithread>();

    HRESULT hr = device->CreateVertexShader(kScaleVS, sizeof(kScaleVS), nullptr, vs_.put());
    if (FAILED(hr)) return OpenFailed("CreateVertexShader", hr);
    hr = device->CreatePixelShader(kNv12LumaPS, sizeof(kNv12LumaPS), nullptr, ps_luma_.put());
    if (FAILED(hr)) return OpenFailed("CreatePixelShader(luma)", hr);
    hr = device->CreatePixelShader(kNv12ChromaPS, sizeof(kNv12ChromaPS), nullptr, ps_chroma_.put());
    if (FAILED(hr)) return OpenFailed("CreatePixelShader(chroma)", hr);

    const int32_t     params[4] = {static_cast<int32_t>(w) - 1, static_cast<int32_t>(h) - 1, 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(params);
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{params, 0, 0};
    hr = device->CreateBuffer(&bd, &init, params_.put());
    if (FAILED(hr)) return OpenFailed("CreateBuffer", hr);

    auto make_plane = [&](DXGI_FORMAT fmt, uint32_t pw, uint32_t ph, winrt::com_ptr<ID3D11Texture2D>& target,
                          winrt::com_ptr<ID3D11RenderTargetView>& rtv,
                          winrt::com_ptr<ID3D11Texture2D>& stage) -> HRESULT {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = pw;
        td.Height           = ph;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = fmt;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET;
        HRESULT r = device->CreateTexture2D(&td, nullptr, target.put());
        if (SUCCEEDED(r)) r = device->CreateRenderTargetView(target.get(), nullptr, rtv.put());
        if (FAILED(r)) return r;
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return device->CreateTexture2D(&td, nullptr, stage.put());
    };
    hr = make_plane(DXGI_FORMAT_R8_UNORM, w, h, luma_, luma_rtv_, luma_stage_);
    if (FAILED(hr)) return OpenFailed("luma plane textures", hr);
    hr = make_plane(DXGI_FORMAT_R8G8_UNORM, w / 2, h / 2, chroma_, chroma_rtv_, chroma_stage_);
    if (FAILED(hr)) return OpenFailed("chroma plane textures", hr);

    // --- x264 ----------------------------------------------------------------------------------
    x264_param_t p;
    if (x264_param_default_preset(&p, X264PresetFor(options.preset), "zerolatency") < 0)
        return OpenFailed("x264_param_default_preset");
    p.pf_log      = X264Log;
    p.i_log_level = X264_LOG_WARNING;
    p.i_csp       = X264_CSP_NV12;
    p.i_width     = static_cast<int>(w);
    p.i_height    = static_cast<int>(h);
    p.i_fps_num   = fps;
    p.i_fps_den   = 1;
    p.i_timebase_num = 1;
    p.i_timebase_den = fps;
    // CFR: the pacer duplicates frames, and rate control should budget per frame, not per timestamp.
    p.b_vfr_input = 0;

    // A fixed IDR cadence like NVENC/AMF (gopLength = fps * keyframe_sec): the ring evicts and a save
    // starts on these, so scenecut-inserted IDRs would only cost bits.
    p.i_keyint_max         = static_cast<int>(fps * keyframe_sec);
    p.i_keyint_min         = p.i_keyint_max;
    p.i_scenecut_threshold = 0;
    p.b_open_gop           = 0;
    p.b_annexb             = 1;
    p.b_repeat_headers     = 1;  // SPS/PPS before every IDR, as NVENC's repeatSPSPPS

    // One frame in, one frame out (the pacer pairs each AU with its tick). zerolatency already sets
    // these; restated because EncodeSlot faults if a frame is ever held back.
    p.i_bframe            = 0;
    p.rc.i_lookahead      = 0;
    p.i_sync_lookahead    = 0;
    p.b_sliced_threads    = 1;

    // BT.709 limited, written explicitly (shaders/nv12.hlsl converts with these coefficients).
    p.vui.b_fullrange = 0;
    p.vui.i_colorprim = 1;
    p.vui.i_transfer  = 1;
    p.vui.i_colmatrix = 1;

    if (options.rate_control == "cbr") {
        p.rc.i_rc_method       = X264_RC_ABR;
        p.rc.i_bitrate         = static_cast<int>(bitrate_kbps);
        p.rc.i_vbv_max_bitrate = static_cast<int>(bitrate_kbps);
        p.rc.i_vbv_buffer_size = static_cast<int>(bitrate_kbps);
        p.i_nal_hrd            = X264_NAL_HRD_CBR;
        p.rc.b_filler          = 1;
    } else if (options.rate_control == "cqp") {
        // "Constant quality": x264's CRF at the same number (QP scale 0-51), its constant-quality
        // mode. A literal constant QP is worse at every size.
        p.rc.i_rc_method   = X264_RC_CRF;
        p.rc.f_rf_constant = static_cast<float>(options.cq);
    } else {
        p.rc.i_rc_method       = X264_RC_ABR;
        p.rc.i_bitrate         = static_cast<int>(bitrate_kbps);
        p.rc.i_vbv_max_bitrate = static_cast<int>(options.max_bitrate_kbps > 0 ? options.max_bitrate_kbps
                                                                                : bitrate_kbps * 5 / 4);
        p.rc.i_vbv_buffer_size = p.rc.i_vbv_max_bitrate;
    }
    if (x264_param_apply_profile(&p, "high") < 0) return OpenFailed("x264_param_apply_profile(high)");

    enc_ = x264_encoder_open(&p);
    if (!enc_) return OpenFailed("x264_encoder_open");

    x264_nal_t* nals  = nullptr;
    int         n_nal = 0;
    if (x264_encoder_headers(enc_, &nals, &n_nal) < 0) return OpenFailed("x264_encoder_headers");
    sequence_params_.clear();
    for (int i = 0; i < n_nal; ++i) {
        // SPS and PPS only: the third header NAL is x264's version SEI, which avcC can't carry.
        if (nals[i].i_type != NAL_SPS && nals[i].i_type != NAL_PPS) continue;
        sequence_params_.insert(sequence_params_.end(), nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    }
    x264_param_t actual;
    x264_encoder_parameters(enc_, &actual);  // i_threads resolved from "auto"
    Log::Info("x264 open  %ux%u @%u  preset %s  rc %s  threads %d", w, h, fps, X264PresetFor(options.preset),
              options.rate_control.c_str(), actual.i_threads);
    return true;
}

bool X264Encoder::RegisterSlot(ID3D11Texture2D* tex, uint32_t, uint32_t) {
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device_->CreateShaderResourceView(tex, nullptr, srv.put());
    if (FAILED(hr)) return OpenFailed("CreateShaderResourceView(slot)", hr);
    srvs_.push_back(std::move(srv));
    return true;
}

X264Encoder::AccessUnit X264Encoder::EncodeSlot(uint32_t slot, uint32_t, uint32_t, uint64_t timestamp) {
    if (slot >= srvs_.size()) {
        ReportFault("EncodeSlot on an unregistered slot");
        return AccessUnit{.ok = false};
    }
    ID3D11DeviceContext* ctx = context_.get();

    // Convert and copy under the device lock (the capture thread shares the immediate context); the
    // Maps wait for the GPU. The lock is released before x264 runs, so capture isn't held off for
    // the encode.
    if (multithread_) multithread_->Enter();
    ID3D11ShaderResourceView* srv    = srvs_[slot].get();
    ID3D11Buffer*             params = params_.get();
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &params);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);

    const D3D11_VIEWPORT luma_vp{0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_), 0.f, 1.f};
    ID3D11RenderTargetView* rtv = luma_rtv_.get();
    ctx->RSSetViewports(1, &luma_vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShader(ps_luma_.get(), nullptr, 0);
    ctx->Draw(3, 0);

    const D3D11_VIEWPORT chroma_vp{0.f, 0.f, static_cast<float>(width_ / 2), static_cast<float>(height_ / 2), 0.f, 1.f};
    rtv = chroma_rtv_.get();
    ctx->RSSetViewports(1, &chroma_vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShader(ps_chroma_.get(), nullptr, 0);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* no_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->CopyResource(luma_stage_.get(), luma_.get());
    ctx->CopyResource(chroma_stage_.get(), chroma_.get());

    D3D11_MAPPED_SUBRESOURCE y{}, uv{};
    HRESULT hr = ctx->Map(luma_stage_.get(), 0, D3D11_MAP_READ, 0, &y);
    if (SUCCEEDED(hr)) {
        hr = ctx->Map(chroma_stage_.get(), 0, D3D11_MAP_READ, 0, &uv);
        if (FAILED(hr)) ctx->Unmap(luma_stage_.get(), 0);
    }
    if (multithread_) multithread_->Leave();
    if (FAILED(hr)) {
        ReportFault("Map(staging) failed");
        return AccessUnit{.ok = false};
    }

    x264_picture_t in;
    x264_picture_init(&in);
    in.img.i_csp       = X264_CSP_NV12;
    in.img.i_plane     = 2;
    in.img.plane[0]    = static_cast<uint8_t*>(y.pData);
    in.img.i_stride[0] = static_cast<int>(y.RowPitch);
    in.img.plane[1]    = static_cast<uint8_t*>(uv.pData);
    in.img.i_stride[1] = static_cast<int>(uv.RowPitch);
    in.i_pts           = static_cast<int64_t>(timestamp);

    x264_picture_t out;
    x264_nal_t*    nals  = nullptr;
    int            n_nal = 0;
    // x264 copies the picture into its own frame before returning, so the planes can be unmapped
    // right after.
    const int bytes = x264_encoder_encode(enc_, &nals, &n_nal, &in, &out);

    if (multithread_) multithread_->Enter();
    ctx->Unmap(chroma_stage_.get(), 0);
    ctx->Unmap(luma_stage_.get(), 0);
    if (multithread_) multithread_->Leave();

    if (bytes < 0) {
        ReportFault("x264_encoder_encode failed");
        return AccessUnit{.ok = false};
    }
    if (bytes == 0 || x264_encoder_delayed_frames(enc_) != 0) {
        ReportFault("x264 held a frame back -- encoder buffering unexpectedly");
        return AccessUnit{.ok = false};
    }
    // x264 guarantees one frame's NAL payloads are contiguous in memory.
    AccessUnit au;
    au.data        = nals[0].p_payload;
    au.size        = static_cast<size_t>(bytes);
    au.is_keyframe = out.b_keyframe != 0;
    return au;
}

void X264Encoder::Close() {
    if (enc_) {
        x264_encoder_close(enc_);
        enc_ = nullptr;
    }
    srvs_.clear();
}

}  // namespace ombra
