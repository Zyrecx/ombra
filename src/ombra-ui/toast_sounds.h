// The toast sounds' resource ids (sounds.rc, linked into ombra-ui only) and their playback.
// The WAVs are Ombra's own, synthesized by scripts/toast-sounds.py.
#pragma once

#define OMBRA_RC_SOUND_SUCCESS 301
#define OMBRA_RC_SOUND_PROBLEM 302
#define OMBRA_RC_SOUND_NEUTRAL 303

#ifndef RC_INVOKED
#include "ombra/config.h"

#include <string_view>

namespace ombra_ui {

// Plays `event`'s sound (ombra/toast_events.h) if its row's Sound is on. `wait`: returns once it
// has played (a windowless --sound-only toast, which exits next); else it plays on while the popup
// shows. A newer toast ending this process cuts it short, as it does the popup.
void PlayToastSound(const ombra::Config& cfg, std::wstring_view event, bool wait);

}  // namespace ombra_ui
#endif
