// Tier B on-device benchmark.
//
// SAFETY CONTRACT: an automatically launched fullscreen media player plausibly crashed
// a test machine mid-game.
//   1. Never runs except as `ombra.exe capturebench`, and never from a caller that isn't a direct,
//      immediate result of a person's own action. The caller is main.cpp's set_config handler,
//      spawning this on receipt of {"engine.autotune":"run"}
//      — but only after ombra-ui's Settings "Run" button has shown an explicit confirm-or-cancel
//      modal warning that a window is about to appear, and only fires this on the "confirm" branch.
//      A human clicking through that warning is the same shape as a human typing the command line
//      directly; what's still forbidden is spawning this on a timer, on startup, on a config
//      change, or any other path with no confirming click in front of it.
//   2. Refuses before drawing anything if a fullscreen/full-monitor window already owns the
//      foreground — see ForegroundLooksFullscreen() below.
//   3. Prints an explicit warning and a countdown before the window appears, so a person who just
//      typed the command has a chance to read it and get their hands off the keyboard/mouse.
//   4. Esc, or the window losing foreground focus, aborts immediately.
//   5. A hard wall-clock cap bounds the run regardless of anything else.
//   6. The window is destroyed on every exit path, including early returns — RAII, not a single
//      cleanup block someone can forget to reach.
//   7. Never goes exclusive-fullscreen, never calls ChangeDisplaySettings, never launches another
//      process or window.
//
// What it measures: presents a small window with a self-identifying pixel pattern (a 16-bit
// ordinal as high-contrast blocks, plus a moving detail block so the encoder has real work), reads
// the ordinal back out of the same BGRA texture the real pipeline captures into — before NVENC,
// via Pacer's PreEncodeCallback hook (pacer.h) — and compares each decoded ordinal's capture
// timestamp against that ordinal's present timestamp. Runs the real Capture + Encoder + Pacer
// classes, unmodified, so the numbers reflect the real pipeline, not a simulation of it.

#include "ombra/capturebench.h"

#include "ombra/capture.h"
#include "ombra/config.h"
#include "ombra/die.h"
#include "ombra/encode_target.h"
#include "ombra/encoder.h"
#include "ombra/frame_scaler.h"
#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/pacer.h"
#include "ombra/paths.h"
#include "ombra/reconfig.h"
#include "ombra/util.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace ombra {

// Refuse if a window already covers its whole monitor — the exclusive/borderless-
// fullscreen shape a game uses. Program Manager and the WorkerW desktop-wallpaper host also cover
// their monitor at idle, so they're excluded by class name rather than being (wrongly) treated as
// "a game is running." Exported (capturebench.h) so controller.cpp's game-mode arming
// uses this exact definition rather than a second, driftable copy.
bool ForegroundLooksFullscreen(HWND window, std::wstring* out_label) {
    HWND fg = window ? window : GetForegroundWindow();
    if (!fg) return false;

    wchar_t cls[256]{};
    GetClassNameW(fg, cls, 255);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return false;

    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    const bool covers_monitor = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                                wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    if (!covers_monitor) return false;

    if (out_label) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        wchar_t name[MAX_PATH] = L"(unknown process)";
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            DWORD sz = MAX_PATH;
            QueryFullProcessImageNameW(h, 0, name, &sz);
            CloseHandle(h);
        }
        *out_label = name;
    }
    return true;
}

namespace {

// --- layout, in the bench window's client pixels -----------------------------------------------
constexpr int kMarginX      = 10;
constexpr int kBlockY       = 20;
constexpr int kBlockSize    = 20;
constexpr int kBlockGap     = 6;
constexpr int kOrdinalBits  = 16;               // 65536 frames of headroom; the run is a few hundred
constexpr int kTotalBlocks  = 2 + kOrdinalBits;  // 2 fixed sync markers (block 0 white, block 1 black)
constexpr int kBlockAreaW   = kTotalBlocks * (kBlockSize + kBlockGap);
constexpr int kDetailY      = kBlockY + kBlockSize + 20;
constexpr int kDetailHeight = 40;
constexpr int kDetailLanes  = 8;
constexpr int kDetailWidth  = 60;
constexpr int kWindowW      = kMarginX * 2 + kBlockAreaW;
constexpr int kWindowH      = kDetailY + kDetailHeight + 10;

constexpr int    kMaxSeconds     = 8;   // hard wall-clock cap on the measured pass
constexpr int     kControlSeconds = 3;   // present-only control pass, no capture
constexpr wchar_t kWindowClassName[] = L"OmbraCaptureBenchWnd";

double Percentile(std::vector<double>& v, double pct) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(pct * (v.size() - 1))];
}

