#include "ombra-ui/overlay_window.h"
#include "ombra-ui/monitor_adapter.h"
#include "ombra-ui/ui/motion.h"

#include <cstring>

#include <chrono>
#include "ombra-ui/ui/attached_panel.h"
#include "ombra-ui/ui/paint.h"
#include "ombra/brand.h"
#include "ombra/brand_mark.h"
#include "ombra/crash.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/protocol.h"
#include "ombra/resource.h"
#include "ombra/util.h"
#include "ombra/version.h"

#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ombra_ui {

namespace {

const wchar_t* kOverlayWindowClass = ombra::kOverlayWindowClassW;

// Defined further down this file (BuildAutotuneModal's helpers); forward-declared here so
// RequestWindowListAsync's worker-thread lambda (needs it well before that point) can use it too.
std::wstring Utf8ToWideLocal(const std::string& s);

// RequestWindowListAsync's worker thread marshals its result back onto the UI thread with this
// message, never by touching OverlayHost
// state or calling RequestRepaint()/on_result directly from the worker thread itself (RequestRepaint
// calls RenderFrame() synchronously against the D2D device context -- a cross-thread call into that
// is a data race, not just a style nit).
constexpr UINT kWmWindowListResult = WM_APP + 1;

struct WindowListResultMsg {
    uint64_t                                       generation;
    std::vector<WindowEntry>                       windows;
    std::function<void(std::vector<WindowEntry>)>  on_result;
};

// RequestMonitorListAsync's result. Every reply reaches its own caller (overlay_window.h).
constexpr UINT kWmMonitorListResult = WM_APP + 3;

struct MonitorListResultMsg {
    std::vector<MonitorEntry>                       monitors;
    std::function<void(std::vector<MonitorEntry>)>  on_result;
};

// RequestMonitorThumbnailsAsync's result -- one message per device as its fetch completes, no
// generation (see overlay_window.h's comment on monitor_thumb_worker_ for why); dropped only if the
// overlay itself closed while the request was in flight (IsWindow(hwnd) check in the worker below).
constexpr UINT kWmMonitorThumbReady = WM_APP + 4;

struct MonitorThumbReadyMsg {
    std::string                                                                device;
    bool                                                                       ok = false;
    UINT                                                                       width = 0;
    UINT                                                                       height = 0;
    std::vector<uint8_t>                                                       bgra;
    std::function<void(std::string, bool, UINT, UINT, std::vector<uint8_t>)>   on_result;
};

// RequestProcessListAsync's result, same generation-guarded shape as MonitorListResultMsg above.
constexpr UINT kWmProcessListResult = WM_APP + 5;

struct ProcessListResultMsg {
    uint64_t                                        generation;
    std::vector<ProcessEntry>                       processes;
    std::function<void(std::vector<ProcessEntry>)>  on_result;
};

// RequestAudioSessionListAsync's result, same generation-guarded shape.
constexpr UINT kWmAudioSessionListResult = WM_APP + 6;
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT_PTR kMeterTimerId = 2;  // the Audio page's level meters
constexpr UINT kMeterTimerMs = 33;
constexpr UINT kWmFrameTick = WM_APP + 12;  // SetFrameTimer's compositor-clock worker

struct AudioSessionListResultMsg {
    uint64_t                                             generation;
    std::vector<AudioSessionEntry>                       sessions;
    std::function<void(std::vector<AudioSessionEntry>)>  on_result;
};

// RequestAudioDeviceListAsync's result, same generation-guarded shape (Settings' Desktop track
// device picker).
constexpr UINT kWmAudioDeviceListResult = WM_APP + 7;

struct AudioDeviceListResultMsg {
    uint64_t                                            generation;
    std::vector<AudioDeviceEntry>                       devices;
    std::function<void(std::vector<AudioDeviceEntry>)>  on_result;
};

// RequestAsync's result -- deliberately no generation (overlay_window.h's comment above
// the worker members: every reply must be applied, none are supersede-and-drop).
constexpr UINT kWmSetConfigResult = WM_APP + 8;

struct SetConfigResultMsg {
    std::optional<ombra::JsonValue>                            resp;
    std::function<void(std::optional<ombra::JsonValue>)>       on_result;
};

// RequestPreviewAsync's result -- same no-generation reasoning as SetConfigResultMsg above.
constexpr UINT kWmPreviewResult = WM_APP + 9;

struct PreviewResultMsg {
    std::optional<ombra::JsonValue> preview_resp;
    std::optional<ombra::JsonValue> status_resp;
    std::function<void(std::optional<ombra::JsonValue>, std::optional<ombra::JsonValue>)> on_result;
};

// RequestFileIconsAsync's result: every icon in one message.
constexpr UINT kWmFileIconsResult = WM_APP + 10;

struct FileIconsResultMsg {
    std::vector<FileIcon>                         icons;
    std::function<void(std::vector<FileIcon>)>    on_result;
};

// UiPoster: a heap std::function in lParam, run and freed by HandleMessage.
constexpr UINT kWmRunOnUi = WM_APP + 11;

// Self-contained PNG decode for the thumbnail worker thread: its own WIC factory instance (never
// OverlayHost::wic_factory_, which is created on and affine to the UI thread) so this can run on a
// detached worker with no cross-thread COM object sharing at all. Mirrors ombra/screenshot.cpp's
// ReadPngAsBgra shape, but that file isn't linked into ombra-ui, so this is its own small copy
// rather than a shared call.
bool DecodePngToBgraLocal(const std::wstring& path, UINT& out_w, UINT& out_h,
                          std::vector<uint8_t>& out_bgra, HRESULT* out_hr = nullptr) {
    HRESULT hr_sink = S_OK;
    HRESULT& hr = out_hr ? *out_hr : hr_sink;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory)))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) return false;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                          nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    UINT w = 0, h = 0;
    if (FAILED(hr = converter->GetSize(&w, &h))) return false;
    if (w == 0 || h == 0) return false;
    const UINT stride = w * 4;
    out_bgra.resize(static_cast<size_t>(stride) * h);
    if (FAILED(hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out_bgra.size()), out_bgra.data())))
        return false;
    out_w = w;
    out_h = h;
    return true;
}

// Panel sizing: a percentage of the monitor's own pixel width replaces the fixed-DIP widths
// (Home 860, Settings 1180), clamped
// to a DIP min/max so
// a 1080p panel doesn't shrink below legibility and a 4K/ultrawide one doesn't sprawl. `view_w` is
// already in device pixels (see ComputePanelBounds), so the percentage is taken directly against it
// -- no DIP<->pixel conversion in that step, only the min/max clamp bounds get scaled by
// dpi_scale_, exactly like the old fixed-width constants did. The width change still snaps rather
// than animating between Home and Settings.
constexpr float kHomeWidthPercent = 0.60f;
constexpr float kHomeMinWidthDip  = 700.0f;
// Deliberately loose -- this test renders 4K at dpi_scale_==1.0 (no OS DPI scaling applied, unlike
// almost every real 4K monitor, which runs above 100%), so a tight cap here made the row look
// small at that resolution. 1900 keeps the row
// close to the full 60% target up through 4K-at-100%-scaling while still capping runaway width on
// a genuine 8K/super-ultrawide monitor.
constexpr float kHomeMaxWidthDip  = 1900.0f;
// Narrowed from 0.60 / 820-1320: the old target read as too wide on a
// 1080p-class monitor, leaving the content card mostly empty padding around its rows. Not narrowed
// further than this -- capture_target_row's help text (config.h's capture.target string, the
// longest row help text in Settings) wraps to a 3rd line below ~1150dip content width, which
// overflows the fixed 84dip tall-row height (screens.cpp:1312) into the row underneath it.
constexpr float kSettingsWidthPercent = 0.52f;
constexpr float kSettingsMinWidthDip  = 780.0f;
constexpr float kSettingsMaxWidthDip  = 1150.0f;

// Home's body height scales off the panel's own (already-clamped) width rather than view height
// directly -- ties tile shape to panel width consistently across very different monitor aspect
// ratios (ultrawide vs. 4K vs. 1080p) instead of two independently-clamped percentages fighting
// each other.
constexpr float kHomeBodyAspect  = 0.34f;
constexpr float kHomeBodyMinDip  = 220.0f;
constexpr float kHomeBodyMaxDip  = 620.0f;  // matches kHomeMaxWidthDip's headroom, same reasoning

// Settings' rail+card row height cap: a taller window left empty space below the rail's icons.
constexpr float kSettingsRowMaxHeightDip = 560.0f;
constexpr float kClipsWidthPercent = 0.62f;
constexpr float kClipsMaxWidthDip  = 1400.0f;

// Minimum clearance between the floating header's bottom edge and the content row below it, used
// by both Position::Top (the row's actual top) and Position::Center (a floor so a very tall
// centered row can't be pushed up under the header) -- matches DrawFloatingHeader's own margin
// (32) + pill height (38 + 4*2) + a 24dip gap.
constexpr float kContentMinTopDip = 32.0f + (38.0f + 4.0f * 2.0f) + 24.0f;

bool SaveBgraToPng(IWICImagingFactory* wic, int width, int height, const uint8_t* bgra_data,
                   size_t row_pitch, const std::wstring& out_path) {
    if (!wic || !bgra_data) return false;

    ComPtr<IWICStream> stream;
    HRESULT hr = wic->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(out_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return false;

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return false;

    const UINT total_bytes = static_cast<UINT>(row_pitch * height);
    hr = frame->WritePixels(height, static_cast<UINT>(row_pitch), total_bytes, const_cast<BYTE*>(bgra_data));
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

}  // namespace

bool DecodeThumbnailPng(const std::wstring& path, UINT& out_w, UINT& out_h, std::vector<uint8_t>& out_bgra) {
    return DecodePngToBgraLocal(path, out_w, out_h, out_bgra);
}

OverlayHost::OverlayHost() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wic_factory_));

    home_screen_ = std::make_unique<HomeScreen>(this);
    settings_screen_ = std::make_unique<SettingsScreen>(this);
    clips_screen_ = std::make_unique<ClipsScreen>(this);
    setup_screen_ = std::make_unique<SetupScreen>(this);

    for (IconButtonControl* b : {&gear_btn_, &back_btn_, &close_btn_}) b->glyph = IconButtonControl::Glyph::Icon;
    gear_btn_.icon = IconType::Settings;
    gear_btn_.on_click = [this]() { SwitchScreen(ActiveScreen::Settings); };

    back_btn_.icon = IconType::Back;
    back_btn_.on_click = [this]() { SwitchScreen(ActiveScreen::Home); };

    close_btn_.icon = IconType::Close;
    close_btn_.on_click = [this]() { CloseOverlay(); };
}

OverlayHost::~OverlayHost() {
    Hide();
    // The process ends right after this: let a trim being written finish (it renames into place
    // only when complete, so a cut-off one leaves just a .part file).
    if (clips_screen_) clips_screen_->WaitForWrites(15000);
    {
        std::lock_guard<std::mutex> lock(hotkey_suspend_mutex_);
        hotkey_suspend_stop_ = true;
    }
    hotkey_suspend_cv_.notify_all();
    if (hotkey_suspend_worker_.joinable()) hotkey_suspend_worker_.join();
    SetFrameTimer(false);
    if (frame_clock_stop_) CloseHandle(frame_clock_stop_);

    // Release every COM interface this object holds before uninitializing the apartment --
    // Release()ing a COM object after CoUninitialize() is undefined behavior (member
    // destructors run after this body, which would otherwise release them post-uninit).
    home_screen_.reset();
    settings_screen_.reset();
    clips_screen_.reset();  // its player holds the D3D11 device
    active_modal_.reset();
    wic_factory_.Reset();
    dcomp_target_.Reset();
    dcomp_visual_.Reset();
    dcomp_device_.Reset();
    dwrite_factory_.Reset();
    d2d_target_bitmap_.Reset();
    logo_bmp_.Reset();
    d2d_context_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    swapchain_.Reset();
    dxgi_factory_.Reset();
    dxgi_device_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();

    CoUninitialize();
}

namespace {
bool g_software_rendering = false;

LPCWSTR CursorResource(CursorKind c) {
    switch (c) {
        case CursorKind::Hand:  return IDC_HAND;
        case CursorKind::Move:  return IDC_SIZEALL;
        case CursorKind::IBeam: return IDC_IBEAM;
        default:                return IDC_ARROW;
    }
}
}  // namespace


void SetSoftwareRendering(bool on) { g_software_rendering = on; }
bool SoftwareRendering() { return g_software_rendering; }

