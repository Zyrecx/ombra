#include "ombra/capture.h"

#include "ombra/die.h"
#include "ombra/encode_target.h"
#include "ombra/log.h"
#include "ombra/screenshot.h"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <inspectable.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace wgc   = winrt::Windows::Graphics::Capture;
namespace wgdx  = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace ombra {

namespace {

std::wstring WideFromUtf8Local(const std::string& s) {
    if (s.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), count);
    return out;
}

std::string Utf8FromWideLocal(const std::wstring& s) {
    if (s.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), count, nullptr, nullptr);
    return out;
}

// "\\.\DISPLAY2" -> "DISPLAY2" -- same stripping ResolveCaptureMonitors (config.cpp) applies to
// MonitorSpec::label, kept in sync here rather than shared since config.cpp is a pure-JSON
// translator with no OS calls (by design, see its own comment) and can't call into capture.cpp.
std::string StripDisplayDevicePrefixLocal(const std::string& device) {
    const std::string prefix = "\\\\.\\";
    if (device.rfind(prefix, 0) == 0) return device.substr(prefix.size());
    return device;
}

// Resolves a \\.\DISPLAYn-style device name (or the literal "primary") to a
// live HMONITOR. "primary" makes exactly the one MonitorFromPoint() call the single-monitor
// path already made — no extra OS calls for the common (single-monitor) case.
HMONITOR ResolveMonitorByDevice(const std::string& device) {
    if (device.empty() || device == "primary")
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    struct Ctx {
        std::wstring want;
        HMONITOR      found = nullptr;
    } ctx{WideFromUtf8Local(device), nullptr};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lparam);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi) && mi.szDevice == c->want) {
                c->found = hmon;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found) return ctx.found;
    Log::Warn("capture target: monitor device \"%s\" not found -- using primary", device.c_str());
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

}  // namespace

HMONITOR MonitorForDevice(const std::string& device) { return ResolveMonitorByDevice(device); }

// Settings monitor picker (list_monitors pipe query, main.cpp). Unlike ResolveMonitorByDevice
// above (which stops at the first match), this collects every display so the UI can offer them
// all as toggles regardless of what capture.monitors currently contains.
std::vector<MonitorInfo> EnumerateAllMonitors() {
    std::vector<MonitorInfo> out;
    // The engine is DPI-unaware, which would report a scaled display's rect in 96-DPI units: the
    // picker shows real resolutions and lays the screens out where they are.
    const DPI_AWARENESS_CONTEXT prev_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
            auto* out_vec = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

            MonitorInfo info;
            info.device  = Utf8FromWideLocal(mi.szDevice);
            info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            info.label   = info.primary ? "Primary" : StripDisplayDevicePrefixLocal(info.device);
            info.left    = mi.rcMonitor.left;
            info.top     = mi.rcMonitor.top;
            info.width   = static_cast<uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left);
            info.height  = static_cast<uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
            out_vec->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    SetThreadDpiAwarenessContext(prev_dpi);

    // Primary first, matching ResolveCaptureMonitors' convention that entry 0 (unsuffixed output
    // filename) is the primary display.
    std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        return a.primary && !b.primary;
    });
    return out;
}

// Settings window-target picker (list_windows pipe query, main.cpp). Capped so a desktop with an
// unusual number of top-level windows doesn't bloat the pipe payload.
std::vector<WindowInfo> EnumerateCapturableWindows() {
    constexpr size_t kMaxWindows = 50;
    std::vector<WindowInfo> out;
    for (const WindowCandidate& w : SnapshotTargetWindows()) {
        if (out.size() >= kMaxWindows) break;
        if (w.title.empty()) continue;  // untitled windows aren't useful picks
        WindowInfo info;
        info.title          = Utf8FromWideLocal(w.title);
        info.image_path     = Utf8FromWideLocal(w.image_path);
        const size_t slash  = w.image_path.find_last_of(L'\\');
        info.process        = Utf8FromWideLocal(slash == std::wstring::npos ? w.image_path : w.image_path.substr(slash + 1));
        info.minimized      = w.minimized;
        info.foreground     = w.foreground;
        info.covers_monitor = w.covers_monitor;
        info.area           = w.area;
        info.hwnd           = w.id;
        out.push_back(std::move(info));
    }
    return out;
}

