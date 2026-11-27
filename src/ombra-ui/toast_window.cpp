// See toast_window.h for the "why a new class, why no pipe" summary.
//
// InitGraphics() below is a deliberate near-duplicate of OverlayHost::InitGraphics
// (overlay_window.cpp) -- same D3D11 -> DXGI -> D2D1 -> DWrite -> DirectComposition sequence, same
// hardware-then-WARP fallback. A shared extraction would mean inventing a struct to carry six COM
// pointers out to a second, much simpler caller; not worth it for one popup window. If the two ever
// need to be reconciled (e.g. a real device-loss/recreate path gets added to one but not the
// other), start here.
#include "ombra-ui/toast_window.h"
#include "ombra-ui/monitor_adapter.h"
#include "ombra/brand.h"
#include "ombra-ui/ui/containers.h"
#include "ombra-ui/ui/theme.h"

#include "ombra/config.h"
#include "ombra/log.h"
#include "ombra/resource.h"
#include "ombra/save_toast.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kToastWindowClass[] = L"OmbraToastWindow";

// Window placement (DIPs, scaled by the toast monitor's own DPI at creation time) and fade timing.
// The card's own size is ToastCard::Width/Height.
constexpr float kMarginDip      = 24.0f;   // gap from the work-area's edge on the axis the card does NOT slide along
constexpr float kEdgeGapDip     = 4.0f;    // gap from the work-area's edge on the slide axis -- deliberately tiny
                                            // (not kMarginDip): the card should
                                            // read as docked/sticking to the screen edge, not floating inset from it.
                                            // The gap is inside the window, which sits flush with the edge, so the
                                            // slide clips at the screen edge rather than 4 dip short of it.

// Motion runs as DirectComposition animations: the card is rendered and presented once, and DWM
// evaluates offset/opacity curves at the panel's own refresh rate (144 Hz here) with no per-frame
// work in this process. The old WM_TIMER loop re-rendered at the 15.6 ms system-timer cadence off a
// GetTickCount64 clock, so frames landed every 2-3 vsyncs at uneven intervals -- visible judder.
// Short travel with cubic easing, the shape Captail's notification uses (WPF, 26/18 px, 230/180 ms).
constexpr double kEnterSlideSec = 0.24;
constexpr double kEnterFadeSec  = 0.17;
constexpr double kLeaveSec      = 0.18;
constexpr float  kEnterTravelDip = 28.0f;
constexpr float  kLeaveTravelDip = 18.0f;
constexpr ULONGLONG kEnterMs = static_cast<ULONGLONG>(kEnterSlideSec * 1000);
constexpr ULONGLONG kLeaveMs = static_cast<ULONGLONG>(kLeaveSec * 1000) + 20;  // + a frame of slack before Close
int64_t QpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

constexpr float kTestMarginPx  = 28.0f;  // --toasttest backdrop around the card

enum class ToastPosition { TopRight, BottomRight, TopLeft, BottomLeft };

ToastPosition ParseToastPosition(const std::string& s) {
    if (s == "top_right")    return ToastPosition::TopRight;
    if (s == "top_left")     return ToastPosition::TopLeft;
    if (s == "bottom_left")  return ToastPosition::BottomLeft;
    return ToastPosition::BottomRight;  // default, matches config.h's default
}

bool ToastPositionIsRight(ToastPosition p) {
    return p == ToastPosition::TopRight || p == ToastPosition::BottomRight;
}

bool ToastPositionIsTop(ToastPosition p) {
    return p == ToastPosition::TopRight || p == ToastPosition::TopLeft;
}

using ToastTone = ToastCard::Tone;

struct ToastInfo {
    IconType icon = IconType::None;
    ombra::ToastText text;
    ToastTone tone = ToastTone::Neutral;
    bool brand = false;  // the brand mark instead of the icon: a save, pending and done
};

