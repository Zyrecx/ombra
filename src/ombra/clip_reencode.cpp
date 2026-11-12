#include "ombra/clip_reencode.h"

#include "ombra/av1.h"
#include "ombra/config.h"
#include "ombra/encode_target.h"
#include "ombra/log.h"
#include "ombra/mux.h"

#include <windows.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ombra {
namespace {

using winrt::com_ptr;

std::string Hr(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

// NV12 decoder output -> the encoder's BGRA input, on the GPU: D3D11's video processor, BT.709
// studio range in, full-range RGB out (what capture hands the encoders; they convert back with
// BT.709 limited VUI). Rebuilt if the decoder's surface size changes.
class Nv12ToBgra {
public:
    bool Open(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* output, uint32_t out_w,
              uint32_t out_h, std::string& error) {
        device_ = device;
        output_ = output;
        out_w_ = out_w;
        out_h_ = out_h;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(vdev_.put()))) ||
            FAILED(context->QueryInterface(IID_PPV_ARGS(vctx_.put())))) {
            error = "this GPU's D3D11 device has no video processor";
            return false;
        }
        vctx_.try_as(vctx1_);
        return true;
    }

    // Draws `subresource` of `tex` (NV12, the decoder's surface) into the output, the `src_w` x
    // `src_h` top-left part of it scaled to the output size.
    bool Convert(ID3D11Texture2D* tex, UINT subresource, uint32_t src_w, uint32_t src_h, std::string& error) {
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        if (!vp_ || desc.Width != in_w_ || desc.Height != in_h_) {
            if (!Build(desc.Width, desc.Height, error)) return false;
        }
        const RECT src{0, 0, static_cast<LONG>((std::min)(src_w, in_w_)), static_cast<LONG>((std::min)(src_h, in_h_))};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.get(), 0, TRUE, &src);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = subresource;
        com_ptr<ID3D11VideoProcessorInputView> in;
        HRESULT hr = vdev_->CreateVideoProcessorInputView(tex, enum_.get(), &ivd, in.put());
        if (FAILED(hr)) { error = Hr("CreateVideoProcessorInputView", hr); return false; }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = in.get();
        hr = vctx_->VideoProcessorBlt(vp_.get(), out_view_.get(), 0, 1, &stream);
        if (FAILED(hr)) { error = Hr("VideoProcessorBlt", hr); return false; }
        return true;
    }

private:
    bool Build(uint32_t in_w, uint32_t in_h, std::string& error) {
        vp_ = nullptr;
        enum_ = nullptr;
        out_view_ = nullptr;
        in_w_ = in_w;
        in_h_ = in_h;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = in_w;
        cd.InputHeight = in_h;
        cd.OutputWidth = out_w_;
        cd.OutputHeight = out_h_;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = vdev_->CreateVideoProcessorEnumerator(&cd, enum_.put());
        if (FAILED(hr)) { error = Hr("CreateVideoProcessorEnumerator", hr); return false; }
        hr = vdev_->CreateVideoProcessor(enum_.get(), 0, vp_.put());
        if (FAILED(hr)) { error = Hr("CreateVideoProcessor", hr); return false; }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = vdev_->CreateVideoProcessorOutputView(output_, enum_.get(), &ovd, out_view_.put());
        if (FAILED(hr)) { error = Hr("CreateVideoProcessorOutputView", hr); return false; }

        ID3D11VideoProcessor* vp = vp_.get();
        vctx_->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx_->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
        const RECT dst{0, 0, static_cast<LONG>(out_w_), static_cast<LONG>(out_h_)};
        vctx_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dst);
        vctx_->VideoProcessorSetOutputTargetRect(vp, TRUE, &dst);
        if (vctx1_) {
            vctx1_->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            vctx1_->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
            in.YCbCr_Matrix = 1;  // BT.709
            in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
            out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            vctx_->VideoProcessorSetStreamColorSpace(vp, 0, &in);
            vctx_->VideoProcessorSetOutputColorSpace(vp, &out);
        }
        return true;
    }

    ID3D11Device*                            device_ = nullptr;
    ID3D11Texture2D*                         output_ = nullptr;
    uint32_t                                 out_w_ = 0, out_h_ = 0, in_w_ = 0, in_h_ = 0;
    com_ptr<ID3D11VideoDevice>               vdev_;
    com_ptr<ID3D11VideoContext>              vctx_;
    com_ptr<ID3D11VideoContext1>             vctx1_;
    com_ptr<ID3D11VideoProcessorEnumerator>  enum_;
    com_ptr<ID3D11VideoProcessor>            vp_;
    com_ptr<ID3D11VideoProcessorOutputView>  out_view_;
};

