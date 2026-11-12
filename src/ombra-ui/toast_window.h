// Short-lived toast popup — spawned by ombra.exe as a brand-new `ombra-ui.exe --toast <event>`
// process per save/buffer-toggle event. Deliberately
// NOT OverlayHost: OverlayHost::RenderFrame is tightly coupled to the Home/Settings screens and the
// full-monitor scrim, neither of which a one-card popup needs. This is a standalone class with its
// own (duplicated, see the .cpp's header comment) D3D11/D2D/DComp init sequence instead.
//
// No pipe connection to the engine: the engine already knows what happened (it's the one spawning
// this), so everything needed crosses via argv (the event name) and a direct, local
// ombra::LoadConfig() disk read (for ui.accent) — never PipeClient/EnsureEngineRunningAndConnect.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ombra_ui {

// Real entry point: creates a small, click-through, non-activating popup window near the corner of
// the foreground app's monitor, slides/fades it in, holds, slides/fades it out, and returns once the window is
// gone. `event` is one of "save" | "save_ok" | "save_nothing_new" | "save_failed" | "shot_failed" | "shot_elsewhere" | "buffer_on" | "buffer_off";
// an unrecognized event logs and returns 1 without creating any window. "save" is spawned at the
// hotkey press with `save_waits` = inherited {ok, nothing_new, failed, engine process, elsewhere}
// handles: it shows "Saving clip…" until one is signalled, then the matching result. `save_info`
// maps the clip count and length (save_toast.h) for "Saved last 30 s".
int RunToast(const std::wstring& event, std::vector<HANDLE> save_waits = {}, HANDLE save_info = nullptr);

// Headless verification: renders one frame of the toast card (fully opaque, mid-hold) to an
// offscreen D3D11 texture and saves it as a PNG, exactly like overlay_window.h's RunOverlayTest —
// no window is ever created or shown. Used by --toasttest so the visual can be reviewed from a PNG
// instead of ever putting a live popup on screen during automated verification.
// `compact`: ui.toast_style compact; `seconds` > 0 titles a save_ok as a clip of that length.
// `alpha`: no test backdrop, the card on transparency (straight-alpha PNG) for compositing.
// `scale`: the DPI scale to draw at (2 = 200%), so a zoomed-in demo shot stays sharp.
int RunToastTest(const std::wstring& out_png_path, const std::wstring& event, bool compact = false,
                 double seconds = 0, bool alpha = false, float scale = 1.0f);

}  // namespace ombra_ui
