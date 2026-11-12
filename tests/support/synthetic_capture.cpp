#include "support/synthetic_capture.h"

#include "ombra/encode_target.h"
#include "ombra/util.h"

#include <d3d11_4.h>

#include <chrono>
#include <cstring>
#include <string>

namespace ombra::test {

SyntheticCapture::SyntheticCapture(std::shared_ptr<SyntheticScene> scene) : scene_(std::move(scene)) {}

SyntheticCapture::~SyntheticCapture() { Close(); }

bool SyntheticCapture::Open(bool capture_cursor, const CaptureTarget&, const LUID& adapter) {
    int fails_left = scene_->fail_next_opens.load(std::memory_order_acquire);
    while (fails_left > 0) {
        if (scene_->fail_next_opens.compare_exchange_weak(fails_left, fails_left - 1,
                                                            std::memory_order_acq_rel)) {
            fault_reason_ = "synthetic injected open failure (test)";
            return false;
        }
    }
    std::string error;
    if (!CreateD3D11DeviceOnAdapter(adapter, device_, context_, error)) {
        fault_reason_ = error;
        return false;
    }
    // The encoder maps the slot textures on the pacer thread while this source draws on its own thread,
    // both through the device's immediate context. Without multithread protection the two race
    // (the first pipeline run hung in the first encode).
    if (auto multithread = device_.try_as<ID3D11Multithread>()) multithread->SetMultithreadProtected(TRUE);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CreateSlots(scene_->width.load() & ~1u, scene_->height.load() & ~1u);
    }
    scene_->cursor.store(capture_cursor);
    scene_->opens.fetch_add(1);
    if (scene_->on_opened) scene_->on_opened();
    return true;
}

void SyntheticCapture::CreateSlots(uint32_t w, uint32_t h) {
    width_.store(w, std::memory_order_release);
    height_.store(h, std::memory_order_release);

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (uint32_t i = 0; i < kSlots; ++i) {
        rtvs_[i]  = nullptr;
        slots_[i] = nullptr;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, slots_[i].put()))) continue;
        if (FAILED(device_->CreateRenderTargetView(slots_[i].get(), nullptr, rtvs_[i].put()))) continue;
        context_->ClearRenderTargetView(rtvs_[i].get(), scene_->fresh_slot_rgba);
    }
    context_->Flush();
    write_slot_ = 0;
    latest_slot_.store(0, std::memory_order_release);
    encoding_slot_.store(kNoSlot, std::memory_order_release);
    has_frame_.store(false, std::memory_order_release);
}

void SyntheticCapture::StartCapture() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&SyntheticCapture::PresentLoop, this);
}

// Like WGC's FrameArrived, but on a clock: pick the next slot that isn't being encoded, draw into it
// and publish it, all under the same lock ClaimSlotForEncoding takes.
void SyntheticCapture::PresentLoop() {
    using clock       = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / (scene_->present_fps ? scene_->present_fps : 60));
    auto       next   = clock::now();
    uint32_t   n      = 0;

    while (running_.load(std::memory_order_acquire)) {
        next += period;
        std::this_thread::sleep_until(next);

        const uint32_t want_w = scene_->width.load() & ~1u;
        const uint32_t want_h = scene_->height.load() & ~1u;
        pending_resize_.OnFrame(want_w, want_h, Width(), Height());

        if (scene_->frozen.load() && has_frame_.load(std::memory_order_acquire)) continue;
        const int64_t now   = QpcNow();
        const bool    flash = now >= scene_->flash_start_qpc.load() && now < scene_->flash_end_qpc.load();
        const float   grey  = static_cast<float>(40 + (scene_->constant.load() ? 0 : n++ % 32)) / 255.0f;
        const float   white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float   dim[4]   = {grey, grey, grey, 1.0f};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            uint32_t next_slot = (write_slot_ + 1) % kSlots;
            if (next_slot == encoding_slot_.load(std::memory_order_acquire)) next_slot = (next_slot + 1) % kSlots;
            if (!rtvs_[next_slot]) continue;
            context_->ClearRenderTargetView(rtvs_[next_slot].get(), flash ? white : dim);
            context_->Flush();
            write_slot_ = next_slot;
            ++frames_published_;
            latest_slot_.store(next_slot, std::memory_order_release);
            has_frame_.store(true, std::memory_order_release);
        }
        if (flash) {
            int64_t unset = 0;
            scene_->first_flash_present_qpc.compare_exchange_strong(unset, QpcNow());
        }
        scene_->presents.fetch_add(1);
    }
}

ICaptureSource::SlotClaim SyntheticCapture::ClaimSlotForEncoding() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t slot = latest_slot_.load(std::memory_order_relaxed);
    encoding_slot_.store(slot, std::memory_order_release);
    return {slot, frames_published_};
}

bool SyntheticCapture::Recreate(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lock(mutex_);
    CreateSlots(w, h);
    pending_resize_.Clear();
    return true;
}

bool SyntheticCapture::IsHealthy() const {
    if (item_closed_.load(std::memory_order_acquire)) return false;
    if (device_removed_.load(std::memory_order_acquire)) return false;
    return device_ && device_->GetDeviceRemovedReason() == S_OK;
}

const char* SyntheticCapture::HealthIssue() const {
    if (item_closed_.load(std::memory_order_acquire)) return "synthetic item closed (test)";
    if (device_removed_.load(std::memory_order_acquire)) return "synthetic device removed (test)";
    if (device_ && device_->GetDeviceRemovedReason() != S_OK) return "DXGI_ERROR_DEVICE_REMOVED/RESET";
    return "healthy";
}

void SyntheticCapture::ReadSlotRegionBGRA(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                          std::vector<uint8_t>& out_bgra, uint32_t& out_row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    out_bgra.clear();
    out_row_pitch = 0;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width            = w;
    sd.Height           = h;
    sd.MipLevels        = 1;
    sd.ArraySize        = 1;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage            = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    staging_            = nullptr;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, staging_.put()))) return;
    D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, slots_[slot].get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    out_bgra.resize(static_cast<size_t>(mapped.RowPitch) * h);
    std::memcpy(out_bgra.data(), mapped.pData, out_bgra.size());
    out_row_pitch = mapped.RowPitch;
    context_->Unmap(staging_.get(), 0);
}

void SyntheticCapture::ReleaseReadbackStaging() {
    std::lock_guard<std::mutex> lock(mutex_);
    staging_ = nullptr;
}

void SyntheticCapture::TestForceContentSize(uint32_t w, uint32_t h) {
    scene_->width.store(w & ~1u);
    scene_->height.store(h & ~1u);
}

void SyntheticCapture::Close() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < kSlots; ++i) {
        rtvs_[i]  = nullptr;
        slots_[i] = nullptr;
    }
    staging_ = nullptr;
}

}  // namespace ombra::test