// Standalone one-shot capture, not a Capture method (see capture.h's
// comment on why). Every failure path below returns ok=false with a reason instead of die()/
// OMBRA_HR -- this is Option A's whole point: opening the picker must survive a missing NVIDIA
// adapter, a window/monitor that went away mid-grab, or WGC declining to hand back a frame.
//
// Shared tail of CaptureWindowThumbnailToPng/CaptureMonitorThumbnailToPng below: everything from
// "item resolved" onward (frame pool, one-shot grab, CPU downscale, PNG write) is identical between
// a window and a monitor target -- only how `item` itself gets constructed differs.
static ThumbnailResult GrabItemThumbnail(ID3D11Device* device, ID3D11DeviceContext* context,
                                          wgc::GraphicsCaptureItem item, uint32_t max_dim,
                                          const std::wstring& out_path) {
    ThumbnailResult result;

    // NVENC-style even rounding isn't required here (no encoder involved), kept anyway so the
    // frame pool and every downstream buffer agree on one convention with the rest of this file.
    auto           sz = item.Size();
    const uint32_t w  = static_cast<uint32_t>(sz.Width) & ~1u;
    const uint32_t h  = static_cast<uint32_t>(sz.Height) & ~1u;
    if (w < 2 || h < 2) {
        result.error = "capture item has no visible content area";
        return result;
    }

    winrt::com_ptr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgi_device.put_void()))) {
        result.error = "QueryInterface(IDXGIDevice) failed";
        return result;
    }
    winrt::com_ptr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()))) {
        result.error = "CreateDirect3D11DeviceFromDXGIDevice failed";
        return result;
    }
    auto rt_device = inspectable.as<wgd3d::IDirect3DDevice>();

    // A free-threaded frame pool delivers via FrameArrived on a WGC pool thread, same as
    // Capture::OnFrameArrived -- it does NOT hand back frames to a bare TryGetNextFrame() poll
    // with no subscription registered (that's how Capture::LatestSlot()/StartCapture() already
    // work, and the one-shot border check that proved this shape border-clean live went through
    // that same subscribed path, not a raw poll). Mirror it here with a
    // condition variable instead of Capture's slot machinery, which this one-shot grab has no use
    // for.
    struct OneShotFrame {
        std::mutex                  mtx;
        std::condition_variable     cv;
        wgc::Direct3D11CaptureFrame frame{nullptr};
        bool                        got = false;
    } frame_ctx;

    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession     session{nullptr};
    winrt::event_token              frame_token{};
    try {
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rt_device, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
            winrt::Windows::Graphics::SizeInt32{static_cast<int32_t>(w), static_cast<int32_t>(h)});
        frame_token = pool.FrameArrived(
            [&frame_ctx](wgc::Direct3D11CaptureFramePool const& sender,
                        winrt::Windows::Foundation::IInspectable const&) {
                auto f = sender.TryGetNextFrame();
                if (!f) return;
                std::lock_guard<std::mutex> lock(frame_ctx.mtx);
                if (!frame_ctx.got) {
                    frame_ctx.frame = f;
                    frame_ctx.got   = true;
                    frame_ctx.cv.notify_all();
                } else {
                    f.Close();  // already have the one frame this grab needs; drop extras
                }
            });
        session = pool.CreateCaptureSession(item);
        try { session.IsCursorCaptureEnabled(false); } catch (...) {}
        // Proven safe for exactly this one-shot shape by a since-removed test:
        // 5/5 iterations clean, no yellow border flash, live and in saved screenshots.
        try { session.IsBorderRequired(false); } catch (winrt::hresult_error const&) {}
        session.StartCapture();
    } catch (winrt::hresult_error const&) {
        result.error = "WGC session failed to start";
        if (pool && frame_token) pool.FrameArrived(frame_token);
        if (pool) pool.Close();
        return result;
    }

    wgc::Direct3D11CaptureFrame frame{nullptr};
    {
        std::unique_lock<std::mutex> lock(frame_ctx.mtx);
        frame_ctx.cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return frame_ctx.got; });
        frame = frame_ctx.frame;
    }
    if (pool && frame_token) pool.FrameArrived(frame_token);
    if (!frame) {
        result.error = "timed out waiting for a frame";
        session.Close();
        pool.Close();
        return result;
    }

    winrt::com_ptr<ID3D11Texture2D> src;
    {
        auto access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), src.put_void()))) {
            result.error = "GetInterface(ID3D11Texture2D) failed";
            frame.Close();
            session.Close();
            pool.Close();
            return result;
        }
    }

    // ContentSize(), not the pool/surface desc -- same reasoning as OnFrameArrived's resolution-
    // change detection below, minus the barrier/Recreate machinery this one-shot grab has no use
    // for. Clamped to [w,h] (the pool's own size) since a first frame can't legitimately report
    // content larger than the pool it was delivered into.
    const uint32_t content_w = (std::min)(static_cast<uint32_t>(frame.ContentSize().Width) & ~1u, w);
    const uint32_t content_h = (std::min)(static_cast<uint32_t>(frame.ContentSize().Height) & ~1u, h);
    const uint32_t cw        = (content_w >= 2) ? content_w : w;
    const uint32_t ch        = (content_h >= 2) ? content_h : h;

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width            = cw;
    sd.Height           = ch;
    sd.MipLevels        = 1;
    sd.ArraySize        = 1;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage            = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, staging.put()))) {
        result.error = "CreateTexture2D(staging) failed";
        frame.Close();
        session.Close();
        pool.Close();
        return result;
    }
    D3D11_BOX box{0, 0, 0, cw, ch, 1};
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src.get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        result.error = "Map(staging) failed";
        frame.Close();
        session.Close();
        pool.Close();
        return result;
    }
    std::vector<uint8_t> full_bgra(static_cast<size_t>(mapped.RowPitch) * ch);
    std::memcpy(full_bgra.data(), mapped.pData, full_bgra.size());
    const uint32_t full_row_pitch = mapped.RowPitch;
    context->Unmap(staging.get(), 0);

    frame.Close();
    session.Close();
    pool.Close();

    // CPU box/nearest downscale to at most max_dim on the longer side -- a one-shot picker
    // thumbnail, not a hot path (the budget is roughly ten tiles at a hundred-some ms
    // each), so no GPU blit/video-processor path is worth building for this.
    uint32_t out_w = cw, out_h = ch;
    if (max_dim > 0 && (cw > max_dim || ch > max_dim)) {
        if (cw >= ch) {
            out_w = max_dim;
            out_h = (std::max)(1u, ch * max_dim / cw);
        } else {
            out_h = max_dim;
            out_w = (std::max)(1u, cw * max_dim / ch);
        }
    }
    std::vector<uint8_t> small_bgra(static_cast<size_t>(out_w) * out_h * 4);
    for (uint32_t y = 0; y < out_h; ++y) {
        const uint32_t sy      = (std::min)(ch - 1, y * ch / out_h);
        const uint8_t* src_row = full_bgra.data() + static_cast<size_t>(sy) * full_row_pitch;
        uint8_t*       dst_row = small_bgra.data() + static_cast<size_t>(y) * out_w * 4;
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint32_t sx = (std::min)(cw - 1, x * cw / out_w);
            std::memcpy(dst_row + x * 4, src_row + sx * 4, 4);
        }
    }

    if (!WriteBgraPng(out_path, out_w, out_h, out_w * 4, small_bgra)) {
        result.error = "PNG encode failed";
        return result;
    }

    result.ok     = true;
    result.width  = out_w;
    result.height = out_h;
    return result;
}