bool OverlayHost::InitGraphics(HMONITOR mon) {
    if (d3d_device_) return true;

    UINT d3d_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    d3d_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL created_level;

    // The GPU that drives the overlay's monitor, else the default one (see monitor_adapter.h).
    ComPtr<IDXGIAdapter1> monitor_adapter = SoftwareRendering() ? nullptr : AdapterForMonitor(mon);
    auto create = [&](IDXGIAdapter* adapter, UINT flags) {
        const D3D_DRIVER_TYPE type = adapter              ? D3D_DRIVER_TYPE_UNKNOWN
                                     : SoftwareRendering() ? D3D_DRIVER_TYPE_WARP
                                                           : D3D_DRIVER_TYPE_HARDWARE;
        return D3D11CreateDevice(adapter, type, nullptr, flags, feature_levels, ARRAYSIZE(feature_levels),
                                 D3D11_SDK_VERSION, &d3d_device_, &created_level, &d3d_context_);
    };

    // Video support lets the Clips preview decode on this same device (ClipPlayer). A device that
    // can't have it still draws the overlay; only the preview is lost.
    HRESULT hr = E_FAIL;
    if (monitor_adapter) {
        hr = create(monitor_adapter.Get(), d3d_flags | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
        if (FAILED(hr)) hr = create(monitor_adapter.Get(), d3d_flags);
        else video_device_ = true;
    }
    const bool on_monitor_gpu = SUCCEEDED(hr);
    if (FAILED(hr)) {
        hr = create(nullptr, d3d_flags | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
        video_device_ = SUCCEEDED(hr);
        if (FAILED(hr)) hr = create(nullptr, d3d_flags);
    }

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, d3d_flags,
            feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
            &d3d_device_, &created_level, &d3d_context_);
        if (FAILED(hr)) {
            ombra::Log::Error("OverlayHost: D3D11CreateDevice failed (0x%08X)", hr);
            return false;
        }
    }

    hr = d3d_device_.As(&dxgi_device_);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device_->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory_));
    if (FAILED(hr)) return false;
    if (DXGI_ADAPTER_DESC ad{}; SUCCEEDED(adapter->GetDesc(&ad))) {
        ombra::Log::Info("OverlayHost: drawing on %ls (%s)", ad.Description,
                         on_monitor_gpu ? "the monitor's GPU" : "default adapter");
    }

    D2D1_FACTORY_OPTIONS d2d_opts{};
#if defined(_DEBUG)
    d2d_opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2d_opts,
                           reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: D2D1CreateFactory failed (0x%08X)", hr);
        return false;
    }

    hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_);
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: D2D1Factory::CreateDevice failed (0x%08X)", hr);
        return false;
    }

    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context_);
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: CreateDeviceContext failed (0x%08X)", hr);
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: DWriteCreateFactory failed (0x%08X)", hr);
        return false;
    }

    theme_.InitFonts(dwrite_factory_.Get());
    logo_bmp_.Reset();

    hr = DCompositionCreateDevice(dxgi_device_.Get(), __uuidof(IDCompositionDevice),
                                  reinterpret_cast<void**>(dcomp_device_.GetAddressOf()));
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: DCompositionCreateDevice failed (0x%08X)", hr);
        return false;
    }

    return true;
}

bool OverlayHost::CreateHostWindow(const RECT& monitor_rect) {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayHost::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kOverlayWindowClass;
    RegisterClassExW(&wc);

    const int x = monitor_rect.left;
    const int y = monitor_rect.top;
    const int w = monitor_rect.right - monitor_rect.left;
    const int h = monitor_rect.bottom - monitor_rect.top;

    DWORD ex_style = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    DWORD style = WS_POPUP;

    hwnd_ = CreateWindowExW(
        ex_style, kOverlayWindowClass, L"" OMBRA_PRODUCT_NAME " Overlay", style,
        x, y, w, h, nullptr, nullptr, inst, this);

    if (!hwnd_) {
        ombra::Log::Error("OverlayHost: CreateWindowExW failed (0x%08X)", GetLastError());
        return false;
    }

    // Screens' live status (the Audio page's per-source tags). Dies with the window on hide.
    SetTimer(hwnd_, kStatusTimerId, 1000, nullptr);
    // Hide stopped the meters; a reopened Audio page starts them again without a rebuild.
    if (settings_screen_ && active_screen_ == ActiveScreen::Settings) settings_screen_->SyncMeters();

    bounds_ = monitor_rect;
    dpi_ = static_cast<int>(GetDpiForWindow(hwnd_));
    dpi_scale_ = static_cast<float>(dpi_) / 96.0f;
    theme_.SetFontScale(dpi_scale_);
    return true;
}

bool OverlayHost::CreateSwapchainAndDComp(int width, int height) {
    if (!dxgi_factory_ || !dcomp_device_ || !hwnd_) return false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.Stereo = FALSE;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = dxgi_factory_->CreateSwapChainForComposition(
        d3d_device_.Get(), &scd, nullptr, &swapchain_);
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: CreateSwapChainForComposition failed (0x%08X)", hr);
        return false;
    }

    hr = dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_);
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: CreateTargetForHwnd failed (0x%08X)", hr);
        return false;
    }

    hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    if (FAILED(hr)) return false;

    hr = dcomp_visual_->SetContent(swapchain_.Get());
    if (FAILED(hr)) return false;

    hr = dcomp_target_->SetRoot(dcomp_visual_.Get());
    if (FAILED(hr)) return false;

    hr = dcomp_device_->Commit();
    if (FAILED(hr)) return false;

    ComPtr<IDXGISurface> surface;
    hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) return false;

    // DPI is fixed at 96 here on purpose -- dpi_scale_ (monitor DPI / 96) is the *only* scaling
    // mechanism, applied manually at every widget draw call. Setting this to the real monitor
    // DPI as well would double-scale everything on a high-DPI display: D2D would auto-scale its
    // logical-unit coordinate space AND every draw call would separately multiply by dpi_scale_.
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    hr = d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &d2d_target_bitmap_);
    if (FAILED(hr)) {
        ombra::Log::Error("OverlayHost: CreateBitmapFromDxgiSurface failed (0x%08X)", hr);
        return false;
    }

    d2d_context_->SetTarget(d2d_target_bitmap_.Get());
    return true;
}

void OverlayHost::RequestRepaint() {
    RenderFrame();
}

// How long the UI thread waits for a fetch before carrying on without it. A healthy engine answers
// well inside this, so the first frame has its data; a stalled one costs this much, not 3 s a request.
constexpr DWORD kEngineGraceMs = 300;

struct OverlayHost::EngineFetch {
    std::mutex              mutex;
    std::condition_variable done_cv;
    bool                    done = false;
    bool                    abandoned = false;  // the UI thread stopped waiting; deliver through hwnd
    HWND                    hwnd = nullptr;     // null until the window exists (Show)
    EngineReplies           replies;
    std::function<void(EngineReplies)> on_done;
};

namespace {
// Called with fetch->mutex held; delivered through the handler every async reply uses. A window that
// has closed meanwhile drops it.
void PostLateFetch(HWND hwnd, std::shared_ptr<OverlayHost::EngineFetch> fetch) {
    auto* msg = new SetConfigResultMsg{std::nullopt, [fetch](std::optional<ombra::JsonValue>) {
        fetch->on_done(std::move(fetch->replies));
    }};
    if (!IsWindow(hwnd) || !PostMessageW(hwnd, kWmSetConfigResult, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
}
}  // namespace

bool OverlayHost::FetchWithGrace(std::vector<const char*> types, std::function<void(EngineReplies)> on_done) {
    auto fetch = std::make_shared<EngineFetch>();
    fetch->on_done = std::move(on_done);
    std::thread([fetch, types] {
        ombra::Log::SetThreadName("engine-fetch");
        EngineReplies replies(types.size());
        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            for (size_t i = 0; i < types.size(); ++i) {
                ombra::JsonValue req = ombra::JsonValue::MakeObject();
                req.Set("type", ombra::JsonValue::MakeStr(types[i]));
                replies[i] = worker_client.Request(std::move(req), 3000);
                if (!replies[i]) break;  // stalled or gone: the rest would only wait as long again
            }
        }
        worker_client.Close();

        std::unique_lock<std::mutex> lock(fetch->mutex);
        fetch->replies = std::move(replies);
        fetch->done = true;
        fetch->done_cv.notify_one();
        if (fetch->abandoned && fetch->hwnd) PostLateFetch(fetch->hwnd, fetch);
    }).detach();

    std::unique_lock<std::mutex> lock(fetch->mutex);
    if (fetch->done_cv.wait_for(lock, std::chrono::milliseconds(kEngineGraceMs), [&] { return fetch->done; })) {
        EngineReplies replies = std::move(fetch->replies);
        lock.unlock();
        fetch->on_done(std::move(replies));
        return true;
    }
    fetch->abandoned = true;
    fetch->hwnd = hwnd_;
    if (!hwnd_) unattached_fetch_ = fetch;
    return false;
}

// A fetch abandoned before the window existed: the reply has somewhere to go now.
void OverlayHost::AttachUnattachedFetch() {
    if (!unattached_fetch_) return;
    const std::shared_ptr<EngineFetch> fetch = std::move(unattached_fetch_);
    unattached_fetch_ = nullptr;
    std::lock_guard<std::mutex> lock(fetch->mutex);
    fetch->hwnd = hwnd_;
    if (fetch->done) PostLateFetch(hwnd_, fetch);
}

void OverlayHost::ActivateScreen(ActiveScreen screen) {
    if (active_screen_ == ActiveScreen::Clips && screen != ActiveScreen::Clips && clips_screen_) clips_screen_->Leave();
    // The setup just wrote the config Settings loaded when the overlay opened. A reply that comes
    // after the grace applies only if nobody has moved on, or it would replace what they're editing.
    if (active_screen_ == ActiveScreen::Setup && screen != ActiveScreen::Setup && settings_screen_ && client_ && client_->Connected()) {
        const uint64_t gen = switch_generation_;
        FetchWithGrace({"get_schema", "get_config"}, [this, gen](EngineReplies replies) {
            if (gen != switch_generation_ || !settings_screen_ || !replies[0]) return;
            settings_screen_->InitSchema(ParseSchema(*replies[0]), replies[1] ? replies[1]->Find("config") : nullptr);
            RequestRepaint();
        });
    }
    // The Audio page's meters tick only while Settings shows (its page decides the rest).
    if (settings_screen_ && screen != active_screen_) {
        if (screen == ActiveScreen::Settings) settings_screen_->SyncMeters();
        else if (active_screen_ == ActiveScreen::Settings) settings_screen_->StopMeters();
    }
    active_screen_ = screen;
}

void OverlayHost::SwitchScreen(ActiveScreen screen) {
    const uint64_t gen = ++switch_generation_;
    // The setup's first frame needs its recommendation, so it opens when the answers are in. With the
    // engine stalled the screen stays where it is meanwhile, responsive, Esc included.
    if (screen == ActiveScreen::Setup && setup_screen_ && client_ && client_->Connected()) {
        FetchWithGrace({"get_config", "get_status"}, [this, gen](EngineReplies replies) {
            if (gen != switch_generation_ || !setup_screen_) return;
            ActivateScreen(ActiveScreen::Setup);
            setup_screen_->Open(replies[1] ? &*replies[1] : nullptr, replies[0] ? &*replies[0] : nullptr);
            RequestRepaint();
        });
        return;
    }
    ActivateScreen(screen);
    if (screen == ActiveScreen::Clips && clips_screen_ && home_screen_) clips_screen_->Open(home_screen_->OutputFolder());
    if (screen == ActiveScreen::Setup && setup_screen_) setup_screen_->Open(nullptr, nullptr);

    // Settings can change values (buffer.seconds, hotkeys, ...) that Home displays -- Home only
    // ever saw the config/status snapshot Show() fetched at overlay-open time, so without this a
    // value changed in Settings still shows its old number on Home until the overlay is reopened.
    // Asynchronous: with the engine stalled or gone, a blocking request froze the overlay for seconds,
    // Esc included. No answer says so rather than leaving Home claiming a replay is ready.
    if (screen == ActiveScreen::Home && home_screen_ && client_) {
        const uint64_t gen = ++home_refresh_generation_;
        ombra::JsonValue config_req = ombra::JsonValue::MakeObject();
        config_req.Set("type", ombra::JsonValue::MakeStr("get_config"));
        RequestAsync(std::move(config_req), 3000, [this, gen](std::optional<ombra::JsonValue> resp) {
            if (gen != home_refresh_generation_ || !resp || !home_screen_) return;
            home_screen_->UpdateState(nullptr, &*resp);
            RequestRepaint();
        });
        ombra::JsonValue status_req = ombra::JsonValue::MakeObject();
        status_req.Set("type", ombra::JsonValue::MakeStr("get_status"));
        RequestAsync(std::move(status_req), 3000, [this, gen](std::optional<ombra::JsonValue> resp) {
            if (gen != home_refresh_generation_ || !home_screen_) return;
            if (resp) home_screen_->UpdateState(&*resp, nullptr);
            else home_screen_->MarkEngineUnavailable();
            RequestRepaint();
        });
    }

    RequestRepaint();
}

void OverlayHost::ShowModal(const std::wstring& title, const std::wstring& msg, bool is_danger,
                            std::function<void()> on_confirm, std::function<void()> on_cancel,
                            ModalButtons buttons) {
    active_modal_ = std::make_unique<ModalDialog>();
    active_modal_->title = title;
    active_modal_->confirm_text = std::move(buttons.confirm);
    active_modal_->cancel_text = std::move(buttons.cancel);
    active_modal_->message = msg;
    active_modal_->is_danger = is_danger;
    active_modal_->visible = true;
    active_modal_->on_confirm = std::move(on_confirm);
    active_modal_->on_cancel = std::move(on_cancel);
    RequestRepaint();
}

void OverlayHost::ProcessModalResult() {
    if (!active_modal_) return;
    const ModalDialog::Result result = active_modal_->last_result;
    if (result == ModalDialog::Result::None) return;

    // Move the callback out and destroy the modal *before* invoking it -- do_apply-style
    // callbacks (SettingsScreen::ApplySettingChange) can themselves show another modal or repaint,
    // and active_modal_ must already be null/reassignable when that happens.
    std::function<void()> cb = (result == ModalDialog::Result::Confirm) ? std::move(active_modal_->on_confirm)
                                                                         : std::move(active_modal_->on_cancel);
    active_modal_.reset();
    if (cb) cb();
    RequestRepaint();
}

void OverlayHost::ShowDropdown(const D2D1_RECT_F& anchor, const std::vector<std::wstring>& options,
                               int selected, std::function<void(int)> on_select, std::function<void()> on_close) {
    auto menu = std::make_unique<MenuPopup>();
    menu->options = options;
    menu->selected = selected;
    menu->Place(anchor, D2D1::RectF(0.0f, 0.0f, static_cast<float>(bounds_.right - bounds_.left),
                                    static_cast<float>(bounds_.bottom - bounds_.top)),
                theme_, dpi_scale_);
    menu->on_select = [this, on_select, on_close](int idx) {
        // This lambda is `active_popup_`'s own member (MenuPopup::on_select) and is
        // still executing when it fires -- `active_popup_.reset()` alone would destroy the
        // MenuPopup (and, with it, this very closure's captured storage, including the
        // by-value `on_select` copy read on the next line) while still inside the call, a
        // use-after-free. `std::move` first keeps the widget (and this closure) alive in `self`
        // until the end of scope, well past the `on_select(idx)` call below; `active_popup_`
        // itself is cleared immediately after the move so a re-entrant ShowDropdown from inside
        // `on_select(idx)` (e.g. a Realloc confirm modal opening another picker) sees it gone.
        // Root cause of a real bug: picking a window in the Settings window-target
        // picker highlighted correctly and closed the dropdown, but visibly applied nothing --
        // config.json's capture.target/capture.target_window never changed. Smaller single-key
        // Enum dropdowns likely only "worked" by luck (freed memory not yet overwritten before the
        // use-after-read); the window-target picker's on_select closure is heap-allocated (it
        // captures a FieldSpec plus a std::vector<std::wstring> of window titles) and reliably hit
        // reused/corrupted memory instead.
        auto self = std::move(active_popup_);
        active_popup_.reset();
        if (on_select && idx >= 0) on_select(idx);
        if (on_close) on_close();
        RequestRepaint();
    };
    active_popup_ = std::move(menu);
    RequestRepaint();
}

void OverlayHost::ShowPopup(std::unique_ptr<Popup> popup, const D2D1_RECT_F& anchor) {
    if (!popup) return;
    popup->Place(anchor, D2D1::RectF(0.0f, 0.0f, static_cast<float>(bounds_.right - bounds_.left),
                                     static_cast<float>(bounds_.bottom - bounds_.top)),
                 theme_, dpi_scale_);
    active_popup_ = std::move(popup);
    RequestRepaint();
}

// A popup that closed itself during the event just handled goes now, outside its own callbacks.
void OverlayHost::ReapClosedPopup() {
    if (active_popup_ && active_popup_->Closed()) {
        auto done = std::move(active_popup_);
        RequestRepaint();
    }
}

void OverlayHost::RequestWindowListAsync(std::function<void(std::vector<WindowEntry>)> on_result) {
    // Detached, not joined: joining here would block the UI thread on the previous request's
    // round trip, exactly the synchronous-stall bug this whole method exists to remove. The
    // worker touches no OverlayHost state (only the by-value hwnd/generation/on_result captures
    // below), so a detached thread outliving this call -- even past a rapid re-open -- is safe;
    // `window_list_generation_` is what makes a superseded reply a no-op rather than a race.
    if (window_list_worker_.joinable()) window_list_worker_.detach();

    const uint64_t generation = window_list_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    const HWND     hwnd       = hwnd_;

    window_list_worker_ = std::thread([hwnd, generation, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("window-list");
        std::vector<WindowEntry> windows;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("list_windows"));
            auto resp = worker_client.Request(req, 1500);
            if (resp && resp->type == ombra::JsonValue::Type::Object) {
                if (const auto* arr = resp->Find("windows"); arr && arr->type == ombra::JsonValue::Type::Array) {
                    for (const auto& w : arr->arr) {
                        const auto* title_v = w.Find("title");
                        const auto* hwnd_v  = w.Find("hwnd");
                        if (!title_v || title_v->type != ombra::JsonValue::Type::String || title_v->str.empty())
                            continue;
                        WindowEntry entry;
                        entry.title = Utf8ToWideLocal(title_v->str);
                        if (const auto* proc_v = w.Find("process"); proc_v && proc_v->type == ombra::JsonValue::Type::String) {
                            entry.process = proc_v->str;
                        }
                        if (const auto* path_v = w.Find("image_path"); path_v && path_v->type == ombra::JsonValue::Type::String) {
                            entry.image_path = Utf8ToWideLocal(path_v->str);
                        }
                        auto flag = [&](const char* key) {
                            const auto* v = w.Find(key);
                            return v && v->type == ombra::JsonValue::Type::Bool && v->b;
                        };
                        entry.is_game  = flag("is_game");
                        entry.not_game = flag("not_game");
                        entry.hwnd  = (hwnd_v && hwnd_v->type == ombra::JsonValue::Type::Number)
                                          ? static_cast<uint64_t>(hwnd_v->num) : 0;
                        windows.push_back(std::move(entry));
                    }
                }
            }
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new WindowListResultMsg{generation, std::move(windows), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmWindowListResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;  // queue full or window gone between the IsWindow check and here
        }
    });
    window_list_worker_.detach();
}

