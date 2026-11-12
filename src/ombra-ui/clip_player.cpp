#include "ombra-ui/clip_player.h"

#include "ombra/log.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

// The engine requires a callback; Tick polls GetReadyState/GetError instead of trusting events,
// which can belong to the previous source.
class ClipPlayer::Notify final : public IMFMediaEngineNotify {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP EventNotify(DWORD, DWORD_PTR, DWORD) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
};

ClipPlayer::ClipPlayer() = default;

ClipPlayer::~ClipPlayer() {
    if (engine_) engine_->Shutdown();
    engine_ex_.Reset();
    engine_.Reset();
    frame_.Reset();
    manager_.Reset();
    device_.Reset();
    if (notify_) notify_->Release();
    if (mf_started_) MFShutdown();
}

bool ClipPlayer::Init(ID3D11Device* device) {
    if (engine_) return true;
    if (!device || FAILED(MFStartup(MF_VERSION))) return false;
    mf_started_ = true;
    device_ = device;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(device_.Get(), token))) {
        ombra::Log::Warn("clips: no DXGI device manager; preview unavailable");
        return false;
    }
    ComPtr<IMFMediaEngineClassFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    notify_ = new Notify();
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 3);
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager_.Get());
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_);
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    const HRESULT hr = factory->CreateInstance(0, attr.Get(), &engine_);
    if (FAILED(hr)) {
        ombra::Log::Warn("clips: IMFMediaEngine creation failed (0x%08lX); preview unavailable", static_cast<unsigned long>(hr));
        engine_.Reset();
        return false;
    }
    engine_.As(&engine_ex_);
    engine_->SetMuted(muted_);
    engine_->SetVolume(volume_);
    SetRate(rate_);
    return true;
}

void ClipPlayer::Open(const std::wstring& path) {
    if (!engine_) return;
    LogPacing();
    state_ = State::Loading;
    streams_selected_ = false;
    selection_dirty_ = false;
    selection_pending_ = false;
    play_when_ready_ = false;
    audio_streams_.clear();
    audio_enabled_.clear();
    error_.clear();
    have_frame_ = false;
    loop_start_ = loop_end_ = 0.0;
    engine_->Pause();
    BSTR url = SysAllocString(path.c_str());
    engine_->SetSource(url);
    SysFreeString(url);
}

void ClipPlayer::Close() {
    if (!engine_) return;
    LogPacing();
    engine_->Pause();
    // An empty source unloads the file (and its decoder surfaces) but keeps the engine.
    engine_->SetSource(nullptr);
    state_ = State::Empty;
    have_frame_ = false;
}

void ClipPlayer::Play() {
    if (!engine_) return;
    if (state_ == State::Loading) {  // Space right after opening a clip: play once it's loaded
        play_when_ready_ = true;
        return;
    }
    if (state_ != State::Ready) return;
    if (engine_->IsEnded()) engine_->SetCurrentTime(loop_end_ > loop_start_ ? loop_start_ : 0.0);
    if (selection_dirty_) {
        selection_dirty_ = false;
        engine_ex_->ApplyStreamSelections();
        selection_pending_ = true;
    }
    const double at = engine_->GetCurrentTime();
    engine_->Play();
    if (selection_pending_) {
        selection_pending_ = false;
        engine_->SetCurrentTime(at);
    }
}

void ClipPlayer::Pause() {
    play_when_ready_ = false;
    if (engine_) engine_->Pause();
    LogPacing();
}

bool ClipPlayer::Playing() const {
    if (engine_ && state_ == State::Loading) return play_when_ready_;
    return engine_ && state_ == State::Ready && !engine_->IsPaused() && !engine_->IsEnded();
}

void ClipPlayer::Seek(double seconds) {
    if (!engine_ || state_ != State::Ready) return;
    LogPacing();
    const double t = std::clamp(seconds, 0.0, Duration());
    // Normal (exact) mode said explicitly: frame stepping relies on it, and approximate would snap
    // to a keyframe.
    if (!engine_ex_ || FAILED(engine_ex_->SetCurrentTimeEx(t, MF_MEDIA_ENGINE_SEEK_MODE_NORMAL))) engine_->SetCurrentTime(t);
}