// The thumbnail grabs' device: the default hardware adapter, whatever vendor. A one-shot grab has
// no encoder to share a device with, so there's no reason to tie it to the recording GPU. Never dies.
static bool CreateThumbnailDevice(winrt::com_ptr<ID3D11Device>& device,
                           winrt::com_ptr<ID3D11DeviceContext>& context, std::string& error) {
    D3D_FEATURE_LEVEL got_level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                  device.put(), &got_level, context.put()))) {
        error = "D3D11CreateDevice failed";
        return false;
    }
    return true;
}

ThumbnailResult CaptureWindowThumbnailToPng(HWND hwnd, uint32_t max_dim, const std::wstring& out_path) {
    ThumbnailResult result;
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        result.error = "window no longer available";
        return result;
    }

    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (!CreateThumbnailDevice(device, context, result.error)) return result;

    wgc::GraphicsCaptureItem item{nullptr};
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem>()
                           .as<IGraphicsCaptureItemInterop>();
        HRESULT hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                              winrt::put_abi(item));
        if (FAILED(hr) || !item) {
            result.error = "CreateForWindow failed -- window may have closed";
            return result;
        }
    } catch (winrt::hresult_error const&) {
        result.error = "CreateForWindow threw";
        return result;
    }

    return GrabItemThumbnail(device.get(), context.get(), item, max_dim, out_path);
}