// Encoded frames go to a scratch file beside the output while the encode runs (a 2-minute clip is
// ~180 MB at 12 Mbps), and the mux reads them back in order.
class Spill {
public:
    explicit Spill(std::wstring path) : path_(std::move(path)) {
        h_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }
    ~Spill() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    Spill(const Spill&) = delete;
    Spill& operator=(const Spill&) = delete;

    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    bool Append(const uint8_t* data, size_t size) {
        DWORD wrote = 0;
        if (!WriteFile(h_, data, static_cast<DWORD>(size), &wrote, nullptr) || wrote != size) return false;
        spans_.push_back({end_, static_cast<uint32_t>(size)});
        end_ += size;
        return true;
    }
    bool Read(size_t i, std::vector<uint8_t>& out) const {
        if (i >= spans_.size()) return false;
        out.resize(spans_[i].size);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(spans_[i].offset);
        ov.OffsetHigh = static_cast<DWORD>(spans_[i].offset >> 32);
        DWORD got = 0;
        return ReadFile(h_, out.data(), spans_[i].size, &got, &ov) && got == spans_[i].size;
    }

private:
    struct Span { uint64_t offset; uint32_t size; };
    std::wstring      path_;
    HANDLE            h_ = INVALID_HANDLE_VALUE;
    uint64_t          end_ = 0;
    std::vector<Span> spans_;
};

// A decoder that hands back system-memory NV12 (software decode: no DXVA for the codec on this GPU,
// or the reader declined the device): each frame is copied into a staging texture and on to a
// GPU one the video processor reads. Rebuilt when the frame size changes. Call under the device's
// multithread lock.
class Nv12Upload {
public:
    // `frame_w` x `frame_h` is the output type's MF_MT_FRAME_SIZE (may be 1088 for 1080 lines),
    // which places the UV plane; `default_stride` its MF_MT_DEFAULT_STRIDE (0: unknown).
    bool Upload(ID3D11Device* device, ID3D11DeviceContext* context, IMFMediaBuffer* buffer, uint32_t frame_w,
                uint32_t frame_h, int32_t default_stride, std::string& error) {
        if (frame_w == 0 || frame_h == 0 || (frame_w | frame_h) & 1) {
            error = "the decoder's frame size is unusable";
            return false;
        }
        if (!gpu_ || w_ != frame_w || h_ != frame_h) {
            gpu_ = nullptr;
            staging_ = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = frame_w;
            td.Height = frame_h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            HRESULT hr = device->CreateTexture2D(&td, nullptr, staging_.put());
            if (FAILED(hr)) { error = Hr("CreateTexture2D(NV12 staging)", hr); return false; }
            td.Usage = D3D11_USAGE_DEFAULT;
            td.CPUAccessFlags = 0;
            // A video processor input view refused (E_INVALIDARG) an NV12 texture bound only as a
            // shader resource; as a render target it takes it.
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device->CreateTexture2D(&td, nullptr, gpu_.put());
            if (FAILED(hr)) { staging_ = nullptr; error = Hr("CreateTexture2D(NV12)", hr); return false; }
            w_ = frame_w;
            h_ = frame_h;
        }

        // The buffer's own pitch where it has one (a 2D buffer), else the type's stride.
        BYTE* src = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        com_ptr<IMF2DBuffer> two_d;
        buffer->QueryInterface(IID_PPV_ARGS(two_d.put()));
        HRESULT hr = two_d ? two_d->Lock2D(&src, &pitch) : E_NOINTERFACE;
        if (FAILED(hr)) {
            two_d = nullptr;
            DWORD max_length = 0;
            hr = buffer->Lock(&src, &max_length, &length);
            if (FAILED(hr)) { error = Hr("IMFMediaBuffer::Lock", hr); return false; }
            pitch = default_stride > 0 ? default_stride : static_cast<LONG>(frame_w);
            if (static_cast<uint64_t>(pitch) * frame_h * 3 / 2 > length) {
                buffer->Unlock();
                error = "the decoder's frame is smaller than its size says";
                return false;
            }
        }
        if (pitch < static_cast<LONG>(frame_w)) {  // bottom-up or malformed; NV12 never is
            if (two_d) two_d->Unlock2D(); else buffer->Unlock();
            error = "the decoder's frame layout is unsupported";
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        hr = context->Map(staging_.get(), 0, D3D11_MAP_WRITE, 0, &map);
        if (SUCCEEDED(hr)) {
            auto* dst = static_cast<BYTE*>(map.pData);
            // Y, then the interleaved UV plane: each starts after the full height of its own surface.
            for (uint32_t y = 0; y < frame_h; ++y)
                std::memcpy(dst + size_t(y) * map.RowPitch, src + size_t(y) * pitch, frame_w);
            BYTE* dst_uv = dst + size_t(map.RowPitch) * frame_h;
            const BYTE* src_uv = src + size_t(pitch) * frame_h;
            for (uint32_t y = 0; y < frame_h / 2; ++y)
                std::memcpy(dst_uv + size_t(y) * map.RowPitch, src_uv + size_t(y) * pitch, frame_w);
            context->Unmap(staging_.get(), 0);
        }
        if (two_d) two_d->Unlock2D(); else buffer->Unlock();
        if (FAILED(hr)) { error = Hr("Map(NV12 staging)", hr); return false; }
        context->CopyResource(gpu_.get(), staging_.get());
        return true;
    }

    ID3D11Texture2D* Texture() const { return gpu_.get(); }

private:
    com_ptr<ID3D11Texture2D> staging_, gpu_;
    uint32_t                 w_ = 0, h_ = 0;
};

// The reader's current output size and stride for `stream`, re-read when it says the type changed.
bool ReadOutputType(IMFSourceReader* reader, DWORD stream, uint32_t& w, uint32_t& h, int32_t& stride) {
    com_ptr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(stream, type.put()))) return false;
    UINT32 fw = 0, fh = 0;
    if (FAILED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &fw, &fh))) return false;
    w = fw;
    h = fh;
    stride = static_cast<int32_t>(MFGetAttributeUINT32(type.get(), MF_MT_DEFAULT_STRIDE, 0));
    return true;
}