// event string (engine.cpp's SpawnToast argv) -> {icon, headline and detail, tone}. The headline
// says what happened; the detail line why, or what follows. Returns false for an unrecognized
// event: log and return 1, no window.
bool LookupToastInfo(const std::wstring& event, ToastInfo* out) {
    struct Entry {
        const wchar_t* event;
        IconType       icon;
        const wchar_t* title;
        const wchar_t* detail;
        ToastTone      tone;
        bool           brand;
    };
    using I = IconType;
    using T = ToastTone;
    static const Entry kEntries[] = {
        {L"save", I::None, L"Saving clip…", L"", T::Neutral, true},
        // Gains its length and game once the engine says (save_toast.h SavedToastText).
        {L"save_ok", I::None, L"Clip saved", L"", T::Success, true},
        {L"save_failed", I::Alert, L"Clip not saved", L"See the log", T::Danger, false},
        {L"save_nothing_new", I::Alert, L"Nothing new to save", L"Already in the last clip", T::Warning, false},
        // The output folder couldn't take the clip (a removed drive, a full disk); save.cpp's fallback.
        {L"save_elsewhere", I::Alert, L"Clip saved to Videos\\" OMBRA_PRODUCT_NAME, L"Output folder unavailable", T::Warning, false},
        // A frame-exact trim that ended after the overlay closed (engine.cpp RunExport): nobody saw it.
        {L"trim_saved", I::None, L"Trim saved", L"", T::Success, true},
        {L"trim_replaced", I::None, L"Trim saved", L"Replaced the original", T::Success, true},
        {L"trim_kept", I::Alert, L"Trim saved", L"Original kept", T::Warning, false},
        {L"trim_failed", I::Alert, L"Trim not saved", L"See the log", T::Danger, false},
        // A screenshot's failure, and its fallback (ScreenshotWorkerLoop). A plain success shows nothing.
        {L"shot_failed", I::Alert, L"Screenshot not saved", L"See the log", T::Danger, false},
        {L"shot_elsewhere", I::Alert, L"Screenshot saved to Videos\\" OMBRA_PRODUCT_NAME, L"Output folder unavailable", T::Warning, false},
        // The buffer's file became unusable (Ring::TakeStorageLost).
        {L"buffer_storage_lost", I::Alert, L"Buffering to RAM", L"Buffer drive unavailable", T::Warning, false},
        {L"buffer_on", I::Replay, L"Buffer on", L"", T::Success, false},
        {L"buffer_off", I::Replay, L"Buffer off", L"", T::Muted, false},
        // A recording's start and end (recording.h RecordEndToast). Saved gains its length (save_toast.h).
        {L"record_started", I::Record, L"Recording started", L"", T::Success, false},
        {L"record_saved", I::Record, L"Recording saved", L"", T::Success, false},
        {L"record_nothing", I::Alert, L"Nothing to record", L"", T::Warning, false},
        {L"record_disk_low", I::Alert, L"Recording saved", L"Disk nearly full", T::Warning, false},
        {L"record_write_failed", I::Alert, L"Recording cut short", L"See the log", T::Danger, false},
        // Recordings a crash left fragmented, made plain at engine start (open_recording.h).
        {L"record_recovered", I::Record, L"Recording recovered", L"", T::Success, false},
        {L"record_failed", I::Alert, L"Recording not saved", L"See the log", T::Danger, false},
        // The engine fell back to x264 because no GPU encoder would open (supervisor.h).
        {L"cpu_encoder", I::Alert, L"Recording on the CPU", L"The GPU encoder failed", T::Warning, false},
        {L"cpu_encoder_old_driver", I::Alert, L"Recording on the CPU", L"NVIDIA driver too old", T::Warning, false},
    };
    for (const Entry& e : kEntries) {
        if (event != e.event) continue;
        *out = {e.icon, {e.title, e.detail, {}}, e.tone, e.brand};
        return true;
    }
    return false;
}

// Copied from overlay_window.cpp's anonymous-namespace SaveBgraToPng --
// see toast_window.h's header comment on why this is a copy rather than a shared extraction.
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

