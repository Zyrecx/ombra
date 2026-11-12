// D3D11 device + Windows Graphics Capture.
//
// Owns the D3D11 device on the encode adapter (WGC and the encoder both run on it) and a small ring of "stage"
// textures — WGC recycles its own pool textures, so each captured frame is copied out before the
// frame closes. Capture is push-model: `FrameArrived` fires on a WGC pool thread and copies into
// the next stage slot; the pacer thread reads whichever slot was most recently published,
// concurrently and without blocking capture, and re-submits it when nothing new has arrived —
// that's what makes WGC's variable framerate into CFR output (WGC delivers zero frames on an
// idle desktop).
//
// Multiple slots, not one, because a single texture registered once with NVENC and written by the
// capture thread while the pacer thread submits it to the encoder is both a torn frame and
// concurrent use of it. All ID3D11DeviceContext calls (the immediate context is not thread-safe)
// go through Capture's own mutex.
#pragma once

#include "ombra/capture_source.h"

#include <windows.h>
#include <d3d11.h>

#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ombra {

// One connected display, for the Settings monitor picker (list_monitors pipe query) -- independent
// of capture.monitors' *chosen* list (config.h's MonitorSpec/ResolveCaptureMonitors), this reports
// every display Windows currently knows about so the UI can offer them all as toggles. `device` is
// the same "\\.\DISPLAYn" string MonitorSpec::device expects, so a UI-picked entry round-trips
// straight into capture.monitors with no translation.
struct MonitorInfo {
    std::string device;
    std::string label;  // "Primary" or the bare "DISPLAYn" (same stripping as MonitorSpec::label)
    // Physical pixels on the virtual desktop; left/top go negative for a screen left of or above
    // the primary.
    int32_t     left = 0;
    int32_t     top = 0;
    uint32_t    width = 0;
    uint32_t    height = 0;
    bool        primary = false;
};
std::vector<MonitorInfo> EnumerateAllMonitors();

// One capturable top-level window, for the Settings window-target picker (list_windows pipe
// query). Built from SnapshotTargetWindows (window_target.h), the same list Capture::Open() picks
// from, minus untitled windows.
struct WindowInfo {
    std::string title;
    std::string process;     // exe filename only (e.g. "notepad.exe")
    std::string image_path;  // full path; empty when the process couldn't be queried
    bool        minimized      = false;
    bool        foreground     = false;
    bool        covers_monitor = false;
    int64_t     area           = 0;
    // A same-session key for "which window do I screenshot right now" only -- never persisted.
    uint64_t hwnd = 0;
};
std::vector<WindowInfo> EnumerateCapturableWindows();

// Result of a one-shot thumbnail capture for the window-picker grid.
struct ThumbnailResult {
    bool        ok = false;
    std::string error;  // set when !ok -- a routine, retryable condition (window closed in the
                         // gap between list_windows and this call, no device, WGC session
                         // failed to open), never a reason to die() the whole engine.
    uint32_t    width = 0;
    uint32_t    height = 0;
};

// Opens a short-lived WGC session against `hwnd` (must still be a valid, visible top-level
// window), grabs one frame, downsamples it so neither dimension exceeds `max_dim`, and writes it
// as a PNG to `out_path` (WriteBgraPng, screenshot.h). Never die()s -- unlike Capture::Open(),
// every failure here returns ok=false with a reason, since this backs a Settings picker that must
// survive any single tile failing without taking the recording engine down with it.
//
// Deliberately not a Capture method: Capture's multi-slot push-model design (kSlots, the
// FrameArrived subscription, ClaimSlotForEncoding) exists for a long-lived encode pipeline this
// one-shot grab doesn't need. Its device is the default hardware adapter, not the encode adapter.
//
// Must be called with COM already initialized MTA on the caller's thread -- same contract
// WriteBgraPng already has.
ThumbnailResult CaptureWindowThumbnailToPng(HWND hwnd, uint32_t max_dim, const std::wstring& out_path);

