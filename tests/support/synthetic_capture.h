// A capture source with no WGC, for GPU tests. A present thread draws solid-colour frames into the
// stage slots on the real encode device, so PipelineSupervisor, the pacer, the encoder and the ring run
// unchanged and nothing has to be on screen.
//
// What gets drawn, and at what size, lives on a SyntheticScene shared across generations: the
// supervisor's capture factory builds a fresh SyntheticCapture on every rebuild, the way it opens a
// fresh WGC session.
#pragma once

#include "ombra/capture_source.h"

#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

struct SyntheticScene {
    // Content size (even). Changing it mid-run is a resolution change: the source reports
    // SizeChanged() until the supervisor recreates it at the new size.
    std::atomic<uint32_t> width{320};
    std::atomic<uint32_t> height{180};
    uint32_t              present_fps = 60;  // read when a source starts

    // Frames presented inside [flash_start_qpc, flash_end_qpc) are white; every other frame is a
    // dim grey that steps each present, so consecutive frames differ. The QPC right after the first
    // white frame was published is recorded: the video half of an A/V sync check.
    std::atomic<int64_t> flash_start_qpc{0};
    std::atomic<int64_t> flash_end_qpc{0};
    std::atomic<int64_t> first_flash_present_qpc{0};

    // frozen: no presents at all (a static screen: WGC delivers nothing, every tick is a duplicate).
    // constant: presents keep landing in new slots, but every one is the same grey.
    std::atomic<bool> frozen{false};
    std::atomic<bool> constant{false};

    // What a freshly created slot holds until the first present lands (read when slots are made).
    // The pacer holds its first encode until HasFrame(), so a clip should never show it; a test sets
    // a marker colour here to catch a never-drawn slot reaching the encoder.
    float fresh_slot_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    std::atomic<int>      opens{0};
    std::atomic<uint64_t> presents{0};

    // Fault injection for the supervisor's nonfatal-rebuild contract (Capture::TryOpen's real
    // counterpart can't be exercised headless -- see tests/support's own note on that gap): each
    // Open()/TryOpen() call while this is > 0 decrements it and fails with FaultReason() set,
    // instead of actually opening a device, standing in for a die()-turned-recoverable fault like
    // "encode adapter gone" during a rebuild.
    std::atomic<int> fail_next_opens{0};

    // The cursor setting the newest source was opened with or last told. on_opened runs at the end
    // of a successful Open(), before the supervisor publishes the source.
    std::atomic<bool>     cursor{true};
    std::function<void()> on_opened;
};

class SyntheticCapture final : public ICaptureSource {
public:
    explicit SyntheticCapture(std::shared_ptr<SyntheticScene> scene);
    ~SyntheticCapture() override;

    bool Open(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) override;
    bool TryOpen(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) override {
        return Open(capture_cursor, target, adapter);
    }
    const std::string& FaultReason() const override { return fault_reason_; }
    void StartCapture() override;
    void Close() override;
    void SetCursorCaptureEnabled(bool enabled) override { scene_->cursor.store(enabled); }

    ID3D11Device*    Device() const override { return device_.get(); }
    ID3D11Texture2D* SlotTexture(uint32_t slot) const override { return slots_[slot].get(); }
    uint32_t         Width() const override { return width_.load(std::memory_order_acquire); }
    uint32_t         Height() const override { return height_.load(std::memory_order_acquire); }

    SlotClaim ClaimSlotForEncoding() override;
    void     ReleaseEncodingSlot() override { encoding_slot_.store(kNoSlot, std::memory_order_release); }
    bool     HasFrame() const override { return has_frame_.load(std::memory_order_acquire); }

    bool     SizeChanged() const override { return pending_resize_.Changed(); }
    uint32_t PendingWidth() const override { return pending_resize_.Width(); }
    uint32_t PendingHeight() const override { return pending_resize_.Height(); }
    uint32_t PendingRevision() const override { return pending_resize_.Revision(); }
    bool     IsTargetMinimized() const override { return false; }
    bool     Recreate(uint32_t w, uint32_t h) override;

    bool        IsHealthy() const override;
    const char* HealthIssue() const override;

    void ReadSlotRegionBGRA(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            std::vector<uint8_t>& out_bgra, uint32_t& out_row_pitch) override;
    void ReleaseReadbackStaging() override;

    void TestInjectItemClosed() override { item_closed_.store(true, std::memory_order_release); }
    void TestInjectDeviceRemoved() override { device_removed_.store(true, std::memory_order_release); }
    void TestForceContentSize(uint32_t w, uint32_t h) override;

private:
    void CreateSlots(uint32_t w, uint32_t h);  // caller holds mutex_
    void PresentLoop();

    std::shared_ptr<SyntheticScene> scene_;

    winrt::com_ptr<ID3D11Device>           device_;
    winrt::com_ptr<ID3D11DeviceContext>    context_;
    winrt::com_ptr<ID3D11Texture2D>        slots_[kSlots];
    winrt::com_ptr<ID3D11RenderTargetView> rtvs_[kSlots];
    winrt::com_ptr<ID3D11Texture2D>        staging_;

    std::mutex            mutex_;  // every ID3D11DeviceContext call, and slot selection
    uint32_t              write_slot_ = 0;
    std::atomic<uint32_t> latest_slot_{0};
    uint64_t              frames_published_ = 0;  // under mutex_; never reset (SlotClaim)
    std::atomic<uint32_t> encoding_slot_{kNoSlot};
    std::atomic<bool>     has_frame_{false};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    PendingResize         pending_resize_;
    std::atomic<bool>     item_closed_{false};
    std::atomic<bool>     device_removed_{false};

    std::atomic<bool> running_{false};
    std::thread       thread_;

    std::string fault_reason_;  // set by Open() on an injected fail_next_opens fault
};

}  // namespace ombra::test