class ToastWindow {
public:
    // Mirrors OverlayHost's ctor/dtor: this process never constructs an OverlayHost (the --toast
    // argv branch returns before RunUi() ever gets there), so nothing else in the process has
    // called CoInitializeEx yet -- ToastWindow owns that pairing itself.
    ToastWindow() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&wic_factory_));
    }
    ~ToastWindow() {
        Close();
        // Release every COM interface before uninitializing the apartment (Release() after
        // CoUninitialize() is undefined behavior) -- same ordering as OverlayHost's destructor.
        wic_factory_.Reset();
        dwrite_factory_.Reset();
        d2d_context_.Reset();
        d2d_device_.Reset();
        d2d_factory_.Reset();
        dxgi_factory_.Reset();
        dxgi_device_.Reset();
        d3d_context_.Reset();
        d3d_device_.Reset();
        CoUninitialize();
    }
    ToastWindow(const ToastWindow&) = delete;
    ToastWindow& operator=(const ToastWindow&) = delete;

    // warp: offscreen test renders stay off the GPU. `mon`: draw on the GPU that drives it.
    bool InitGraphics(bool warp = false, HMONITOR mon = nullptr);
    // width/height/x/y are already DPI-scaled physical pixels -- caller resolves the target
    // monitor's DPI before calling this (see RunToast).
    bool CreateHostWindow(int x, int y, int width, int height);
    bool CreateSwapchainAndDComp(int width, int height);
    void RenderCard();  // draws the card at rest and presents it; motion is DComp's (Animate)
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }

    void RunLoop();  // pumps messages and waits on the save handles until the leave animation ends

    // Headless: renders the card at rest into an offscreen texture and saves it as a PNG.
    // Never creates hwnd_/swapchain_/dcomp_* -- matches RenderOffscreenTest's shape.
    bool RenderOffscreenTest(int width, int height, const std::wstring& out_png_path, bool alpha = false,
                             float scale = 1.0f);

    ToastInfo info{};
    float dpi_scale = 1.0f;
    Theme& GetTheme() { return theme_; }
    HWND Hwnd() const { return hwnd_; }

    // The card's window never moves after creation -- see the comment above DrawCard for
    // why. This only records which way the *content* slides.
    void SetSlideFromRight(bool from_right) { slide_from_right_ = from_right; }
    // Space between the card and the window's edge-side border (kEdgeGapDip, physical px).
    void SetEdgeGap(float px) { edge_gap_ = px; }

    // A "save" toast: shown at the hotkey press, pending until one of the result handles is
    // signalled (ok, nothing_new, failed events, the engine process exiting, elsewhere), then it
    // holds the result. The sixth, an auto-reset event, says the clip's end or names moved.
    // `info` maps a SaveToastInfo the engine fills before it signals ok.
    void SetSaveWaits(std::vector<HANDLE> waits, HANDLE info_mapping) {
        waits_   = std::move(waits);
        info_    = info_mapping;
        pending_ = waits_.size() == kSaveWaits;
        // The engine wrote the asking app's name before spawning this process: shown in every state.
        if (info_) {
            if (const void* view = MapViewOfFile(info_, FILE_MAP_READ, 0, 0, sizeof(ombra::SaveToastInfo))) {
                const auto* saved = static_cast<const ombra::SaveToastInfo*>(view);
                if (const auto live = ombra::ReadToastLive(*saved)) {
                    app      = live->app;
                    end_qpc_ = live->end_qpc;
                }
                info_seconds = saved->seconds;
                info_count   = saved->clips;
                UnmapViewOfFile(view);
            }
        }
    }
    std::wstring app;     // the API app whose action this is; empty for the user's own
    float info_seconds = 0;  // the length a one-shot toast's mapping names (a recording's); 0: none
    uint32_t info_count = 0;  // and how many files it covers (recovered recordings)
    bool IsPending() const { return pending_; }
    void SetCompact(bool compact) { compact_ = compact; }
    void SetDetails(bool on) { details_ = on; }    // ui.toast_details
    void SetTimeBar(bool on) { time_bar_ = on; }   // ui.toast_time_bar
    void SetHoldMs(ULONGLONG ms) { hold_ms_ = ms; }  // ui.toast_duration
    // The card's height for this toast: two lines when it will ever show a detail line (a save
    // always may: its result's), and the time bar's foot. Theme and DPI set first.
    float CardHeight(bool save) const;
    // The bar in --toasttest's still frame: how much of it is left, 0..1.
    float test_bar_fill = 0.0f;
    bool test_pending = false;  // --toasttest's "save": drawn as the live pending card, no track

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    ToastCard MakeCard(float width, float height) const;
    void DrawCard(float width, float height);
    bool PollSaveResult();  // true once a pending save's result has replaced the card's content
    // A pending card's countdown, and its end and names re-read when the engine `changed` them;
    // true if the card needs drawing again.
    bool RefreshPending(bool changed);
    bool Animate(float x_from, float x_to, float op_from, float op_to, bool ease_out);
    // The time bar's fill as a child visual whose clip DWM drains over the hold, after `delay_sec`
    // (what is left of the enter): no frame drawn in this process while it runs.
    bool StartBar(double delay_sec);
    bool StartBarOnce(double delay_sec);

    static constexpr size_t kSaveWaits = 6;  // five results, then the change event
    static constexpr DWORD  kSaveResults = 5;
    HWND hwnd_ = nullptr;
    std::vector<HANDLE> waits_;
    HANDLE info_ = nullptr;
    bool compact_ = false;
    bool details_ = true;
    bool time_bar_ = true;
    ULONGLONG hold_ms_ = 3000;
    bool pending_ = false;
    ULONGLONG start_tick_ = 0;
    int64_t start_qpc_ = 0, end_qpc_ = 0, qpc_freq_ = 1;
    bool slide_from_right_ = true;
    float edge_gap_ = 0.0f;
    std::wstring adapter_name_;

    ComPtr<ID3D11Device>        d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<IDXGIDevice>         dxgi_device_;
    ComPtr<IDXGIFactory2>       dxgi_factory_;
    ComPtr<IDXGISwapChain1>     swapchain_;

    ComPtr<ID2D1Factory1>       d2d_factory_;
    ComPtr<ID2D1Device>         d2d_device_;
    ComPtr<ID2D1DeviceContext>  d2d_context_;
    ComPtr<ID2D1Bitmap1>        d2d_target_bitmap_;

    ComPtr<IDWriteFactory>      dwrite_factory_;
    ComPtr<IDCompositionDevice> dcomp_device_;
    ComPtr<IDCompositionTarget> dcomp_target_;
    ComPtr<IDCompositionVisual> dcomp_visual_;
    ComPtr<IDCompositionEffectGroup> dcomp_effect_;
    ComPtr<IDCompositionVisual> bar_visual_;
    ComPtr<IDCompositionSurface> bar_surface_;
    ComPtr<IDCompositionRectangleClip> bar_clip_;

    ComPtr<IWICImagingFactory>  wic_factory_;

    Theme theme_;
};

bool ToastWindow::InitGraphics(bool warp, HMONITOR mon) {
    if (d3d_device_) return true;

    UINT d3d_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    d3d_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL created_level;

    ComPtr<IDXGIAdapter1> monitor_adapter = warp ? nullptr : AdapterForMonitor(mon);
    HRESULT hr = E_FAIL;
    if (monitor_adapter) {
        hr = D3D11CreateDevice(monitor_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, d3d_flags,
                               feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
                               &d3d_device_, &created_level, &d3d_context_);
    }
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, d3d_flags,
            feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
            &d3d_device_, &created_level, &d3d_context_);
    }
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, d3d_flags,
            feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
            &d3d_device_, &created_level, &d3d_context_);
        if (FAILED(hr)) {
            ombra::Log::Warn("ToastWindow: D3D11CreateDevice failed (0x%08X)", hr);
            return false;
        }
    }

    hr = d3d_device_.As(&dxgi_device_);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device_->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(adapter->GetDesc(&adapter_desc))) adapter_name_ = adapter_desc.Description;

    hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory_));
    if (FAILED(hr)) return false;

    D2D1_FACTORY_OPTIONS d2d_opts{};
#if defined(_DEBUG)
    d2d_opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2d_opts,
                           reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        ombra::Log::Warn("ToastWindow: D2D1CreateFactory failed (0x%08X)", hr);
        return false;
    }

    hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_);
    if (FAILED(hr)) return false;

    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context_);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    theme_.InitFonts(dwrite_factory_.Get());

    hr = DCompositionCreateDevice(dxgi_device_.Get(), __uuidof(IDCompositionDevice),
                                  reinterpret_cast<void**>(dcomp_device_.GetAddressOf()));
    if (FAILED(hr)) return false;

    return true;
}

