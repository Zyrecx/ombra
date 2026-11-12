#pragma once

// Motion: hover and selection
// fade in kFadeMs, popups fade in over kPopupFadeMs, press is instant, nothing loops. A Fade is
// read at draw time from the widget's plain state (`hovered` stays the field a gallery or a test
// sets), so a widget with motion off draws exactly what it did before.
//
// Frames only during a transition: a Fade that is still moving when it is read marks the frame
// (TakeRunning), and the host keeps its frame clock on only while that keeps happening.

namespace ombra_ui {

namespace motion {

double NowMs();  // steady_clock (QPC): GetTickCount64's 15.6 ms steps judder a 100 ms fade at 144 Hz
// Tests drive time by hand; nullptr restores the real clock.
void SetClockForTest(double (*now_ms)());

// On while Windows "Animation effects" is on (AnimationsEnabled, cached: RefreshEnabled re-reads it
// on WM_SETTINGCHANGE) and nothing forced it off. --drive and the gallery force it off, so every
// snapshot shows end states.
bool Enabled();
void RefreshEnabled();
void SetForcedOff(bool off);
// `--drive`'s record mode: fades run whatever Windows' "Animation effects" says, on a virtual clock.
void SetForcedOn(bool on);

// True if a Fade was read mid-transition since the last call, i.e. another frame is needed.
bool TakeRunning();

}  // namespace motion

// Who needs the host's frame clock: Clips playback, or a fade still running. What one tick paints.
struct FrameDemand {
    bool clips = false, motion = false;
    bool Running() const { return clips || motion; }
    // `clips_has_frame` is Clips' OnFrameTimer, asked only while it wants ticks (it advances the video).
    bool PaintOnTick(bool clips_has_frame) const { return motion || (clips && clips_has_frame); }
};

// 0..1 toward a bool. A reversal starts from where the fade is, and takes the time left for that
// distance, so flicking the mouse across a control never jumps.
class Fade {
public:
    float Track(bool on, int ms);
    float Value() const;
    void Snap(bool on);  // jump to the end state (a widget re-created, a popup closed)

private:
    float from_ = 0.0f, to_ = 0.0f;
    double start_ = 0.0;
    double ms_ = 0.0;
    bool init_ = false;
};

// Hover moving between the items of one widget (rail pages, tabs, menu rows): the item entered
// fades in while the one left fades out. Two fades, not one per item.
class ItemFade {
public:
    void Track(int index, int ms);
    float Amount(int index) const;

private:
    int in_ = -1, out_ = -1;
    Fade in_fade_, out_fade_;
};

}  // namespace ombra_ui