double ClipPlayer::Position() const { return engine_ && state_ == State::Ready ? engine_->GetCurrentTime() : 0.0; }

double ClipPlayer::Duration() const {
    if (!engine_ || state_ != State::Ready) return 0.0;
    const double d = engine_->GetDuration();
    return std::isfinite(d) ? d : 0.0;
}

void ClipPlayer::SetMuted(bool muted) {
    muted_ = muted;
    if (engine_) engine_->SetMuted(muted);
}

void ClipPlayer::SetVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    if (engine_) engine_->SetVolume(volume_);
}

void ClipPlayer::SetRate(double rate) {
    rate_ = rate;
    pacing_.last_pts = -1;
    if (!engine_) return;
    // The default rate is what a new source starts at; the other is the one playing now.
    engine_->SetDefaultPlaybackRate(rate_);
    engine_->SetPlaybackRate(rate_);
}

void ClipPlayer::SetAudioTrackEnabled(size_t index, bool enabled) {
    if (!engine_ex_ || index >= audio_streams_.size() || audio_enabled_[index] == enabled) return;
    audio_enabled_[index] = enabled;
    engine_ex_->SetStreamSelection(audio_streams_[index], enabled);
    if (!Playing()) {
        selection_dirty_ = true;  // applied by the next Play (selection_dirty_)
        return;
    }
    engine_ex_->ApplyStreamSelections();
    selection_pending_ = true;
}

bool ClipPlayer::EnsureFrame(UINT max_w, UINT max_h) {
    if (!video_w_ || !video_h_ || !max_w || !max_h) return false;
    const double scale = (std::min)({1.0, double(max_w) / video_w_, double(max_h) / video_h_});
    const UINT w = (std::max)(2u, static_cast<UINT>(std::lround(video_w_ * scale)));
    const UINT h = (std::max)(2u, static_cast<UINT>(std::lround(video_h_ * scale)));
    if (frame_) {
        D3D11_TEXTURE2D_DESC d{};
        frame_->GetDesc(&d);
        if (d.Width == w && d.Height == h) return true;
        frame_.Reset();
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &frame_))) return false;
    ++frame_generation_;
    have_frame_ = false;  // a new texture holds nothing until the next transfer
    return true;
}