bool ToastWindow::CreateHostWindow(int x, int y, int width, int height) {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ToastWindow::WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kToastWindowClass;
    RegisterClassExW(&wc);

    // LAYERED + TRANSPARENT: click-through for real -- hit-testing skips the window, so the game
    // under it keeps its clicks and its cursor (TRANSPARENT alone still takes them). NOACTIVATE:
    // never steals focus/foreground. TOOLWINDOW: no taskbar or alt-tab entry, as OverlayHost's popup.
    const DWORD ex_style = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                           WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    const DWORD win_style = WS_POPUP;

    hwnd_ = CreateWindowExW(ex_style, kToastWindowClass, L"" OMBRA_PRODUCT_NAME " Toast", win_style,
                            x, y, width, height, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        ombra::Log::Warn("ToastWindow: CreateWindowExW failed (0x%08X)", GetLastError());
        return false;
    }
    // A layered window shows nothing until its attributes are set; fully opaque here, since the
    // card's alpha is DirectComposition's.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    // Never let this popup get burned into the desktop capture that's the whole point of the app --
    // (the overlay panel has this same latent gap today, not fixed here).
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);

    dpi_scale = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    theme_.SetFontScale(dpi_scale);
    return true;
}

bool ToastWindow::CreateSwapchainAndDComp(int width, int height) {
    if (!dxgi_factory_ || !dcomp_device_ || !hwnd_) return false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = dxgi_factory_->CreateSwapChainForComposition(d3d_device_.Get(), &scd, nullptr, &swapchain_);
    if (FAILED(hr)) return false;

    hr = dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_);
    if (FAILED(hr)) return false;

    hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    if (FAILED(hr)) return false;

    hr = dcomp_visual_->SetContent(swapchain_.Get());
    if (FAILED(hr)) return false;

    hr = dcomp_device_->CreateEffectGroup(&dcomp_effect_);
    if (FAILED(hr)) return false;
    hr = dcomp_visual_->SetEffect(dcomp_effect_.Get());
    if (FAILED(hr)) return false;

    hr = dcomp_target_->SetRoot(dcomp_visual_.Get());
    if (FAILED(hr)) return false;
    // No Commit yet: Animate() commits the tree together with the first animation, so the window
    // never composes a frame of the card at rest before the enter animation starts.

    ComPtr<IDXGISurface> surface;
    hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    hr = d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &d2d_target_bitmap_);
    if (FAILED(hr)) return false;

    d2d_context_->SetTarget(d2d_target_bitmap_.Get());
    return true;
}

float ToastWindow::CardHeight(bool save) const {
    ToastCard probe;
    probe.detail = info.text.detail;
    probe.app = app;
    probe.details = details_;
    const bool two_lines = (save && (details_ || !app.empty())) || !probe.ShownDetail().empty();
    return ToastCard::Height(theme_, dpi_scale, two_lines, time_bar_ && !compact_);
}

// The card is ToastCard (ui/containers.h), at (0, 0) and `width` x `height`. A pending save shows
// no track: its bar starts with the result.
ToastCard ToastWindow::MakeCard(float width, float height) const {
    ToastCard card;
    card.title = info.text.title;
    card.detail = info.text.detail;
    card.stat = info.text.stat;
    card.icon = info.icon;
    card.tone = info.tone;
    card.brand = info.brand;
    card.app = app;
    card.details = details_;
    card.compact = compact_;
    card.trailing = slide_from_right_;
    card.time_bar = time_bar_;
    card.bar_track = !pending_ && !test_pending;
    card.bar_fill = test_bar_fill;
    card.bounds = D2D1::RectF(0, 0, width, height);
    return card;
}

void ToastWindow::DrawCard(float width, float height) {
    d2d_context_->SetTextRenderingParams(theme_.TextRendering());  // ui.font's grid-fit (Theme::TextRendering)
    MakeCard(width, height).Draw(d2d_context_.Get(), theme_, dpi_scale);
}

void ToastWindow::RenderCard() {
    if (!d2d_context_ || !d2d_target_bitmap_) return;
    d2d_context_->BeginDraw();
    d2d_context_->Clear(D2D1::ColorF(0, 0, 0, 0));
    const D2D1_SIZE_F sz = d2d_target_bitmap_->GetSize();
    d2d_context_->SetTransform(D2D1::Matrix3x2F::Translation(slide_from_right_ ? 0.0f : edge_gap_, 0.0f));
    DrawCard(sz.width - edge_gap_, sz.height);
    d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = d2d_context_->EndDraw();
    // Present(0): nothing is paced by this present any more, so don't block on a vsync.
    if (SUCCEEDED(hr) && swapchain_) swapchain_->Present(0, 0);
}

// One cubic segment from `from` to `to` over `sec`: ease-out 1-(1-s)^3 = 3s-3s^2+s^3, ease-in s^3.
static ComPtr<IDCompositionAnimation> MakeCurve(IDCompositionDevice* dev, float from, float to,
                                                double sec, bool ease_out) {
    ComPtr<IDCompositionAnimation> anim;
    if (FAILED(dev->CreateAnimation(&anim))) return nullptr;
    const float d = to - from;
    const float t = static_cast<float>(sec);
    if (ease_out) anim->AddCubic(0.0, from, 3.0f * d / t, -3.0f * d / (t * t), d / (t * t * t));
    else          anim->AddCubic(0.0, from, 0.0f, 0.0f, d / (t * t * t));
    anim->End(sec, to);
    return anim;
}

