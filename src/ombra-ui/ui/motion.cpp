#include "ombra-ui/ui/motion.h"

#include "ombra-ui/ui/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ombra_ui {

namespace motion {

namespace {
double (*g_clock)() = nullptr;
bool g_forced_off = false;
bool g_forced_on = false;
int g_system = -1;  // AnimationsEnabled, cached; -1 = not read yet
bool g_running = false;
}  // namespace

double NowMs() {
    if (g_clock) return g_clock();
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void SetClockForTest(double (*now_ms)()) { g_clock = now_ms; }

bool Enabled() {
    if (g_forced_off) return false;
    if (g_forced_on) return true;
    if (g_system < 0) g_system = AnimationsEnabled() ? 1 : 0;
    return g_system == 1;
}

void RefreshEnabled() { g_system = AnimationsEnabled() ? 1 : 0; }
void SetForcedOff(bool off) { g_forced_off = off; }
void SetForcedOn(bool on) { g_forced_on = on; }

bool TakeRunning() {
    const bool r = g_running;
    g_running = false;
    return r;
}

}  // namespace motion

float Fade::Value() const {
    if (ms_ <= 0.0) return to_;
    const double t = (motion::NowMs() - start_) / ms_;
    if (t >= 1.0) return to_;
    motion::g_running = true;  // read mid-way: the frame after this one differs
    const float u = static_cast<float>((std::max)(0.0, t));
    const float e = 1.0f - (1.0f - u) * (1.0f - u);  // ease out: quick to respond, soft to land
    return from_ + (to_ - from_) * e;
}

float Fade::Track(bool on, int ms) {
    const float target = on ? 1.0f : 0.0f;
    if (!init_ || !motion::Enabled()) {
        Snap(on);
        return target;
    }
    if (target != to_) {
        from_ = Value();
        to_ = target;
        start_ = motion::NowMs();
        ms_ = ms * std::fabs(to_ - from_);
    }
    return Value();
}

void Fade::Snap(bool on) {
    from_ = to_ = on ? 1.0f : 0.0f;
    ms_ = 0.0;
    init_ = true;
}

void ItemFade::Track(int index, int ms) {
    if (index == in_) {
        in_fade_.Track(index >= 0, ms);
        out_fade_.Track(false, ms);
        return;
    }
    // The item left keeps fading from where it was; the new one starts from nothing.
    out_ = in_;
    out_fade_ = in_fade_;
    out_fade_.Track(false, ms);
    in_ = index;
    in_fade_.Snap(false);
    in_fade_.Track(index >= 0, ms);
}

float ItemFade::Amount(int index) const {
    if (index < 0) return 0.0f;
    if (index == in_) return in_fade_.Value();
    if (index == out_) return out_fade_.Value();
    return 0.0f;
}

}  // namespace ombra_ui
