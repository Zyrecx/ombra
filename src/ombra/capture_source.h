// What the pacer and PipelineSupervisor need from a frame source: D3D11 textures on the encode
// device holding the newest picture, handed to the encoder one claimed slot at a time.
//
// Capture (Windows Graphics Capture) is the production source. Tests implement this with synthetic
// frames on the real encode device, so the pacer, NVENC, the ring and the supervisor's rebuild and
// resolution paths all run for real without anything on screen. Push model: the source publishes
// frames whenever it has them; the pacer reads whichever slot is newest on its own clock.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include "ombra/window_target.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ombra {

// A source's "the content isn't the size I capture at" state: written on the frame thread, read by
// the supervisor's watchdog, which reopens once it holds still (kResizeSettleDelay). A frame back at
// the capture size withdraws it, so a change that reversed inside the debounce costs nothing: the
// reopen cuts a ring barrier, and a save can't reach past one.
class PendingResize {
public:
    enum class Step { None, Started, Reverted };

    Step OnFrame(uint32_t content_w, uint32_t content_h, uint32_t cur_w, uint32_t cur_h) {
        if (content_w == 0 || content_h == 0) return Step::None;
        const bool was_changed = changed_.load(std::memory_order_acquire);
        if (content_w == cur_w && content_h == cur_h) {
            if (!was_changed) return Step::None;
            changed_.store(false, std::memory_order_release);
            return Step::Reverted;
        }
        width_.store(content_w, std::memory_order_release);
        height_.store(content_h, std::memory_order_release);
        if (!was_changed) started_.fetch_add(1, std::memory_order_acq_rel);
        changed_.store(true, std::memory_order_release);
        return was_changed ? Step::None : Step::Started;
    }
    void     Clear() { changed_.store(false, std::memory_order_release); }
    bool     Changed() const { return changed_.load(std::memory_order_acquire); }
    uint32_t Width() const { return width_.load(std::memory_order_acquire); }
    uint32_t Height() const { return height_.load(std::memory_order_acquire); }
    // Moves each time a change starts. Two polls can read the same size with a withdrawal between
    // them: the same revision is what says it held still.
    uint32_t Revision() const { return started_.load(std::memory_order_acquire); }

private:
    std::atomic<bool>     changed_{false};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<uint32_t> started_{0};
};

// What Open() should capture. Re-resolved from scratch on every
// Open() call — PipelineSupervisor::BuildGeneration() calls Open() again on every rebuild, so
// "re-resolved on every pipeline rebuild" falls out of that for free: there is no
// cached resolution to go stale, and a relaunched game window is picked back up on the next rebuild
// rather than requiring a restart.
struct CaptureTarget {
    // capture.target: "monitor" -> Monitor, "window" -> Window (one program; nothing is recorded
    // while it's closed), "game" -> WindowWithFallback over the Games list (window.is_game), the
    // display while no game is on screen.
    enum class Mode { Monitor, Window, WindowWithFallback };
    static Mode ParseMode(const std::string& capture_target) {
        return capture_target == "window" ? Mode::Window
             : capture_target == "game"   ? Mode::WindowWithFallback
                                          : Mode::Monitor;
    }
    Mode mode = Mode::Monitor;
    // Mode::Monitor, and also the fallback target for WindowWithFallback: the literal "primary" or
    // a \\.\DISPLAYn-style device name (config.h's MonitorSpec::device). Default "primary" makes
    // CaptureTarget{}'s default-constructed value byte-identical to every pre-step-4 caller.
    std::string monitor_device = "primary";
    // Mode::Window / WindowWithFallback: which windows count (window_target.h).
    WindowTarget window;
    // The window the caller wants when several match (the one being recorded, or the one the
    // supervisor decided to switch to). ChooseTargetWindow treats it as the current window, so an
    // Open() between two resolves lands where the resolve pointed instead of re-picking.
    uint64_t preferred_window = 0;
};

class ICaptureSource {
public:
    // Two slots, not one: a texture the encoder is reading while the source writes it is a torn
    // frame. The source never writes the claimed slot, so during a long encode it keeps rewriting
    // the other one, and the next claim gets the newest frame. A third slot adds no frame to that
    // (only the latest is ever claimed) and costs one stage texture (8 MB at 1080p).
    // See ClaimSlotForEncoding.
    static constexpr uint32_t kSlots  = 2;
    static constexpr uint32_t kNoSlot = ~0u;

    virtual ~ICaptureSource() = default;