bool ToastWindow::Animate(float x_from, float x_to, float op_from, float op_to, bool ease_out) {
    if (!dcomp_device_ || !dcomp_visual_ || !dcomp_effect_) return false;
    IDCompositionDevice* dev = dcomp_device_.Get();
    const double slide_sec = ease_out ? kEnterSlideSec : kLeaveSec;
    const double fade_sec  = ease_out ? kEnterFadeSec  : kLeaveSec;
    ComPtr<IDCompositionAnimation> x  = MakeCurve(dev, x_from, x_to, slide_sec, ease_out);
    ComPtr<IDCompositionAnimation> op = MakeCurve(dev, op_from, op_to, fade_sec, ease_out);
    if (!x || !op) return false;
    if (FAILED(dcomp_visual_->SetOffsetX(x.Get())) || FAILED(dcomp_effect_->SetOpacity(op.Get()))) return false;
    return SUCCEEDED(dcomp_device_->Commit());
}

bool ToastWindow::StartBar(double delay_sec) {
    if (bar_visual_) return false;  // already running
    if (StartBarOnce(delay_sec)) return true;
    // A part-built bar: dropped, so the result's call can try again.
    if (bar_visual_ && dcomp_visual_) dcomp_visual_->RemoveVisual(bar_visual_.Get());
    bar_visual_.Reset();
    bar_clip_.Reset();
    bar_surface_.Reset();
    return false;
}

bool ToastWindow::StartBarOnce(double delay_sec) {
    if (!time_bar_ || compact_ || !dcomp_device_ || !dcomp_visual_ || !d2d_target_bitmap_) return false;
    const D2D1_SIZE_F sz = d2d_target_bitmap_->GetSize();
    const ToastCard card = MakeCard(sz.width - edge_gap_, sz.height);
    const D2D1_RECT_F track = card.BarTrack(theme_, dpi_scale);
    const float x0 = track.left + (slide_from_right_ ? 0.0f : edge_gap_);
    const UINT w = static_cast<UINT>(std::ceil(track.right - track.left));
    const UINT h = static_cast<UINT>(std::ceil(track.bottom - track.top));
    if (!w || !h) return false;

    // The fill: one solid surface of the tone, drawn once.
    HRESULT hr = dcomp_device_->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &bar_surface_);
    if (FAILED(hr)) return false;
    ComPtr<IDXGISurface> dxgi;
    POINT at{};
    if (FAILED(bar_surface_->BeginDraw(nullptr, IID_PPV_ARGS(&dxgi), &at))) return false;
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> target;
    hr = d2d_context_->CreateBitmapFromDxgiSurface(dxgi.Get(), &bp, &target);
    if (SUCCEEDED(hr)) {
        d2d_context_->SetTarget(target.Get());
        d2d_context_->BeginDraw();
        d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
        ComPtr<ID2D1SolidColorBrush> brush;
        d2d_context_->CreateSolidColorBrush(card.Hue(theme_), &brush);
        if (brush)
            d2d_context_->FillRectangle(D2D1::RectF(static_cast<float>(at.x), static_cast<float>(at.y),
                                                    static_cast<float>(at.x + w), static_cast<float>(at.y + h)),
                                        brush.Get());
        hr = d2d_context_->EndDraw();
        d2d_context_->SetTarget(d2d_target_bitmap_.Get());
    }
    bar_surface_->EndDraw();
    if (FAILED(hr)) return false;

    // Its clip, rounded as the track is, drains from the right: full until the enter ends, then
    // linear to nothing over the hold.
    if (FAILED(dcomp_device_->CreateVisual(&bar_visual_)) || FAILED(dcomp_device_->CreateRectangleClip(&bar_clip_)))
        return false;
    const float fw = static_cast<float>(w), r = static_cast<float>(h) / 2;
    bar_clip_->SetLeft(0.0f);
    bar_clip_->SetTop(0.0f);
    bar_clip_->SetBottom(static_cast<float>(h));
    bar_clip_->SetTopLeftRadiusX(r);
    bar_clip_->SetTopLeftRadiusY(r);
    bar_clip_->SetTopRightRadiusX(r);
    bar_clip_->SetTopRightRadiusY(r);
    bar_clip_->SetBottomLeftRadiusX(r);
    bar_clip_->SetBottomLeftRadiusY(r);
    bar_clip_->SetBottomRightRadiusX(r);
    bar_clip_->SetBottomRightRadiusY(r);
    ComPtr<IDCompositionAnimation> right;
    if (FAILED(dcomp_device_->CreateAnimation(&right))) return false;
    const double hold = static_cast<double>(hold_ms_) / 1000.0, delay = (std::max)(0.0, delay_sec);
    if (delay > 0) right->AddCubic(0.0, fw, 0.0f, 0.0f, 0.0f);
    right->AddCubic(delay, fw, -fw / static_cast<float>(hold), 0.0f, 0.0f);
    right->End(delay + hold, 0.0f);
    bar_clip_->SetRight(right.Get());
    bar_visual_->SetContent(bar_surface_.Get());
    bar_visual_->SetOffsetX(x0);
    bar_visual_->SetOffsetY(track.top);
    bar_visual_->SetClip(bar_clip_.Get());
    // A child: it rides the card's slide and fade.
    if (FAILED(dcomp_visual_->AddVisual(bar_visual_.Get(), TRUE, nullptr))) return false;
    return SUCCEEDED(dcomp_device_->Commit());
}