void OverlayHost::RequestMonitorListAsync(std::function<void(std::vector<MonitorEntry>)> on_result) {
    if (monitor_list_worker_.joinable()) monitor_list_worker_.detach();

    const HWND hwnd = hwnd_;

    monitor_list_worker_ = std::thread([hwnd, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("monitor-list");
        std::vector<MonitorEntry> monitors;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("list_monitors"));
            auto resp = worker_client.Request(req, 1500);
            if (resp && resp->type == ombra::JsonValue::Type::Object) {
                if (const auto* arr = resp->Find("monitors"); arr && arr->type == ombra::JsonValue::Type::Array) {
                    for (const auto& m : arr->arr) {
                        const auto* device_v = m.Find("device");
                        if (!device_v || device_v->type != ombra::JsonValue::Type::String) continue;
                        MonitorEntry entry;
                        entry.device = device_v->str;
                        if (const auto* label_v = m.Find("label"); label_v && label_v->type == ombra::JsonValue::Type::String) {
                            entry.label = label_v->str;
                        }
                        if (const auto* w_v = m.Find("width"); w_v && w_v->type == ombra::JsonValue::Type::Number) {
                            entry.width = static_cast<int>(w_v->num);
                        }
                        if (const auto* h_v = m.Find("height"); h_v && h_v->type == ombra::JsonValue::Type::Number) {
                            entry.height = static_cast<int>(h_v->num);
                        }
                        if (const auto* p_v = m.Find("primary"); p_v && p_v->type == ombra::JsonValue::Type::Bool) {
                            entry.primary = p_v->b;
                        }
                        const auto* l_v = m.Find("left");
                        const auto* t_v = m.Find("top");
                        if (l_v && t_v && l_v->type == ombra::JsonValue::Type::Number && t_v->type == ombra::JsonValue::Type::Number) {
                            entry.left = static_cast<int>(l_v->num);
                            entry.top = static_cast<int>(t_v->num);
                            entry.positioned = true;
                        }
                        monitors.push_back(std::move(entry));
                    }
                }
            }
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new MonitorListResultMsg{std::move(monitors), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmMonitorListResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    });
    monitor_list_worker_.detach();
}

void OverlayHost::RequestMonitorThumbnailsAsync(
    std::vector<std::string> devices,
    std::function<void(std::string, bool, UINT, UINT, std::vector<uint8_t>)> on_result) {
    StartThumbnailWorker(std::move(devices), [](const std::string& device) {
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("capture_monitor_thumbnail"));
        req.Set("device", ombra::JsonValue::MakeStr(device));
        return req;
    }, std::move(on_result));
}

void OverlayHost::RequestWindowThumbnailsAsync(
    std::vector<uint64_t> hwnds,
    std::function<void(uint64_t, bool, UINT, UINT, std::vector<uint8_t>)> on_result) {
    std::vector<std::string> keys;
    keys.reserve(hwnds.size());
    for (uint64_t h : hwnds) keys.push_back(std::to_string(h));
    StartThumbnailWorker(std::move(keys), [](const std::string& key) {
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("capture_window_thumbnail"));
        req.Set("hwnd", ombra::JsonValue::MakeNum(static_cast<double>(std::stoull(key))));
        return req;
    }, [on_result = std::move(on_result)](std::string key, bool ok, UINT w, UINT h, std::vector<uint8_t> bgra) {
        if (on_result) on_result(std::stoull(key), ok, w, h, std::move(bgra));
    });
}

void OverlayHost::StartThumbnailWorker(
    std::vector<std::string> devices,
    std::function<ombra::JsonValue(const std::string&)> make_request,
    std::function<void(std::string, bool, UINT, UINT, std::vector<uint8_t>)> on_result) {
    if (monitor_thumb_worker_.joinable()) monitor_thumb_worker_.detach();

    const HWND hwnd = hwnd_;

    // Sequential, one PipeClient connection, deliberately not one worker per device (screens.h's
    // comment on why: each thumbnail request opens its own standalone WGC session on the engine
    // side, and running several of those at once would contend for the same GPU the user may be
    // gaming on).
    monitor_thumb_worker_ = std::thread([hwnd, devices = std::move(devices), make_request = std::move(make_request),
                                          on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("monitor-thumbs");
        // DecodePngToBgraLocal's WIC CoCreateInstance needs an apartment on this thread. Without
        // one it works only while something else in the process holds an MTA (likely why previews
        // came and went, then stopped once ee4cec7 removed the popup worker that set one up).
        ombra::ComInit com;
        PipeClient worker_client;
        const bool connected = worker_client.Connect(1500) && worker_client.Hello(1500);
        for (auto& device : devices) {
            if (!hwnd || !IsWindow(hwnd)) break;  // overlay closed while this was in flight

            bool                 ok = false;
            UINT                 w = 0, h = 0;
            std::vector<uint8_t> bgra;
            // Each failure is logged: the tile shows a placeholder for the rest of the overlay
            // session either way, and the engine only logs its own side.
            if (!connected) {
                ombra::Log::Warn("thumbnail %s: engine pipe not connected", device.c_str());
            } else if (auto resp = worker_client.Request(make_request(device), 3000);
                       !resp || resp->type != ombra::JsonValue::Type::Object) {
                ombra::Log::Warn("thumbnail %s: no reply from engine", device.c_str());
            } else {
                const auto* ok_v = resp->Find("ok");
                const auto* path_v = resp->Find("path");
                if (ok_v && ok_v->type == ombra::JsonValue::Type::Bool && ok_v->b &&
                    path_v && path_v->type == ombra::JsonValue::Type::String) {
                    HRESULT hr = S_OK;
                    ok = DecodePngToBgraLocal(Utf8ToWideLocal(path_v->str), w, h, bgra, &hr);
                    if (!ok) ombra::Log::Warn("thumbnail %s: PNG decode failed hr=0x%08lX", device.c_str(), hr);
                }
            }

            auto* msg = new MonitorThumbReadyMsg{device, ok, w, h, std::move(bgra), on_result};
            if (!hwnd || !IsWindow(hwnd) ||
                !PostMessageW(hwnd, kWmMonitorThumbReady, 0, reinterpret_cast<LPARAM>(msg))) {
                delete msg;
            }
        }
        worker_client.Close();
    });
    monitor_thumb_worker_.detach();
}

void OverlayHost::RequestProcessListAsync(std::function<void(std::vector<ProcessEntry>)> on_result) {
    if (process_list_worker_.joinable()) process_list_worker_.detach();

    const uint64_t generation = process_list_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    const HWND     hwnd       = hwnd_;

    process_list_worker_ = std::thread([hwnd, generation, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("process-list");
        std::vector<ProcessEntry> processes;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("list_processes"));
            auto resp = worker_client.Request(req, 1500);
            if (resp && resp->type == ombra::JsonValue::Type::Object) {
                if (const auto* arr = resp->Find("processes"); arr && arr->type == ombra::JsonValue::Type::Array) {
                    for (const auto& p : arr->arr) {
                        const auto* name_v = p.Find("name");
                        if (!name_v || name_v->type != ombra::JsonValue::Type::String || name_v->str.empty())
                            continue;
                        ProcessEntry entry;
                        entry.name = name_v->str;
                        if (const auto* disp_v = p.Find("display_name");
                            disp_v && disp_v->type == ombra::JsonValue::Type::String) {
                            entry.display_name = disp_v->str;
                        }
                        processes.push_back(std::move(entry));
                    }
                }
            }
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new ProcessListResultMsg{generation, std::move(processes), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmProcessListResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    });
    process_list_worker_.detach();
}