ThumbnailResult CaptureMonitorThumbnailToPng(const std::string& device_str, uint32_t max_dim,
                                              const std::wstring& out_path) {
    ThumbnailResult result;
    HMONITOR hmon = ResolveMonitorByDevice(device_str);
    if (!hmon) {
        result.error = "monitor no longer available";
        return result;
    }

    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (!CreateThumbnailDevice(device, context, result.error)) return result;

    wgc::GraphicsCaptureItem item{nullptr};
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem>()
                           .as<IGraphicsCaptureItemInterop>();
        HRESULT hr = interop->CreateForMonitor(hmon, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                               winrt::put_abi(item));
        if (FAILED(hr) || !item) {
            result.error = "CreateForMonitor failed -- monitor may have been disconnected";
            return result;
        }
    } catch (winrt::hresult_error const&) {
        result.error = "CreateForMonitor threw";
        return result;
    }

    return GrabItemThumbnail(device.get(), context.get(), item, max_dim, out_path);
}

bool Capture::TryOpen(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) {
    nonfatal_open_ = true;
    return Open(capture_cursor, target, adapter);
}

// Open()'s own nonfatal-aware variants of die()/OMBRA_HR -- mirrors encoder.cpp's
// OMBRA_OPEN_FAIL/OMBRA_NVC_OPEN split (see encoder.h's TryOpen comment for why this distinction
// exists). Both macros assume they run inside a member function with `nonfatal_open_` and
// `fault_reason_` in scope, and both `return false` on the nonfatal path -- only valid inside
// Open() itself. Named CAPTURE_-prefixed rather than reusing encoder.cpp's OMBRA_OPEN_FAIL name:
// different translation units so it compiles either way, but a future header consolidation of
// these two open-failure macro pairs shouldn't have to rename one out from under the other.
#define CAPTURE_OPEN_FAIL(msg)                                           \
    do {                                                                 \
        if (!nonfatal_open_) die(msg);                                   \
        fault_reason_ = (msg);                                           \
        Log::Warn("capture open failed  %s", (msg));                   \
        return false;                                                    \
    } while (0)