bool ToastWindow::RenderOffscreenTest(int width, int height, const std::wstring& out_png_path, bool alpha, float scale) {
    if (!InitGraphics(/*warp=*/true)) return false;

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
    dpi_scale = scale;
    theme_.SetFontScale(dpi_scale);

    // The card over a busy, game-like backdrop, so translucency and contrast can be judged.
    {
        const float m = kTestMarginPx * scale;
        d2d_context_->BeginDraw();
        // alpha: the card alone on transparency, for compositing over real footage (the demo video).
        if (alpha) d2d_context_->Clear(D2D1::ColorF(0, 0, 0, 0));
        else {
        ComPtr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP gs[] = {{0.0f, D2D1::ColorF(0.95f, 0.55f, 0.20f)}, {0.45f, D2D1::ColorF(0.15f, 0.35f, 0.45f)},
                                         {1.0f, D2D1::ColorF(0.05f, 0.08f, 0.12f)}};
        d2d_context_->CreateGradientStopCollection(gs, 3, &stops);
        ComPtr<ID2D1LinearGradientBrush> grad;
        d2d_context_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(static_cast<float>(width), static_cast<float>(height))),
            stops.Get(), &grad);
        d2d_context_->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), grad.Get());
        ComPtr<ID2D1SolidColorBrush> spot;
        d2d_context_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 0.9f, 0.9f), &spot);
        d2d_context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(m + 60.0f, m + 10.0f), 40.0f, 26.0f), spot.Get());
        spot->SetColor(D2D1::ColorF(0.9f, 0.2f, 0.2f, 1.0f));
        d2d_context_->FillRectangle(D2D1::RectF(width - m - 90.0f, m + 30.0f, width - m - 30.0f, height - 6.0f), spot.Get());
        }
        d2d_context_->SetTransform(D2D1::Matrix3x2F::Translation(m, m));
        DrawCard(width - 2.0f * m, height - 2.0f * m);
        d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
        d2d_context_->EndDraw();
    }

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
    bool saved = true;
    if (!out_png_path.empty() && wic_factory_) {
        size_t slash = out_png_path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) CreateDirectoryW(out_png_path.substr(0, slash).c_str(), nullptr);
        if (alpha) {
            // The PNG is straight alpha; the texture is premultiplied.
            std::vector<uint8_t> straight(static_cast<size_t>(width) * height * 4);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const uint8_t* s = pixels + static_cast<size_t>(y) * mapped.RowPitch + x * 4;
                    uint8_t* d = straight.data() + (static_cast<size_t>(y) * width + x) * 4;
                    const int a = s[3];
                    for (int c = 0; c < 3; ++c) d[c] = a ? static_cast<uint8_t>((std::min)(255, (s[c] * 255 + a / 2) / a)) : 0;
                    d[3] = static_cast<uint8_t>(a);
                }
            saved = SaveBgraToPng(wic_factory_.Get(), width, height, straight.data(), width * 4, out_png_path);
        } else {
            saved = SaveBgraToPng(wic_factory_.Get(), width, height, pixels, mapped.RowPitch, out_png_path);
        }
    }

    d3d_context_->Unmap(staging.Get(), 0);
    return saved;
}