void OverlayHost::RequestAudioSessionListAsync(
    std::function<void(std::vector<AudioSessionEntry>)> on_result) {
    if (audio_session_list_worker_.joinable()) audio_session_list_worker_.detach();

    const uint64_t generation =
        audio_session_list_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    const HWND hwnd = hwnd_;

    audio_session_list_worker_ = std::thread([hwnd, generation, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("audio-sessions");
        std::vector<AudioSessionEntry> sessions;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("list_audio_sessions"));
            auto resp = worker_client.Request(req, 1500);
            if (resp && resp->type == ombra::JsonValue::Type::Object) {
                if (const auto* arr = resp->Find("sessions"); arr && arr->type == ombra::JsonValue::Type::Array) {
                    for (const auto& s : arr->arr) {
                        const auto* name_v = s.Find("name");
                        if (!name_v || name_v->type != ombra::JsonValue::Type::String || name_v->str.empty())
                            continue;
                        AudioSessionEntry entry;
                        entry.name = name_v->str;
                        if (const auto* disp_v = s.Find("display_name");
                            disp_v && disp_v->type == ombra::JsonValue::Type::String) {
                            entry.display_name = disp_v->str;
                        }
                        sessions.push_back(std::move(entry));
                    }
                }
            }
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new AudioSessionListResultMsg{generation, std::move(sessions), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmAudioSessionListResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    });
    audio_session_list_worker_.detach();
}

void OverlayHost::RequestAudioDeviceListAsync(
    std::function<void(std::vector<AudioDeviceEntry>)> on_result) {
    if (audio_device_list_worker_.joinable()) audio_device_list_worker_.detach();

    const uint64_t generation =
        audio_device_list_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    const HWND hwnd = hwnd_;

    audio_device_list_worker_ = std::thread([hwnd, generation, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("audio-devices");
        std::vector<AudioDeviceEntry> devices;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("list_audio_devices"));
            auto resp = worker_client.Request(req, 1500);
            if (resp && resp->type == ombra::JsonValue::Type::Object) {
                if (const auto* arr = resp->Find("devices"); arr && arr->type == ombra::JsonValue::Type::Array) {
                    for (const auto& d : arr->arr) {
                        const auto* id_v = d.Find("id");
                        if (!id_v || id_v->type != ombra::JsonValue::Type::String || id_v->str.empty())
                            continue;
                        AudioDeviceEntry entry;
                        entry.id = id_v->str;
                        if (const auto* name_v = d.Find("name");
                            name_v && name_v->type == ombra::JsonValue::Type::String) {
                            entry.name = name_v->str;
                        }
                        if (const auto* def_v = d.Find("is_default");
                            def_v && def_v->type == ombra::JsonValue::Type::Bool) {
                            entry.is_default = def_v->b;
                        }
                        if (const auto* in_v = d.Find("input"); in_v && in_v->type == ombra::JsonValue::Type::Bool) {
                            entry.input = in_v->b;
                        }
                        devices.push_back(std::move(entry));
                    }
                }
            }
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new AudioDeviceListResultMsg{generation, std::move(devices), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmAudioDeviceListResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    });
    audio_device_list_worker_.detach();
}

void OverlayHost::SetHotkeysSuspended(bool suspended) {
    {
        std::lock_guard<std::mutex> lock(hotkey_suspend_mutex_);
        hotkey_suspend_wanted_ = suspended;
        if (!hotkey_suspend_worker_.joinable()) {
            hotkey_suspend_worker_ = std::thread(&OverlayHost::HotkeySuspendLoop, this);
        }
    }
    hotkey_suspend_cv_.notify_all();
}

void OverlayHost::HotkeySuspendLoop() {
    ombra::Log::SetThreadName("hotkey-suspend");
    // Renewal well inside the engine's 10 s lease (HotkeyRegistry::kSuspendLeaseMs).
    constexpr auto kRenewEvery = std::chrono::seconds(4);
    auto send = [](bool suspend) {
        PipeClient client;
        if (!client.Connect(1500) || !client.Hello(1500)) return;
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("hotkeys_suspend"));
        req.Set("suspend", ombra::JsonValue::MakeBool(suspend));
        client.Request(std::move(req), 1500);
        client.Close();
    };
    bool sent = false;
    std::unique_lock<std::mutex> lock(hotkey_suspend_mutex_);
    while (true) {
        if (hotkey_suspend_stop_) {
            if (sent) {
                lock.unlock();
                send(false);
            }
            return;
        }
        const bool want = hotkey_suspend_wanted_;
        if (want != sent || want) {
            lock.unlock();
            send(want);
            lock.lock();
            sent = want;
        }
        if (sent) {
            hotkey_suspend_cv_.wait_for(lock, kRenewEvery,
                                        [&] { return hotkey_suspend_stop_ || !hotkey_suspend_wanted_; });
        } else {
            hotkey_suspend_cv_.wait(lock, [&] { return hotkey_suspend_stop_ || hotkey_suspend_wanted_; });
        }
    }
}