    // Creates the device on `adapter` (the encode adapter's LUID) and kSlots BGRA stage textures at
    // even dimensions.
    // Returns false only for a retryable "target not there yet" (strict window mode); every other
    // failure still die()s here -- correct only at startup (the "fatal at startup"
    // bucket). Use TryOpen() below for anything running after startup.
    virtual bool Open(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) = 0;
    // Non-fatal counterpart, mirroring IVideoEncoder::TryOpen (encoder.h): every Open() failure
    // that would otherwise die() instead returns false with FaultReason() set. The one routine
    // "target not found" case (strict window mode) still returns false with FaultReason() left
    // empty -- PipelineSupervisor::BuildGeneration tells the two apart by that emptiness, same as
    // it already does for the encoder half.
    virtual bool TryOpen(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) = 0;
    // Meaningful only right after a false return from Open()/TryOpen(); see TryOpen's comment on
    // the empty-string convention for the routine "not found" case.
    virtual const std::string& FaultReason() const = 0;
    // Frames begin landing in stage slots asynchronously from here on.
    virtual void StartCapture() = 0;
    virtual void Close()        = 0;
    virtual void SetCursorCaptureEnabled(bool enabled) = 0;

    virtual ID3D11Device*    Device() const                  = 0;
    virtual ID3D11Texture2D* SlotTexture(uint32_t slot) const = 0;
    // Runtime-mutable (Recreate), so read once per tick by the pacer.
    virtual uint32_t Width() const  = 0;
    virtual uint32_t Height() const = 0;

    // `frame` counts frames published by this source (0 = none yet), read with the slot: two claims
    // with the same frame encoded the same picture (the pacer's duplicate-tick count). A slot index
    // can't tell that: with few slots a new frame often lands in the slot claimed last tick.
    struct SlotClaim {
        uint32_t slot  = 0;
        uint64_t frame = 0;
    };
    // Atomically reads the latest slot and marks it as being encoded, in the same critical section
    // the source uses to pick the next slot to write: never "read latest, then claim". One slot at
    // a time; screenshots go through the pre-encode callback, not a second claimant.
    virtual SlotClaim ClaimSlotForEncoding() = 0;
    virtual void     ReleaseEncodingSlot()  = 0;
    // True once a frame has been published into a slot since Open()/Recreate(). Before that every
    // slot is a freshly created, never-drawn texture (black), so the pacer holds its first encode
    // until this is true, for a bounded time (pacer.cpp).
    virtual bool     HasFrame() const       = 0;

    // The source is delivering content at a size it isn't set up for. PipelineSupervisor polls
    // this, debounces it, and drives Recreate() with the pacer stopped and the encoder reopened.
    virtual bool     SizeChanged() const       = 0;
    virtual uint32_t PendingWidth() const      = 0;
    virtual uint32_t PendingHeight() const     = 0;
    virtual uint32_t PendingRevision() const   = 0;  // PendingResize::Revision
    virtual bool     IsTargetMinimized() const = 0;
    // The window being recorded (HWND value), 0 for a monitor or a source with no window.
    virtual uint64_t TargetWindowId() const { return 0; }
    // false: the source can't deliver at the new size; the caller rebuilds the pipeline instead.
    virtual bool     Recreate(uint32_t w, uint32_t h) = 0;

    // Recoverable faults (device removed, item closed). Cheap: polled once per pacer tick.
    // HealthIssue() is meaningful only when IsHealthy() is false.
    virtual bool        IsHealthy() const   = 0;
    virtual const char* HealthIssue() const = 0;

    // CPU readback of a region of `slot` as BGRA8, for screenshots and capturebench. Only call
    // while `slot` can't be overwritten (from the pacer's pre-encode callback). Strides by
    // `out_row_pitch`, which the driver may pad past w * 4. `out_bgra` comes back empty if the GPU
    // refused (a device reset between the pacer's health check and here): this runs on the pacer.
    virtual void ReadSlotRegionBGRA(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    std::vector<uint8_t>& out_bgra, uint32_t& out_row_pitch) = 0;
    virtual void ReleaseReadbackStaging() = 0;

    // Fault and size-change simulation for tests (pipeline_gpu_tests.cpp).
    virtual void TestInjectItemClosed()                       = 0;
    virtual void TestInjectDeviceRemoved()                    = 0;
    virtual void TestForceContentSize(uint32_t w, uint32_t h) = 0;
};

}  // namespace ombra