void ToastWindow::Close() {
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_bitmap_.Reset();
    if (dcomp_target_) dcomp_target_->SetRoot(nullptr);
    bar_visual_.Reset();
    bar_clip_.Reset();
    bar_surface_.Reset();
    dcomp_effect_.Reset();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    swapchain_.Reset();
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

bool ToastWindow::RefreshPending(bool changed) {
    if (!pending_) return false;
    std::wstring name = app;
    if (changed && info_) {  // the engine moved the end or the names: read them again
        if (const void* view = MapViewOfFile(info_, FILE_MAP_READ, 0, 0, sizeof(ombra::SaveToastInfo))) {
            if (const auto live = ombra::ReadToastLive(*static_cast<const ombra::SaveToastInfo*>(view))) {
                end_qpc_ = live->end_qpc;
                name     = live->app;
            }
            UnmapViewOfFile(view);
        }
    }
    ombra::ToastText text = ombra::PendingSaveText(end_qpc_, QpcNow(), qpc_freq_);
    if (text.title == info.text.title && text.detail == info.text.detail && name == app) return false;
    info.text = std::move(text);
    app       = name;
    return true;
}

bool ToastWindow::PollSaveResult() {
    if (!pending_) return false;
    // The results only: the change event after them is RefreshPending's.
    const DWORD r = WaitForMultipleObjects(kSaveResults, waits_.data(), FALSE, 0);
    const wchar_t* event = nullptr;
    if (r == WAIT_OBJECT_0)          event = L"save_ok";
    else if (r == WAIT_OBJECT_0 + 1) event = L"save_nothing_new";
    else if (r == WAIT_OBJECT_0 + 2 || r == WAIT_OBJECT_0 + 3 || r == WAIT_FAILED) event = L"save_failed";
    else if (r == WAIT_OBJECT_0 + 4) event = L"save_elsewhere";
    else if (QpcNow() >= ombra::PendingGiveUpQpc(start_qpc_, end_qpc_, qpc_freq_)) event = L"save_failed";
    if (!event) return false;
    LookupToastInfo(event, &info);
    if (r == WAIT_OBJECT_0 && info_) {
        if (const void* view = MapViewOfFile(info_, FILE_MAP_READ, 0, 0, sizeof(ombra::SaveToastInfo))) {
            info.text = ombra::SavedToastText(*static_cast<const ombra::SaveToastInfo*>(view));
            UnmapViewOfFile(view);
        }
    }
    pending_ = false;
    return true;
}

void ToastWindow::RunLoop() {
    start_tick_ = GetTickCount64();
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpc_freq_  = freq.QuadPart > 0 ? freq.QuadPart : 1;
    start_qpc_ = QpcNow();
    RefreshPending(/*changed=*/false);  // the countdown from the first frame (SetSaveWaits read the end)
    PollSaveResult();  // a save that finished before this process got here shows its result directly

    // Content first, then the enter animation and the visual tree in one Commit: the first composed
    // frame is already the animation's t=0 (transparent, offset), so nothing flashes and no glyph
    // rasterization lands mid-motion.
    RenderCard();
    const float sign = slide_from_right_ ? 1.0f : -1.0f;
    Animate(sign * kEnterTravelDip * dpi_scale, 0.0f, 0.0f, 1.0f, /*ease_out=*/true);
    if (!pending_) StartBar(static_cast<double>(kEnterMs) / 1000.0);

    // Spawn-to-first-frame, so a normal save shows how long process startup costs.
    FILETIME created{}, unused{}, now{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &unused, &unused, &unused)) {
        GetSystemTimePreciseAsFileTime(&now);
        const auto ticks = [](const FILETIME& f) {
            return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        ombra::Log::Info("toast first frame %.0f ms after spawn on %ls%s",
                         static_cast<double>(ticks(now) - ticks(created)) / 10000.0,
                         adapter_name_.c_str(), pending_ ? " (save still running)" : "");
    }

    // No timers: block on the save handles (while pending) and the message queue, with the next
    // phase deadline as the timeout. Deadlines only need ~16 ms precision; DWM owns the motion.
    ULONGLONG leave_at = pending_ ? 0 : start_tick_ + kEnterMs + hold_ms_;
    ULONGLONG close_at = 0;
    while (IsOpen()) {
        const ULONGLONG t = GetTickCount64();
        if (close_at) {
            if (t >= close_at) { Close(); break; }
        } else if (!pending_ && t >= leave_at) {
            Animate(0.0f, sign * kLeaveTravelDip * dpi_scale, 1.0f, 0.0f, /*ease_out=*/false);
            close_at = t + kLeaveMs;
            continue;
        }
        DWORD timeout = 0;
        if (pending_ && !close_at) {
            // The countdown's next second, else the give-up: in QPC, as the engine's end is.
            const int64_t qpc  = QpcNow();
            int64_t       next = ombra::PendingGiveUpQpc(start_qpc_, end_qpc_, qpc_freq_);
            if (const int64_t change = ombra::PendingTitleChangeQpc(end_qpc_, qpc, qpc_freq_))
                next = (std::min)(next, change);
            const int64_t ms = next > qpc ? (next - qpc) * 1000 / qpc_freq_ + 1 : 0;
            timeout = static_cast<DWORD>((std::min<int64_t>)(ms, 0x7FFFFFFF));
        } else {
            const ULONGLONG deadline = close_at ? close_at : leave_at;
            timeout = static_cast<DWORD>(deadline > t ? deadline - t : 0);
        }
        const DWORD n = pending_ ? static_cast<DWORD>(waits_.size()) : 0;
        // The change event is auto-reset: this wait consumes it, so its return is the only notice.
        const DWORD woke = MsgWaitForMultipleObjects(n, n ? waits_.data() : nullptr, FALSE, timeout, QS_ALLINPUT);
        const bool changed = pending_ && woke == WAIT_OBJECT_0 + kSaveResults;

        if (PollSaveResult()) {
            RenderCard();  // swap "Saving clip…" for the result in place; the card is already in view
            const ULONGLONG tick = GetTickCount64(), hold_from = (std::max)(tick, start_tick_ + kEnterMs);
            leave_at = hold_from + hold_ms_;
            StartBar(static_cast<double>(hold_from - tick) / 1000.0);
        } else if (RefreshPending(changed)) {
            RenderCard();  // a second off the countdown, or another app joined the clip
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { Close(); break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

LRESULT CALLBACK ToastWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ToastWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ToastWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ToastWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int RunToast(const std::wstring& event, std::vector<HANDLE> save_waits, HANDLE save_info) {
    ToastInfo info{};
    if (!LookupToastInfo(event, &info)) {
        ombra::Log::Error("RunToast: unrecognized event \"%ls\"", event.c_str());
        return 1;
    }

    // Local, self-contained config read -- no pipe/engine connection. ui.toast_enabled/ui.toast_on_* aren't re-checked here: ombra.exe already decided to
    // spawn this process, and re-checking risks a stale double-negative if the two disagree.
    const ombra::ConfigLoadResult loaded = ombra::LoadConfig();
    const ombra::Config& cfg = loaded.config;
    const ToastPosition position = ParseToastPosition(cfg.ui_toast_position);
    const bool from_right = ToastPositionIsRight(position);
    const bool from_top = ToastPositionIsTop(position);

    // Anchor to whatever monitor the foreground app (the game, most of the time) is on -- a toast
    // that pops up on a monitor the user isn't looking at is worse than not showing one at all.
    HWND fg = GetForegroundWindow();
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                      : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    ToastWindow toast;
    toast.info = info;
    toast.SetSaveWaits(std::move(save_waits), save_info);
    if (event == L"record_saved") toast.info.text = ombra::RecordingSavedText(toast.info_seconds);
    if (event == L"record_recovered")
        toast.info.text = ombra::RecordingRecoveredText(toast.info_seconds, toast.info_count);
    toast.SetSlideFromRight(from_right);
    toast.SetCompact(cfg.ui_toast_style == "compact");
    toast.SetDetails(cfg.ui_toast_details);
    toast.SetTimeBar(cfg.ui_toast_time_bar);
    toast.SetHoldMs(static_cast<ULONGLONG>((std::clamp)(static_cast<int>(cfg.ui_toast_duration), 1, 15)) * 1000);
    if (!toast.InitGraphics(/*warp=*/false, mon)) return 1;
    if (const auto it = loaded.raw.find("ui.themes"); it != loaded.raw.end())
        toast.GetTheme().user_themes = ombra::ParseUserThemes(it->second);
    toast.GetTheme().ApplyThemeKey(cfg.ui_theme);  // re-applies the accent below
    toast.GetTheme().ApplyAccentKey(cfg.ui_accent);  // a preset or "#rrggbb"
    toast.GetTheme().ApplyUiConfig([&](const char* key) -> const ombra::JsonValue* {  // ui.corners
        const auto it = loaded.raw.find(key);
        return it == loaded.raw.end() ? nullptr : &it->second;
    });

    // The window is created once, at its final resting spot, and never moved again: repositioning
    // a topmost/NOREDIRECTIONBITMAP/DComp popup with SetWindowPos on every ~15-33ms timer tick was visibly
    // stuttery (DWM has real per-move sync cost for a window with these attributes), and letting the
    // window travel out past the monitor's own edge could clip onto an adjacent monitor. The slide
    // is a DirectComposition offset animation on the visual (see Animate) --
    // free of both problems, since the window's screen footprint never changes.
    // RestX/RestY use kEdgeGapDip (not kMarginDip) on the slide axis so the card reads as docked
    // against the screen edge rather than floating inset from it, also per that feedback.
    auto RestX = [&](int w, float) { return from_right ? mi.rcWork.right - w : mi.rcWork.left; };
    auto RestY = [&](int h, float scale) { return from_top ? mi.rcWork.top + static_cast<int>(kMarginDip * scale)
                                                             : mi.rcWork.bottom - h - static_cast<int>(kMarginDip * scale); };

    // First pass: create at a 96-dpi guess so we have an HWND to query the real per-monitor DPI
    // from. Second pass corrects size/position and creates the swapchain at the real scale --
    // avoids adding a GetDpiForMonitor/shcore dependency this codebase doesn't otherwise use.
    // The card's size comes from the theme's density (ToastCard::Height), set above.
    const bool save = toast.IsPending() || event == L"save_ok";
    const int guess_w = static_cast<int>(ToastCard::Width(toast.GetTheme(), 1.0f) + kEdgeGapDip);
    const int guess_h = static_cast<int>(std::ceil(toast.CardHeight(save)));
    if (!toast.CreateHostWindow(RestX(guess_w, 1.0f), RestY(guess_h, 1.0f), guess_w, guess_h)) return 1;

    const int gap = static_cast<int>(kEdgeGapDip * toast.dpi_scale);
    const int w = static_cast<int>(std::ceil(ToastCard::Width(toast.GetTheme(), toast.dpi_scale))) + gap;
    const int h = static_cast<int>(std::ceil(toast.CardHeight(save)));
    toast.SetEdgeGap(static_cast<float>(gap));
    const int rest_x = RestX(w, toast.dpi_scale);
    const int rest_y = RestY(h, toast.dpi_scale);
    SetWindowPos(toast.Hwnd(), nullptr, rest_x, rest_y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);

    if (!toast.CreateSwapchainAndDComp(w, h)) return 1;

    // SW_SHOWNOACTIVATE (not SW_SHOW): shows the window without activating it -- WS_EX_NOACTIVATE
    // already keeps it out of the foreground on click, but ShowWindow itself can still transfer
    // activation on some paths, so this belt-and-suspenders the "never steals focus" requirement.
    ShowWindow(toast.Hwnd(), SW_SHOWNOACTIVATE);
    toast.RunLoop();
    return 0;
}

int RunToastTest(const std::wstring& out_png_path, const std::wstring& event, bool compact, double seconds, bool alpha,
                 float scale, const std::wstring& app, uint32_t clips, const std::wstring& title,
                 const std::wstring& game) {
    ToastInfo info{};
    if (!LookupToastInfo(event, &info)) {
        ombra::Log::Error("RunToastTest: unrecognized event \"%ls\"", event.c_str());
        return 1;
    }
    // A pending save's seconds are the clip's footage still to come: its countdown.
    if (event == L"save" && seconds > 0) info.text = ombra::PendingSaveText(static_cast<int64_t>(seconds * 1000), 0, 1000);
    if (event == L"save_ok") {
        ombra::SaveToastInfo saved;
        saved.clips = clips;
        saved.seconds = static_cast<float>(seconds);
        ombra::SetToastGame(saved, game);
        info.text = ombra::SavedToastText(saved);
    }
    if (event == L"record_saved") info.text = ombra::RecordingSavedText(seconds);
    if (event == L"record_recovered") info.text = ombra::RecordingRecoveredText(seconds, clips);
    if (!title.empty()) info.text.title = title;
    ToastWindow toast;
    toast.info = info;
    toast.app = app;
    toast.SetCompact(compact);
    toast.test_bar_fill = 0.6f;  // a still from mid-hold
    toast.test_pending = event == L"save";
    // The card's size at `scale` (RenderOffscreenTest sets the font scale to it, as here).
    toast.GetTheme().SetFontScale(scale);
    toast.dpi_scale = scale;
    const float card_h = toast.CardHeight(event == L"save" || event == L"save_ok");
    const int w = static_cast<int>(std::ceil(ToastCard::Width(toast.GetTheme(), scale) + 2.0f * kTestMarginPx * scale));
    const int h = static_cast<int>(std::ceil(card_h + 2.0f * kTestMarginPx * scale));
    const bool ok = toast.RenderOffscreenTest(w, h, out_png_path, alpha, scale);
    return ok ? 0 : 1;
}

}  // namespace ombra_ui