void OverlayHost::RequestAsync(ombra::JsonValue req, DWORD timeout_ms,
                               std::function<void(std::optional<ombra::JsonValue>)> on_result) {
    const HWND hwnd = hwnd_;

    std::thread([hwnd, req = std::move(req), timeout_ms, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("engine-request");
        std::optional<ombra::JsonValue> resp;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            resp = worker_client.Request(std::move(req), timeout_ms);
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new SetConfigResultMsg{std::move(resp), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmSetConfigResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    }).detach();
}

void OverlayHost::RequestPreviewAsync(
    const ombra::JsonValue& config,
    std::function<void(std::optional<ombra::JsonValue>, std::optional<ombra::JsonValue>)> on_result) {
    const HWND hwnd = hwnd_;

    std::thread([hwnd, config, on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("config-preview");
        std::optional<ombra::JsonValue> preview_resp;
        std::optional<ombra::JsonValue> status_resp;

        PipeClient worker_client;
        if (worker_client.Connect(1500) && worker_client.Hello(1500)) {
            ombra::JsonValue req = ombra::JsonValue::MakeObject();
            req.Set("type", ombra::JsonValue::MakeStr("preview_config"));
            req.Set("config", config);
            preview_resp = worker_client.Request(req, 3000);

            ombra::JsonValue status_req = ombra::JsonValue::MakeObject();
            status_req.Set("type", ombra::JsonValue::MakeStr("get_status"));
            status_resp = worker_client.Request(status_req, 1500);
        }
        worker_client.Close();

        if (!hwnd || !IsWindow(hwnd)) return;  // overlay closed while this was in flight
        auto* msg = new PreviewResultMsg{std::move(preview_resp), std::move(status_resp), std::move(on_result)};
        if (!PostMessageW(hwnd, kWmPreviewResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    }).detach();
}

void OverlayHost::RequestFileIconsAsync(std::vector<std::wstring> paths, UINT px,
                                        std::function<void(std::vector<FileIcon>)> on_result) {
    const HWND hwnd = hwnd_;
    if (!hwnd) return;
    std::thread([hwnd, px, paths = std::move(paths), on_result = std::move(on_result)]() mutable {
        ombra::Log::SetThreadName("file-icons");
        ombra::ComInit com;  // WIC on this thread, never the UI thread's factory
        std::vector<FileIcon> icons;
        icons.reserve(paths.size());
        for (const auto& p : paths) {
            if (!IsWindow(hwnd)) return;  // overlay closed while this was in flight
            icons.push_back(LoadFileIcon(p, px));
        }
        auto* msg = new FileIconsResultMsg{std::move(icons), std::move(on_result)};
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, kWmFileIconsResult, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    }).detach();
}

void OverlayHost::RunAsync(std::function<void()> work) {
    std::thread([work = std::move(work)] {
        ombra::Log::SetThreadName("async");
        work();
    }).detach();
}

ScreenHost::UiPost OverlayHost::UiPoster() {
    const HWND hwnd = hwnd_;
    return [hwnd](std::function<void()> fn) {
        auto* heap = new std::function<void()>(std::move(fn));
        if (!hwnd || !IsWindow(hwnd) || !PostMessageW(hwnd, kWmRunOnUi, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
    };
}

ID3D11Device* OverlayHost::VideoDevice() {
    return video_device_ ? d3d_device_.Get() : nullptr;
}

void OverlayHost::SetMeterTimer(bool on) {
    meter_timer_on_ = on;
    if (!hwnd_) return;  // set when the window is made
    if (on) SetTimer(hwnd_, kMeterTimerId, kMeterTimerMs, nullptr);
    else KillTimer(hwnd_, kMeterTimerId);
}

void OverlayHost::SetFrameTimer(bool on) {
    frame_demand_.clips = on;
    UpdateFrameClock();
}

void OverlayHost::UpdateFrameClock() {
    if (!frame_demand_.Running() || !hwnd_) {
        if (frame_clock_worker_.joinable()) {
            SetEvent(frame_clock_stop_);
            frame_clock_worker_.join();
        }
        return;
    }
    if (frame_clock_worker_.joinable()) return;
    if (!frame_clock_stop_) frame_clock_stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!frame_clock_stop_) return;
    ResetEvent(frame_clock_stop_);
    frame_tick_pending_ = false;
    const HWND hwnd = hwnd_;
    frame_clock_worker_ = std::thread([this, hwnd] {
        ombra::Log::SetThreadName("frame-clock");
        for (;;) {
            // The timeout keeps frames coming if the clock idles (nothing else on screen changing).
            const DWORD r = DCompositionWaitForCompositorClock(1, &frame_clock_stop_, 20);
            if (r == WAIT_OBJECT_0) return;
            if (r == WAIT_FAILED) Sleep(15);
            if (!frame_tick_pending_.exchange(true)) PostMessageW(hwnd, kWmFrameTick, 0, 0);
        }
    });
}

IconButtonControl& OverlayHost::NavButton() {
    return active_screen_ == ActiveScreen::Home ? gear_btn_ : back_btn_;
}

void OverlayHost::UpdateCursor(D2D1_POINT_2F pt) {
    // Same priority as WM_LBUTTONDOWN's routing: a modal or open flyout owns every click.
    CursorKind c = CursorKind::Arrow;
    if (active_modal_ && active_modal_->visible) {
        c = active_modal_->CursorAt(pt);
    } else if (active_popup_ && active_popup_->visible) {
        c = active_popup_->CursorAt(pt);
    } else if (close_btn_.CursorAt(pt) != CursorKind::Arrow ||
               NavButton().CursorAt(pt) != CursorKind::Arrow) {
        c = CursorKind::Hand;
    } else if (active_screen_ == ActiveScreen::Home && home_screen_) {
        c = home_screen_->CursorAt(pt);
    } else if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
        c = settings_screen_->CursorAt(pt);
    } else if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
        c = clips_screen_->CursorAt(pt);
    } else if (active_screen_ == ActiveScreen::Setup && setup_screen_) {
        c = setup_screen_->CursorAt(pt);
    }
    if (c != cursor_) {
        cursor_ = c;
        SetCursor(LoadCursorW(nullptr, CursorResource(c)));
    }
}

void OverlayHost::CloseOverlay() {
    Hide();
}

PipeClient& OverlayHost::GetPipeClient() {
    return client_ ? *client_ : fallback_client_;
}

float OverlayHost::ContentWidth(float view_w) const {
    const bool is_home = (active_screen_ == ActiveScreen::Home);
    const float percent = is_home ? kHomeWidthPercent : kSettingsWidthPercent;
    const float min_w   = (is_home ? kHomeMinWidthDip : kSettingsMinWidthDip) * dpi_scale_;
    const float max_w   = (is_home ? kHomeMaxWidthDip : kSettingsMaxWidthDip) * dpi_scale_;
    const float target_w = view_w * percent;
    const float clamped_w = std::clamp(target_w, min_w, max_w);
    // The narrow-viewport safety net (56dp total side margin) still wins over the percentage/min
    // target on a viewport too small for either -- same guarantee the old fixed-width formula gave.
    return (std::min)(clamped_w, view_w - 56.0f * dpi_scale_);
}

bool OverlayHost::IsPanelAttached() const {
    return theme_.panel_attach == PanelAttach::Attached && theme_.panel_position != PanelPosition::Center;
}

OverlayHost::Placement OverlayHost::ComputePlacement(float w, float h, float view_w, float view_h) const {
    const float s = dpi_scale_;
    const float margin = 32.0f * s;
    const float min_top = kContentMinTopDip * s;
    // A solid frame border pushes everything inward; corner-only frames (thickness 0) don't.
    const float inset = theme_.screen_frame ? theme_.screen_frame_thickness * s : 0.0f;

    Placement p;
    p.attached = IsPanelAttached();
    // An attached panel is a real surface around the content, so it needs padding; a floating row
    // is just the content (each widget draws its own cards).
    const float pad = PanelPadding();
    const float ow = w + 2.0f * pad;
    const float oh = h + 2.0f * pad;

    // Cross-axis centering. Never above the header clearance floor, so a tall centered row can't be
    // pushed up under the header.
    const float center_x = (view_w - ow) * 0.5f;
    const float center_y = (std::max)(min_top, (view_h - oh) * 0.5f);
    float ox = center_x;
    float oy = center_y;
    switch (theme_.panel_position) {
        case PanelPosition::Center: break;
        case PanelPosition::Top:    oy = p.attached ? inset : min_top; break;
        case PanelPosition::Bottom: oy = view_h - oh - (p.attached ? inset : margin + inset); break;
        case PanelPosition::Left:   ox = p.attached ? inset : margin + inset; break;
        case PanelPosition::Right:  ox = view_w - ow - (p.attached ? inset : margin + inset); break;
    }
    p.content = D2D1::RectF(ox + pad, oy + pad, ox + pad + w, oy + pad + h);

    if (p.attached) {
        PanelEdge edge = PanelEdge::Top;
        switch (theme_.panel_position) {
            case PanelPosition::Bottom: edge = PanelEdge::Bottom; break;
            case PanelPosition::Left:   edge = PanelEdge::Left; break;
            case PanelPosition::Right:  edge = PanelEdge::Right; break;
            default: break;
        }
        // Same value for both radii until something animates the reveal (see AttachedPanelGeometry).
        const float r = theme_.size.radius_panel() * s;
        p.panel = MakeAttachedGeometry(edge, D2D1::RectF(ox, oy, ox + ow, oy + oh), r, r);
    }
    return p;
}

D2D1_COLOR_F OverlayHost::SurfaceFill() const {
    // Frame and attached panel must read as ONE surface, so they are opaque: two translucent fills
    // overlapping at the fillets would show a seam at double alpha. The Settings and Clips panels
    // are this colour too, so an attached panel and its content card fuse.
    return theme_.color.panel;
}

float OverlayHost::PanelPadding() const {
    return IsPanelAttached() ? theme_.size.card_padding() * dpi_scale_ : 0.0f;
}

void OverlayHost::DrawScreenFrame(float view_w, float view_h) {
    if (!d2d_context_ || !d2d_factory_ || !theme_.screen_frame) return;
    auto geo = BuildScreenFramePath(d2d_factory_.Get(), D2D1::RectF(0.0f, 0.0f, view_w, view_h),
                                    theme_.screen_frame_thickness * dpi_scale_,
                                    theme_.screen_frame_radius * dpi_scale_);
    if (!geo) return;
    D2D1_COLOR_F color = SurfaceFill();
    if (theme_.screen_frame_color == ScreenFrameColor::Contrast) color = theme_.color.text;
    else if (theme_.screen_frame_color == ScreenFrameColor::Accent) color = theme_.color.accent;
    ComPtr<ID2D1SolidColorBrush> brush;
    d2d_context_->CreateSolidColorBrush(color, &brush);
    d2d_context_->FillGeometry(geo.Get(), brush.Get());
}

void OverlayHost::DrawAttachedPanel(const AttachedPanelGeometry& g) {
    if (!d2d_context_ || !d2d_factory_) return;
    auto geo = BuildAttachedPanelPath(d2d_factory_.Get(), g);
    if (!geo) return;
    ComPtr<ID2D1SolidColorBrush> fill, border;
    d2d_context_->CreateSolidColorBrush(SurfaceFill(), &fill);
    d2d_context_->FillGeometry(geo.Get(), fill.Get());
}

namespace {
constexpr int kBrandMarkBitmapPx = 128;  // rendered once per colour at a fixed size, D2D scales it down
constexpr wchar_t kWordmark[] = L"" OMBRA_WORDMARK;

// Where the top of a layout box for `text` in `format` goes so the capitals' middle sits on
// `center_y`. The wordmark is all capitals: centring its line box (or hanging it from the top, as
// before) puts it visibly above the mark, since the box leaves room for descenders it doesn't have.
float CapCenteredTop(IDWriteFactory* dw, IDWriteTextFormat* format, const wchar_t* text, float center_y) {
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_LINE_METRICS line{};
    UINT32 lines = 0;
    ComPtr<IDWriteFontCollection> fonts;
    if (!dw || FAILED(dw->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format, 4096.0f, 4096.0f, &layout)) ||
        FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines == 0 || FAILED(format->GetFontCollection(&fonts)))
        return center_y - format->GetFontSize() * 0.5f;
    wchar_t family[64]{};
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> fam;
    ComPtr<IDWriteFont> font;
    float cap = format->GetFontSize() * 0.7f;  // Inter's cap height is 0.727 em
    if (SUCCEEDED(format->GetFontFamilyName(family, 64)) && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists &&
        SUCCEEDED(fonts->GetFontFamily(index, &fam)) &&
        SUCCEEDED(fam->GetFirstMatchingFont(format->GetFontWeight(), format->GetFontStretch(), format->GetFontStyle(), &font))) {
        DWRITE_FONT_METRICS m{};
        font->GetMetrics(&m);
        if (m.designUnitsPerEm) cap = format->GetFontSize() * m.capHeight / m.designUnitsPerEm;
    }
    return center_y + cap * 0.5f - line.baseline;
}
}  // namespace

void OverlayHost::DrawBrandLogo(float cx, float cy, float size) {
    if (!d2d_context_) return;
    // The brand mark (brand_mark.h) with its ring in the picked colour, as the tray draws it.
    // Rendered on the CPU once per colour, not per repaint.
    const uint32_t rgb = theme_.PickedRgb();
    if (!logo_bmp_ || logo_rgb_ != rgb) {
        logo_bmp_.Reset();
        const auto pixels = ombra::RenderBrandMark(kBrandMarkBitmapPx, ((rgb >> 16) & 255) / 255.0f,
                                                   ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f);
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT hr = d2d_context_->CreateBitmap(D2D1::SizeU(kBrandMarkBitmapPx, kBrandMarkBitmapPx), pixels.data(),
                                                      kBrandMarkBitmapPx * 4, props, &logo_bmp_);
        if (FAILED(hr) || !logo_bmp_) {
            ombra::Log::Warn("DrawBrandLogo: CreateBitmap failed (hr=0x%08lX)", hr);
            logo_bmp_.Reset();
            return;
        }
        logo_rgb_ = rgb;
    }
    const D2D1_RECT_F dest = D2D1::RectF(cx - size * 0.5f, cy - size * 0.5f, cx + size * 0.5f, cy + size * 0.5f);
    d2d_context_->DrawBitmap(logo_bmp_.Get(), dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

// Floating header shared by both screens: Settings has no wrapping panel, matching Home. Brand
// logo+wordmark floats with no background in the top-left corner; a small independent dark pill
// holds gear/back+close in the top-right corner -- neither is part of a wrapping panel (there
// isn't one on either screen any more). Fixed screen-corner insets, not panel-relative.
void OverlayHost::DrawFloatingHeader(float view_w, float view_h) {
    if (!d2d_context_) return;
    (void)view_h;  // both floating elements anchor off view_w/fixed margins only, for now

    const float margin = 32.0f * dpi_scale_;
    const float logo_sz = 40.0f * dpi_scale_;
    const float logo_cx = margin + logo_sz * 0.5f;
    const float logo_cy = margin + logo_sz * 0.5f;
    DrawBrandLogo(logo_cx, logo_cy, logo_sz);

    const Sizes& z = theme_.size;
    const paint::Paint p{d2d_context_.Get(), theme_, dpi_scale_};
    // Inter whatever ui.font is (Theme::WordmarkFont), without grid-fit even under System's params.
    if (IDWriteTextFormat* wordmark = theme_.WordmarkFont()) {
        ComPtr<IDWriteRenderingParams> ui_params;
        d2d_context_->GetTextRenderingParams(&ui_params);
        d2d_context_->SetTextRenderingParams(theme_.NoGridFit());
        ComPtr<ID2D1SolidColorBrush> brush;
        d2d_context_->CreateSolidColorBrush(theme_.color.text, &brush);
        const float top = CapCenteredTop(theme_.dwrite.Get(), wordmark, kWordmark, logo_cy);
        DrawString(d2d_context_.Get(), kWordmark, static_cast<UINT32>(wcslen(kWordmark)), wordmark,
                   D2D1::RectF(logo_cx + logo_sz * 0.5f + z.space_md() * dpi_scale_, top, view_w * 0.5f, top + logo_sz),
                   brush.Get());
        d2d_context_->SetTextRenderingParams(ui_params.Get());
    }

    // Gear/back+close pill: a small standalone rounded rect in the top-right corner, its corners
    // concentric with the buttons' hover fill.
    const float btn_sz = z.control() * dpi_scale_;
    const float pill_pad = z.space_xs() * dpi_scale_;
    const bool nav = active_screen_ != ActiveScreen::Setup;  // setup goes back by its own steps
    const float pill_w = btn_sz * (nav ? 2.0f : 1.0f) + pill_pad * (nav ? 3.0f : 2.0f);
    const float pill_h = btn_sz + pill_pad * 2.0f;
    const D2D1_RECT_F pill_rect = D2D1::RectF(view_w - margin - pill_w, margin, view_w - margin, margin + pill_h);
    const float pill_r = (z.radius_control() + z.space_xs()) * dpi_scale_;
    p.Fill(pill_rect, pill_r, theme_.color.card);
    p.Stroke(pill_rect, pill_r, theme_.color.line);

    float bx = pill_rect.left + pill_pad;
    const float by = pill_rect.top + pill_pad;
    IconButtonControl& toggle_btn = NavButton();
    if (nav) {
        toggle_btn.bounds = D2D1::RectF(bx, by, bx + btn_sz, by + btn_sz);
        toggle_btn.Draw(d2d_context_.Get(), theme_, dpi_scale_);
        bx += btn_sz + pill_pad;
    } else {
        toggle_btn.bounds = D2D1::RectF(0, 0, 0, 0);
    }
    close_btn_.bounds = D2D1::RectF(bx, by, bx + btn_sz, by + btn_sz);
    close_btn_.Draw(d2d_context_.Get(), theme_, dpi_scale_);
}

void OverlayHost::RenderFrame() {
    if (!d2d_context_ || !d2d_target_bitmap_) return;

    d2d_context_->BeginDraw();
    d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d_context_->SetTextRenderingParams(theme_.TextRendering());  // follows ui.font (Theme::TextRendering)
    d2d_context_->Clear(D2D1::ColorF(0, 0, 0, 0));  // fully transparent -- desktop/game stays visible

    const float view_w = static_cast<float>(bounds_.right - bounds_.left);
    const float view_h = static_cast<float>(bounds_.bottom - bounds_.top);

    // Full-screen scrim: dims the whole monitor behind the overlay, not just any one element's own
    // footprint. Shared by both screens regardless of their chrome model.
    {
        ComPtr<ID2D1SolidColorBrush> scrim_brush;
        d2d_context_->CreateSolidColorBrush(theme_.color.scrim, &scrim_brush);
        D2D1_RECT_F scrim_rect = D2D1::RectF(0.0f, 0.0f, view_w, view_h);
        d2d_context_->FillRectangle(&scrim_rect, scrim_brush.Get());
    }

    // Screen frame goes under everything else so the header and panel sit on top of it.
    DrawScreenFrame(view_w, view_h);

    // Shared floating header (brand + gear/back + close pill) for both screens. Content row sizing
    // is the phase-1 percentage-of-view-width formula (ContentWidth); a visible panel background
    // only exists when ui.panel_attach is on (drawn per screen below).
    DrawFloatingHeader(view_w, view_h);

    if (active_screen_ == ActiveScreen::Home && home_screen_) {
        const float pad = PanelPadding();
        const float row_w = ContentWidth(view_w) - 2.0f * pad;
        const float row_h = std::clamp(row_w * kHomeBodyAspect, kHomeBodyMinDip * dpi_scale_, kHomeBodyMaxDip * dpi_scale_);
        const Placement place = ComputePlacement(row_w, row_h, view_w, view_h);
        if (place.attached) DrawAttachedPanel(place.panel);

        home_screen_->Layout(place.content, dpi_scale_);
        home_screen_->Draw(d2d_context_.Get(), theme_, dpi_scale_);
    } else if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
        // Rail + content card sit directly below the floating header -- OverlayHost
        // only computes the combined row's bounds, same role it plays for Home's tile row above.
        // Capped at kSettingsRowMaxHeightDip rather than stretching to the bottom margin -- on a
        // typical monitor most rail/group pages don't need anywhere near full height, and a full-
        // height card left the rail's icon stack surrounded by a lot of empty space below it.
        const float pad = PanelPadding();
        float row_w = ContentWidth(view_w) - 2.0f * pad;
        // Memory card: a third box on the side away from the rail. Floating: it hangs off the outside
        // of the row, so rail + settings card sit exactly where they do without it (widening the row
        // moved the main card off-centre). Attached: the panel is a surface
        // around the content, so the row and the panel grow together. Either way the card is left out
        // when it doesn't fit; rail and card are never squeezed to make room.
        const float card_extra = SettingsScreen::MemoryCardExtraWidth(dpi_scale_);
        const bool card_left = theme_.rail_position == RailPosition::Right;
        const bool card_inside = IsPanelAttached();
        bool show_card = theme_.memory_card;
        if (show_card && card_inside) {
            show_card = row_w + card_extra <= view_w - 56.0f * dpi_scale_ - 2.0f * pad;
            if (show_card) row_w += card_extra;
        }
        const float margin = 32.0f * dpi_scale_;
        const float max_row_h = (view_h - margin) - kContentMinTopDip * dpi_scale_ - 2.0f * pad;
        // One height for every page, deliberately. Sizing the card to each page's own content
        // removed the empty band below short pages but made the whole panel -- rail included --
        // jump when switching to a tall page like Monitors.
        // A stable panel beats a tight one; the empty band is the price.
        const float row_h = (std::min)(kSettingsRowMaxHeightDip * dpi_scale_, max_row_h);
        const Placement place = ComputePlacement(row_w, row_h, view_w, view_h);
        if (show_card && !card_inside) {
            const float edge = 28.0f * dpi_scale_;
            show_card = card_left ? place.content.left - card_extra >= edge
                                  : place.content.right + card_extra <= view_w - edge;
        }
        settings_screen_->SetMemoryCardSlot(show_card, card_left, card_inside);
        settings_screen_->Layout(place.content, dpi_scale_);

        if (place.attached) DrawAttachedPanel(place.panel);
        settings_screen_->Draw(d2d_context_.Get(), theme_, dpi_scale_);

        std::wstring tooltip_text;
        D2D1_POINT_2F tooltip_pos{};
        if (settings_screen_->QueryHoverTooltip(&tooltip_text, &tooltip_pos)) {
            DrawTooltip(d2d_context_.Get(), theme_, dpi_scale_, tooltip_text, tooltip_pos);
        }
    } else if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
        // Wider than Settings and as tall as the screen allows: the preview is the point.
        const float pad = PanelPadding();
        const float row_w = (std::min)(std::clamp(view_w * kClipsWidthPercent, kSettingsMinWidthDip * dpi_scale_,
                                                  kClipsMaxWidthDip * dpi_scale_),
                                       view_w - 56.0f * dpi_scale_) - 2.0f * pad;
        const float margin = 32.0f * dpi_scale_;
        const float row_h = (std::min)((view_h - margin) - kContentMinTopDip * dpi_scale_ - 2.0f * pad,
                                       ClipsScreen::PanelHeightFor(row_w, theme_, dpi_scale_));
        const Placement place = ComputePlacement(row_w, row_h, view_w, view_h);
        if (place.attached) DrawAttachedPanel(place.panel);
        clips_screen_->Layout(place.content, dpi_scale_);
        clips_screen_->Draw(d2d_context_.Get(), theme_, dpi_scale_);
        std::wstring tooltip_text;
        D2D1_POINT_2F tooltip_pos{};
        if (clips_screen_->QueryHoverTooltip(&tooltip_text, &tooltip_pos))
            DrawTooltip(d2d_context_.Get(), theme_, dpi_scale_, tooltip_text, tooltip_pos);
    } else if (active_screen_ == ActiveScreen::Setup && setup_screen_) {
        // Settings' box: the same panel size on every page, and no rail or memory card.
        const float pad = PanelPadding();
        const float row_w = ContentWidth(view_w) - 2.0f * pad;
        const float margin = 32.0f * dpi_scale_;
        const float row_h = (std::min)(kSettingsRowMaxHeightDip * dpi_scale_,
                                       (view_h - margin) - kContentMinTopDip * dpi_scale_ - 2.0f * pad);
        const Placement place = ComputePlacement(row_w, row_h, view_w, view_h);
        if (place.attached) DrawAttachedPanel(place.panel);
        // A step is laid out to a fixed height with its buttons at the foot, and setup doesn't
        // scroll. A panel shorter than that (1920x1080 at 200%: 406 DIP) draws the step smaller,
        // fonts too, rather than overlapping; the rest of the frame keeps the window's scale.
        const float setup_scale = setup_screen_->FitScale(theme_, place.content.bottom - place.content.top, dpi_scale_);
        theme_.SetFontScale(setup_scale);
        setup_screen_->Layout(place.content, setup_scale);
        setup_screen_->Draw(d2d_context_.Get(), theme_, setup_scale);
        theme_.SetFontScale(dpi_scale_);

        std::wstring tooltip_text;
        D2D1_POINT_2F tooltip_pos{};
        if (setup_screen_->QueryHoverTooltip(&tooltip_text, &tooltip_pos)) {
            DrawTooltip(d2d_context_.Get(), theme_, dpi_scale_, tooltip_text, tooltip_pos);
        }
    }

    if (active_modal_ && active_modal_->visible) {
        active_modal_->bounds = D2D1::RectF(0.0f, 0.0f, view_w, view_h);
        active_modal_->Draw(d2d_context_.Get(), theme_, dpi_scale_);
    }
    if (active_popup_ && active_popup_->visible) {
        active_popup_->Draw(d2d_context_.Get(), theme_, dpi_scale_);
    }

    HRESULT hr = d2d_context_->EndDraw();
    if (SUCCEEDED(hr) && swapchain_) hr = swapchain_->Present(1, 0);
    if (forced_render_hr_ != S_OK) hr = forced_render_hr_;
    // A driver update or GPU reset. Everything drawn on this device is gone and the window would sit
    // frozen on top of the game, so close: this process ends and the next open starts on a new device.
    if (!device_lost_ && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                          hr == DXGI_ERROR_DEVICE_HUNG || hr == D2DERR_RECREATE_TARGET)) {
        device_lost_ = true;
        ombra::Log::Warn("overlay: graphics device lost (0x%08lX, reason 0x%08lX) -- closing",
                          static_cast<unsigned long>(hr),
                          static_cast<unsigned long>(d3d_device_ ? d3d_device_->GetDeviceRemovedReason() : S_OK));
        UiPoster()([this] { CloseOverlay(); });
    }
    // Here, not in the tick handler: a hover's repaint is synchronous (RequestRepaint), so the fade
    // it starts has to switch the clock on from the frame that drew its first step.
    const bool fading = motion::TakeRunning();
    if (fading != frame_demand_.motion) {
        frame_demand_.motion = fading;
        UpdateFrameClock();
    }
}