#define CAPTURE_HR_OPEN(expr)                                            \
    do {                                                                 \
        HRESULT hr_ = (expr);                                            \
        if (FAILED(hr_)) {                                               \
            if (!nonfatal_open_) die_hr(#expr, hr_);                     \
            char hrbuf_[16];                                             \
            std::snprintf(hrbuf_, sizeof(hrbuf_), "0x%08lX",              \
                          static_cast<unsigned long>(hr_));               \
            fault_reason_ = std::string(#expr) + "  hr=" + hrbuf_;       \
            Log::Warn("capture open failed  %s  hr=%s", #expr, hrbuf_); \
            return false;                                                \
        }                                                                \
    } while (0)

bool Capture::Open(bool capture_cursor, const CaptureTarget& target, const LUID& adapter) {
    if (!wgc::GraphicsCaptureSession::IsSupported())
        CAPTURE_OPEN_FAIL("GraphicsCaptureSession::IsSupported() == false");

    // Resolve the target *before* touching the GPU at all (advisor review): a
    // strict Mode::Window with no match is a routine, retryable "not launched yet" condition, and
    // resolving first means the 30s-capped rebuild-retry loop never pays for a create-device/
    // destroy-device cycle on every attempt where the answer was always going to be "not found."
    HWND target_hwnd = nullptr;
    bool use_window   = false;
    if (target.mode == CaptureTarget::Mode::Window ||
        target.mode == CaptureTarget::Mode::WindowWithFallback) {
        const std::vector<WindowCandidate> windows = SnapshotTargetWindows();
        const int pick = ChooseTargetWindow(target.window, windows, target.preferred_window);
        if (pick >= 0) {
            const WindowCandidate& w = windows[static_cast<size_t>(pick)];
            target_hwnd = reinterpret_cast<HWND>(w.id);
            Log::InfoW(L"capture target window  %s  «%s»", w.image_path.c_str(), w.title.c_str());
        }
        if (target_hwnd) {
            use_window = true;
        } else if (target.mode == CaptureTarget::Mode::Window) {
            // Silent: logged once-per-transition by PipelineSupervisor (which persists across
            // rebuilds), not here (a fresh Capture every attempt has no "already logged" memory).
            // Deliberately leaves fault_reason_ empty -- BuildGeneration tells this routine "not
            // found yet" case apart from a real TryOpen() fault by that emptiness. If a third early
            // return like this is ever added, keep it that way or update BuildGeneration's check.
            return false;
        } else {
            Log::Info("capture target: no matching window -- capturing the monitor instead this generation");
        }
    }
    HMONITOR target_monitor = use_window ? nullptr : ResolveMonitorByDevice(target.monitor_device);

    // The encode adapter, not the default one: the encoder reads these stage textures directly, so
    // they have to live on its device. On a hybrid laptop the default adapter is often the other GPU.
    std::string device_error;
    if (!CreateD3D11DeviceOnAdapter(adapter, device_, context_, device_error))
        CAPTURE_OPEN_FAIL(device_error.c_str());
    // OnFrameArrived copies on a WGC pool thread under context_mutex_, but NVENC maps the stage
    // textures on the pacer thread through the same immediate context without that mutex. The
    // WinRT device wrapper below does not turn protection on (ombra-tests --gpu checks it); a
    // synthetic source racing NVENC this way deadlocked in the first encode.
    device_.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);

    auto interop =
        winrt::get_activation_factory<wgc::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    if (use_window) {
        HRESULT hr = interop->CreateForWindow(target_hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                              winrt::put_abi(item_));
        if (FAILED(hr)) {
            // Deliberately NOT OMBRA_HR/die() (advisor review): the window this call just matched
            // can still fail to be captured (protected content, or it closed in the gap between
            // the resolve above and here) — that is exactly the class of fault we
            // already treat as recoverable-at-runtime, not a reason to take the whole engine down.
            Log::Warn("CreateForWindow failed (hr=0x%08lX) for a matched window -- treating as "
                      "not found", static_cast<unsigned long>(hr));
            if (target.mode == CaptureTarget::Mode::Window) return false;
            use_window     = false;
            target_monitor = ResolveMonitorByDevice(target.monitor_device);
        }
    }
    if (!use_window) {
        CAPTURE_HR_OPEN(interop->CreateForMonitor(target_monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                           winrt::put_abi(item_)));
    }
    // Only set once use_window is final (the CreateForWindow-failure fallback above
    // can still flip it back to false after target_hwnd was resolved).
    target_hwnd_ = use_window ? target_hwnd : nullptr;

    // The "WGC item closed" fault (monitor unplugged, display topology change,
    // resolution change, or the captured window closing) — WGC signals this
    // itself rather than requiring us to poll for it. Confirmed to reuse the same fault path for a
    // window item as a monitor item (both are GraphicsCaptureItem.Closed): PipelineSupervisor's
    // existing recovery (capture->IsHealthy() -> TeardownGeneration -> RebuildWithBackoff -> a
    // fresh Capture::Open() re-resolving the target) needed no changes to work for window targets.
    item_closed_token_ = item_.Closed({this, &Capture::OnItemClosed});

    // NVENC wants even dimensions. The same rounding has to happen again on every Recreate() —
    // see OnFrameArrived's size-change detection, which rounds before comparing.
    auto sz = item_.Size();
    width_  = sz.Width & ~1u;
    height_ = sz.Height & ~1u;
    Log::Info("capture target    %s  %ux%u", use_window ? "window" : "monitor", width_.load(),
              height_.load());

    winrt::com_ptr<IDXGIDevice> dxgi_device;
    CAPTURE_HR_OPEN(device_->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgi_device.put_void()));
    winrt::com_ptr<IInspectable> inspectable;
    CAPTURE_HR_OPEN(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    rt_device_ = inspectable.as<wgd3d::IDirect3DDevice>();

    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        rt_device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
        winrt::Windows::Graphics::SizeInt32{static_cast<int32_t>(width_),
                                             static_cast<int32_t>(height_)});
    session_ = pool_.CreateCaptureSession(item_);
    try {
        session_.IsCursorCaptureEnabled(capture_cursor);
    } catch (...) {}
    bool border_off = false;
    try {
        session_.IsBorderRequired(false);  // Windows 11 only; throws on Win10
        border_off = true;
    } catch (winrt::hresult_error const&) {
    }
    Log::Info("capture border    %s", border_off ? "disabled" : "NOT disableable (Win10)");

    CAPTURE_HR_OPEN(CreateStageTextures());
    return true;
}

void Capture::SetCursorCaptureEnabled(bool enabled) {
    if (session_) {
        try {
            session_.IsCursorCaptureEnabled(enabled);
        } catch (...) {}
    }
}

// Allocates (or reallocates) the kSlots stage textures at the current width_/height_. Callers must
// hold context_mutex_ if anything else could be touching the slots — Open() runs before any frame
// can arrive; Recreate() takes the lock itself.
HRESULT Capture::CreateStageTextures() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = Width();
    td.Height           = Height();
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    stage_rtv_ = nullptr;  // FillStageColor's view is bound to the old stage_[0]
    for (auto& slot : stage_) slot = nullptr;
    for (auto& slot : stage_) {
        const HRESULT hr = device_->CreateTexture2D(&td, nullptr, slot.put());
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

bool Capture::Recreate(uint32_t w, uint32_t h) {
    // Drop frames for the duration: OnFrameArrived would otherwise copy an old-size frame into a
    // half-swapped slot set, and pool_.Recreate() below is called outside context_mutex_ (a WGC
    // callback taking that same lock while we hold it is a deadlock we don't need to risk).
    frames_suspended_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        width_.store(w, std::memory_order_release);
        height_.store(h, std::memory_order_release);
        if (const HRESULT hr = CreateStageTextures(); FAILED(hr)) {
            // Frames stay suspended: the slot set is half-built. The supervisor rebuilds instead.
            Log::Warn("capture recreate at %ux%u failed  hr=0x%08lX", w, h, static_cast<unsigned long>(hr));
            return false;
        }
        // The runtime and driver keep freed allocations cached for reuse, so without this the
        // old-size slots (and the encoder's surfaces, closed just before) stay resident: +45 MB GPU
        // and +54 MB private after one 1080p -> 1440p -> 1080p trip, measured by the gpu-soak
        // real-WGC round-trip test (2026-10-20). Trim requires ClearState first; the pacer is
        // stopped and the scaler binds its whole state per draw, so nothing relies on it.
        context_->ClearState();
        context_->Flush();
        if (winrt::com_ptr<IDXGIDevice3> dxgi; SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(dxgi.put())))) dxgi->Trim();
        write_slot_ = 0;
        latest_slot_.store(0, std::memory_order_release);
        encoding_slot_.store(kNoSlot, std::memory_order_release);
        has_frame_.store(false, std::memory_order_release);
    }
    // The watchdog calls this with no handler above it, and a device reset makes this WinRT call
    // throw: uncaught, that ends the engine instead of the rebuild a false return asks for.
    try {
        if (fail_next_pool_recreate_.exchange(false, std::memory_order_acq_rel))
            winrt::throw_hresult(DXGI_ERROR_DEVICE_RESET);
        pool_.Recreate(rt_device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                       winrt::Windows::Graphics::SizeInt32{static_cast<int32_t>(w),
                                                            static_cast<int32_t>(h)});
    } catch (winrt::hresult_error const& e) {
        Log::Warn("capture frame pool recreate at %ux%u failed  hr=0x%08lX", w, h,
                   static_cast<unsigned long>(static_cast<HRESULT>(e.code())));
        return false;  // frames stay suspended, as above
    }
    pending_resize_.Clear();
    frames_suspended_.store(false, std::memory_order_release);
    Log::Info("capture recreated at %ux%u", w, h);
    return true;
}