LRESULT CALLBACK BenchWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// RAII: the window, its D3D11 device/swapchain/RTV, and the window class registration all live and
// die together on every exit path. No code path after construction
// needs to remember to clean this up, including an early `return` from a gate check or an
// exception from a D3D call.
class BenchWindow {
public:
    BenchWindow() = default;
    ~BenchWindow() { Destroy(); }
    BenchWindow(const BenchWindow&) = delete;

    bool Create(const RECT& monitor) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = BenchWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClassName;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        class_atom_ = RegisterClassExW(&wc);
        if (!class_atom_) return false;

        // WS_POPUP: no border/titlebar. WS_EX_TOOLWINDOW: no taskbar button, not in the normal
        // alt-tab list — reduces how much this looks like a real application window to the user
        // and to anything else on the desktop. Use the captured monitor's origin so the ordinal
        // readback coordinates match even on a non-primary display.
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClassName,
                                 L"ombra capturebench", WS_POPUP, monitor.left, monitor.top, kWindowW, kWindowH, nullptr,
                                 nullptr, wc.hInstance, nullptr);
        if (!hwnd_) return false;

        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferDesc.Width  = kWindowW;
        scd.BufferDesc.Height = kWindowH;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count  = 1;
        scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount       = 1;
        scd.OutputWindow      = hwnd_;
        scd.Windowed          = TRUE;
        scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL got{};
        if (FAILED(D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &scd, swapchain_.put(), device_.put(), &got,
                context_.put())))
            return false;
        if (FAILED(context_->QueryInterface(IID_PPV_ARGS(context1_.put())))) return false;

        // Prevent Alt+Enter from ever taking this swapchain exclusive-fullscreen,
        // belt and suspenders on top of never requesting it in the first place.
        winrt::com_ptr<IDXGIFactory> factory;
        if (SUCCEEDED(swapchain_->GetParent(IID_PPV_ARGS(factory.put()))))
            factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

        winrt::com_ptr<ID3D11Texture2D> backbuffer;
        if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(backbuffer.put())))) return false;
        if (FAILED(device_->CreateRenderTargetView(backbuffer.get(), nullptr, rtv_.put())))
            return false;

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        return true;
    }

    void Destroy() {
        rtv_ = nullptr;
        context1_ = nullptr;
        context_ = nullptr;
        swapchain_ = nullptr;
        device_ = nullptr;
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (class_atom_) { UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr)); class_atom_ = 0; }
    }

    HWND Hwnd() const { return hwnd_; }

    // Draws one frame's ordinal pattern and presents it (vsync-throttled — Present(1,0) blocks
    // until the next vblank, which is also what paces this whole loop; no separate timer needed).
    void DrawAndPresent(uint32_t ordinal) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        D3D11_RECT full{0, 0, kWindowW, kWindowH};
        context1_->ClearView(rtv_.get(), black, &full, 1);

        auto block_rect = [](int index) {
            const int x = kMarginX + index * (kBlockSize + kBlockGap);
            return D3D11_RECT{x, kBlockY, x + kBlockSize, kBlockY + kBlockSize};
        };
        D3D11_RECT sync0 = block_rect(0);
        context1_->ClearView(rtv_.get(), white, &sync0, 1);  // block 1 (sync, always black) needs no draw
        for (int bit = 0; bit < kOrdinalBits; ++bit) {
            if ((ordinal >> bit) & 1u) {
                D3D11_RECT r = block_rect(2 + bit);
                context1_->ClearView(rtv_.get(), white, &r, 1);
            }
        }
        const int lane = static_cast<int>(ordinal % kDetailLanes);
        D3D11_RECT detail{kMarginX + lane * kDetailWidth, kDetailY,
                           kMarginX + lane * kDetailWidth + kDetailWidth, kDetailY + kDetailHeight};
        context1_->ClearView(rtv_.get(), white, &detail, 1);

        swapchain_->Present(1, 0);
    }

    ID3D11Device* Device() const { return device_.get(); }

