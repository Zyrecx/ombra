#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "ombra-ui/clips_screen.h"
#include "ombra-ui/setup_screen.h"
#include "ombra-ui/ui/motion.h"
#include "ombra-ui/pipe_client.h"
#include "ombra-ui/screens.h"
#include "ombra-ui/ui/theme.h"
#include "ombra-ui/ui/attached_panel.h"
#include "ombra-ui/ui/controls.h"
#include "ombra-ui/ui/widget.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

class OverlayHost : public ScreenHost {
public:
    OverlayHost();
    ~OverlayHost() override;

    // Initializes DirectX, Direct2D, DirectWrite, and DirectComposition factories, on the GPU that
    // drives `mon` when one is given
    bool InitGraphics(HMONITOR mon = nullptr);

    // Shows the overlay window over the active monitor
    bool Show(PipeClient* client = nullptr, ActiveScreen first_screen = ActiveScreen::Home);
    ActiveScreen ActiveScreenForTest() const { return active_screen_; }

    // Hides and destroys the overlay window, restoring previous foreground window
    void Hide();

    bool IsVisible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }

    // Headless test render: renders 1 frame to an offscreen D2D target and optionally writes PNG
    bool RenderOffscreenTest(int width, int height, const std::wstring& out_png_path,
                             bool* out_card_drawn = nullptr);

    // Selects which screen (and, for Settings, which rail icon -- 0..5, see SettingsScreen::
    // SetActiveRail) the next RenderOffscreenTest call renders. Test-only -- the real overlay
    // always starts on Home via Show().
    void SetTestScreen(ActiveScreen screen, int settings_rail_index = 0, int settings_sub_tab_index = 0);
    // Test-only: the DPI scale RenderOffscreenTest renders at (default 1.0 = 100%).
    void SetTestDpiScale(float scale) { test_dpi_scale_ = scale; }

    // Message loop pump when shown modally
    void RunModalLoop();

    // `ombra-ui.exe --drive` (driver.cpp): this host with no visible window. Frames render into an
    // offscreen width x height texture; a message-only window receives the async replies and the
    // status timer. `client` connected: state comes from that engine as in Show(); otherwise the
    // schema's defaults, as the snapshots use.
    bool StartHeadless(int width, int height, float dpi_scale, PipeClient* client);
    // One input message through the live window's own handler.
    void DispatchForDriver(UINT msg, WPARAM wp, LPARAM lp);
    // The last rendered frame, premultiplied BGRA, tightly packed.
    bool ReadFrame(std::vector<uint8_t>& bgra, int& width, int& height);
    void RenderForDriver() { RenderFrame(); }
    void ForceRenderResultForTest(HRESULT hr) { forced_render_hr_ = hr; }
    bool MousePressedForTest() const { return mouse_pressed_; }
    bool HeadlessOpen() const { return hwnd_ != nullptr; }  // false once Esc/close hid it
    ActiveScreen ActiveScreenForDriver() const { return active_screen_; }
    SetupScreen* GetSetupScreen() { return setup_screen_.get(); }
    D2D1_RECT_F HeaderButtonBounds(bool close) const;  // gear (Home) / back (Settings), or close
    bool ModalOpen() const { return active_modal_ && active_modal_->visible; }
    bool PopupOpen() const { return active_popup_ && active_popup_->visible; }
    float DpiScale() const { return dpi_scale_; }

    // ScreenHost implementation
    void RequestRepaint() override;
    HWND OwnerWindow() const override { return hwnd_; }
    void SwitchScreen(ActiveScreen screen) override;
    void ShowModal(const std::wstring& title, const std::wstring& msg,
                   bool is_danger, std::function<void()> on_confirm,
                   std::function<void()> on_cancel = nullptr,
                   ModalButtons buttons = {}) override;
    void ShowDropdown(const D2D1_RECT_F& anchor, const std::vector<std::wstring>& options,
                      int selected, std::function<void(int)> on_select,
                      std::function<void()> on_close = nullptr) override;
    void ShowPopup(std::unique_ptr<Popup> popup, const D2D1_RECT_F& anchor) override;
    void RequestWindowListAsync(std::function<void(std::vector<WindowEntry>)> on_result) override;
    void RequestMonitorListAsync(std::function<void(std::vector<MonitorEntry>)> on_result) override;
    void RequestMonitorThumbnailsAsync(
        std::vector<std::string> devices,
        std::function<void(std::string device, bool ok, UINT w, UINT h, std::vector<uint8_t> bgra)>
            on_result) override;
    void RequestWindowThumbnailsAsync(
        std::vector<uint64_t> hwnds,
        std::function<void(uint64_t hwnd, bool ok, UINT w, UINT h, std::vector<uint8_t> bgra)>
            on_result) override;
    void RequestProcessListAsync(std::function<void(std::vector<ProcessEntry>)> on_result) override;
    void RequestAudioSessionListAsync(
        std::function<void(std::vector<AudioSessionEntry>)> on_result) override;
    void RequestAudioDeviceListAsync(
        std::function<void(std::vector<AudioDeviceEntry>)> on_result) override;
    void RequestAsync(ombra::JsonValue req, DWORD timeout_ms,
                      std::function<void(std::optional<ombra::JsonValue>)> on_result) override;
    void RequestPreviewAsync(
        const ombra::JsonValue& config,
        std::function<void(std::optional<ombra::JsonValue> preview_resp,
                            std::optional<ombra::JsonValue> status_resp)> on_result) override;
    void RequestFileIconsAsync(std::vector<std::wstring> paths, UINT px,
                               std::function<void(std::vector<FileIcon>)> on_result) override;
    void CloseOverlay() override;
    void RunAsync(std::function<void()> work) override;
    UiPost UiPoster() override;
    ID3D11Device* VideoDevice() override;
    void SetFrameTimer(bool on) override;
    void SetMeterTimer(bool on) override;
    void SetHotkeysSuspended(bool suspended) override;
    PipeClient& GetPipeClient() override;
    Theme& GetTheme() override { return theme_; }

    HomeScreen* GetHomeScreen() { return home_screen_.get(); }
    SettingsScreen* GetSettingsScreen() { return settings_screen_.get(); }
    ClipsScreen* GetClipsScreen() { return clips_screen_.get(); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool CreateHostWindow(const RECT& monitor_rect);
    bool CreateSwapchainAndDComp(int width, int height);
    void RenderFrame();
    void CleanupWindowResources();
    void LoadFromEngine();
    void ApplyEngineState(const std::optional<ombra::JsonValue>& schema_resp,
                          const std::optional<ombra::JsonValue>& config_resp,
                          const std::optional<ombra::JsonValue>& status_resp);
    void ShowPendingAutotuneModal();
    bool         crash_ask_shown_ = false;
    ActiveScreen first_screen_ = ActiveScreen::Home;

public:
    // A crash from before setup was finished waits for the user's answer (crash.h UnansweredCrash):
    // asked once per open, over Home, unless another modal is up. --drive calls it after a fixture.
    void ShowCrashAsk();

private:
    void ActivateScreen(ActiveScreen screen);  // what any switch does, without the setup's fetch

public:
    struct EngineFetch;

private:
    using EngineReplies = std::vector<std::optional<ombra::JsonValue>>;
    // Asks the engine for `types` in order on a worker thread, one reply slot each. Answered within
    // the grace: `on_done` has run when this returns true. Otherwise false, and `on_done` runs on the
    // UI thread when the answers come, unless the window has closed.
    bool FetchWithGrace(std::vector<const char*> types, std::function<void(EngineReplies)> on_done);
    void AttachUnattachedFetch();
    std::shared_ptr<EngineFetch> unattached_fetch_;
    uint64_t                     switch_generation_ = 0;

    // Checks active_modal_->last_result after an OnMouseDown/OnKeyDown call that may have set it
    // and, if so, destroys the modal and invokes the corresponding callback -- never from inside
    // the modal's own OnMouseDown/OnKeyDown (see ModalDialog::Result's comment in widget.h for why).
    void ProcessModalResult();

    // Panel geometry. Both screens are floating-element layouts (a shared floating header plus
    // screen-specific boxes: Home's 3 tiles; Settings' rail + content card). With ui.panel_attach
    // on and an edge position, a backing panel is drawn around the content and fused into the screen
    // edge (ui/attached_panel.h); ui.screen_frame optionally rounds the screen's corners.
    struct Placement {
        D2D1_RECT_F content{};              // what the active screen lays itself out in
        bool attached = false;
        AttachedPanelGeometry panel{};      // valid only when `attached`
    };
    float ContentWidth(float view_w) const;
    bool IsPanelAttached() const;           // attach requested AND position is an edge (Center floats)
    // Places a w x h content row per ui.panel_position / ui.panel_attach / the frame border. When
    // attached, the caller passes the width/height already reduced by the panel padding.
    Placement ComputePlacement(float w, float h, float view_w, float view_h) const;
    D2D1_COLOR_F SurfaceFill() const;
    float PanelPadding() const;             // an attached panel's padding around the content, 0 floating
    void DrawScreenFrame(float view_w, float view_h);
    void DrawAttachedPanel(const AttachedPanelGeometry& g);
    void DrawBrandLogo(float cx, float cy, float size);
    // The mark, rendered once per ring colour (Theme::PickedRgb) on this device.
    ComPtr<ID2D1Bitmap> logo_bmp_;
    uint32_t logo_rgb_ = 0;
    void DrawFloatingHeader(float view_w, float view_h);

    // engine.autotune result surfacing (Show()'s get_status poll) -- built as data first, shown as
    // a real ShowModal() only once the overlay's own D3D11/D2D1 resources exist (ShowModal's
    // RequestRepaint needs them), hence the two-step build-then-show instead of calling ShowModal
    // directly from inside the status-fetch block.
    struct PendingModal {
        std::wstring title;
        std::wstring message;
        std::function<void()> on_confirm;
        std::function<void()> on_cancel;
        ModalButtons buttons;
    };
    std::optional<PendingModal> BuildAutotuneModal(const std::string& state,
                                                    const ombra::JsonValue* result);
    std::optional<PendingModal> pending_autotune_modal_;

    // RequestWindowListAsync's worker thread posts its result to hwnd_ as a private WM_APP
    // message, carrying a heap-allocated
    // result struct in lParam that HandleMessage takes ownership of. `window_list_generation_` is
    // bumped on every RequestWindowListAsync call; a result whose generation doesn't match the
    // latest is a superseded request (popup closed and reopened before the first reply landed) and
    // is dropped silently rather than invoking a stale `on_result`. The worker thread is detached,
    // not joined -- it touches no OverlayHost state, only by-value captures -- and redone from
    // scratch per call rather than kept alive with an idle timeout: not a hot path worth
    // designing around yet.
    std::thread            window_list_worker_;
    std::atomic<uint64_t>  window_list_generation_{0};

    // RequestMonitorListAsync: same worker shape as window_list_worker_ above, but no generation.
    // Home and Settings both ask, and each guards its own reply; dropping the older one left
    // Settings' Displays card "Loading displays…" for good.
    std::thread            monitor_list_worker_;

    // RequestMonitorThumbnailsAsync: one worker per call, processing every requested device in
    // sequence over a single PipeClient connection (screens.h's comment on why: no concurrent WGC
    // sessions). No shared generation counter -- a result is keyed by its own `device` string, and
    // the IsWindow(hwnd) check inside the worker is what stops a stale post after the overlay closes.
    std::thread            monitor_thumb_worker_;
    // The shared body of both thumbnail requests: `keys` in sequence, `make_request` turns one key
    // into its pipe request, results post back keyed by that same string.
    void StartThumbnailWorker(std::vector<std::string> keys,
                              std::function<ombra::JsonValue(const std::string&)> make_request,
                              std::function<void(std::string, bool, UINT, UINT, std::vector<uint8_t>)> on_result);

    // Settings' Audio tab: RequestProcessListAsync/RequestAudioSessionListAsync, same detached-worker/
    // generation-check shape as monitor_list_worker_/monitor_list_generation_ above. Separate
    // workers/generations since a process-list fetch (BuildAudioSourceRows' prefetch, or either
    // on-click picker) and an audio-session fetch can be in flight independently.
    std::thread            process_list_worker_;
    std::atomic<uint64_t>  process_list_generation_{0};
    std::thread            audio_session_list_worker_;
    std::atomic<uint64_t>  audio_session_list_generation_{0};
    // Desktop track's device picker (RequestAudioDeviceListAsync) -- same shape, own worker/
    // generation since it can be in flight independently of the process/session-list fetches.
    std::thread            audio_device_list_worker_;
    std::atomic<uint64_t>  audio_device_list_generation_{0};

    // SetHotkeysSuspended: one long-lived worker, so suspend/resume reach the engine in the order
    // they were asked for (a thread per request could deliver a quick Esc's resume before the
    // suspend), and it renews the engine's lease every few seconds while recording.
    void HotkeySuspendLoop();
    // SetFrameTimer: a worker waits on DWM's compositor clock and posts one tick per refresh
    // (coalesced: never more than one queued), so clip frames land on vsync instead of the 15.6 ms
    // system timer, which skipped 3-6% of a 60 fps clip's frames (--drive --gpu, 2026-10-20).
    // Two callers want ticks: Clips playback (SetFrameTimer) and a fade still running at the end of
    // RenderFrame (motion::TakeRunning). The clock runs while either does; neither turns off the other's.
    FrameDemand             frame_demand_;
    bool                    meter_timer_on_ = false;
    void                    UpdateFrameClock();
    std::thread             frame_clock_worker_;
    HANDLE                  frame_clock_stop_ = nullptr;
    std::atomic<bool>       frame_tick_pending_{false};
    std::thread             hotkey_suspend_worker_;
    std::mutex              hotkey_suspend_mutex_;
    std::condition_variable hotkey_suspend_cv_;
    bool                    hotkey_suspend_wanted_ = false;
    bool                    hotkey_suspend_stop_   = false;

    // RequestSetConfigAsync/RequestPreviewAsync deliberately
    // have no member thread/generation counter, unlike every worker above: those are "give me
    // current state" queries where dropping a superseded reply is correct (a fresher one is already
    // in flight), but every set_config/preview_config reply here corresponds to a distinct request
    // SettingsScreen must apply, not just the most recent one -- so nothing here needs cancelling,
    // and the worker thread is detached inline with no member to hold or race against. The one real
    // ordering hazard this leaves (two writes to the same config key resolving out of order) is
    // guarded on the caller's side, screens.h's set_config_seq_by_key_.

    HWND hwnd_ = nullptr;
    HWND prev_foreground_ = nullptr;
    RECT bounds_{};
    int dpi_ = 96;
    float dpi_scale_ = 1.0f;
    float test_dpi_scale_ = 1.0f;

    PipeClient* client_ = nullptr;
    PipeClient  fallback_client_;

    Theme theme_;
    ActiveScreen active_screen_ = ActiveScreen::Home;
    std::unique_ptr<HomeScreen>     home_screen_;
    std::unique_ptr<SettingsScreen> settings_screen_;
    std::unique_ptr<ClipsScreen>    clips_screen_;
    std::unique_ptr<SetupScreen>    setup_screen_;
    std::unique_ptr<ModalDialog>    active_modal_;
    std::unique_ptr<Popup> active_popup_;  // a menu or the colour editor, above everything but a modal
    void ReapClosedPopup();

    // Pointer shape for the spot under the mouse (UpdateCursor, after each mouse move); WM_SETCURSOR
    // re-applies it so Windows doesn't reset it to the class arrow.
    void UpdateCursor(D2D1_POINT_2F pt);
    CursorKind cursor_ = CursorKind::Arrow;

    IconButtonControl& NavButton();  // gear on Home, back everywhere else
    bool video_device_ = false;  // d3d_device_ has D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    IconButtonControl gear_btn_;
    IconButtonControl back_btn_;
    IconButtonControl close_btn_;

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

    ComPtr<IWICImagingFactory>  wic_factory_;
    ComPtr<ID3D11Texture2D>     headless_tex_;  // StartHeadless's render target
    HRESULT  forced_render_hr_ = S_OK;  // ForceRenderResultForTest
    bool     device_lost_ = false;
    bool     mouse_pressed_ = false;  // the left button went down here and hasn't come up
    uint64_t home_refresh_generation_ = 0;
};

// `width`/`height` and the chrome overrides exist for the sizing/position/edge-style snapshots
// (main.cpp's --selftest). `position` defaults to Center, matching ui.panel_position's real config
// default (config.h); callers that don't pass `position` see the same placement a real,
// freshly-installed overlay would.
// `before_render`, if set, runs after SetTestScreen but before the offscreen render -- lets a
// caller reach into the freshly-constructed host (e.g. GetSettingsScreen()) to set up state a
// snapshot needs (a staged Save/Discard bar edit) that no combination of the parameters above can
// express.
// `--snapshots` creates its D3D11 devices on WARP instead of the hardware adapter, so it never
// touches the GPU while a game may be running. Set once, before any InitGraphics.
void SetSoftwareRendering(bool on);
bool SoftwareRendering();

int RunOverlayTest(const std::wstring& out_png_path, ActiveScreen screen = ActiveScreen::Home,
                   int settings_rail_index = 0, int width = 1920, int height = 1080,
                   PanelPosition position = PanelPosition::Center,
                   PanelAttach attach = PanelAttach::Floating,
                   int settings_sub_tab_index = 0,
                   std::function<void(OverlayHost&)> before_render = nullptr);

}  // namespace ombra_ui