void Capture::OnItemClosed(wgc::GraphicsCaptureItem const&,
                            winrt::Windows::Foundation::IInspectable const&) {
    item_closed_.store(true, std::memory_order_release);
}

bool Capture::IsHealthy() const {
    if (item_closed_.load(std::memory_order_acquire)) return false;
    if (forced_device_removed_.load(std::memory_order_acquire)) return false;
    // GetDeviceRemovedReason() returns cached driver state (S_OK until the device actually goes
    // away) — no GPU round-trip, safe to call once per pacer tick.
    return device_ && device_->GetDeviceRemovedReason() == S_OK;
}

const char* Capture::HealthIssue() const {
    if (item_closed_.load(std::memory_order_acquire))
        return "WGC capture item closed (monitor unplugged, topology/resolution change)";
    if (forced_device_removed_.load(std::memory_order_acquire))
        return "device-removed injected by faulttest (synthetic, not a real TDR)";
    if (device_ && device_->GetDeviceRemovedReason() != S_OK)
        return "DXGI_ERROR_DEVICE_REMOVED/RESET";
    return "healthy";
}

void Capture::OnFrameArrived(wgc::Direct3D11CaptureFramePool const& sender,
                              winrt::Windows::Foundation::IInspectable const&) {
    auto frame = sender.TryGetNextFrame();
    if (!frame) return;  // pool woken spuriously or frame already drained
    if (frames_suspended_.load(std::memory_order_acquire)) {
        frame.Close();  // mid-Recreate: the slots this would copy into are being reallocated
        return;
    }

    winrt::com_ptr<ID3D11Texture2D> src;
    {
        auto access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        // A worker thread: die() here would take the engine down for one bad frame.
        if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), src.put_void()))) {
            frame.Close();
            return;
        }
    }
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    // Resolution change: detect via Direct3D11CaptureFrame::ContentSize(), not the surface desc:
    // WGC allocates surfaces at
    // the *pool's* size and places the content inside them, so on a monitor resolution change the
    // desc can stay put while ContentSize moves (this is what robmikh/Win32CaptureSample keys
    // Recreate off). Both are logged on the first change so a hand-off run can confirm which one
    // actually moves on real hardware rather than taking the docs' word for it. Even-rounded before
    // comparing, or a 1919-wide report would flap against a 1918-wide capture forever.
    const uint32_t forced_w  = forced_content_w_.load(std::memory_order_acquire);
    const uint32_t forced_h  = forced_content_h_.load(std::memory_order_acquire);
    const uint32_t content_w = forced_w ? forced_w
                                        : static_cast<uint32_t>(frame.ContentSize().Width) & ~1u;
    const uint32_t content_h = forced_h ? forced_h
                                        : static_cast<uint32_t>(frame.ContentSize().Height) & ~1u;
    const uint32_t cur_w = Width(), cur_h = Height();
    // Logged once per change, not once per frame: this runs on the WGC pool thread, not the pacer's
    // per-tick path, but every frame until the supervisor's 200ms poll notices would still be a flood.
    // Either way the frame is still copied: it is clamped to the old slot size below, so it is a
    // cropped-but-valid picture, and dropping frames until the supervisor reacts would show up as a
    // freeze at the head of the *pre*-barrier segment for no benefit.
    switch (pending_resize_.OnFrame(content_w, content_h, cur_w, cur_h)) {
        case PendingResize::Step::Started:
            Log::Info("capture size change  content=%ux%u  desc=%ux%u  current=%ux%u", content_w,
                      content_h, sd.Width, sd.Height, cur_w, cur_h);
            break;
        case PendingResize::Step::Reverted:
            Log::Info("capture size change withdrawn  back at %ux%u", cur_w, cur_h);
            break;
        case PendingResize::Step::None:
            break;
    }

    // Slot selection and the copy must be one critical section: two overlapping FrameArrived
    // invocations computing the same next_slot before either copies would defeat the multi-slot
    // design (two writers, one texture — the torn-frame case slots exist to prevent).
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        // Again under the lock: a Recreate may have swapped the slots since the check at the top, and
        // a box sized for the old ones would overrun smaller new ones.
        if (frames_suspended_.load(std::memory_order_acquire)) {
            frame.Close();
            return;
        }
        const D3D11_BOX box{0, 0, 0, (std::min)(sd.Width, Width()), (std::min)(sd.Height, Height()), 1};
        uint32_t next_slot = (write_slot_ + 1) % kSlots;
        // Never write into the slot the encoder is currently reading — a synchronous encode takes
        // several ms, and WGC can deliver frames inside it. Skip past it; only one slot can be
        // encoding_slot_ at a time, so with kSlots >= 2 this always finds a free slot in one step
        // (with 2, frames arriving mid-encode all rewrite the other slot).
        static_assert(kSlots >= 2, "need at least one slot free of the one being encoded");
        if (next_slot == encoding_slot_.load(std::memory_order_acquire))
            next_slot = (next_slot + 1) % kSlots;
        context_->CopySubresourceRegion(stage_[next_slot].get(), 0, 0, 0, 0, src.get(), 0, &box);
        write_slot_ = next_slot;
        ++frames_published_;
        latest_slot_.store(next_slot, std::memory_order_release);
        has_frame_.store(true, std::memory_order_release);
    }
    frame.Close();
}

