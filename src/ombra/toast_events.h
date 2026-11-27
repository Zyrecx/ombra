// Which Settings row (Appearance > Notifications) a toast event belongs to, and whether that row's
// popup is on. One table for the engine (every spawn site asks it) and ombra-ui (the toast's
// sound, Settings' table). Header-only: ombra_core and ombra_ui_core both use it.
#pragma once

#include "ombra/config.h"

#include <string_view>

namespace ombra {

enum class ToastRow { kClipSaved, kRecording, kScreenshots, kBuffer, kProblems };

// Every event ombra-ui's LookupToastInfo knows (a ui test checks both ways). The rule: a failure,
// or a result that isn't what was asked (a fallback folder, an original kept, a disk running out),
// is a Problem; "nothing new" / "nothing to record" answer the press itself, so stay on their row.
struct ToastEventRow { std::wstring_view event; ToastRow row; };
inline constexpr ToastEventRow kToastEventRows[] = {
    {L"save", ToastRow::kClipSaved},
    {L"save_ok", ToastRow::kClipSaved},
    {L"save_nothing_new", ToastRow::kClipSaved},
    {L"trim_saved", ToastRow::kClipSaved},
    {L"trim_replaced", ToastRow::kClipSaved},
    {L"record_started", ToastRow::kRecording},
    {L"record_saved", ToastRow::kRecording},
    {L"record_nothing", ToastRow::kRecording},
    {L"record_recovered", ToastRow::kRecording},
    {L"shot_saved", ToastRow::kScreenshots},
    {L"buffer_on", ToastRow::kBuffer},
    {L"buffer_off", ToastRow::kBuffer},
    {L"save_failed", ToastRow::kProblems},
    {L"save_elsewhere", ToastRow::kProblems},
    {L"trim_kept", ToastRow::kProblems},
    {L"trim_failed", ToastRow::kProblems},
    {L"shot_failed", ToastRow::kProblems},
    {L"shot_elsewhere", ToastRow::kProblems},
    {L"record_disk_low", ToastRow::kProblems},
    {L"record_write_failed", ToastRow::kProblems},
    {L"record_failed", ToastRow::kProblems},
    {L"buffer_storage_lost", ToastRow::kProblems},
    {L"cpu_encoder", ToastRow::kProblems},
    {L"cpu_encoder_old_driver", ToastRow::kProblems},
};

// An event missing from the table is a Problem: it shows rather than going silently ungated.
inline ToastRow ToastRowFor(std::wstring_view event) {
    for (const ToastEventRow& e : kToastEventRows)
        if (e.event == event) return e.row;
    return ToastRow::kProblems;
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

}  // namespace ombra
