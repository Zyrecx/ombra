#pragma once

// The Clips screen's preview: Media Foundation's IMFMediaEngine in frame-server mode, drawing into
// a texture on the overlay's own D3D11 device (so Direct2D draws it with no copy). It demuxes,
// decodes (DXVA where the GPU can), plays every audio track and keeps them in sync; this class only
// pumps frames out of it. One engine per overlay session, reused for every clip: creating and
// destroying one per clip leaked ~3 MB each time (probe, 2026-10-19), switching sources didn't.
//
// UI thread only. The engine's own threads report through EventNotify, which only sets atomics.

#include <windows.h>
#include <d3d11.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <vector>

namespace ombra_ui {

class ClipPlayer {
public:
    ClipPlayer();
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    enum class State { Empty, Loading, Ready, Failed };

    // `device` must have been created with D3D11_CREATE_DEVICE_VIDEO_SUPPORT; it's made
    // multithread-protected here (the engine decodes on its own threads). False: no preview.
    bool Init(ID3D11Device* device);
    bool Initialized() const { return engine_ != nullptr; }

    // Starts loading `path`, paused at the first frame. State() turns Ready or Failed.
    void Open(const std::wstring& path);
    void Close();  // stops and unloads; the engine stays for the next Open

    State GetState() const { return state_; }
    const std::wstring& Error() const { return error_; }

    void Play();
    void Pause();
    bool Playing() const;
    void Seek(double seconds);  // exact: lands on the frame showing at `seconds`, not a nearby keyframe
    double Position() const;
    double Duration() const;
    void SetMuted(bool muted);
    bool Muted() const { return muted_; }
    void SetVolume(double volume);  // 0..1, all tracks together
    double Volume() const { return volume_; }
    // Preview speed, 1 = normal. Kept across Open like the volume; a save is never affected.
    void SetRate(double rate);
    double Rate() const { return rate_; }
    // Audio tracks in file order (ClipInfo::audio_track_names). All play by default; a switched-off
    // track stays off for the rest of this clip.
    size_t AudioTrackCount() const { return audio_streams_.size(); }
    void SetAudioTrackEnabled(size_t index, bool enabled);
    // While playing, reaching `end` jumps back to `start` (the trim range plays on a loop).
    void SetLoop(double start, double end) { loop_start_ = start; loop_end_ = end; }

    // Frame timer: polls the engine's events and, when a new frame is due, copies it into a
    // texture sized to fit `max_w` x `max_h` (aspect kept). True when anything visible changed.
    bool Tick(UINT max_w, UINT max_h);
    ID3D11Texture2D* Frame() const { return frame_.Get(); }
    // Bumped whenever Frame() is replaced by a new texture (a cached D2D bitmap must be rebuilt).
    uint64_t FrameGeneration() const { return frame_generation_; }
    UINT VideoWidth() const { return video_w_; }
    UINT VideoHeight() const { return video_h_; }

    // True when Media Foundation has an AV1 decoder (the Store's AV1 Video Extension).
    static bool HasAv1Decoder();

private:
    class Notify;
    bool EnsureFrame(UINT max_w, UINT max_h);
    void NotePacing(LONGLONG pts);
    void LogPacing();

    Microsoft::WRL::ComPtr<ID3D11Device>          device_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager>  manager_;
    Microsoft::WRL::ComPtr<IMFMediaEngine>        engine_;
    Microsoft::WRL::ComPtr<IMFMediaEngineEx>      engine_ex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>       frame_;
    Notify*                                       notify_ = nullptr;
    bool                                          mf_started_ = false;

    State        state_ = State::Empty;
    std::wstring error_;
    UINT         video_w_ = 0, video_h_ = 0;
    uint64_t     frame_generation_ = 0;
    bool         muted_ = false;
    bool         have_frame_ = false;  // a frame was transferred since the last Open/Seek
    bool         streams_selected_ = false;  // every audio track enabled for this source
    bool         play_when_ready_ = false;   // Play() arrived while loading
    // Audio track choices not yet applied. ApplyStreamSelections while paused leaves every later
    // paused seek unfinished until Play (IsSeeking stays true, no frame, the clock falls back; a
    // second seek can spin the engine), so they're applied only when playback starts.
    bool         selection_dirty_ = false;
    // ApplyStreamSelections took effect only at the next Play, and that Play started the clip over
    // from 0: the next Play puts the position back.
    bool         selection_pending_ = false;
    double       volume_ = 1.0;
    double       rate_ = 1.0;
    std::vector<DWORD> audio_streams_;       // engine stream index of each audio track
    std::vector<bool>  audio_enabled_;
    double       loop_start_ = 0.0, loop_end_ = 0.0;

    // Playback pacing, logged when a stretch of playback ends (pause, seek, close): frames shown,
    // frames the clock skipped (a pts step over 1.5x the clip's frame step) and how evenly the
    // shown frames were spaced in wall time.
    struct Pacing {
        LONGLONG last_pts = -1;
        LONGLONG frame_step = 0;     // smallest pts step seen (100 ns)
        int64_t  last_qpc = 0;
        uint32_t frames = 0, skipped = 0;
        std::vector<float> wall_ms;  // interval before each shown frame
    } pacing_;
};

}  // namespace ombra_ui