namespace {
std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return L"";
    const int req = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring out(static_cast<size_t>(req), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), req);
    return out;
}
}  // namespace

// Builds the modal that surfaces one completed/failed engine.autotune run (see AutotuneContext in
// main.cpp): shows the measured numbers and, if any, the one preset recommendation, and only ever
// applies it on an explicit Confirm click, routed through the normal set_config/ApplySettingChange
// path so it inherits that path's tier handling. Both Confirm and Cancel ack the result (set_config
// {"engine.autotune":"ack"}) so it isn't shown again on the next overlay open.
std::optional<OverlayHost::PendingModal> OverlayHost::BuildAutotuneModal(const std::string& state,
                                                                          const ombra::JsonValue* result) {
    // Off the UI thread: PipeClient::Request reconnects for up to 1.5 s when the engine is gone.
    auto ack = [this]() {
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("set_config"));
        ombra::JsonValue patch = ombra::JsonValue::MakeObject();
        patch.Set("engine.autotune", ombra::JsonValue::MakeStr("ack"));
        req.Set("config", std::move(patch));
        RequestAsync(std::move(req), 3000, nullptr);
    };

    PendingModal modal;
    if (state == "failed" || !result) {
        modal.title = L"Auto-Tune Failed";
        std::string reason = "capturebench did not produce a result";
        if (result) {
            if (const auto* r = result->Find("reason"); r && r->type == ombra::JsonValue::Type::String) {
                reason = r->str;
            }
        }
        modal.message = L"The benchmark didn't complete: " + Utf8ToWideLocal(reason);
        // Info-only acknowledgment: one button. ack() is the only thing that resets
        // AutotuneContext::state back to Idle engine-side (main.cpp); a dismissal that skipped it
        // would latch this stale result forever -- every future run's compare_exchange_strong(Idle,
        // Running) would keep failing and re-show this same modal. Esc maps to the sole action.
        modal.buttons = {L"OK", L""};
        modal.on_confirm = ack;
        return modal;
    }

    const auto* aborted_v = result->Find("aborted");
    const bool aborted = aborted_v && aborted_v->type == ombra::JsonValue::Type::Bool && aborted_v->b;
    const auto* reason_v = result->Find("reason");
    const std::string reason = (reason_v && reason_v->type == ombra::JsonValue::Type::String) ? reason_v->str : "";
    const auto* rec_v = result->Find("recommended_preset");
    const std::string recommended = (rec_v && rec_v->type == ombra::JsonValue::Type::String) ? rec_v->str : "";
    const auto* lat_p99_v = result->Find("latency_p99_ms");
    const double lat_p99 = (lat_p99_v && lat_p99_v->type == ombra::JsonValue::Type::Number) ? lat_p99_v->num : 0.0;

    if (aborted) {
        modal.title = L"Auto-Tune Aborted";
        modal.message = L"The benchmark was interrupted: " + Utf8ToWideLocal(reason);
        modal.buttons = {L"OK", L""};  // info-only, see the failed-result branch above
        modal.on_confirm = ack;
        return modal;
    }

    wchar_t buf[320];
    if (recommended.empty()) {
        modal.title = L"Auto-Tune Complete";
        swprintf_s(buf, L"Measured end-to-end latency p99 %.1f ms.\n\n%s", lat_p99, Utf8ToWideLocal(reason).c_str());
        modal.message = buf;
        modal.buttons = {L"OK", L""};  // info-only, see the failed-result branch above
        modal.on_confirm = ack;
        return modal;
    }

    modal.title = L"Auto-Tune Complete";
    swprintf_s(buf, L"Measured end-to-end latency p99 %.1f ms.\n\n%s\n\nApply encoder preset %s?",
               lat_p99, Utf8ToWideLocal(reason).c_str(), Utf8ToWideLocal(recommended).c_str());
    modal.message = buf;
    modal.on_confirm = [this, ack, recommended]() {
        ombra::JsonValue req = ombra::JsonValue::MakeObject();
        req.Set("type", ombra::JsonValue::MakeStr("set_config"));
        ombra::JsonValue patch = ombra::JsonValue::MakeObject();
        patch.Set("encode.preset", ombra::JsonValue::MakeStr(recommended));
        req.Set("config", std::move(patch));
        // ack() is sent now, not from this callback: closing the overlay drops callbacks, and a
        // missed ack latches the result. The two keys are independent.
        RequestAsync(std::move(req), 3000, nullptr);
        ack();
    };
    modal.buttons = {L"Apply", L"Not now"};
    modal.on_cancel = ack;
    return modal;
}

// Schema, config, status and a latched autotune result from the engine, if connected. Show() and
// StartHeadless() share it.
void OverlayHost::LoadFromEngine() {
    if (!client_ || !client_->Connected()) return;

    // Past the grace the overlay opens saying so, and this fills it in when the answers come.
    auto late = std::make_shared<bool>(false);
    const bool answered = FetchWithGrace({"get_schema", "get_config", "get_status"}, [this, late](EngineReplies replies) {
        ApplyEngineState(replies[0], replies[1], replies[2]);
        if (!*late) return;
        if (settings_screen_ && replies[0]) settings_screen_->FetchPreviewAsync();
        ShowPendingAutotuneModal();
        ShowCrashAsk();
        RequestRepaint();
    });
    if (!answered) {
        *late = true;
        home_screen_->MarkEngineUnavailable();
    }
}

void OverlayHost::ShowPendingAutotuneModal() {
    if (!pending_autotune_modal_) return;
    ShowModal(pending_autotune_modal_->title, pending_autotune_modal_->message,
              /*is_danger=*/false, pending_autotune_modal_->on_confirm,
              pending_autotune_modal_->on_cancel, pending_autotune_modal_->buttons);
    pending_autotune_modal_ = std::nullopt;
}