// Same one-shot shape as CaptureWindowThumbnailToPng above (own D3D11 device + WGC session, never
// die()s), for the Settings monitor picker instead of the window picker. `device` is a
// MonitorInfo::device string (list_monitors) -- resolved to a live HMONITOR the same way
// Capture::Open() resolves capture.monitors entries (ResolveMonitorByDevice, capture.cpp).
ThumbnailResult CaptureMonitorThumbnailToPng(const std::string& device, uint32_t max_dim, const std::wstring& out_path);

// A \\.\DISPLAYn device name, or "primary", as a live HMONITOR (null when it is gone).
HMONITOR MonitorForDevice(const std::string& device);

// The production ICaptureSource: Windows Graphics Capture into kSlots stage textures.
class Capture : public ICaptureSource {
public:
    // Creates the D3D11 device on `adapter` (the encode adapter, encode_target.h), and opens a WGC session against `target`
    // (default: the primary monitor) sized to even dimensions (NVENC requirement). Allocates kSlots
    // stage textures.
    //
    // Returns false, touching no GPU/WGC object at all, only for target.mode == Mode::Window with
    // no matching window found — a routine, retryable "not launched yet" condition
    // ("game", Mode::WindowWithFallback, degrades to monitor capture instead of returning false here;
    // plain window does not). Every other failure path dies (die()/die_hr()) -- correct only at
    // startup (the "fatal at startup" bucket). Use TryOpen() below on any path that
    // runs after startup, or a recoverable adapter/driver hiccup during a fault rebuild takes the
    // whole process down with it.
    bool Open(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) override;
    // Non-fatal counterpart, for PipelineSupervisor's fault-rebuild path -- see
    // ICaptureSource::TryOpen. Every die()/die_hr() call inside Open() below becomes a
    // FaultReason()-and-return-false instead when this was used to open.
    bool TryOpen(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) override;
    const std::string& FaultReason() const { return fault_reason_; }
    void SetCursorCaptureEnabled(bool enabled);

    ID3D11Device*     Device() const { return device_.get(); }
    ID3D11Texture2D*  SlotTexture(uint32_t slot) const { return stage_[slot].get(); }
    // Atomic because Recreate() makes these runtime-mutable: the pacer thread
    // reads them once per tick to size EncodeSlot, and OnFrameArrived reads them on a WGC pool
    // thread. Before Recreate() existed, they were written once in Open() and never again.
    uint32_t          Width() const { return width_.load(std::memory_order_acquire); }
    uint32_t          Height() const { return height_.load(std::memory_order_acquire); }

    // Starts the WGC session and the FrameArrived subscription. Frames begin landing in stage
    // slots asynchronously from here on.
    void StartCapture();

    // Atomically reads the latest slot and marks it as being encoded, under the same mutex
    // OnFrameArrived's slot-select-and-copy uses — the two operations must be one critical
    // section, not "read latest, then separately claim it," or a FrameArrived between those two
    // steps can select and start copying into the very slot about to be claimed: a synchronous
    // encode can take several ms, long enough for WGC to deliver frames into the slot NVENC is
    // reading.
    SlotClaim ClaimSlotForEncoding();
    void     ReleaseEncodingSlot() { encoding_slot_.store(kNoSlot, std::memory_order_release); }
    bool     HasFrame() const override { return has_frame_.load(std::memory_order_acquire); }

    // The monitor changed resolution and WGC is now delivering
    // frames at a size this Capture is not set up for. Detected in OnFrameArrived by comparing the
    // frame's ContentSize() against width_/height_ — the surface itself stays at the *pool's* size,
    // so the texture desc is not a reliable signal (it is logged alongside ContentSize on the first
    // change so a hand-off run can confirm which one actually moved on real hardware).
    // PipelineSupervisor polls this and drives Recreate(); nothing here acts on it by itself.
    bool     SizeChanged() const { return pending_resize_.Changed(); }
    uint32_t PendingWidth() const { return pending_resize_.Width(); }
    uint32_t PendingHeight() const { return pending_resize_.Height(); }
    uint32_t PendingRevision() const { return pending_resize_.Revision(); }

