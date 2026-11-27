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

class ToastCard;

// `event`'s card as the popup draws it: headline, detail, icon, tone; no geometry and no ui.*
// options. False for an unrecognized event. Settings' sample toast is a "save_ok".
bool ToastCardFor(const std::wstring& event, ToastCard* out);

// Every event the popup knows, for the test that each sits on a Settings row (ombra/toast_events.h).
std::vector<std::wstring> ToastEventNames();

// Real entry point: creates a small, click-through, non-activating popup window near the corner of
// the foreground app's monitor, slides/fades it in, holds, slides/fades it out, and returns once the window is
// gone. `event` is one of ToastEventNames(); an unrecognized event logs and returns 1 without creating any window. "save" is spawned at the
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
// `app`: the API app whose action it is (empty: the user's own); `clips` > 1 titles several clips.
// "save" with `seconds`: an app's clip with that much footage still to come (its countdown).
// `title`, when set, replaces the headline (wording trials); `game` names a save_ok's game.
// Drawn as the defaults are: the detail line and a time bar part drained.
int RunToastTest(const std::wstring& out_png_path, const std::wstring& event, bool compact = false,
                 double seconds = 0, bool alpha = false, float scale = 1.0f,
                 const std::wstring& app = {}, uint32_t clips = 1, const std::wstring& title = {},
                 const std::wstring& game = {});

}  // namespace ombra_ui