struct MfSession {
    bool ok = false;
    MfSession() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfSession() { if (ok) MFShutdown(); }
};

}  // namespace

TrimResult ReencodeClip(const std::wstring& src, const std::wstring& dst, double start_s, double end_s,
                        const std::vector<bool>* keep_audio, const ReencodeOptions& options,
                        const ReencodeProgress& progress) {
    TrimResult r;
    ClipInfo info;
    if (!ReadClipInfo(src, info, &r.error)) return r;
    const uint32_t fps = static_cast<uint32_t>(std::lround(info.fps));
    if (fps == 0 || info.duration_s <= 0.0) { r.error = "the clip has no frame rate"; return r; }
    const double clip_end = info.video_offset_s + info.duration_s;
    end_s = (std::min)(end_s, clip_end);
    const double eps = 0.5 / fps;
    if (end_s <= start_s + eps) { r.error = "nothing to keep"; return r; }

    // The GPU that encodes decodes too, so frames never cross adapters.
    std::optional<EncodeAdapter> adapter = ResolveEncodeAdapter(options.adapter);
    if (!adapter) { r.error = "no GPU to decode on"; return r; }
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    if (!CreateD3D11DeviceOnAdapter(adapter->luid, device, context, r.error, D3D11_CREATE_DEVICE_VIDEO_SUPPORT)) return r;
    // Media Foundation's decoder and this thread share the immediate context (engine.md).
    com_ptr<ID3D11Multithread> mt = device.as<ID3D11Multithread>();
    mt->SetMultithreadProtected(TRUE);

    // The encoder first: no point decoding for one that won't open.
    const bool hardware = adapter->backend != EncoderBackend::X264;
    const VideoCodec wanted = hardware ? info.codec : VideoCodec::H264;
    // The image, not the coded size: a padded source's extra rows would become picture.
    const EncodePlan plan = PlanEncode({info.picture_width, info.picture_height}, wanted, adapter->av1_align_w, adapter->av1_align_h,
                                       options.av1_unaligned);
    EncoderOptions opts = options.encoder;
    opts.codec = plan.codec;
    opts.rate_control = "vbr";  // the source's bitrate, so the trim is about the size a copy would be
    const uint32_t source_kbps = static_cast<uint32_t>(
        (std::max)(1000.0, static_cast<double>(info.video_bytes) * 8.0 / info.duration_s / 1000.0));
    const uint32_t out_w = plan.size.width, out_h = plan.size.height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = out_w;
    td.Height = out_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    com_ptr<ID3D11Texture2D> slot;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, slot.put());
    if (FAILED(hr)) { r.error = Hr("CreateTexture2D(slot)", hr); return r; }

    EncoderBackend used = adapter->backend;
    std::unique_ptr<IVideoEncoder> encoder = CreateVideoEncoder(used);
    uint32_t kbps = BitrateForOpenedCodec(source_kbps, info.codec, plan.codec);
    bool opened = encoder->TryOpen(device.get(), out_w, out_h, fps, kbps, options.keyframe_sec, opts) &&
                  encoder->RegisterSlot(slot.get(), out_w, out_h);
    if (!opened && hardware) {
        Log::Warn("reencode: %s would not open (%s); encoding on the CPU", BackendName(adapter->backend),
                  encoder->FaultReason().c_str());
        used = EncoderBackend::X264;
        encoder = CreateVideoEncoder(used);
        opts.codec = VideoCodec::H264;
        kbps = BitrateForOpenedCodec(source_kbps, info.codec, VideoCodec::H264);
        opened = encoder->TryOpen(device.get(), out_w, out_h, fps, kbps, options.keyframe_sec, opts) &&
                 encoder->RegisterSlot(slot.get(), out_w, out_h);
    }
    if (!opened) { r.error = "the encoder would not open: " + encoder->FaultReason(); return r; }

    Nv12ToBgra convert;
    if (!convert.Open(device.get(), context.get(), slot.get(), out_w, out_h, r.error)) return r;

    MfSession mf;
    if (!mf.ok) { r.error = "Media Foundation would not start"; return r; }
    UINT token = 0;
    com_ptr<IMFDXGIDeviceManager> manager;
    hr = MFCreateDXGIDeviceManager(&token, manager.put());
    if (SUCCEEDED(hr)) hr = manager->ResetDevice(device.get(), token);
    if (FAILED(hr)) { r.error = Hr("MFCreateDXGIDeviceManager", hr); return r; }
    com_ptr<IMFAttributes> attr;
    MFCreateAttributes(attr.put(), 2);
    if (options.gpu_decode) {
        attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.get());
        attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    com_ptr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(src.c_str(), attr.get(), reader.put());
    if (FAILED(hr)) { r.error = Hr("MFCreateSourceReaderFromURL", hr); return r; }
    const DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(kVideo, TRUE);
    com_ptr<IMFMediaType> type;
    MFCreateMediaType(type.put());
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader->SetCurrentMediaType(kVideo, nullptr, type.get());
    if (FAILED(hr)) {
        r.error = info.codec == VideoCodec::AV1 ? "no AV1 decoder (the AV1 Video Extension)" : Hr("SetCurrentMediaType(NV12)", hr);
        return r;
    }
    uint32_t frame_w = 0, frame_h = 0;
    int32_t frame_stride = 0;
    ReadOutputType(reader.get(), kVideo, frame_w, frame_h, frame_stride);
    Nv12Upload upload;

    // From the keyframe at or before the start; the frames ahead of it are decoded and dropped.
    double seek_s = info.video_offset_s;
    for (double k : info.keyframes_s) if (k <= start_s + eps) seek_s = k;
    if (seek_s > info.video_offset_s + eps) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = std::llround((seek_s - info.video_offset_s) * 1e7);
        hr = reader->SetCurrentPosition(GUID_NULL, pos);
        if (FAILED(hr)) { r.error = Hr("SetCurrentPosition", hr); return r; }
    }

    Spill spill(dst + L".part.video");
    if (!spill.Ok()) {
        const DWORD err = GetLastError();
        Log::Warn("reencode: could not create %ls.part.video (error %lu)", dst.c_str(), err);
        r.error = "could not create the scratch file (error " + std::to_string(err) + ")";
        return r;
    }
    std::vector<MuxVideoSample> samples;
    const uint32_t total = static_cast<uint32_t>((std::max)(1LL, std::llround((end_s - start_s) * fps)));
    double first_s = -1.0, last_s = 0.0;
    bool hit_end = false;
    uint64_t last_tick = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        com_ptr<IMFSample> sample;
        hr = reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, sample.put());
        if (FAILED(hr)) { r.error = Hr("ReadSample", hr); return r; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            ReadOutputType(reader.get(), kVideo, frame_w, frame_h, frame_stride);
        if (!sample) continue;
        const double t = static_cast<double>(ts) / 1e7 + info.video_offset_s;
        if (t < start_s - eps) continue;
        if (t >= end_s - eps) { hit_end = true; last_s = t; break; }

        com_ptr<IMFMediaBuffer> buffer;
        sample->GetBufferByIndex(0, buffer.put());
        com_ptr<IMFDXGIBuffer> dxgi = buffer ? buffer.try_as<IMFDXGIBuffer>() : nullptr;
        com_ptr<ID3D11Texture2D> tex;
        UINT sub = 0;
        const bool on_gpu =
            dxgi && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(tex.put()))) && SUCCEEDED(dxgi->GetSubresourceIndex(&sub));
        if (!on_gpu && !buffer) { r.error = "the decoder returned an empty frame"; return r; }
        if (first_s < 0.0) {
            first_s = t;
            if (!on_gpu) Log::Info("reencode: the decoder returns system memory; uploading %ux%u frames", frame_w, frame_h);
        }
        uint64_t tick = static_cast<uint64_t>((std::max)(0LL, std::llround((t - first_s) * fps)));
        if (!samples.empty() && tick <= last_tick) tick = last_tick + 1;
        mt->Enter();
        bool converted = true;
        if (!on_gpu) {
            converted = upload.Upload(device.get(), context.get(), buffer.get(), frame_w, frame_h, frame_stride, r.error);
            tex.copy_from(upload.Texture());
            sub = 0;
        }
        if (converted) converted = convert.Convert(tex.get(), sub, info.picture_width, info.picture_height, r.error);
        mt->Leave();
        if (!converted) return r;
        const IVideoEncoder::AccessUnit au = encoder->EncodeSlot(0, out_w, out_h, tick);
        if (!au.ok) { r.error = "encoding failed: " + encoder->FaultReason(); return r; }
        if (samples.empty() && !au.is_keyframe) {
            encoder->UnlockLast();
            r.error = "the encoder's first frame isn't a keyframe";
            return r;
        }
        const bool appended = spill.Append(au.data, au.size);
        encoder->UnlockLast();
        if (!appended) { r.error = "could not write the scratch file (disk full?)"; return r; }
        samples.push_back({au.is_keyframe, tick});
        last_tick = tick;
        last_s = t;
        if (progress && !progress(static_cast<uint32_t>(samples.size()), (std::max)(total, static_cast<uint32_t>(samples.size())))) {
            r.error = "cancelled";
            return r;
        }
    }
    if (samples.empty()) { r.error = "no frames decoded in the range"; return r; }
    // The last frame holds until the first frame past the range, or the clip's end.
    const double out_end = hit_end ? last_s : clip_end;
    r.start_s = first_s;
    r.end_s = out_end;
    const uint64_t end_tick = static_cast<uint64_t>((std::max)(0LL, std::llround((out_end - first_s) * fps)));
    const uint32_t last_ticks = static_cast<uint32_t>((std::max)(uint64_t{1}, end_tick > last_tick ? end_tick - last_tick : 1));

    std::vector<MuxAudioTrack> audio;
    if (!ReadClipAudio(src, r.start_s, r.end_s, keep_audio, audio, r.error)) return r;

    const std::vector<uint8_t>& header = encoder->SequenceParams();
    uint32_t coded_w = out_w, coded_h = out_h;
    if (encoder->Codec() == VideoCodec::AV1) Av1CodedSize(header.data(), header.size(), coded_w, coded_h);
    auto read = [&](size_t i, std::vector<uint8_t>& annexb) { return spill.Read(i, annexb); };
    const std::wstring part = dst + L".part";
    if (!MuxWriteMp4(part, header, coded_w, coded_h, fps, samples, read, audio, encoder->Codec(), last_ticks,
                     MuxPicture{out_w, out_h})) {
        DeleteFileW(part.c_str());
        r.error = "writing the trimmed clip failed (see the log)";
        return r;
    }
    if (!MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(part.c_str());
        r.error = "could not rename the trimmed clip into place";
        return r;
    }
    Log::Info("reencode: %zu frames %.3f-%.3f s, %s %s %ux%u at %u kbps", samples.size(), r.start_s, r.end_s,
              BackendName(used),
              VideoCodecName(encoder->Codec()), coded_w, coded_h, kbps);
    r.ok = true;
    return r;
}

}  // namespace ombra