    // Resizes the frame pool and the stage textures to `w` x `h` (already rounded even by the
    // caller) and clears SizeChanged(). The encoder cannot change resolution in place, so the
    // caller must close and reopen it around this call — and must have stopped the pacer first,
    // since every stage texture is destroyed and recreated here. Frame delivery is suppressed for
    // the duration (arriving frames are dropped, not copied into a half-swapped slot set).
    bool Recreate(uint32_t w, uint32_t h) override;

    // True when this Capture is targeting a specific window (Mode::Window
    // / WindowWithFallback) and that window is currently minimized. Always false for monitor-mode
    // capture. PipelineSupervisor's watchdog checks this before reacting to SizeChanged() — a
    // minimized window's reported ContentSize isn't a resolution the user actually cares about
    // capturing, and reopening on it would cut a ring barrier (buffer-reset symptom) for a
    // transient state that reverses itself the moment the window is restored.
    bool IsTargetMinimized() const { return target_hwnd_ && IsIconic(target_hwnd_); }
    uint64_t TargetWindowId() const override { return reinterpret_cast<uint64_t>(target_hwnd_); }

    // For the screenshot test (capture_gpu_tests.cpp): fills slot 0 with a solid colour instead of capturing. Must be called before
    // StartCapture().
    void FillStageColor(const float rgba[4]);

    // Copies a small region of `slot`'s texture back to the CPU as BGRA8. Test/diagnostic use only
    // (`capturebench`) — the real save/encode pipeline never touches the CPU on
    // purpose (the whole reason for an encoded-bitstream ring). `out_row_pitch` is the mapped
    // staging texture's actual row pitch, which a driver is free to pad past `w * 4` bytes — a
    // caller must stride by it, not assume tight packing. Caller is responsible for calling this
    // only while `slot` can't be concurrently overwritten (capturebench does so from Pacer's
    // pre-encode hook, under the same guarantee EncodeSlot relies on).
    void ReadSlotRegionBGRA(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            std::vector<uint8_t>& out_bgra, uint32_t& out_row_pitch);

    // Releases ReadSlotRegionBGRA's staging texture. capturebench calls this rarely enough (once
    // per bench run, small fixed region) that leaving it resident never mattered; a full-frame
    // screenshot is ~15 MB at 1440p against an RSS goal already
    // recorded as missed — that one is worth not holding onto between hotkey presses. Safe to call
    // any time; the next ReadSlotRegionBGRA call just recreates it lazily as before.
    void ReleaseReadbackStaging();

    // DXGI_ERROR_DEVICE_REMOVED/RESET and "WGC item closed" are recoverable-at-
    // runtime faults, not die()s. Cheap on purpose — GetDeviceRemovedReason() just returns cached
    // driver state, no GPU wait — so PipelineSupervisor/Pacer can poll it once per tick.
    // `HealthIssue()` is valid only when IsHealthy() is false; returns why, for logging.
    bool        IsHealthy() const;
    const char* HealthIssue() const;

    // Test-only. Nothing drives these on the real Capture (ombra-tests injects into
    // SyntheticCapture instead). Simulates the two recoverable
    // faults IsHealthy() watches for, without needing a real device removal (not reachable from
    // user mode) or a monitor unplug. Exercises PipelineSupervisor's rebuild path; proves nothing
    // about the real detection code actually firing on a genuine device-removed/item-closed event
    // — that stays [hand-off] (Ctrl+Shift+Win+B / physical monitor unplug), same posture as
    // the AMF call.
    void TestInjectItemClosed() { item_closed_.store(true, std::memory_order_release); }
    void TestInjectDeviceRemoved() { forced_device_removed_.store(true, std::memory_order_release); }
    // One-shot WinRT failure at the frame-pool call, after stage textures have been replaced.
    void TestFailNextFramePoolRecreate() { fail_next_pool_recreate_.store(true, std::memory_order_release); }
    // Test-only. Makes OnFrameArrived read this size instead of the frame's real ContentSize(),
    // simulating a monitor resolution change without changing the user's display mode.
    //
    // An override rather than a one-shot "pretend a change is pending" flag, because a one-shot
    // flag is not a faithful simulation and the first version of restest proved it: after
    // Recreate() resized the pool, every subsequent real frame still reported the monitor's actual
    // ContentSize, so the detector correctly fired again and snapped straight back to native. A
    // real resolution change reports the new size on every frame from then on; so does this.
    //
    // What it exercises: the real detection code in OnFrameArrived, plus the whole reaction
    // (Recreate, encoder reopen, barrier, per-segment SPS/PPS). What it does not: whether a real
    // monitor mode change actually surfaces as a ContentSize move rather than closing the WGC item
    // outright (which would land in the device-loss path instead). That stays [hand-off], same
    // posture as the device-removed injection.
    void TestForceContentSize(uint32_t w, uint32_t h) {
        forced_content_w_.store(w & ~1u, std::memory_order_release);
        forced_content_h_.store(h & ~1u, std::memory_order_release);
    }