private:
    HWND  hwnd_       = nullptr;
    ATOM  class_atom_ = 0;
    winrt::com_ptr<ID3D11Device>          device_;
    winrt::com_ptr<ID3D11DeviceContext>   context_;
    winrt::com_ptr<ID3D11DeviceContext1>  context1_;
    winrt::com_ptr<IDXGISwapChain>        swapchain_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
};

// Reads one block's centre pixel out of a region already fetched via Capture::ReadSlotRegionBGRA.
bool SampleBlockIsWhite(const std::vector<uint8_t>& buf, uint32_t row_pitch, int block_index) {
    const int local_x = block_index * (kBlockSize + kBlockGap) + kBlockSize / 2;
    const int local_y = kBlockSize / 2;
    const size_t off = static_cast<size_t>(local_y) * row_pitch + static_cast<size_t>(local_x) * 4;
    if (off + 2 >= buf.size()) return false;
    return buf[off] > 128;  // BGRA — B channel is enough to tell black from white
}

bool DecodeOrdinal(const std::vector<uint8_t>& buf, uint32_t row_pitch, uint32_t* out_ordinal) {
    if (!SampleBlockIsWhite(buf, row_pitch, 0)) return false;  // sync marker 0 must be white
    if (SampleBlockIsWhite(buf, row_pitch, 1)) return false;   // sync marker 1 must be black
    uint32_t v = 0;
    for (int bit = 0; bit < kOrdinalBits; ++bit)
        if (SampleBlockIsWhite(buf, row_pitch, 2 + bit)) v |= (1u << bit);
    *out_ordinal = v;
    return true;
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// p1 (fastest) .. p7 (slowest, best quality) — encode.preset's own allowed-value order (config.h).
constexpr const char* kPresetOrder[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};

// The one recommendation this makes: a faster preset when the measured pipeline is spending most
// of the frame budget on pacer-wait + encode at p99. Never touches bitrate/resolution/fps — only
// preset latency is actually measured here, so only preset is recommended (advisor review during
// this feature's design: don't let a latency benchmark recommend a quality knob it never measured).
struct AutotuneRecommendation {
    std::string recommended_preset;  // empty = no change recommended
    std::string reason;
};

AutotuneRecommendation RecommendPreset(const std::string& current_preset, uint32_t fps,
                                        double encode_p99_ms, double wait_overshoot_p99_ms,
                                        size_t latency_samples) {
    AutotuneRecommendation rec;
    if (latency_samples < 20) {
        rec.reason = "not enough decoded samples to make a recommendation";
        return rec;
    }

    const double frame_budget_ms = 1000.0 / static_cast<double>(fps);
    const double used_p99_ms = encode_p99_ms + wait_overshoot_p99_ms;
    const bool at_risk = used_p99_ms > frame_budget_ms * 0.8;

    if (!at_risk) {
        rec.reason = "comfortable headroom at the current preset — no change recommended";
        return rec;
    }

    int idx = -1;
    for (size_t i = 0; i < std::size(kPresetOrder); ++i) {
        if (current_preset == kPresetOrder[i]) { idx = static_cast<int>(i); break; }
    }
    if (idx <= 0) {
        rec.reason = "using most of the frame budget even at the fastest preset — try a lower "
                     "resolution, bitrate, or capture framerate instead";
        return rec;
    }

    rec.recommended_preset = kPresetOrder[idx - 1];
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "pacer wait + encode used %.1f%% of the %.2fms frame budget at p99 — a faster "
                  "preset trades a little quality for headroom",
                  (used_p99_ms / frame_budget_ms) * 100.0, frame_budget_ms);
    rec.reason = buf;
    return rec;
}

}  // namespace