void OverlayHost::ShowCrashAsk() {
    if (crash_ask_shown_ || (active_modal_ && active_modal_->visible) || first_screen_ != ActiveScreen::Home) return;
    // Nowhere to send it (a build without a key; an isolated instance's test key is the child's
    // business). Setup finished with sharing off: that answer covers it.
    if (!*ombra::SentryDsn() && !ombra::Instance().isolated) return;
    if (ombra::SetupCompleted() &&
        GetFileAttributesW((ombra::crash::CrashesDir() + L"\\sharing_on").c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    const std::wstring base = ombra::crash::UnansweredCrash();
    if (base.empty()) return;
    crash_ask_shown_ = true;
    ShowModal(L"" OMBRA_PRODUCT_NAME " crashed",
              L"It crashed before setup was finished. Send the crash report to the developer? It has the error, your Windows version and GPUs, "
              L"and a memory snapshot that may hold personal details.",
              false,
              [this, base] {
                  ombra::crash::AnswerCrash(base, true);
                  ombra::Log::Info("crash ask  send");
                  ombra::JsonValue req = ombra::JsonValue::MakeObject();
                  req.Set("type", ombra::JsonValue::MakeStr("upload_soon"));
                  RequestAsync(std::move(req), 3000, [](std::optional<ombra::JsonValue>) {});
              },
              [base] {
                  ombra::crash::AnswerCrash(base, false);
                  ombra::Log::Info("crash ask  don't send");
              },
              ModalButtons{L"Send", L"Don't send"});
}

void OverlayHost::ApplyEngineState(const std::optional<ombra::JsonValue>& schema_resp,
                                   const std::optional<ombra::JsonValue>& config_resp,
                                   const std::optional<ombra::JsonValue>& status_resp) {
    if (schema_resp) {
        auto specs = ParseSchema(*schema_resp);
        const ombra::JsonValue* cfg_obj = config_resp ? config_resp->Find("config") : nullptr;
        settings_screen_->InitSchema(specs, cfg_obj);
    }
    if (config_resp) {
        if (const auto* cfg = config_resp->Find("config"); cfg) {
            theme_.ApplyUiConfig([cfg](const char* key) { return cfg->Find(key); });
        }
    }
    home_screen_->UpdateState(status_resp ? &*status_resp : nullptr,
                              config_resp ? &*config_resp : nullptr);
    if (!status_resp) home_screen_->MarkEngineUnavailable();

    // engine.autotune's result is latched (main.cpp's AutotuneContext) until acked, so this
    // fires once per completed run no matter how many times the overlay is opened/closed while
    // it was latched -- shown by the caller once its render target exists (ShowModal's
    // RequestRepaint needs it).
    pending_autotune_modal_ = std::nullopt;
    if (status_resp) {
        if (const auto* state_v = status_resp->Find("autotune_state");
            state_v && state_v->type == ombra::JsonValue::Type::String &&
            (state_v->str == "done" || state_v->str == "failed")) {
            pending_autotune_modal_ = BuildAutotuneModal(state_v->str, status_resp->Find("autotune_result"));
        }
    }
}

bool OverlayHost::Show(PipeClient* client, ActiveScreen first_screen) {
    client_ = client;
    first_screen_ = first_screen;
    prev_foreground_ = GetForegroundWindow();

    HMONITOR mon = nullptr;
    if (prev_foreground_ && IsWindow(prev_foreground_) &&
        prev_foreground_ != GetDesktopWindow() && prev_foreground_ != GetShellWindow()) {
        mon = MonitorFromWindow(prev_foreground_, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }

    if (!InitGraphics(mon)) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    const int width = mi.rcMonitor.right - mi.rcMonitor.left;
    const int height = mi.rcMonitor.bottom - mi.rcMonitor.top;

    if (!CreateHostWindow(mi.rcMonitor)) return false;
    if (!CreateSwapchainAndDComp(width, height)) {
        CleanupWindowResources();
        return false;
    }
    // After the window exists, still hidden: the engine state builds Settings' pages, whose list
    // fetches (displays, windows, audio, thumbnails) post their replies to hwnd_. Loaded before it,
    // they posted to nothing and stayed pending: Displays read "Loading displays…" for good.
    LoadFromEngine();
    if (settings_screen_ && client_ && client_->Connected()) settings_screen_->FetchPreviewAsync();
    AttachUnattachedFetch();
    // Before the window shows, so a first run's setup doesn't start with a frame of Home.
    if (first_screen != ActiveScreen::Home) SwitchScreen(first_screen);

    ShowWindow(hwnd_, SW_SHOW);

    // Plain SetForegroundWindow from a freshly spawned process is exactly the case Windows'
    // foreground lock exists to deny — tray.cpp's AllowSetForegroundWindow() covers the
    // RegisterHotKey/WH_KEYBOARD_LL trigger paths (which count as this process's parent having
    // just received a real input message), but hotkeys.backend=poll's GetAsyncKeyState trigger
    // delivers no such message to anyone, so that grant has nothing to stand on and silently does
    // nothing (the overlay stayed topmost/visible but never actually took
    // keyboard focus — Esc kept reaching the game underneath — until the panel was clicked once).
    // AttachThreadInput sidesteps needing that permission at all: it links this thread's input
    // processing to whatever window currently owns foreground, which is sufficient standing for
    // SetForegroundWindow on its own, and — unlike the alternative "synthesize a fake Alt tap via
    // SendInput" workaround — injects no input event, so it carries none of the "did this process
    // just inject a keystroke" signal that anti-cheat software has been observed to watch
    // for on titles this overlay may be opened over.
    const HWND prior_foreground = GetForegroundWindow();
    DWORD foreground_thread = 0;
    const bool attach_input =
        prior_foreground && prior_foreground != hwnd_ &&
        (foreground_thread = GetWindowThreadProcessId(prior_foreground, nullptr)) != 0 &&
        foreground_thread != GetCurrentThreadId() &&
        AttachThreadInput(GetCurrentThreadId(), foreground_thread, TRUE);

    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);

    if (attach_input) AttachThreadInput(GetCurrentThreadId(), foreground_thread, FALSE);

    ShowPendingAutotuneModal();
    ShowCrashAsk();

    RenderFrame();
    ombra::Log::Info("OverlayHost: overlay shown on monitor (%d,%d - %dx%d)",
                     mi.rcMonitor.left, mi.rcMonitor.top, width, height);
    return true;
}

void OverlayHost::Hide() {
    if (settings_screen_) settings_screen_->CancelShortcutRecording();
    if (settings_screen_) settings_screen_->StopMeters();
    if (setup_screen_) setup_screen_->CancelShortcutRecording();
    if (clips_screen_) clips_screen_->Leave();
    if (hwnd_) {
        ombra::Log::Info("OverlayHost: dismissing and destroying overlay window");
        CleanupWindowResources();
        if (prev_foreground_ && IsWindow(prev_foreground_)) {
            SetForegroundWindow(prev_foreground_);
            prev_foreground_ = nullptr;
        }
    }
}

void OverlayHost::CleanupWindowResources() {
    frame_demand_ = {};
    UpdateFrameClock();
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_bitmap_.Reset();
    if (dcomp_target_) dcomp_target_->SetRoot(nullptr);
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    swapchain_.Reset();

    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void OverlayHost::RunModalLoop() {
    MSG msg;
    while (IsVisible() && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK OverlayHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OverlayHost* host = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        host = reinterpret_cast<OverlayHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    } else {
        host = reinterpret_cast<OverlayHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (host) {
        return host->HandleMessage(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT OverlayHost::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // A shortcut row is recording: every key goes to it, before Esc-closes-overlay and before
    // DefWindowProc turns Alt/F10 into menu activation or Alt+F4 into a close. Alt combos arrive as
    // WM_SYSKEYDOWN, never WM_KEYDOWN.
    if (active_screen_ == ActiveScreen::Settings && settings_screen_ && settings_screen_->IsRecordingShortcut() &&
        !(active_modal_ && active_modal_->visible)) {
        switch (msg) {
            case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
                settings_screen_->OnShortcutKey(static_cast<UINT>(wp), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                RenderFrame();
                return 0;
            case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
                return 0;
            default:
                break;
        }
    }
    if (active_screen_ == ActiveScreen::Setup && setup_screen_ && setup_screen_->IsRecordingShortcut() &&
        !(active_modal_ && active_modal_->visible)) {
        switch (msg) {
            case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
                setup_screen_->OnShortcutKey(static_cast<UINT>(wp), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                RenderFrame();
                return 0;
            case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
                return 0;
            default:
                break;
        }
    }
    switch (msg) {
        case WM_KILLFOCUS:
            // Alt+Tab mid-recording: the keys stop coming, so stop listening (and resume the engine's).
            if (settings_screen_ && settings_screen_->IsRecordingShortcut()) {
                settings_screen_->CancelShortcutRecording();
                RenderFrame();
            }
            if (setup_screen_ && setup_screen_->IsRecordingShortcut()) {
                setup_screen_->CancelShortcutRecording();
                RenderFrame();
            }
            break;

        case WM_KEYDOWN: {
            const UINT vk = static_cast<UINT>(wp);
            if (vk == VK_ESCAPE) {
                // Esc in a text field cancels the edit; it doesn't close the overlay.
                if (!(active_modal_ && active_modal_->visible) && !(active_popup_ && active_popup_->visible) &&
                    active_screen_ == ActiveScreen::Settings && settings_screen_ && settings_screen_->IsEditingText()) {
                    settings_screen_->OnKeyDown(vk);
                    RenderFrame();
                    return 0;
                }
                if (active_modal_ && active_modal_->visible) {
                    active_modal_->OnKeyDown(vk);  // one button: Esc acknowledges, as its click does
                    ProcessModalResult();
                    RenderFrame();
                    return 0;
                }
                if (active_popup_ && active_popup_->visible) {
                    active_popup_->OnKeyDown(vk);  // a menu dismisses; the colour editor puts its colour back
                    ReapClosedPopup();
                    RenderFrame();
                    return 0;
                }
                // Esc doesn't throw away what's been picked in the setup; its X does.
                if (active_screen_ == ActiveScreen::Setup) return 0;
                // Esc in the clip player goes back to the grid; the next one closes the overlay.
                if (active_screen_ == ActiveScreen::Clips && clips_screen_ && clips_screen_->PlayerOpen()) {
                    clips_screen_->OnKeyDown(vk);
                    RenderFrame();
                    return 0;
                }
                Hide();
                return 0;
            }
            if (active_modal_ && active_modal_->visible) {
                if (active_modal_->OnKeyDown(vk)) {
                    ProcessModalResult();
                    RenderFrame();
                    return 0;
                }
            }
            if (active_popup_ && active_popup_->visible) {
                if (active_popup_->OnKeyDown(vk)) {
                    ReapClosedPopup();
                    RenderFrame();
                    return 0;
                }
            }
            if (active_screen_ == ActiveScreen::Home && home_screen_) {
                if (home_screen_->OnKeyDown(vk)) {
                    RenderFrame();
                    return 0;
                }
            } else if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
                if (settings_screen_->OnKeyDown(vk)) {
                    RenderFrame();
                    return 0;
                }
            } else if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
                if (clips_screen_->OnKeyDown(vk)) {
                    RenderFrame();
                    return 0;
                }
            }
            break;
        }

        case WM_CHAR:
            if (!(active_modal_ && active_modal_->visible) && active_popup_ && active_popup_->visible) {
                active_popup_->OnChar(static_cast<wchar_t>(wp));
                RenderFrame();
                return 0;
            }
            if (!(active_modal_ && active_modal_->visible) && active_screen_ == ActiveScreen::Settings &&
                settings_screen_ && settings_screen_->OnChar(static_cast<wchar_t>(wp))) {
                RenderFrame();
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            // Alt+Tab or a system dialog took the mouse mid-press, so the release would go elsewhere.
            // End the press off every control: drags stop, and no button under the pointer fires.
            if (mouse_pressed_ && reinterpret_cast<HWND>(lp) != hwnd_) {
                SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(-1, -1));
            }
            break;

        case WM_MOUSEMOVE: {
            // Signed: with the mouse captured a drag can pass the window's left or top edge.
            const D2D1_POINT_2F pt = D2D1::Point2F(static_cast<float>(static_cast<short>(LOWORD(lp))),
                                                   static_cast<float>(static_cast<short>(HIWORD(lp))));
            bool rep = gear_btn_.OnMouseMove(pt);
            rep |= back_btn_.OnMouseMove(pt);
            rep |= close_btn_.OnMouseMove(pt);
            if (active_modal_ && active_modal_->visible) {
                rep |= active_modal_->OnMouseMove(pt);
            } else if (active_popup_ && active_popup_->visible) {
                rep |= active_popup_->OnMouseMove(pt);
            } else if (active_screen_ == ActiveScreen::Home && home_screen_) {
                rep |= home_screen_->OnMouseMove(pt);
            } else if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
                rep |= settings_screen_->OnMouseMove(pt);
            } else if (active_screen_ == ActiveScreen::Setup && setup_screen_) {
                rep |= setup_screen_->OnMouseMove(pt);
            } else if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
                rep |= clips_screen_->OnMouseMove(pt);
            }
            UpdateCursor(pt);
            if (rep) RenderFrame();
            break;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, CursorResource(cursor_)));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            mouse_pressed_ = true;
            SetCapture(hwnd_);
            const D2D1_POINT_2F pt = D2D1::Point2F(static_cast<float>(LOWORD(lp)), static_cast<float>(HIWORD(lp)));
            if (active_modal_ && active_modal_->visible) {
                if (active_modal_->OnMouseDown(pt)) {
                    ProcessModalResult();
                    RenderFrame();
                    return 0;
                }
            } else if (active_popup_ && active_popup_->visible) {
                if (active_popup_->OnMouseDown(pt)) {
                    ReapClosedPopup();
                    RenderFrame();
                    return 0;
                }
            } else if (close_btn_.OnMouseDown(pt)) {
                RenderFrame();
                return 0;
            } else if (NavButton().OnMouseDown(pt)) {
                RenderFrame();
                return 0;
            } else if (active_screen_ == ActiveScreen::Home && home_screen_) {
                if (home_screen_->OnMouseDown(pt)) {
                    RenderFrame();
                    return 0;
                }
            } else if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
                if (settings_screen_->OnMouseDown(pt)) {
                    RenderFrame();
                    return 0;
                }
            } else if (active_screen_ == ActiveScreen::Setup && setup_screen_) {
                if (setup_screen_->OnMouseDown(pt)) {
                    RenderFrame();
                    return 0;
                }
            } else if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
                if (clips_screen_->OnMouseDown(pt)) {
                    RenderFrame();
                    return 0;
                }
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (mouse_pressed_) {
                mouse_pressed_ = false;  // first: ReleaseCapture sends WM_CAPTURECHANGED
                ReleaseCapture();
            }
            const D2D1_POINT_2F pt = D2D1::Point2F(static_cast<float>(static_cast<short>(LOWORD(lp))),
                                                   static_cast<float>(static_cast<short>(HIWORD(lp))));
            for (IconButtonControl* b : {&gear_btn_, &back_btn_, &close_btn_}) b->OnMouseUp(pt);
            if (active_modal_ && active_modal_->visible) {
                active_modal_->OnMouseUp(pt);
                RenderFrame();
            } else if (active_popup_ && active_popup_->visible && active_popup_->OnMouseUp(pt)) {
                // Ends a drag in the colour editor. Any other release goes on to the page: the
                // press that opened a menu has to end on the Select (or ⋯) that took it.
                ReapClosedPopup();
                RenderFrame();
            } else if (active_screen_ == ActiveScreen::Home && home_screen_) {
                home_screen_->OnMouseUp(pt);
                RenderFrame();
            } else if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
                settings_screen_->OnMouseUp(pt);
                RenderFrame();
            } else if (active_screen_ == ActiveScreen::Setup && setup_screen_) {
                setup_screen_->OnMouseUp(pt);
            } else if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
                clips_screen_->OnMouseUp(pt);
                RenderFrame();
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            if (active_popup_ && active_popup_->visible) {
                active_popup_->OnMouseWheel(delta);
                RenderFrame();
                return 0;
            }
            if (active_screen_ == ActiveScreen::Settings && settings_screen_) {
                settings_screen_->OnMouseWheel(delta);
                RenderFrame();
                return 0;
            }
            if (active_screen_ == ActiveScreen::Clips && clips_screen_) {
                clips_screen_->OnMouseWheel(delta);
                RenderFrame();
                return 0;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            RenderFrame();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETCLIENTAREAANIMATION) motion::RefreshEnabled();
            if (lp && std::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 &&
                settings_screen_) {
                settings_screen_->OnSystemThemeChanged();  // ui.theme "system" follows Windows live
                RequestRepaint();
            }
            break;

        case WM_TIMER:
            if (wp == kStatusTimerId && active_screen_ == ActiveScreen::Settings && settings_screen_)
                settings_screen_->OnStatusTick();
            // Paints only when a meter lights or drops a block: silence costs no frames.
            if (wp == kMeterTimerId && active_screen_ == ActiveScreen::Settings && settings_screen_ &&
                settings_screen_->OnMeterTick())
                RequestRepaint();
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            if (msg == kWmWindowListResult) {
                std::unique_ptr<WindowListResultMsg> owned(reinterpret_cast<WindowListResultMsg*>(lp));
                if (owned->generation == window_list_generation_.load(std::memory_order_relaxed) &&
                    owned->on_result) {
                    owned->on_result(std::move(owned->windows));
                }
                return 0;
            }
            if (msg == kWmMonitorListResult) {
                std::unique_ptr<MonitorListResultMsg> owned(reinterpret_cast<MonitorListResultMsg*>(lp));
                if (owned->on_result) {
                    owned->on_result(std::move(owned->monitors));
                }
                return 0;
            }
            if (msg == kWmMonitorThumbReady) {
                std::unique_ptr<MonitorThumbReadyMsg> owned(reinterpret_cast<MonitorThumbReadyMsg*>(lp));
                if (owned->on_result) {
                    owned->on_result(std::move(owned->device), owned->ok, owned->width, owned->height,
                                      std::move(owned->bgra));
                }
                return 0;
            }
            if (msg == kWmProcessListResult) {
                std::unique_ptr<ProcessListResultMsg> owned(reinterpret_cast<ProcessListResultMsg*>(lp));
                if (owned->generation == process_list_generation_.load(std::memory_order_relaxed) &&
                    owned->on_result) {
                    owned->on_result(std::move(owned->processes));
                }
                return 0;
            }
            if (msg == kWmAudioSessionListResult) {
                std::unique_ptr<AudioSessionListResultMsg> owned(
                    reinterpret_cast<AudioSessionListResultMsg*>(lp));
                if (owned->generation == audio_session_list_generation_.load(std::memory_order_relaxed) &&
                    owned->on_result) {
                    owned->on_result(std::move(owned->sessions));
                }
                return 0;
            }
            if (msg == kWmAudioDeviceListResult) {
                std::unique_ptr<AudioDeviceListResultMsg> owned(
                    reinterpret_cast<AudioDeviceListResultMsg*>(lp));
                if (owned->generation == audio_device_list_generation_.load(std::memory_order_relaxed) &&
                    owned->on_result) {
                    owned->on_result(std::move(owned->devices));
                }
                return 0;
            }
            if (msg == kWmSetConfigResult) {
                std::unique_ptr<SetConfigResultMsg> owned(reinterpret_cast<SetConfigResultMsg*>(lp));
                if (owned->on_result) owned->on_result(std::move(owned->resp));
                return 0;
            }
            if (msg == kWmFrameTick) {
                frame_tick_pending_ = false;
                const bool clips_frame = frame_demand_.clips && active_screen_ == ActiveScreen::Clips && clips_screen_ &&
                                         clips_screen_->OnFrameTimer();
                if (frame_demand_.PaintOnTick(clips_frame)) RenderFrame();
                return 0;
            }
            if (msg == kWmRunOnUi) {
                std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
                if (*fn) (*fn)();
                return 0;
            }
            if (msg == kWmFileIconsResult) {
                std::unique_ptr<FileIconsResultMsg> owned(reinterpret_cast<FileIconsResultMsg*>(lp));
                if (owned->on_result) owned->on_result(std::move(owned->icons));
                return 0;
            }
            if (msg == kWmPreviewResult) {
                std::unique_ptr<PreviewResultMsg> owned(reinterpret_cast<PreviewResultMsg*>(lp));
                if (owned->on_result) owned->on_result(std::move(owned->preview_resp), std::move(owned->status_resp));
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Headless host for `ombra-ui.exe --drive` (driver.cpp)
// ---------------------------------------------------------------------------------------------

bool OverlayHost::StartHeadless(int width, int height, float dpi_scale, PipeClient* client) {
    client_ = client;
    if (!InitGraphics()) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    headless_tex_.Reset();
    if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &headless_tex_))) return false;
    ComPtr<IDXGISurface> surface;
    if (FAILED(headless_tex_.As(&surface))) return false;
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    d2d_target_bitmap_.Reset();
    if (FAILED(d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &d2d_target_bitmap_))) return false;
    d2d_context_->SetTarget(d2d_target_bitmap_.Get());

    // Message-only: never shown, can't take focus or the foreground. It exists so the async
    // workers' PostMessageW replies and the status timer reach HandleMessage as they do live.
    if (!hwnd_) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayHost::WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"OmbraDriverHost";
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"" OMBRA_PRODUCT_NAME " driver", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, inst, this);
        if (!hwnd_) return false;
        SetTimer(hwnd_, kStatusTimerId, 1000, nullptr);
        if (settings_screen_ && active_screen_ == ActiveScreen::Settings) settings_screen_->SyncMeters();
    }

    bounds_    = {0, 0, width, height};
    dpi_scale_ = dpi_scale;
    dpi_       = static_cast<int>(96.0f * dpi_scale);
    theme_.SetFontScale(dpi_scale_);

    if (client_ && client_->Connected()) {
        LoadFromEngine();
        if (settings_screen_) settings_screen_->FetchPreviewAsync();
        ShowPendingAutotuneModal();
    } else {
        SetTestScreen(ActiveScreen::Settings);  // seeds the schema with defaults
        active_screen_ = ActiveScreen::Home;
        // The theme too, as Show() does from get_config; otherwise the first staged edit re-applies
        // the ui.* defaults and the panel jumps.
        const ombra::JsonValue defaults = ombra::SerializeConfigFlat(ombra::Config{});
        theme_.ApplyUiConfig([&defaults](const char* key) { return defaults.Find(key); });
    }
    RenderFrame();
    return true;
}

