// Which Settings row (Appearance > Notifications) a toast event belongs to, which sound it makes,
// and whether that row's popup and sound are on. One table for the engine (every spawn site asks
// it) and ombra-ui (the toast's sound, Settings' table). Header-only: ombra_core and ombra_ui_core
// both use it.
#pragma once

#include "ombra/config.h"

#include <string_view>

namespace ombra {

enum class ToastRow { kClipSaved, kRecording, kScreenshots, kBuffer, kProblems };

// The three sounds (src/ombra-ui/sounds, scripts/toast-sounds.py). kNone: the pending "save",
// whose result makes the sound.
enum class ToastSound { kNone, kSuccess, kProblem, kNeutral };

// Every event ombra-ui's LookupToastInfo knows (a ui test checks both ways). The rule: a failure,
// or a result that isn't what was asked (a fallback folder, an original kept, a disk running out),
// is a Problem; "nothing new" / "nothing to record" answer the press itself, so stay on their row
// with the neutral sound, as the buffer's on and off do.
struct ToastEventRow { std::wstring_view event; ToastRow row; ToastSound sound; };
inline constexpr ToastEventRow kToastEventRows[] = {
    {L"save", ToastRow::kClipSaved, ToastSound::kNone},
    {L"save_ok", ToastRow::kClipSaved, ToastSound::kSuccess},
    {L"save_nothing_new", ToastRow::kClipSaved, ToastSound::kNeutral},
    {L"trim_saved", ToastRow::kClipSaved, ToastSound::kSuccess},
    {L"trim_replaced", ToastRow::kClipSaved, ToastSound::kSuccess},
    {L"record_started", ToastRow::kRecording, ToastSound::kSuccess},
    {L"record_saved", ToastRow::kRecording, ToastSound::kSuccess},
    {L"record_nothing", ToastRow::kRecording, ToastSound::kNeutral},
    {L"record_recovered", ToastRow::kRecording, ToastSound::kSuccess},
    {L"shot_saved", ToastRow::kScreenshots, ToastSound::kSuccess},
    {L"buffer_on", ToastRow::kBuffer, ToastSound::kNeutral},
    {L"buffer_off", ToastRow::kBuffer, ToastSound::kNeutral},
    {L"save_failed", ToastRow::kProblems, ToastSound::kProblem},
    {L"save_elsewhere", ToastRow::kProblems, ToastSound::kProblem},
    {L"trim_kept", ToastRow::kProblems, ToastSound::kProblem},
    {L"trim_failed", ToastRow::kProblems, ToastSound::kProblem},
    {L"shot_failed", ToastRow::kProblems, ToastSound::kProblem},
    {L"shot_elsewhere", ToastRow::kProblems, ToastSound::kProblem},
    {L"record_disk_low", ToastRow::kProblems, ToastSound::kProblem},
    {L"record_write_failed", ToastRow::kProblems, ToastSound::kProblem},
    {L"record_failed", ToastRow::kProblems, ToastSound::kProblem},
    {L"buffer_storage_lost", ToastRow::kProblems, ToastSound::kProblem},
    {L"cpu_encoder", ToastRow::kProblems, ToastSound::kProblem},
    {L"cpu_encoder_old_driver", ToastRow::kProblems, ToastSound::kProblem},
};

// An event missing from the table is a Problem: it shows rather than going silently ungated.
inline const ToastEventRow* FindToastEvent(std::wstring_view event) {
    for (const ToastEventRow& e : kToastEventRows)
        if (e.event == event) return &e;
    return nullptr;
}
inline ToastRow ToastRowFor(std::wstring_view event) {
    const ToastEventRow* e = FindToastEvent(event);
    return e ? e->row : ToastRow::kProblems;
}
inline ToastSound ToastSoundFor(std::wstring_view event) {
    const ToastEventRow* e = FindToastEvent(event);
    return e ? e->sound : ToastSound::kProblem;
}

inline bool ToastPopupOn(const Config& c, ToastRow row) {
    if (!c.ui_toast_enabled) return false;
    switch (row) {
    case ToastRow::kClipSaved:   return c.ui_toast_on_save;
    case ToastRow::kRecording:   return c.ui_toast_on_record;
    case ToastRow::kScreenshots: return c.ui_toast_on_screenshot;
    case ToastRow::kBuffer:      return c.ui_toast_on_buffer_toggle;
    case ToastRow::kProblems:    return c.ui_toast_on_problem;
    }
    return true;
}
inline bool ToastPopupOn(const Config& c, std::wstring_view event) { return ToastPopupOn(c, ToastRowFor(event)); }

// A row's Sound, separate from its Popup; the master switch off silences it too. An app's toast
// always pops up, but its sound still follows the row.
inline bool ToastSoundOn(const Config& c, ToastRow row) {
    if (!c.ui_toast_enabled) return false;
    switch (row) {
    case ToastRow::kClipSaved:   return c.ui_toast_sound_on_save;
    case ToastRow::kRecording:   return c.ui_toast_sound_on_record;
    case ToastRow::kScreenshots: return c.ui_toast_sound_on_screenshot;
    case ToastRow::kBuffer:      return c.ui_toast_sound_on_buffer_toggle;
    case ToastRow::kProblems:    return c.ui_toast_sound_on_problem;
    }
    return true;
}
inline bool ToastSoundOn(const Config& c, std::wstring_view event) {
    return ToastSoundFor(event) != ToastSound::kNone && ToastSoundOn(c, ToastRowFor(event));
}

// What a spawn site starts: the popup (which plays its own sound when the row has it on), a
// windowless toast process that only plays the sound, or nothing.
enum class ToastShow { kNothing, kSoundOnly, kPopup };
inline ToastShow ToastShowFor(const Config& c, ToastRow row) {
    if (ToastPopupOn(c, row)) return ToastShow::kPopup;
    return ToastSoundOn(c, row) ? ToastShow::kSoundOnly : ToastShow::kNothing;
}
inline ToastShow ToastShowFor(const Config& c, std::wstring_view event) {
    if (ToastPopupOn(c, event)) return ToastShow::kPopup;
    return ToastSoundOn(c, event) ? ToastShow::kSoundOnly : ToastShow::kNothing;
}

}  // namespace ombra