    void Close();

    // Revokes the Closed subscription before this object goes away -- without it, a WGC item kept
    // alive by the system past this Capture's lifetime could still fire OnItemClosed against a
    // dangling `this`. TeardownGeneration() already calls Close() explicitly before dropping the
    // unique_ptr on every normal path; this exists for the nonfatal Open()/TryOpen() failure paths,
    // which return false with no such explicit call -- same reason NvencEncoder's destructor calls
    // Close() (encoder.h).
    ~Capture() override { Close(); }

private:
    HRESULT CreateStageTextures();  // first failure; the slots are then unusable
    void OnItemClosed(
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& sender,
        winrt::Windows::Foundation::IInspectable const& args);
    void OnFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    winrt::com_ptr<ID3D11Device>           device_;
    winrt::com_ptr<ID3D11DeviceContext>    context_;
    winrt::com_ptr<ID3D11Texture2D>        stage_[kSlots];
    winrt::com_ptr<ID3D11RenderTargetView> stage_rtv_;  // colortest only, created lazily

    // ReadSlotRegionBGRA's staging texture, sized lazily to whatever region was last requested and
    // reused across calls (capturebench requests the same fixed region every tick).
    winrt::com_ptr<ID3D11Texture2D> readback_staging_;
    uint32_t                        readback_w_ = 0, readback_h_ = 0;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem        item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession     session_{nullptr};
    winrt::event_token                                            frame_arrived_token_{};

    std::mutex            context_mutex_;  // guards every ID3D11DeviceContext call
    uint32_t               write_slot_ = 0;
    std::atomic<uint32_t> latest_slot_{0};
    uint64_t              frames_published_ = 0;  // under context_mutex_; never reset (SlotClaim)
    std::atomic<uint32_t> encoding_slot_{kNoSlot};
    std::atomic<bool>     has_frame_{false};  // set with latest_slot_, cleared by Recreate

    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};

    // Resolution-change detection and the Recreate() handshake.
    PendingResize         pending_resize_;
    std::atomic<bool>     frames_suspended_{false};  // set across Recreate(); frames are dropped
    std::atomic<bool>     fail_next_pool_recreate_{false};
    std::atomic<uint32_t> forced_content_w_{0};  // restest only, see TestForceContentSize
    std::atomic<uint32_t> forced_content_h_{0};

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rt_device_{nullptr};

    winrt::event_token        item_closed_token_{};
    std::atomic<bool>         item_closed_{false};
    std::atomic<bool>         forced_device_removed_{false};  // faulttest only, see TestInject*

    // The window resolved by Open() when target.mode is Window/
    // WindowWithFallback; nullptr for monitor-mode capture or a WindowWithFallback generation that
    // fell back. Set once in Open(), cleared in Close() — not re-resolved outside of a rebuild, same
    // as everything else about the target (re-resolution happens by rebuilding, not by
    // this class watching the handle live). IsIconic() is cheap enough to call every watchdog poll
    // (200ms) with no caching needed.
    HWND target_hwnd_ = nullptr;

    bool        nonfatal_open_ = false;  // set by TryOpen
    std::string fault_reason_;
};

}  // namespace ombra