std::wstring AutotuneResultFilePath() {
    const std::wstring& dir = Instance().data_dir;
    if (dir.empty()) return L"";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\autotune_result.json";
}

int RunCaptureBench(const Config& cfg, const std::string& monitor_device) {
    const uint32_t fps = static_cast<uint32_t>(ResolveFramerate(cfg.capture_framerate, DisplayRefreshHz(monitor_device)));
    const uint32_t bitrate_kbps = static_cast<uint32_t>(cfg.encode_bitrate_kbps);
    const uint32_t keyframe_sec = static_cast<uint32_t>(cfg.encode_keyframe_sec);
    const auto& current_preset = cfg.encode_preset;
    std::wstring fg_label;
    if (ForegroundLooksFullscreen(/*window=*/nullptr, &fg_label)) {
        std::wprintf(
            L"\n  REFUSED  a window covering its whole monitor is in the foreground (%s).\n"
            L"           capturebench draws on screen and will not run while that looks like a\n"
            L"           fullscreen game or app. Alt-tab out of it and run capturebench again.\n\n",
            fg_label.c_str());
        Log::Warn("capturebench refused: fullscreen-shaped foreground window (%ls)", fg_label.c_str());
        return 1;
    }

    std::printf(
        "\n  capturebench is about to open a small window (%dx%d, top-left corner of your\n"
        "  configured monitor) and measure capture latency for up to %ds.\n\n"
        "  DO NOT alt-tab, click other windows, or touch the mouse/keyboard during the test.\n"
        "  Press Esc at any time to abort immediately.\n\n",
        kWindowW, kWindowH, kMaxSeconds);
    for (int s = 3; s >= 1; --s) {
        std::printf("  starting in %d...\n", s);
        std::fflush(stdout);
        Sleep(1000);
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        std::printf("  aborted before starting (Esc held during countdown)\n");
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Without this, an unaware process's windows are DPI-virtualized: Windows lets it think it's
    // drawing at 96 DPI and DWM bitmap-scales the result onto the real (usually higher-DPI)
    // desktop before compositing. WGC captures the real, scaled, blurred pixels — so the
    // hard-edged black/white blocks this file draws would land at the wrong sub-pixel positions by
    // the scale factor, corrupting almost every decode. Confirmed empirically, not theoretically:
    // the first run of this code (no DPI awareness) decoded 459/481 ticks "successfully" but every
    // single one produced garbage far from any ordinal actually presented (8135 "dropped" ordinals
    // out of 481 ticks — physically impossible for a real gap, only explainable by reading noise).
    // Declaring per-monitor-v2 awareness makes CreateWindowExW's coordinates and WGC's captured
    // pixels agree 1:1, which is what every offset in this file already assumes.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HMONITOR monitor = MonitorForDevice(monitor_device);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitor_info)) {
        std::printf("  FAIL  monitor %s is not connected\n", monitor_device.c_str());
        return 1;
    }
    BenchWindow window;
    if (!window.Create(monitor_info.rcMonitor)) {
        std::printf("  FAIL  could not create the bench window/D3D11 device\n");
        return 1;
    }
    // Deliberate, after the warning/countdown above, not before — see the class comment on
    // WS_EX_TOOLWINDOW/WS_POPUP for why this is a narrow, self-contained window rather than
    // anything that looks like taking over the desktop.
    SetForegroundWindow(window.Hwnd());

    const auto selected = ResolveEncodeAdapter(cfg.encode_adapter, monitor);
    if (!selected) {
        std::printf("  FAIL  no usable encode adapter for encode.adapter=%s\n", cfg.encode_adapter.c_str());
        return 1;
    }
    const EncodeAdapter encode_adapter = *selected;
    Capture capture;
    CaptureTarget target;
    target.monitor_device = monitor_device;
    capture.Open(cfg.capture_capture_cursor, target, encode_adapter.luid);
    // Match the engine's options and size/codec policy. Read the ordinal before scaling, just
    // as the real pipeline's screenshot callback does; scale the actual encoder input.
    EncoderOptions options = EncoderOptionsFor(cfg);
    const auto plan = PlanEncode(ResolveOutputSize(capture.Width(), capture.Height(), cfg.capture_resolution),
                                 options.codec, encode_adapter.av1_align_w, encode_adapter.av1_align_h,
                                 options.av1_unaligned);
    options.codec = plan.codec;
    std::unique_ptr<FrameScaler> scaler;
    if (plan.size != VideoSize{capture.Width(), capture.Height()}) {
        std::vector<ID3D11Texture2D*> slots;
        for (uint32_t slot = 0; slot < Capture::kSlots; ++slot) slots.push_back(capture.SlotTexture(slot));
        scaler = std::make_unique<FrameScaler>();
        std::string error;
        if (!scaler->Open(capture.Device(), slots, capture.Width(), capture.Height(),
                          plan.size.width, plan.size.height, error)) {
            Log::Error("capturebench: %s", error.c_str());
            std::printf("  FAIL  scaler: %s\n", error.c_str());
            return 1;
        }
    }
    auto encoder_ptr = CreateVideoEncoder(encode_adapter.backend);
    IVideoEncoder& encoder = *encoder_ptr;
    if (!encoder.TryOpen(capture.Device(), plan.size.width, plan.size.height, fps, bitrate_kbps,
                         keyframe_sec, options)) {
        std::printf("  FAIL  the encoder would not open at %ux%u\n", plan.size.width, plan.size.height);
        return 1;
    }
    for (uint32_t slot = 0; slot < (scaler ? 1u : Capture::kSlots); ++slot) {
        if (!encoder.RegisterSlot(scaler ? scaler->Output() : capture.SlotTexture(slot),
                                  plan.size.width, plan.size.height)) {
            std::printf("  FAIL  the encoder rejected a capture texture\n");
            return 1;
        }
    }
    capture.StartCapture();

    const size_t expected_ticks = static_cast<size_t>(kMaxSeconds) * fps + 64;

    // Indexed directly by the 16-bit ordinal — no wraparound reconstruction needed since a run
    // this short (a few hundred frames) never wraps a 65536-entry space. 0 means "never presented
    // this value yet," which is indistinguishable from a genuine qpc of 0 but QueryPerformanceCounter
    // never actually returns 0 in practice, so this is a safe sentinel, not a real ambiguity.
    std::vector<int64_t> present_qpc_by_ordinal(1u << kOrdinalBits, 0);

    std::vector<double>   latency_ms;
    latency_ms.reserve(expected_ticks);
    std::atomic<uint64_t> decode_failures{0};
    std::atomic<uint64_t> duplicated{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<int64_t>  last_decoded{-1};

    LARGE_INTEGER qpc_freq_li{};
    QueryPerformanceFrequency(&qpc_freq_li);
    const double qpc_freq = static_cast<double>(qpc_freq_li.QuadPart);

    Pacer pacer;
    pacer.Start(
        capture, encoder, fps,
        [](const IVideoEncoder::AccessUnit&, const Tick&) {},  // real encode happens; output is discarded
        [&](ICaptureSource& cap, uint32_t slot, const Tick& tick) {
            std::vector<uint8_t> buf;
            uint32_t             row_pitch = 0;
            cap.ReadSlotRegionBGRA(slot, kMarginX, kBlockY, kBlockAreaW, kBlockSize, buf, row_pitch);
            uint32_t decoded = 0;
            if (buf.empty() || !DecodeOrdinal(buf, row_pitch, &decoded)) {
                decode_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const int64_t presented_qpc = present_qpc_by_ordinal[decoded];
            if (presented_qpc != 0) {
                latency_ms.push_back(static_cast<double>(tick.qpc - presented_qpc) * 1000.0 / qpc_freq);
            }
            const int64_t prev = last_decoded.exchange(static_cast<int64_t>(decoded));
            if (prev >= 0) {
                if (static_cast<int64_t>(decoded) == prev) {
                    duplicated.fetch_add(1, std::memory_order_relaxed);
                } else if (decoded > prev + 1 && decoded - prev < (1u << (kOrdinalBits - 1))) {
                    dropped.fetch_add(static_cast<uint64_t>(decoded - prev - 1), std::memory_order_relaxed);
                }
            }
        }, scaler.get());

    // Present loop, main thread: draws the ordinal pattern, records its present-time qpc, and
    // checks the abort conditions every iteration. Present(1,0) (vsync) paces this to the
    // display's refresh rate — no separate timer needed.
    uint32_t ordinal = 0;
    bool     aborted_esc = false, aborted_focus = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kMaxSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        PumpMessages();
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { aborted_esc = true; break; }
        if (GetForegroundWindow() != window.Hwnd()) { aborted_focus = true; break; }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        present_qpc_by_ordinal[ordinal % (1u << kOrdinalBits)] = now.QuadPart;
        window.DrawAndPresent(ordinal % (1u << kOrdinalBits));
        ++ordinal;
    }
    const uint32_t frames_presented = ordinal;

    pacer.Stop();
    const VideoCodec measured_codec = encoder.Codec();
    const bool encoder_failed = encoder.Faulted();
    encoder.Close();
    scaler.reset();
    capture.Close();

    // Control pass ("presents without capturing"): same window, no Capture/Encoder/Pacer
    // running at all, just measuring achieved present rate so the report can say whether
    // presentation itself was ever the bottleneck.
    uint32_t control_frames = 0;
    if (!aborted_esc && !aborted_focus) {
        const auto control_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kControlSeconds);
        while (std::chrono::steady_clock::now() < control_deadline) {
            PumpMessages();
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;
            if (GetForegroundWindow() != window.Hwnd()) break;
            window.DrawAndPresent(control_frames % (1u << kOrdinalBits));
            ++control_frames;
        }
    }

    window.Destroy();

    std::printf("\n  --- capturebench ---\n");
    if (aborted_esc) std::printf("  aborted          Esc pressed\n");
    if (aborted_focus) std::printf("  aborted          window lost foreground focus\n");
    std::printf("  frames presented %u  decode failures %llu\n", frames_presented,
                static_cast<unsigned long long>(decode_failures.load()));
    std::printf("  ordinals         duplicated %llu  dropped %llu\n",
                static_cast<unsigned long long>(duplicated.load()),
                static_cast<unsigned long long>(dropped.load()));
    if (!latency_ms.empty()) {
        std::vector<double> lat_copy = latency_ms;
        std::printf("  e2e latency      p50 %.2f ms  p99 %.2f ms  (present QPC -> capture QPC, n=%zu)\n",
                    Percentile(lat_copy, 0.50), Percentile(lat_copy, 0.99), lat_copy.size());
    } else {
        std::printf("  e2e latency      no samples decoded — see decode failures above\n");
    }
    std::printf("  pacer ticks      %llu  duplicates %llu  late %llu  resyncs %llu\n",
                static_cast<unsigned long long>(pacer.TicksSubmitted()),
                static_cast<unsigned long long>(pacer.DuplicateTicks()),
                static_cast<unsigned long long>(pacer.LateTicks()),
                static_cast<unsigned long long>(pacer.ResyncCount()));
    const PacerLatencyStats lat = pacer.ComputeLatencyStats();
    std::printf("  wait overshoot   p50 %.3f ms  p99 %.3f ms\n", lat.wait_overshoot_p50_ms,
                lat.wait_overshoot_p99_ms);
    std::printf("  encode duration  p50 %.3f ms  p99 %.3f ms\n", lat.encode_p50_ms, lat.encode_p99_ms);
    if (control_frames > 0)
        std::printf("  control pass     %u frames presented in %ds with no capture running (%.1f fps)\n",
                    control_frames, kControlSeconds,
                    static_cast<double>(control_frames) / kControlSeconds);

    Log::Info("capturebench      frames=%u decode_failures=%llu duplicated=%llu dropped=%llu "
              "latency_p50_ms=%.2f latency_p99_ms=%.2f pacer_late=%llu pacer_resyncs=%llu "
              "control_fps=%.1f aborted_esc=%d aborted_focus=%d",
              frames_presented, static_cast<unsigned long long>(decode_failures.load()),
              static_cast<unsigned long long>(duplicated.load()),
              static_cast<unsigned long long>(dropped.load()),
              latency_ms.empty() ? 0.0 : Percentile(latency_ms, 0.50),
              latency_ms.empty() ? 0.0 : Percentile(latency_ms, 0.99),
              static_cast<unsigned long long>(pacer.LateTicks()),
              static_cast<unsigned long long>(pacer.ResyncCount()),
              control_frames > 0 ? static_cast<double>(control_frames) / kControlSeconds : 0.0,
              aborted_esc ? 1 : 0, aborted_focus ? 1 : 0);

    const bool aborted = aborted_esc || aborted_focus;
    const AutotuneRecommendation rec = encoder_failed
        ? AutotuneRecommendation{"", "encoder failed during the run; no recommendation available"}
        : aborted
        ? AutotuneRecommendation{"", "run was aborted before enough samples were collected"}
        : RecommendPreset(current_preset, fps, lat.encode_p99_ms, lat.wait_overshoot_p99_ms,
                          latency_ms.size());

    JsonValue result = JsonValue::MakeObject();
    result.Set("aborted", JsonValue::MakeBool(aborted));
    result.Set("frames_presented", JsonValue::MakeNum(frames_presented));
    result.Set("decode_failures", JsonValue::MakeNum(static_cast<double>(decode_failures.load())));
    result.Set("latency_p50_ms", JsonValue::MakeNum(latency_ms.empty() ? 0.0 : Percentile(latency_ms, 0.50)));
    result.Set("latency_p99_ms", JsonValue::MakeNum(latency_ms.empty() ? 0.0 : Percentile(latency_ms, 0.99)));
    result.Set("encode_p50_ms", JsonValue::MakeNum(lat.encode_p50_ms));
    result.Set("encode_p99_ms", JsonValue::MakeNum(lat.encode_p99_ms));
    result.Set("wait_overshoot_p50_ms", JsonValue::MakeNum(lat.wait_overshoot_p50_ms));
    result.Set("wait_overshoot_p99_ms", JsonValue::MakeNum(lat.wait_overshoot_p99_ms));
    result.Set("pacer_late_ticks", JsonValue::MakeNum(static_cast<double>(pacer.LateTicks())));
    result.Set("encoder_backend", JsonValue::MakeStr(BackendName(encode_adapter.backend)));
    result.Set("monitor", JsonValue::MakeStr(monitor_device));
    result.Set("width", JsonValue::MakeNum(plan.size.width));
    result.Set("height", JsonValue::MakeNum(plan.size.height));
    result.Set("fps", JsonValue::MakeNum(fps));
    result.Set("bitrate_kbps", JsonValue::MakeNum(bitrate_kbps));
    result.Set("codec", JsonValue::MakeStr(VideoCodecName(measured_codec)));
    result.Set("keyframe_sec", JsonValue::MakeNum(keyframe_sec));
    result.Set("capture_cursor", JsonValue::MakeBool(cfg.capture_capture_cursor));
    result.Set("scope", JsonValue::MakeStr("single-monitor desktop timing diagnostic; not setup or game-load recommendations"));
    result.Set("rate_control", JsonValue::MakeStr(options.rate_control));
    result.Set("cq", JsonValue::MakeNum(options.cq));
    result.Set("max_bitrate_kbps", JsonValue::MakeNum(options.max_bitrate_kbps));
    result.Set("current_preset", JsonValue::MakeStr(current_preset));
    result.Set("recommended_preset", JsonValue::MakeStr(rec.recommended_preset));
    result.Set("reason", JsonValue::MakeStr(rec.reason));

    const std::wstring result_path = AutotuneResultFilePath();
    if (!result_path.empty()) {
        std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
        if (out) {
            WriteJson(out, result);
        } else {
            Log::Error("capturebench: could not write result file");
            return 1;
        }
        out.flush();
        if (!out) return 1;
    }

    return encoder_failed ? 1 : 0;
}

}  // namespace ombra