void Capture::StartCapture() {
    frame_arrived_token_ = pool_.FrameArrived({this, &Capture::OnFrameArrived});
    session_.StartCapture();
}

ICaptureSource::SlotClaim Capture::ClaimSlotForEncoding() {
    std::lock_guard<std::mutex> lock(context_mutex_);
    const uint32_t slot = latest_slot_.load(std::memory_order_relaxed);
    encoding_slot_.store(slot, std::memory_order_release);
    return {slot, frames_published_};
}

void Capture::FillStageColor(const float rgba[4]) {
    std::lock_guard<std::mutex> lock(context_mutex_);
    if (!stage_rtv_) OMBRA_HR(device_->CreateRenderTargetView(stage_[0].get(), nullptr, stage_rtv_.put()));
    context_->ClearRenderTargetView(stage_rtv_.get(), rgba);
}

void Capture::ReadSlotRegionBGRA(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  std::vector<uint8_t>& out_bgra, uint32_t& out_row_pitch) {
    std::lock_guard<std::mutex> lock(context_mutex_);
    out_bgra.clear();
    out_row_pitch = 0;
    if (!readback_staging_ || readback_w_ != w || readback_h_ != h) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width            = w;
        sd.Height           = h;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage            = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        readback_staging_ = nullptr;
        readback_w_ = 0;
        readback_h_ = 0;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, readback_staging_.put()))) return;
        readback_w_ = w;
        readback_h_ = h;
    }
    D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    context_->CopySubresourceRegion(readback_staging_.get(), 0, 0, 0, 0, stage_[slot].get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(readback_staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    out_bgra.resize(static_cast<size_t>(mapped.RowPitch) * h);
    std::memcpy(out_bgra.data(), mapped.pData, out_bgra.size());
    out_row_pitch = mapped.RowPitch;
    context_->Unmap(readback_staging_.get(), 0);
}

void Capture::ReleaseReadbackStaging() {
    std::lock_guard<std::mutex> lock(context_mutex_);
    readback_staging_ = nullptr;
    readback_w_ = 0;
    readback_h_ = 0;
}

void Capture::Close() {
    // Token members are zeroed after revoking so a second Close() call (the destructor now always
    // makes one, on top of every explicit caller -- see ~Capture()'s comment) doesn't re-revoke an
    // already-removed WinRT event registration on every normal teardown, not just the rare
    // nonfatal-Open()-failure path the destructor exists for.
    if (item_ && item_closed_token_) item_.Closed(item_closed_token_);
    item_closed_token_ = {};
    if (pool_ && frame_arrived_token_) pool_.FrameArrived(frame_arrived_token_);
    frame_arrived_token_ = {};
    if (session_) session_.Close();
    if (pool_) pool_.Close();
    target_hwnd_ = nullptr;
}

}  // namespace ombra