bool ClipPlayer::Tick(UINT max_w, UINT max_h) {
    if (!engine_ || state_ == State::Empty) return false;
    bool changed = false;

    // State comes from the engine itself, not from the notify flags: switching clips quickly, the
    // previous source's late events (an error from unloading it, its LOADEDDATA) would otherwise be
    // read as the new clip's.
    auto source_error = [this]() -> USHORT {
        ComPtr<IMFMediaError> err;
        if (FAILED(engine_->GetError(&err)) || !err) return MF_MEDIA_ENGINE_ERR_NOERROR;
        return err->GetErrorCode();
    };
    if (state_ == State::Loading) {
        if (const USHORT code = source_error(); code != MF_MEDIA_ENGINE_ERR_NOERROR) {
            ComPtr<IMFMediaError> err;
            engine_->GetError(&err);
            const HRESULT hr = err ? err->GetExtendedErrorCode() : E_FAIL;
            state_ = State::Failed;
            wchar_t buf[96];
            swprintf_s(buf, L"This clip can't be played (error %u, 0x%08lX)", code, static_cast<unsigned long>(hr));
            error_ = buf;
            ombra::Log::Warn("clips: media engine error %u hr=0x%08lX", code, static_cast<unsigned long>(hr));
            return true;
        }
        const USHORT ready = engine_->GetReadyState();
        if (ready < MF_MEDIA_ENGINE_READY_HAVE_METADATA) return false;
        DWORD w = 0, h = 0;
        engine_->GetNativeVideoSize(&w, &h);
        video_w_ = w;
        video_h_ = h;
        // Every audio track plays, as the clip sounds with all of them: on its own the engine
        // picks one per alternate group (it picked Mic over Desktop on the user's clips).
        if (engine_ex_ && !streams_selected_) {
            streams_selected_ = true;
            DWORD streams = 0;
            engine_ex_->GetNumberOfStreams(&streams);
            for (DWORD i = 0; i < streams; ++i) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(engine_ex_->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &v)) && v.vt == VT_CLSID &&
                    *v.puuid == MFMediaType_Audio) {
                    engine_ex_->SetStreamSelection(i, TRUE);
                    audio_streams_.push_back(i);
                    audio_enabled_.push_back(true);
                }
                PropVariantClear(&v);
            }
            selection_dirty_ = true;  // applied by the first Play
        }
        if (ready < MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA) return false;
        state_ = State::Ready;
        if (play_when_ready_) {
            play_when_ready_ = false;
            if (selection_dirty_) engine_ex_->ApplyStreamSelections();
            selection_dirty_ = selection_pending_ = false;  // from 0 either way
            engine_->Play();
        }
        changed = true;
    }
    if (state_ != State::Ready) return changed;

    if (source_error() != MF_MEDIA_ENGINE_ERR_NOERROR) {
        state_ = State::Failed;
        error_ = L"Playback stopped with an error";
        return true;
    }
    if (loop_end_ > loop_start_ && !engine_->IsPaused() && engine_->GetCurrentTime() >= loop_end_) {
        engine_->SetCurrentTime(loop_start_);
        pacing_.last_pts = -1;
    }

    LONGLONG pts = 0;
    if (!EnsureFrame(max_w, max_h)) return changed;
    const bool new_frame = engine_->OnVideoStreamTick(&pts) == S_OK;
    if (new_frame || !have_frame_) {
        D3D11_TEXTURE2D_DESC d{};
        frame_->GetDesc(&d);
        const MFVideoNormalizedRect src{0.0f, 0.0f, 1.0f, 1.0f};
        const RECT dst{0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
        const MFARGB border{0, 0, 0, 255};
        if (SUCCEEDED(engine_->TransferVideoFrame(frame_.Get(), &src, &dst, &border))) {
            have_frame_ = true;
            changed = true;
            if (new_frame && !engine_->IsPaused() && rate_ == 1.0) NotePacing(pts);
        }
    }
    return changed;
}

void ClipPlayer::NotePacing(LONGLONG pts) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    Pacing& p = pacing_;
    if (p.last_pts >= 0 && pts > p.last_pts) {
        const LONGLONG step = pts - p.last_pts;
        if (!p.frame_step || step < p.frame_step) p.frame_step = step;
        if (step * 2 > p.frame_step * 3) p.skipped += static_cast<uint32_t>((step + p.frame_step / 2) / p.frame_step - 1);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        p.wall_ms.push_back(static_cast<float>(double(now.QuadPart - p.last_qpc) * 1000.0 / double(f.QuadPart)));
        ++p.frames;
    }
    p.last_pts = pts;
    p.last_qpc = now.QuadPart;
}

void ClipPlayer::LogPacing() {
    Pacing& p = pacing_;
    if (p.frames >= 30 && p.frame_step > 0) {
        std::vector<float> w = p.wall_ms;
        std::sort(w.begin(), w.end());
        double mean = 0.0, var = 0.0;
        for (float v : w) mean += v;
        mean /= double(w.size());
        for (float v : w) var += (v - mean) * (v - mean);
        ombra::Log::Info("clips: played %u frames at %.1f fps, %u skipped; interval mean %.1f sd %.1f "
                         "p5 %.1f p95 %.1f max %.1f ms",
                         p.frames, 1e7 / double(p.frame_step), p.skipped, mean, std::sqrt(var / double(w.size())),
                         w[w.size() * 5 / 100], w[w.size() * 95 / 100], w.back());
    }
    p = Pacing{};
}

bool ClipPlayer::HasAv1Decoder() {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_AV1};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL, &in, nullptr, &acts, &count))) return false;
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return count > 0;
}

}  // namespace ombra_ui