void OverlayHost::DispatchForDriver(UINT msg, WPARAM wp, LPARAM lp) {
    if (hwnd_) HandleMessage(hwnd_, msg, wp, lp);
}

bool OverlayHost::ReadFrame(std::vector<uint8_t>& bgra, int& width, int& height) {
    if (!headless_tex_) return false;
    D3D11_TEXTURE2D_DESC td{};
    headless_tex_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &staging))) return false;
    d3d_context_->CopyResource(staging.Get(), headless_tex_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    width  = static_cast<int>(td.Width);
    height = static_cast<int>(td.Height);
    bgra.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(bgra.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(width) * 4);
    }
    d3d_context_->Unmap(staging.Get(), 0);
    return true;
}

D2D1_RECT_F OverlayHost::HeaderButtonBounds(bool close) const {
    if (close) return close_btn_.bounds;
    return active_screen_ == ActiveScreen::Home ? gear_btn_.bounds : back_btn_.bounds;
}

void OverlayHost::SetTestScreen(ActiveScreen screen, int settings_rail_index, int settings_sub_tab_index) {
    active_screen_ = screen;
    if (screen == ActiveScreen::Settings && settings_screen_) {
        // Seed a minimal schema so the settings screen has something to lay out even without a
        // live engine connection (test-only path -- Show() does the real fetch). Also seed
        // default config values (rather than nullptr) so snapshot renders show real defaults
        // (e.g. framerate 60, bitrate 40000) instead of every widget falling back to its
        // first-option/off placeholder -- that placeholder look was indistinguishable from a
        // real value-binding bug when reviewing the PNGs.
        if (settings_screen_->Specs().empty()) {
            auto specs = ParseSchema(ombra::SerializeSettingsSchema());
            ombra::Config defaults;
            ombra::JsonValue defaults_json = ombra::SerializeConfigFlat(defaults);
            settings_screen_->InitSchema(specs, &defaults_json);
        }
        settings_screen_->SetActiveRail(settings_rail_index);
        if (settings_sub_tab_index != 0) settings_screen_->SetActiveSubTab(settings_sub_tab_index);
    }
}

bool OverlayHost::RenderOffscreenTest(int width, int height, const std::wstring& out_png_path,
                                      bool* out_card_drawn) {
    if (!InitGraphics()) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) return false;

    ComPtr<IDXGISurface> surface;
    hr = tex.As(&surface);
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    ComPtr<ID2D1Bitmap1> target_bm;
    hr = d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bm);
    if (FAILED(hr)) return false;

    d2d_context_->SetTarget(target_bm.Get());
    d2d_target_bitmap_ = target_bm;

    bounds_ = { 0, 0, width, height };
    dpi_scale_ = test_dpi_scale_;
    dpi_ = static_cast<int>(96.0f * dpi_scale_);
    theme_.SetFontScale(dpi_scale_);

    RenderFrame();

    d2d_context_->SetTarget(nullptr);
    d2d_target_bitmap_.Reset();

    D3D11_TEXTURE2D_DESC staging_desc = td;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) return false;

    d3d_context_->CopyResource(staging.Get(), tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const uint8_t* pixels = static_cast<const uint8_t*>(mapped.pData);

    // Far corner (outside the panel) should carry the full-screen scrim's translucent alpha --
    // dimmed like the rest of the desktop/game behind the overlay, but not opaque like the panel.
    // The scrim is full-viewport, so the bar here checks
    // "translucent, not opaque" rather than "fully clear".
    const size_t corner_idx = (height - 5) * mapped.RowPitch + 5 * 4;
    const uint8_t corner_a = pixels[corner_idx + 3];

    // Sample somewhere guaranteed to be opaque-ish chrome. Neither screen has a wrapping panel any
    // more -- both share the same floating gear/back +
    // close pill in the top-right corner (DrawFloatingHeader), so one sample point now covers both.
    const float margin = 32.0f;
    const float btn_sz = 38.0f;
    const float pill_pad = 4.0f;
    const float pill_w = btn_sz * 2.0f + pill_pad * 3.0f;
    const float pill_h = btn_sz + pill_pad * 2.0f;
    const int sample_x = static_cast<int>(static_cast<float>(width) - margin - pill_w * 0.5f);
    const int sample_y = static_cast<int>(margin + pill_h * 0.5f);
    const size_t nav_idx = static_cast<size_t>(sample_y) * mapped.RowPitch + static_cast<size_t>(sample_x) * 4;
    const uint8_t nav_a = pixels[nav_idx + 3];

    // With the screen frame on, this corner sits inside the frame's rounded-off spandrel, so it must
    // be opaque instead -- which also proves the frame actually rendered.
    const bool backdrop_ok = theme_.screen_frame ? corner_a >= 250 : (corner_a > 40 && corner_a < 140);
    const bool card_ok = nav_a >= 200;

    if (out_card_drawn) *out_card_drawn = (backdrop_ok && card_ok);

    if (!out_png_path.empty() && wic_factory_) {
        size_t slash = out_png_path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            CreateDirectoryW(out_png_path.substr(0, slash).c_str(), nullptr);
        }
        SaveBgraToPng(wic_factory_.Get(), width, height, pixels, mapped.RowPitch, out_png_path);
    }

    d3d_context_->Unmap(staging.Get(), 0);
    return backdrop_ok && card_ok;
}

int RunOverlayTest(const std::wstring& out_png_path, ActiveScreen screen, int settings_rail_index,
                   int width, int height, PanelPosition position, PanelAttach attach,
                   int settings_sub_tab_index, std::function<void(OverlayHost&)> before_render) {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    };

    OverlayHost host;
    if (!host.InitGraphics()) {
        check(false, "Direct2D offscreen overlay frame rendered successfully");
        return 1;
    }
    host.SetTestScreen(screen, settings_rail_index, settings_sub_tab_index);
    if (before_render) before_render(host);
    host.GetTheme().panel_position = position;
    host.GetTheme().panel_attach = attach;
    bool card_drawn = false;
    const bool render_ok = host.RenderOffscreenTest(width, height, out_png_path, &card_drawn);

    check(render_ok, "Direct2D offscreen overlay frame rendered successfully");
    check(card_drawn, "Overlay scrim/panel geometry verified in pixel buffer (dimmed corner, opaque nav bar)");

    return fails == 0 ? 0 : 1;
}

}  // namespace ombra_ui
