#include "ombra-ui/setup_screen.h"

#include "ombra-ui/ui/paint.h"
#include "ombra/brand.h"
#include "ombra/hotkey_spec.h"
#include "ombra/log.h"
#include "ombra/paths.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace ombra_ui {

using ombra::JsonValue;

std::wstring FirstRunMarkerPath() {
    const std::wstring dir = ombra::Instance().config_dir;
    return dir.empty() ? L"" : dir + L"\\first_run_done";
}

bool FirstRunComplete() {
    const std::wstring path = FirstRunMarkerPath();
    // Nowhere to keep the marker: asking on every launch would be worse than never asking.
    if (path.empty()) return true;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void WriteMarker(const std::wstring& path) {
    if (path.empty()) return;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ombra::Log::Warn("ombra-ui: could not write setup marker %ls (err=%lu)", path.c_str(), GetLastError());
        return;
    }
    CloseHandle(h);
}

void MarkFirstRunComplete() {
    const std::wstring path = FirstRunMarkerPath();
    if (path.empty()) return;
    WriteMarker(path);
    const std::wstring shown = path + L".shown";
    DeleteFileW(shown.c_str());
}

void NoteFirstRunShown() {
    const std::wstring path = FirstRunMarkerPath();
    if (path.empty()) return;
    const std::wstring shown = path + L".shown";
    int count = 0;
    if (std::ifstream in(shown); in) in >> count;
    if (++count >= kFirstRunMaxShows) {
        MarkFirstRunComplete();
        return;
    }
    std::ofstream(shown) << count;
}

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string StrOf(const JsonValue* obj, const char* key, const std::string& fallback) {
    if (!obj) return fallback;
    const JsonValue* v = obj->Find(key);
    return v && v->type == JsonValue::Type::String ? v->str : fallback;
}

bool BoolOf(const JsonValue* obj, const char* key, bool fallback) {
    if (!obj) return fallback;
    const JsonValue* v = obj->Find(key);
    return v && v->type == JsonValue::Type::Bool ? v->b : fallback;
}

// "NVIDIA GeForce RTX 5060 Laptop GPU" -> "RTX 5060 Laptop": the name a person calls it.
std::wstring ShortGpuName(std::wstring name) {
    for (const wchar_t* prefix : {L"NVIDIA GeForce ", L"NVIDIA ", L"AMD ", L"Advanced Micro Devices "})
        if (name.rfind(prefix, 0) == 0) { name.erase(0, wcslen(prefix)); break; }
    for (const wchar_t* suffix : {L" GPU", L" Graphics"}) {
        const size_t n = wcslen(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0) { name.resize(name.size() - n); break; }
    }
    return name;
}

std::wstring FreeOn(const std::wstring& path) {
    std::wstring probe = path;
    ULARGE_INTEGER avail{};
    while (!probe.empty() && !GetDiskFreeSpaceExW(probe.c_str(), &avail, nullptr, nullptr)) {
        const size_t up = probe.find_last_of(L"\\/");
        probe = up == std::wstring::npos ? L"" : probe.substr(0, up);
    }
    if (probe.empty()) return L"";
    return FormatMemory(static_cast<double>(avail.QuadPart)) + L" free on " + path.substr(0, 2);
}

// True for a spinning disk. Needs no admin rights: a zero-access handle may ask this.
bool DriveIsHdd(wchar_t letter) {
    const wchar_t path[] = {L'\\', L'\\', L'.', L'\\', letter, L':', 0};
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &got, nullptr);
    CloseHandle(h);
    return ok && seek.IncursSeekPenalty;
}

// Where a disk replay should live without asking: a hard drive with room, else the drive with the
// most free space, in an Ombra folder at its root (the engine creates it). `why` says which.
std::wstring SuggestRingFile(int64_t needed, std::wstring* why) {
    wchar_t best = 0, best_hdd = 0;
    ULONGLONG best_free = 0, best_hdd_free = 0;
    int drives = 0, hdds = 0;
    const DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        const wchar_t root[] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER avail{};
        if (!GetDiskFreeSpaceExW(root, &avail, nullptr, nullptr)) continue;
        ++drives;
        if (avail.QuadPart > best_free) { best_free = avail.QuadPart; best = c; }
        if (DriveIsHdd(c)) {
            ++hdds;
            if (avail.QuadPart > best_hdd_free && avail.QuadPart > static_cast<ULONGLONG>(needed) * 2) {
                best_hdd_free = avail.QuadPart;
                best_hdd = c;
            }
        }
    }
    const wchar_t pick = best_hdd ? best_hdd : best;
    if (!pick) return L"";
    if (why) *why = best_hdd ? L"A hard drive, with the most room."
                  : hdds == 0 && drives > 1 ? L"Most free space. All your drives are SSDs."
                  : hdds == 0 ? L"Your only drive, an SSD."
                  : L"Most free space.";
    return std::wstring(1, pick) + L":\\" OMBRA_PRODUCT_NAME L"\\ring.dat";
}

}  // namespace

SetupScreen::SetupScreen(ScreenHost* host) : host_(host) {}

template <class T> T& SetupScreen::Get(const std::string& name) {
    auto& slot = widgets_[name];
    if (!slot) slot = std::make_shared<T>();
    return *static_cast<T*>(slot.get());
}

D2D1_RECT_F SetupScreen::R(float x, float y, float w, float h) const {
    return D2D1::RectF(bounds_.left + x * s_, bounds_.top + y * s_, bounds_.left + (x + w) * s_, bounds_.top + (y + h) * s_);
}

bool SetupScreen::Hovering(const D2D1_RECT_F& r) const {
    return mouse_.x >= r.left && mouse_.x < r.right && mouse_.y >= r.top && mouse_.y < r.bottom;
}

float SetupScreen::Put(Widget& w, float x, float y) {
    const LayoutContext ctx{s_, theme_};
    const D2D1_SIZE_F m = w.Measure(LayoutConstraints{}, ctx);
    const float px = bounds_.left + x * s_, py = bounds_.top + y * s_;
    w.Arrange(D2D1::RectF(px, py, px + m.width, py + m.height), ctx);
    w.Draw(dc_, *theme_, s_);
    live_.push_back(&w);
    return m.width / s_;
}

float SetupScreen::PutMid(Widget& w, float x, float row_y, float row_h) {
    const float h = w.Measure(LayoutConstraints{}, LayoutContext{s_, theme_}).height / s_;
    return Put(w, x, row_y + (row_h - h) * 0.5f);
}

SwitchControl& SetupScreen::Switch(const std::string& name, bool on, std::function<void(bool)> fn) {
    auto& c = Get<SwitchControl>(name);
    c.on = on;
    c.on_change = std::move(fn);
    return c;
}

SegmentedControl& SetupScreen::Seg(const std::string& name, std::vector<std::wstring> options, int selected, std::function<void(int)> fn) {
    auto& c = Get<SegmentedControl>(name);
    c.options = std::move(options);
    c.selected = selected;
    c.on_change = std::move(fn);
    return c;
}

SelectControl& SetupScreen::Select(const std::string& name, std::vector<std::wstring> options, int selected, float width, std::function<void(int)> fn) {
    auto& c = Get<SelectControl>(name);
    c.options = std::move(options);
    c.selected = selected;
    c.width_dip = width;
    c.on_change = fn;
    SelectControl* self = &c;
    c.on_open = [this, self, fn](SelectControl&) {
        self->open = true;
        host_->ShowDropdown(self->bounds, self->options, self->selected, fn, [self] { self->open = false; });
    };
    return c;
}

ButtonControl& SetupScreen::Button(const std::string& name, const std::wstring& label, ButtonControl::Kind kind, std::function<void()> fn) {
    auto& c = Get<ButtonControl>(name);
    c.label = label;
    c.kind = kind;
    c.on_click = std::move(fn);
    return c;
}

KeyCaptureControl& SetupScreen::KeyField(const std::string& name, std::string* spec, const std::string* other) {
    auto& c = Get<KeyCaptureControl>(name);
    if (!c.Recording()) c.spec = Wide(*spec);
    const paint::Paint measure{dc_, *theme_, s_};
    c.width_dip = c.Recording() ? 88.0f : measure.Keys(paint::SplitKeys(c.spec), 0, 0, theme_->size.control_xs() * s_, false) / s_ + 8;
    c.validate = [this, other](const std::wstring& candidate) -> std::wstring {
        ombra::HotkeyCombo want, theirs;
        if (!ombra::ParseHotkeySpec(Narrow(candidate), want)) return L"" OMBRA_PRODUCT_NAME " can't bind that.";
        if (ombra::ParseHotkeySpec(*other, theirs) && theirs == want) return L"already used by the other shortcut.";
        for (const char* key : {"hotkeys.toggle_buffer", "hotkeys.screenshot"}) {
            const std::string taken = StrOf(&ui_config_, key, "");
            if (ombra::ParseHotkeySpec(taken, theirs) && theirs == want) return Wide(taken) + L" is already a shortcut in Settings.";
        }
        return L"";
    };
    c.on_commit = [spec](const std::wstring& v) { *spec = Narrow(v); };
    KeyCaptureControl* self = &c;
    c.on_recording_changed = [this, self](bool on) {
        recording_ = on ? self : nullptr;
        if (host_) host_->SetHotkeysSuspended(on);
    };
    return c;
}

bool SetupScreen::IsRecordingShortcut() const { return recording_ != nullptr; }
void SetupScreen::OnShortcutKey(UINT vk, bool down) { if (recording_) recording_->OnCaptureKey(vk, down); }
void SetupScreen::CancelShortcutRecording() { if (recording_) recording_->CancelRecording(); }

void SetupScreen::Recommend(bool keep_preferences) {
    const ombra::SetupPlan old = plan_;
    plan_ = ombra::RecommendSetup(facts_);
    if (keep_preferences) {
        plan_.output_root = old.output_root;
        plan_.per_game_folders = old.per_game_folders;
        plan_.toasts = old.toasts;
        plan_.toast_position = old.toast_position;
        plan_.hotkey_save = old.hotkey_save;
        plan_.hotkey_overlay = old.hotkey_overlay;
        plan_.theme = old.theme;
        plan_.accent = old.accent;
        plan_.corners = old.corners;
        plan_.panel_position = old.panel_position;
        plan_.screen_frame = old.screen_frame;
        plan_.sharing = old.sharing;
    }
}

void SetupScreen::Open(const JsonValue* status, const JsonValue* config) {
    step_ = 0;
    open_row_ = -1;

    // The monitor Ombra records first: the primary. preview_config would answer only for a built
    // pipeline, and this is the display the recommendation is about.
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        facts_.width = static_cast<int>(mode.dmPelsWidth);
        facts_.height = static_cast<int>(mode.dmPelsHeight);
        facts_.display_hz = static_cast<int>(mode.dmDisplayFrequency);
    }
    facts_.gpus.clear();
    gpus_.clear();
    if (status) {
        if (const JsonValue* list = status->Find("recording_gpus"); list && list->type == JsonValue::Type::Array) {
            for (const JsonValue& g : list->arr) {
                GpuInfo info;
                info.vendor = StrOf(&g, "vendor", "");
                info.name = Wide(StrOf(&g, "name", ""));
                info.av1 = BoolOf(&g, "av1", false);
                if (const JsonValue* a = g.Find("av1_align_w"); a && a->type == JsonValue::Type::Number) info.align_w = static_cast<uint32_t>((std::max)(1.0, a->num));
                if (const JsonValue* a = g.Find("av1_align_h"); a && a->type == JsonValue::Type::Number) info.align_h = static_cast<uint32_t>((std::max)(1.0, a->num));
                facts_.gpus.push_back({info.vendor, info.av1, info.align_w, info.align_h});
                gpus_.push_back(std::move(info));
            }
        }
        display_owner_ = Wide(StrOf(status, "gpu_display_owner", ""));
        if (const JsonValue* v = status->Find("sys_mem_total_mb"); v && v->type == JsonValue::Type::Number) ram_gb_ = v->num / 1024.0;
    }
    if (ram_gb_ <= 0.0) {
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) ram_gb_ = static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    }

    const JsonValue defaults = ombra::SerializeConfigFlat(ombra::Config{});
    const JsonValue* cfg = config ? config->Find("config") : nullptr;
    const JsonValue* c = cfg ? cfg : &defaults;
    ui_config_ = *c;
    output_root_raw_ = StrOf(c, "output.root", "");
    plan_.output_root = output_root_raw_;
    plan_.per_game_folders = BoolOf(c, "output.per_game_folders", true);
    plan_.toasts = BoolOf(c, "ui.toast_enabled", true);
    plan_.toast_position = StrOf(c, "ui.toast_position", "top_right");
    plan_.hotkey_save = StrOf(c, "hotkeys.save_replay", "Alt+F10");
    plan_.hotkey_overlay = StrOf(c, "hotkeys.overlay", "Alt+Z");
    plan_.theme = StrOf(c, "ui.theme", "dark");
    plan_.accent = StrOf(c, "ui.accent", "theme");
    plan_.corners = StrOf(c, "ui.corners", "default");
    plan_.panel_position = StrOf(c, "ui.panel_position", "center");
    plan_.screen_frame = BoolOf(c, "ui.screen_frame", false);
    plan_.sharing = kSharingKeys[SharingIndex(StrOf(c, "diagnostics.sharing", "errors"))];
    has_existing_sources_ = false;
    if (const JsonValue* v = c->Find("audio.sources"); v && v->type == JsonValue::Type::Array) {
        existing_sources_ = *v;
        has_existing_sources_ = true;
    }
    Recommend(true);
    output_root_shown_ = ombra::ResolveOutputRoot(output_root_raw_);
    output_free_ = FreeOn(output_root_shown_);
}

void SetupScreen::SetFactsForTest(const ombra::SetupFacts& facts, std::vector<GpuInfo> gpus, const std::wstring& display_owner, double ram_gb) {
    facts_ = facts;
    gpus_ = std::move(gpus);
    display_owner_ = display_owner;
    ram_gb_ = ram_gb;
    Recommend(true);
}

void SetupScreen::SetStorage(const std::string& storage) {
    plan_.storage = storage;
    if (storage == "file" && plan_.file_path.empty()) {
        plan_.file_path = Narrow(SuggestRingFile(ombra::SetupDiskFileBytes(plan_, facts_), &disk_note_));
    }
}

void SetupScreen::ApplyThemePreview() {
    ui_config_.Set("ui.theme", JsonValue::MakeStr(plan_.theme));
    ui_config_.Set("ui.accent", JsonValue::MakeStr(plan_.accent));
    ui_config_.Set("ui.corners", JsonValue::MakeStr(plan_.corners));
    ui_config_.Set("ui.panel_position", JsonValue::MakeStr(plan_.panel_position));
    ui_config_.Set("ui.screen_frame", JsonValue::MakeBool(plan_.screen_frame));
    host_->GetTheme().ApplyUiConfig([this](const char* key) { return ui_config_.Find(key); });
    host_->RequestRepaint();
}

void SetupScreen::Finish() {
    if (recording_) recording_->CancelRecording();
    const JsonValue patch = ombra::SetupPatch(plan_, has_existing_sources_ ? &existing_sources_ : nullptr);
    host_->RequestSetConfigAsync(patch, [this](std::optional<JsonValue> resp) {
        const JsonValue* ok = resp && resp->type == JsonValue::Type::Object ? resp->Find("ok") : nullptr;
        if (ok && ok->type == JsonValue::Type::Bool && ok->b) {
            MarkFirstRunComplete();
            WriteMarker(ombra::SetupCompletedPath());
            host_->SwitchScreen(ActiveScreen::Home);
            return;
        }
        const std::string err = resp ? StrOf(&*resp, "error", "") : "The engine didn't answer.";
        ombra::Log::Warn("setup: set_config failed: %s", err.c_str());
        host_->ShowModal(L"Couldn't apply the setup", Wide(err), false, nullptr);
    });
}

void SetupScreen::Layout(const D2D1_RECT_F& bounds, float dpi_scale) {
    bounds_ = bounds;
    s_ = dpi_scale;
}

void SetupScreen::Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) {
    dc_ = dc;
    theme_ = &theme;
    s_ = dpi_scale;
    live_.clear();
    zones_.clear();
    const paint::Paint p{dc, theme, s_};
    p.Fill(bounds_, theme.size.radius_panel() * s_, theme.color.panel);
    p.Stroke(bounds_, theme.size.radius_panel() * s_, theme.color.line);
    DrawHeader();
    if (step_ == 0) DrawRecording();
    else if (step_ == 1) DrawClips();
    else if (step_ == 2) DrawAppearance();
    else DrawSharing();
    dc_ = nullptr;
}

bool SetupScreen::OnMouseMove(D2D1_POINT_2F pt) {
    mouse_ = pt;
    for (Widget* w : live_) w->OnMouseMove(pt);
    return true;
}

CursorKind SetupScreen::CursorAt(D2D1_POINT_2F pt) const {
    for (const Widget* w : live_) {
        const CursorKind c = w->CursorAt(pt);
        if (c != CursorKind::Arrow) return c;
    }
    for (const Zone& z : zones_)
        if (pt.x >= z.rect.left && pt.x < z.rect.right && pt.y >= z.rect.top && pt.y < z.rect.bottom) return CursorKind::Hand;
    return CursorKind::Arrow;
}

bool SetupScreen::QueryHoverTooltip(std::wstring* text, D2D1_POINT_2F* anchor) const {
    for (const Widget* w : live_)
        if (w->QueryTooltip(text, anchor)) return true;
    return false;
}

bool SetupScreen::OnMouseDown(D2D1_POINT_2F pt) {
    for (Widget* w : live_)
        if (w->OnMouseDown(pt)) return true;
    for (const Zone& z : zones_) {
        if (pt.x >= z.rect.left && pt.x < z.rect.right && pt.y >= z.rect.top && pt.y < z.rect.bottom) {
            const auto fn = z.fn;  // it may change the step; zones_ is rebuilt at the next Draw
            fn();
            return true;
        }
    }
    return false;
}

bool SetupScreen::OnMouseUp(D2D1_POINT_2F pt) {
    bool any = false;
    for (Widget* w : live_) any |= w->OnMouseUp(pt);
    return any;
}

// -- Pieces ---------------------------------------------------------------------------------------

void SetupScreen::DrawHeader() {
    const paint::Paint p{dc_, *theme_, s_};
    const wchar_t* names[] = {L"Recording", L"Clips", L"Appearance", L"Reports"};
    const IconType icons[] = {IconType::Record, IconType::Film, IconType::Palette, IconType::Bug};
    constexpr int n = 4;
    const float width = (bounds_.right - bounds_.left) / s_ - 48;
    const float r = 14, pad = 8, margin = 12, y = 18;
    float label_w[n], items = 0;
    for (int i = 0; i < n; ++i) {
        label_w[i] = p.TextW(names[i], TypeRole::Label) / s_ + 1;
        items += 2 * r + pad + label_w[i];
    }
    const float line = (width - items - 2 * (n - 1) * margin) / (n - 1);
    float x = 24;
    const auto& c = theme_->color;
    for (int i = 0; i < n; ++i) {
        const bool done = i < step_, here = i == step_;
        const auto mid = R(x + r, y + r, 0, 0);
        p.Circle(mid.left, mid.top, r * s_, done || here ? c.accent : c.raised);
        if (done) {
            p.Check(mid.left, mid.top, c.on_accent);
        } else {
            auto brush = p.B(here ? c.on_accent : c.text2);
            DrawIcon(dc_, *theme_, icons[i], mid.left, mid.top, 15 * s_, brush.Get());
        }
        p.Text(names[i], TypeRole::Label, R(x + 2 * r + pad, y, label_w[i], 2 * r), here ? c.text : c.text3);
        // A finished step can be gone back to from the header too.
        if (done) {
            const int target = i;
            zones_.push_back({R(x, y, 2 * r + pad + label_w[i], 2 * r), [this, target] { step_ = target; open_row_ = -1; }});
        }
        x += 2 * r + pad + label_w[i] + margin;
        if (i + 1 < n) {
            const auto a = R(x, y + r, 0, 0), b = R(x + line, y + r, 0, 0);
            p.Line(a.left, a.top, b.left, b.top, done ? c.accent : c.line_strong, 1.0f);
            x += line + margin;
        }
    }
}

void SetupScreen::DrawTitle(const std::wstring& title, const std::wstring& sub, float y) {
    const paint::Paint p{dc_, *theme_, s_};
    const float w = (bounds_.right - bounds_.left) / s_ - 48;
    p.Text(title, TypeRole::Title, R(24, y, w, 24), theme_->color.text);
    p.Text(sub, TypeRole::Body, R(24, y + 25, w, 20), theme_->color.text2);
}

void SetupScreen::DrawCard(float x, float y, float w, float h) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto r = R(x, y, w, h);
    p.Fill(r, theme_->size.radius_card() * s_, theme_->color.card);
    p.CardEdge(r, theme_->size.radius_card() * s_);
}

void SetupScreen::DrawDisc(float cx, float cy, float r, IconType icon, bool accent) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto mid = R(cx, cy, 0, 0);
    p.Circle(mid.left, mid.top, r * s_, accent ? theme_->color.accent_soft : theme_->color.raised);
    auto brush = p.B(accent ? theme_->color.accent_text : theme_->color.text);
    DrawIcon(dc_, *theme_, icon, mid.left, mid.top, r * s_, brush.Get());
}

// An icon, a value and one short description; a click opens its controls under it.
constexpr float kRowH = 56, kOpenH = 48;

float SetupScreen::FitScale(const Theme& theme, float panel_height, float dpi_scale) const {
    // Each step's lowest card, in DrawRecording/DrawClips/DrawAppearance, then its gap and footer.
    const float content = step_ == 0 ? 102 + 6 * kRowH + (open_row_ >= 0 ? kOpenH : 0.0f)
                        : step_ == 1 ? 108.0f + 64 + 52 + 12 + 212
                        : step_ == 2 ? 112.0f + 360
                                     : 112.0f + 208;
    const float needed = content + 16 + theme.size.control() + 24;
    return dpi_scale * (std::min)(1.0f, panel_height / (needed * dpi_scale));
}

void SetupScreen::DrawRow(float x, float y, float w, int index, IconType icon, const std::wstring& value, const std::wstring& desc) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const auto row = R(x + 4, y + 3, w - 8, kRowH - 6);
    if (index >= 0 && Hovering(row)) p.Fill(row, theme_->size.radius_control() * s_, c.HoverLayer(1.0f));
    DrawDisc(x + 34, y + kRowH * 0.5f, 18, icon);
    p.Text(value, TypeRole::Label, R(x + 64, y + 10, w - 110, 18), c.text, TextOpts{TextAlign::Leading, false, true});
    p.Text(desc, TypeRole::Caption, R(x + 64, y + 30, w - 110, 16), c.text2, TextOpts{TextAlign::Leading, false, true});
    if (index < 0) return;
    const auto mid = R(x + w - 24, y + kRowH * 0.5f, 0, 0);
    p.Chevron(mid.left, mid.top, open_row_ == index, c.text2);
    zones_.push_back({R(x, y, w, kRowH), [this, index] { open_row_ = open_row_ == index ? -1 : index; }});
}

void SetupScreen::DrawOpenRow(float x, float y, float w, int index) {
    (void)w;
    float ox = x + 64;
    const float h = 36;  // kOpenH leaves the rest below; kRowH's own margin is the gap above
    if (index == 0) {
        std::vector<std::wstring> names;
        int selected = 0;
        const ombra::RecordingGpuCaps* pick = ombra::PickRecordingGpu(facts_.gpus, plan_.adapter);
        for (size_t i = 0; i < gpus_.size(); ++i) {
            names.push_back(ShortGpuName(gpus_[i].name));
            if (pick && gpus_[i].vendor == pick->vendor) selected = static_cast<int>(i);
        }
        if (names.empty()) names.push_back(L"Automatic");
        PutMid(Select("gpu", names, selected, 220, [this](int i) {
            if (i < 0 || i >= static_cast<int>(gpus_.size())) return;
            plan_.adapter = gpus_[i].vendor;
            if (!gpus_[i].av1) plan_.codec = "h264";
        }), ox, y, h);
    } else if (index == 1) {
        static const std::pair<const wchar_t*, const char*> kSizes[] = {{L"Native", "native"}, {L"1440p", "1440p"}, {L"1080p", "1080p"}, {L"720p", "720p"}};
        std::vector<std::wstring> names;
        std::vector<std::string> keys;
        int selected = 0;
        for (const auto& [label, key] : kSizes) {
            const int shortside = key == std::string("1440p") ? 1440 : key == std::string("1080p") ? 1080 : key == std::string("720p") ? 720 : 0;
            if (shortside && shortside >= (std::min)(facts_.width, facts_.height)) continue;  // never scales up
            if (plan_.resolution == key) selected = static_cast<int>(names.size());
            names.push_back(shortside ? std::wstring(label) : std::wstring(L"Native (") + std::to_wstring(facts_.height) + L"p)");
            keys.push_back(key);
        }
        ox += PutMid(Select("size", names, selected, 130, [this, keys](int i) {
            if (i >= 0 && i < static_cast<int>(keys.size())) plan_.resolution = keys[i];
        }), ox, y, h) + 8;
        std::vector<int> rates = {30, 60};
        const int display = ombra::ResolveFramerate(0, facts_.display_hz);
        if (display > 60) rates.push_back(display);
        std::vector<std::wstring> labels;
        int sel = 1;
        for (size_t i = 0; i < rates.size(); ++i) {
            labels.push_back(std::to_wstring(rates[i]));
            if (rates[i] == plan_.framerate) sel = static_cast<int>(i);
        }
        PutMid(Seg("fps", labels, sel, [this, rates](int i) { plan_.framerate = rates[i]; }), ox, y, h);
    } else if (index == 2) {
        const ombra::RecordingGpuCaps* pick = ombra::PickRecordingGpu(facts_.gpus, plan_.adapter);
        const bool av1_ok = !pick || pick->av1;
        std::vector<std::wstring> codecs = av1_ok ? std::vector<std::wstring>{L"AV1", L"H.264"} : std::vector<std::wstring>{L"H.264"};
        ox += PutMid(Select("codec", codecs, plan_.codec == "av1" && av1_ok ? 0 : (av1_ok ? 1 : 0), 88, [this, av1_ok](int i) {
            plan_.codec = av1_ok && i == 0 ? "av1" : "h264";
        }), ox, y, h) + 8;
        static const char* kRates[] = {"vbr", "cbr", "cqp"};
        int rc = 0;
        for (int i = 0; i < 3; ++i) if (plan_.rate_control == kRates[i]) rc = i;
        ox += PutMid(Select("rate", {L"VBR", L"CBR", L"CQP"}, rc, 80, [this](int i) { if (i >= 0 && i < 3) plan_.rate_control = kRates[i]; }), ox, y, h) + 8;
        auto& mbps = Get<StepperControl>("mbps");
        mbps.value = std::round(ombra::SetupBitrateKbps(plan_, facts_) / 1000.0);
        mbps.min = 1;
        mbps.max = 200;
        mbps.step = 1;
        mbps.unit = L"Mbps";
        mbps.on_change = [this](double v) {
            plan_.preset = "custom";
            plan_.custom_kbps = static_cast<int>(v * 1000.0);
        };
        ox += PutMid(mbps, ox, y, h) + 4;
        // A bitrate set by hand stops following the size and frame rate; this hands it back.
        if (plan_.preset == "custom") {
            auto& reset = Get<IconButtonControl>("mbps_reset");
            reset.glyph = IconButtonControl::Glyph::Reset;
            reset.tooltip = L"Back to the recommended bitrate";
            reset.on_click = [this] {
                plan_.preset = ombra::RecommendSetup(facts_).preset;
                plan_.custom_kbps = 0;
            };
            PutMid(reset, ox, y, h);
        }
    } else if (index == 3) {
        static const double kLengths[] = {30, 60, 120, 300};
        int sel = 1;
        for (int i = 0; i < 4; ++i) if (plan_.seconds == kLengths[i]) sel = i;
        PutMid(Seg("length", {L"30 s", L"1 min", L"2 min", L"5 min"}, sel, [this](int i) { plan_.seconds = kLengths[i]; }), ox, y, h);
    } else if (index == 4) {
        PutMid(Seg("when", {L"Always", L"Only in games"}, plan_.mode == "game" ? 1 : 0, [this](int i) { plan_.mode = i == 1 ? "game" : "desktop"; }), ox, y, h);
    }
}

void SetupScreen::DrawLane(float x, float y, float w, float h, std::vector<IconType> icons, const std::wstring& name, float seed, bool selected) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const auto box = R(x, y, w, h);
    p.Fill(box, theme_->size.radius_inner() * s_, selected ? c.accent_soft : c.HoverLayer(1.0f));
    p.Stroke(box, theme_->size.radius_inner() * s_, selected ? c.accent_line : c.line);
    const auto ink = selected ? c.accent_text : c.text2;
    auto brush = p.B(ink);
    float ix = x + 8;
    for (IconType icon : icons) {
        const auto mid = R(ix + 7, y + h * 0.5f, 0, 0);
        DrawIcon(dc_, *theme_, icon, mid.left, mid.top, 14 * s_, brush.Get());
        ix += 18;
    }
    const float tw = p.TextW(name, TypeRole::Caption) / s_ + 1;
    p.Text(name, TypeRole::Caption, R(ix + 2, y, tw, h), selected ? c.text : c.text2);
    // A fixed pattern, not the person's audio: it only says "sound lives here".
    int i = 0;
    for (float wx = ix + tw + 14; wx < x + w - 8; wx += 4, ++i) {
        const float k = std::fabs(std::sin(i * 0.7f + seed) * std::cos(i * 0.23f + seed * 2.0f));
        const float bar = (h - 8) * (0.2f + 0.8f * k);
        const auto a = R(wx, y + (h - bar) * 0.5f, 0, bar);
        p.Line(a.left, a.top, a.left, a.bottom, ink, 1.5f);
    }
}

constexpr float kLanesH = 48;

void SetupScreen::DrawTrackTile(float x, float y, float w, float h, const std::wstring& title, bool merged, bool selected, bool recommended, std::function<void()> fn) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const auto box = R(x, y, w, h);
    const float r = theme_->size.radius_card() * s_;
    if (selected) p.Fill(box, r, c.accent_soft);
    else if (Hovering(box)) p.Fill(box, r, c.HoverLayer(1.0f));
    p.Stroke(box, r, selected ? c.accent_line : c.line, selected ? 1.5f : 1.0f);
    p.Text(title, TypeRole::Label, R(x + 12, y + 10, w - 24, 18), c.text);
    if (recommended) {
        const std::wstring tag = L"Recommended";
        const float tw = p.TextW(tag, TypeRole::Caption) / s_ + 1;
        p.Text(tag, TypeRole::Caption, R(x + w - 12 - tw, y + 10, tw, 18), c.success);
    }
    if (merged) {
        DrawLane(x + 12, y + 34, w - 24, h - 44, {IconType::Volume, IconType::Mic}, L"Desktop + Mic", 0.4f, selected);
    } else {
        const float lane = (h - 44 - 4) * 0.5f;
        DrawLane(x + 12, y + 34, w - 24, lane, {IconType::Volume}, L"Desktop", 1.3f, selected);
        DrawLane(x + 12, y + 34 + lane + 4, w - 24, lane, {IconType::Mic}, L"Mic", 2.9f, selected);
    }
    zones_.push_back({box, std::move(fn)});
}

void SetupScreen::DrawStoreTile(float x, float y, float w, float h, IconType icon, const std::wstring& name, const std::wstring& fact, bool selected, bool recommended, std::function<void()> fn) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const auto box = R(x, y, w, h);
    const float r = theme_->size.radius_card() * s_;
    if (selected) p.Fill(box, r, c.accent_soft);
    else if (Hovering(box)) p.Fill(box, r, c.HoverLayer(1.0f));
    p.Stroke(box, r, selected ? c.accent_line : c.line, selected ? 1.5f : 1.0f);
    DrawDisc(x + 30, y + h * 0.5f, 18, icon, selected);
    const float ty = y + h * 0.5f - 28;  // name and two lines of fact, centred on the disc
    p.Text(name, TypeRole::Label, R(x + 58, ty, w - 66, 18), c.text);
    if (recommended) {
        const std::wstring tag = L"Recommended";
        const float tw = p.TextW(tag, TypeRole::Caption) / s_ + 1;
        p.Text(tag, TypeRole::Caption, R(x + w - 10 - tw, ty, tw, 18), c.success);
    }
    p.Text(fact, TypeRole::Caption, R(x + 58, ty + 20, w - 66, 48), c.text2, TextOpts{TextAlign::Leading, true, false, false});
    zones_.push_back({box, std::move(fn)});
}

void SetupScreen::DrawKeysFoot() {
    const paint::Paint p{dc_, *theme_, s_};
    const float ch = theme_->size.control();
    const float by = (bounds_.bottom - bounds_.top) / s_ - 24 - ch;
    float kx = 24;
    const auto piece = [&](IconType icon, const std::wstring& text, const std::string& name, std::string* spec, const std::string* other) {
        auto brush = p.B(theme_->color.text2);
        const auto mid = R(kx + 7, by + ch * 0.5f, 0, 0);
        DrawIcon(dc_, *theme_, icon, mid.left, mid.top, 14 * s_, brush.Get());
        kx += 20;
        const float w = p.TextW(text, TypeRole::Body) / s_ + 1;
        p.Text(text, TypeRole::Body, R(kx, by, w, ch), theme_->color.text2);
        kx += w + 12;
        // Just the keys, as in Settings: the control's hover layer and hand cursor say it's editable.
        auto& field = KeyField(name, spec, other);
        kx += PutMid(field, kx, by, ch) + 16;
    };
    piece(IconType::Save, L"Save a clip", "key_save", &plan_.hotkey_save, &plan_.hotkey_overlay);
    piece(IconType::Sliders, L"Open this panel", "key_overlay", &plan_.hotkey_overlay, &plan_.hotkey_save);
}

void SetupScreen::DrawFootButtons(const std::wstring& primary, std::function<void()> on_primary, bool back) {
    const float W = (bounds_.right - bounds_.left) / s_, ch = theme_->size.control();
    const float by = (bounds_.bottom - bounds_.top) / s_ - 24 - ch;
    const LayoutContext ctx{s_, theme_};
    auto& go = Button("primary", primary, ButtonControl::Kind::Primary, std::move(on_primary));
    float right = W - 24 - go.Measure(LayoutConstraints{}, ctx).width / s_;
    Put(go, right, by);
    if (back) {
        auto& b = Button("back", L"Back", ButtonControl::Kind::Secondary, [this] { --step_; open_row_ = -1; });
        right -= 8 + b.Measure(LayoutConstraints{}, ctx).width / s_;
        Put(b, right, by);
    }
}

// -- Step 1 ---------------------------------------------------------------------------------------

void SetupScreen::DrawRecording() {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const float W = (bounds_.right - bounds_.left) / s_, cw = W - 48, gap = 12, lw = cw * 0.55f, rw = cw - lw - gap;

    const ombra::RecordingGpuCaps* pick = ombra::PickRecordingGpu(facts_.gpus, plan_.adapter);
    std::wstring gpu_name = L"Automatic", gpu_full;
    for (const GpuInfo& g : gpus_)
        if (pick && g.vendor == pick->vendor) { gpu_full = g.name; gpu_name = ShortGpuName(g.name); }
    wchar_t ram[32];
    swprintf_s(ram, L"%.1f GB", ram_gb_);
    // No GPU named when the engine's status didn't arrive in time (Open(nullptr, ...)): the row says
    // "Automatic", the sentence just leaves the GPU out rather than read "your Automatic".
    const std::wstring gpu_clause = gpu_full.empty() ? L"" : pick->vendor == "cpu" ? L"CPU, " : gpu_name + L", ";
    DrawTitle(L"Recommended for this PC",
              L"Picked for your " + gpu_clause + std::to_wstring(facts_.display_hz) + L" Hz monitor and " + ram + L" of memory.", 54);

    const float top = 102, open_h = open_row_ >= 0 ? kOpenH : 0.0f, h = 6 * kRowH + open_h;
    DrawCard(24, top, lw, h);

    const std::wstring gpu_desc = pick && pick->vendor == "cpu" ? L"No video encoder on a GPU, so the CPU records"
                                : !display_owner_.empty() && display_owner_ != gpu_full ? L"Your monitor is on another GPU, so frames cross over"
                                : L"Records on the GPU your monitor is connected to";
    const ombra::VideoSize out = ombra::ResolveOutputSize(static_cast<uint32_t>(facts_.width), static_cast<uint32_t>(facts_.height), plan_.resolution);
    const std::wstring picture = std::to_wstring((std::min)(out.width, out.height)) + L"p, " + std::to_wstring(plan_.framerate) + L" fps";
    const std::wstring mbps = std::to_wstring(static_cast<int>(std::round(ombra::SetupBitrateKbps(plan_, facts_) / 1000.0))) + L" Mbps";
    const std::wstring codec = plan_.codec == "av1" ? L"AV1" : L"H.264";
    const std::wstring quality_desc = plan_.rate_control == "cqp" ? L"Quality stays fixed, so a busy scene can shorten the replay"
                                    : plan_.preset == "custom" ? L"Your bitrate: it won't follow size or frame rate"
                                    : plan_.codec == "av1" ? L"Sharper clips at the same size" : L"Plays on anything";
    const int secs = static_cast<int>(plan_.seconds);
    const std::wstring length = secs < 60 ? L"Last " + std::to_wstring(secs) + L" seconds"
                              : secs == 60 ? L"Last 1 minute" : L"Last " + std::to_wstring(secs / 60) + L" minutes";
    const std::wstring memory = FormatMemory(static_cast<double>(ombra::SetupMemoryBytes(plan_, facts_)));

    float y = top;
    const auto row = [&](int index, IconType icon, const std::wstring& value, const std::wstring& desc) {
        if (index > 0) {
            const auto a = R(24 + 16, y, lw - 32, 0);
            p.Line(a.left, a.top, a.right, a.top, c.line, 1.0f);
        }
        DrawRow(24, y, lw, index, icon, value, desc);
        y += kRowH;
        if (open_row_ == index) {
            DrawOpenRow(24, y, lw, index);
            y += open_h;
        }
    };
    row(0, IconType::Zap, gpu_name, gpu_desc);
    row(1, IconType::Monitor, picture, L"Your full screen, smooth enough for clips");
    row(2, IconType::Gauge, codec + L", " + mbps, quality_desc);
    row(3, IconType::Replay, length, L"Uses up to " + memory + L" of your memory");
    row(4, IconType::Gamepad2, plan_.mode == "game" ? L"Only in games" : L"Always ready",
        plan_.mode == "game" ? L"Nothing records until a game is running" : L"Or only while a game is running");
    {
        const auto a = R(24 + 16, y, lw - 32, 0);
        p.Line(a.left, a.top, a.right, a.top, c.line, 1.0f);
        DrawRow(24, y, lw, -1, IconType::Power, L"Start with Windows", L"Ready when a game starts");
        PutMid(Switch("autostart", plan_.autostart, [this](bool on) { plan_.autostart = on; }), 24 + lw - 16 - 36, y, kRowH);
    }

    const float rx = 24 + lw + gap;
    DrawCard(rx, top, rw, h);
    p.Text(L"Audio in your clips", TypeRole::Label, R(rx + 16, top + 14, rw - 32, 18), c.text);
    float ax = rx + 16;
    auto& desktop = Get<ToggleChipControl>("desktop");
    desktop.label = L"Desktop";
    desktop.icon = IconType::Volume;
    desktop.on = plan_.desktop_audio;
    desktop.on_change = [this](bool on) { plan_.desktop_audio = on; };
    ax += PutMid(desktop, ax, top + 40, 32) + 8;
    auto& mic = Get<ToggleChipControl>("mic");
    mic.label = L"Microphone";
    mic.icon = IconType::Mic;
    mic.on = plan_.mic;
    mic.on_change = [this](bool on) { plan_.mic = on; };
    PutMid(mic, ax, top + 40, 32);
    const float tiles = top + 80;
    if (plan_.desktop_audio && plan_.mic) {
        const float tile_h = (top + h - 16 - tiles - 8) * 0.5f;
        DrawTrackTile(rx + 16, tiles, rw - 32, tile_h, L"Together in one track", true, plan_.one_track, true, [this] { plan_.one_track = true; });
        DrawTrackTile(rx + 16, tiles + tile_h + 8, rw - 32, tile_h, L"Separate tracks", false, !plan_.one_track, false, [this] { plan_.one_track = false; });
    } else if (plan_.desktop_audio || plan_.mic) {
        DrawLane(rx + 16, tiles, rw - 32, kLanesH, {plan_.mic ? IconType::Mic : IconType::Volume}, plan_.mic ? L"Mic" : L"Desktop", 1.3f, true);
    } else {
        p.Text(L"Clips will have no sound.", TypeRole::Body, R(rx + 16, tiles, rw - 32, 24), c.text2);
    }

    DrawKeysFoot();
    DrawFootButtons(L"Next", [this] { step_ = 1; open_row_ = -1; }, false);
    const LayoutContext ctx{s_, theme_};
    auto& rec = Button("recommended", L"Use recommended", ButtonControl::Kind::Secondary, [this] { Recommend(true); step_ = 3; open_row_ = -1; });
    const float ch = theme_->size.control(), by = (bounds_.bottom - bounds_.top) / s_ - 24 - ch;
    const float next_w = Get<ButtonControl>("primary").Measure(LayoutConstraints{}, ctx).width / s_;
    Put(rec, W - 24 - next_w - 8 - rec.Measure(LayoutConstraints{}, ctx).width / s_, by);
}

// -- Step 2 ---------------------------------------------------------------------------------------

void SetupScreen::DrawNotifyPicture(float x, float y, float w, float h) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const auto screen = R(x, y, w, h);
    const float r = theme_->size.radius_control() * s_;
    p.Fill(screen, r, c.panel);
    p.Stroke(screen, r, c.line_strong);
    const float m = 10, sw = 84, sh = 22, tw = 124, th = 36;
    struct Corner { const char* key; bool right, bottom; };
    static const Corner kCorners[] = {{"top_left", false, false}, {"top_right", true, false}, {"bottom_left", false, true}, {"bottom_right", true, true}};
    for (const Corner& corner : kCorners) {
        const bool chosen = plan_.toast_position == corner.key;
        const float bw = chosen ? tw : sw, bh = chosen ? th : sh;
        const float bx = corner.right ? x + w - m - bw : x + m, by = corner.bottom ? y + h - m - bh : y + m;
        const auto box = R(bx, by, bw, bh);
        // The whole quadrant is the click target, not just the small slot.
        const auto quad = R(corner.right ? x + w * 0.5f : x, corner.bottom ? y + h * 0.5f : y, w * 0.5f, h * 0.5f);
        const std::string key = corner.key;
        zones_.push_back({quad, [this, key] { plan_.toast_position = key; }});
        if (!chosen) {
            if (Hovering(quad)) p.Fill(box, theme_->size.radius_inner() * s_, c.HoverLayer(1.0f));
            p.Stroke(box, theme_->size.radius_inner() * s_, c.line_strong);
            continue;
        }
        const float a = plan_.toasts ? 1.0f : 0.4f;
        p.Fill(box, theme_->size.radius_control() * s_, paint::Faded(c.raised, a));
        p.Stroke(box, theme_->size.radius_control() * s_, paint::Faded(c.accent_line, a), 1.5f);
        const auto mid = R(bx + 18, by + th * 0.5f, 0, 0);
        p.Circle(mid.left, mid.top, 10 * s_, paint::Faded(c.accent_soft, a));
        p.Check(mid.left, mid.top, paint::Faded(c.accent_text, a));
        p.Text(L"Clip saved", TypeRole::Caption, R(bx + 34, by, tw - 38, th), paint::Faded(c.text, a));
    }
}

void SetupScreen::DrawClips() {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const float W = (bounds_.right - bounds_.left) / s_, cw = W - 48, gap = 12, lw = cw * 0.62f, rw = cw - lw - gap;
    DrawTitle(L"Your clips", L"Where they go, and where the replay waits until you save.", 54);
    const float top = 108;
    const bool disk = plan_.storage == "file";
    const float folder_h = 64, game_h = 52, h1 = folder_h + game_h;
    DrawCard(24, top, lw, h1);
    DrawDisc(24 + 30, top + folder_h * 0.5f, 16, IconType::Folder);
    p.Text(L"Clip folder", TypeRole::Label, R(24 + 56, top + 14, 150, 18), c.text);
    p.Text(output_free_, TypeRole::Caption, R(24 + 56, top + 33, 150, 16), c.text2);
    auto& folder = Get<PathControl>("folder");
    folder.path = output_root_shown_;
    folder.width_dip = (std::min)(270.0f, lw - 56 - 150 - 16);
    folder.on_browse = [this] {
        const std::wstring picked = PickFolderDialog(host_->OwnerWindow(), output_root_shown_);
        if (picked.empty()) return;
        output_root_shown_ = picked;
        plan_.output_root = Narrow(picked);
        output_free_ = FreeOn(picked);
    };
    PutMid(folder, 24 + lw - 16 - folder.width_dip, top, folder_h);
    {
        const auto a = R(24 + 16, top + folder_h, lw - 32, 0);
        p.Line(a.left, a.top, a.right, a.top, c.line, 1.0f);
    }
    DrawDisc(24 + 30, top + folder_h + game_h * 0.5f, 14, IconType::Gamepad2);
    p.Text(L"A folder per game", TypeRole::Label, R(24 + 56, top + folder_h, 300, game_h), c.text);
    PutMid(Switch("per_game", plan_.per_game_folders, [this](bool on) { plan_.per_game_folders = on; }), 24 + lw - 16 - 36, top + folder_h, game_h);

    const float sy = top + h1 + gap, sh = 212.0f, th = disk ? 116.0f : 156.0f, ty = sy + 40;
    DrawCard(24, sy, lw, sh);
    p.Text(L"Keep the replay in", TypeRole::Label, R(24 + 16, sy + 14, lw - 32, 18), c.text);
    const float tw = (lw - 32 - 8) * 0.5f;
    ombra::SetupPlan in_memory = plan_;
    in_memory.storage = "ram";
    DrawStoreTile(24 + 16, ty, tw, th, IconType::Zap, L"Memory",
                  L"Up to " + FormatMemory(static_cast<double>(ombra::SetupMemoryBytes(in_memory, facts_))) + L". Nothing is written until you save.",
                  !disk, true, [this] { SetStorage("ram"); });
    wchar_t fact[96];
    swprintf_s(fact, L"For very long replays. Writes about %.0f GB an hour, all the time.",
               ombra::SetupBitrateKbps(plan_, facts_) * 1000.0 / 8.0 * 3600.0 / 1e9);
    DrawStoreTile(24 + 16 + tw + 8, ty, tw, th, IconType::Save, L"Disk", fact, disk, false, [this] { SetStorage("file"); });
    if (disk) {
        const float py = ty + th + 4, pw = (std::min)(270.0f, lw - 32 - 200);
        auto& ring = Get<PathControl>("ring");
        ring.path = Wide(plan_.file_path);
        ring.width_dip = pw;
        ring.on_browse = [this] {
            const std::wstring picked = PickFolderDialog(host_->OwnerWindow(), L"");
            if (picked.empty()) return;
            plan_.file_path = Narrow(picked + L"\\ring.dat");
            disk_note_.clear();
        };
        PutMid(ring, 24 + 16, py, 32);
        p.Text(disk_note_, TypeRole::Caption, R(24 + 16 + pw + 12, py, lw - 32 - pw - 12, 32), c.text2, TextOpts{TextAlign::Leading, true, false, true});
    }

    const float rx = 24 + lw + gap, rh = h1 + gap + sh;
    DrawCard(rx, top, rw, rh);
    DrawDisc(rx + 30, top + 30, 14, IconType::Bell);
    p.Text(L"Notifications", TypeRole::Label, R(rx + 52, top + 12, rw - 110, 18), c.text);
    p.Text(L"When a clip saves", TypeRole::Caption, R(rx + 52, top + 31, rw - 110, 16), c.text2);
    PutMid(Switch("toasts", plan_.toasts, [this](bool on) { plan_.toasts = on; }), rx + rw - 16 - 36, top + 8, 44);
    DrawNotifyPicture(rx + 16, top + 60, rw - 32, rh - 60 - 38);
    p.Text(L"Click a corner to move it.", TypeRole::Caption, R(rx + 16, top + rh - 34, rw - 32, 26), c.text2, TextOpts{TextAlign::Center});

    DrawFootButtons(L"Next", [this] { step_ = 2; open_row_ = -1; }, true);
}

// -- Step 3 ---------------------------------------------------------------------------------------

void SetupScreen::DrawThemeTile(float x, float y, float w, float h, ThemeMode mode, const std::wstring& name, const std::string& key,
                                bool split) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const bool selected = plan_.theme == key;
    const auto box = R(x, y, w, h);
    const float r = theme_->size.radius_card() * s_;
    if (selected) p.Fill(box, r, c.accent_soft);
    else if (Hovering(box)) p.Fill(box, r, c.HoverLayer(1.0f));
    p.Stroke(box, r, selected ? c.accent_line : c.line, selected ? 1.5f : 1.0f);
    const float ax = x + 10, ay = y + 10, aw = w - 20, ah = h - 42;
    const auto art = [&](ThemeMode m) {
        Theme mini;
        mini.ApplyMode(m);
        mini.ApplyAccentKey(plan_.accent);
        p.Fill(R(ax, ay, aw, ah), theme_->size.radius_control() * s_, mini.color.panel);
        const float cx = ax + 8, cy = ay + 8, cw = aw - 16, ch = ah - 16;
        p.Fill(R(cx, cy, cw, ch), theme_->size.radius_inner() * s_, mini.color.card);
        p.Fill(R(cx + 8, cy + 8, cw * 0.45f, 6), 3 * s_, mini.color.text);
        p.Fill(R(cx + 8, cy + 20, cw * 0.65f, 5), 2.5f * s_, mini.color.text3);
        p.Fill(R(cx + 8, cy + ch - 18, std::min(44.0f, cw * 0.45f), 10), 5 * s_, mini.color.accent);
        p.Fill(R(cx + cw - 30, cy + ch - 18, 22, 10), 5 * s_, mini.color.accent);
    };
    if (split) {
        const float mid = ax + aw * 0.5f;
        dc_->PushAxisAlignedClip(R(ax, ay, mid - ax, ah), D2D1_ANTIALIAS_MODE_ALIASED);
        art(ThemeMode::Dark);
        dc_->PopAxisAlignedClip();
        dc_->PushAxisAlignedClip(R(mid, ay, ax + aw - mid, ah), D2D1_ANTIALIAS_MODE_ALIASED);
        art(ThemeMode::Light);
        dc_->PopAxisAlignedClip();
    } else {
        art(mode);
    }
    p.Text(name, TypeRole::Label, R(x + 12, y + h - 30, w - 24, 22), c.text);
    zones_.push_back({box, [this, key] { plan_.theme = key; ApplyThemePreview(); }});
}

void SetupScreen::DrawCornerTile(float x, float y, float w, float h, float radius, const std::wstring& name, const std::string& key) {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const bool selected = plan_.corners == key;
    const auto box = R(x, y, w, h);
    const float r = theme_->size.radius_card() * s_;
    if (selected) p.Fill(box, r, c.accent_soft);
    else if (Hovering(box)) p.Fill(box, r, c.HoverLayer(1.0f));
    p.Stroke(box, r, selected ? c.accent_line : c.line, selected ? 1.5f : 1.0f);
    const float side = h - 24;  // a square; Round's radius makes it a circle
    const auto art = R(x + 12, y + 12, side, side);
    p.Fill(art, std::min(radius, side * 0.5f) * s_, c.raised);
    p.Stroke(art, std::min(radius, side * 0.5f) * s_, c.text3, 1.5f);
    p.Text(name, TypeRole::Label, R(x + 12 + side + 10, y, w - side - 26, h), c.text);
    zones_.push_back({box, [this, key] { plan_.corners = key; ApplyThemePreview(); }});
}

void SetupScreen::DrawAppearance() {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const float W = (bounds_.right - bounds_.left) / s_, cw = W - 48, gap = 12, lw = cw * 0.58f, rw = cw - lw - gap;
    DrawTitle(L"Make it yours", L"Changes show right away. There's more in Settings > Appearance.", 54);
    const float top = 112, h = 360;
    DrawCard(24, top, lw, h);
    p.Text(L"Theme", TypeRole::Label, R(24 + 16, top + 14, lw - 32, 18), c.text);
    const float tw = (lw - 32 - 24) / 3, th = 140;
    DrawThemeTile(24 + 16, top + 40, tw, th, ThemeMode::Dark, L"Dark", "dark");
    DrawThemeTile(24 + 16 + tw + 12, top + 40, tw, th, ThemeMode::Light, L"Light", "light");
    DrawThemeTile(24 + 16 + 2 * (tw + 12), top + 40, tw, th, ThemeMode::Dark, L"System", "system", true);
    const float cy = top + 40 + th + 16;
    p.Text(L"Corners", TypeRole::Label, R(24 + 16, cy, lw - 32, 18), c.text);
    const float ctw = (lw - 32 - 16) / 3, cth = 58;
    DrawCornerTile(24 + 16, cy + 26, ctw, cth, 0, L"Sharp", "sharp");
    DrawCornerTile(24 + 16 + ctw + 8, cy + 26, ctw, cth, 7, L"Default", "default");
    DrawCornerTile(24 + 16 + 2 * (ctw + 8), cy + 26, ctw, cth, 999, L"Round", "round");
    const float fy = cy + 26 + cth + 14;
    p.Text(L"Screen frame", TypeRole::Label, R(24 + 16, fy, lw - 100, 18), c.text);
    p.Text(L"Rounds the screen's corners while the overlay is open.", TypeRole::Caption, R(24 + 16, fy + 19, lw - 100, 16), c.text2);
    PutMid(Switch("screen_frame", plan_.screen_frame, [this](bool on) { plan_.screen_frame = on; ApplyThemePreview(); }),
           24 + lw - 16 - 36, fy, 36);

    const float rx = 24 + lw + gap;
    DrawCard(rx, top, rw, h);
    p.Text(L"Accent colour", TypeRole::Label, R(rx + 16, top + 14, rw - 32, 18), c.text);
    auto& accent = Get<ColorPickerControl>("accent");
    SetAccentSwatches(accent, *theme_);
    accent.selected = AccentSwatchIndex(AccentIdFromKey(plan_.accent));
    ombra::ParseHexRgb(plan_.accent, &accent.custom);
    accent.on_change = [this](int i) {
        if (i >= 0 && i < kAccentSwatchCount) {
            plan_.accent = AccentIdToKey(kAccentSwatches[i]);
            ApplyThemePreview();
        }
    };
    accent.on_edit_custom = [this, custom = accent.custom](const D2D1_RECT_F& swatch) {
        const auto hex = [](uint32_t rgb) {
            char buf[8];
            snprintf(buf, sizeof(buf), "#%06x", rgb & 0xffffff);
            return std::string(buf);
        };
        const std::string original = plan_.accent;
        auto editor = std::make_unique<ColorEditorPopup>();
        editor->SetColor(custom);
        editor->on_preview = [this, hex](uint32_t rgb) { plan_.accent = hex(rgb); ApplyThemePreview(); };
        editor->on_commit = [this, hex](uint32_t rgb) { plan_.accent = hex(rgb); ApplyThemePreview(); };
        editor->on_cancel = [this, original] { plan_.accent = original; ApplyThemePreview(); };
        plan_.accent = hex(custom);
        ApplyThemePreview();
        host_->ShowPopup(std::move(editor), swatch);
    };
    Put(accent, rx + 16, top + 40);
    p.Text(L"Where the panel sits", TypeRole::Label, R(rx + 16, top + 96, rw - 32, 18), c.text);
    auto& panel = Get<PanelPositionControl>("panel");
    panel.options = {L"center", L"top", L"bottom", L"left", L"right"};
    static const char* kPositions[] = {"center", "top", "bottom", "left", "right"};
    panel.selected = 0;
    for (int i = 0; i < 5; ++i) if (plan_.panel_position == kPositions[i]) panel.selected = i;
    panel.max_width_dip = rw - 32;
    panel.on_change = [this](int i) {
        if (i >= 0 && i < 5) { plan_.panel_position = kPositions[i]; ApplyThemePreview(); }
    };
    Put(panel, rx + 16 + (rw - 32 - panel.max_width_dip) * 0.5f, top + 122);

    DrawFootButtons(L"Next", [this] { step_ = 3; open_row_ = -1; }, true);
}

// -- Step 4 ---------------------------------------------------------------------------------------

// Error reporting is asked here, every time, so each person decides it; the
// choice starts at Error reports, the opt-out default. "Use recommended" lands here too.
void SetupScreen::DrawSharing() {
    const paint::Paint p{dc_, *theme_, s_};
    const auto& c = theme_->color;
    const float W = (bounds_.right - bounds_.left) / s_, cw = W - 48, gap = 12, lw = cw * 0.62f, rw = cw - lw - gap;
    DrawTitle(L"When something goes wrong",
              L"What " OMBRA_PRODUCT_NAME L" may send so it can be fixed. You can change this in Settings > Advanced.", 54);
    const float top = 112, pad = 16;
    auto& detail = Get<LevelDetail>("sharing");
    if (detail.stops.empty()) {
        detail.stops = SharingStops();
        detail.tiles->options = SharingLabels();
    }
    detail.width_dip = lw - 2 * pad;
    detail.tiles->selected = SharingIndex(plan_.sharing);
    detail.tiles->on_change = [this](int i) { plan_.sharing = kSharingKeys[i]; };
    // Both cards as tall as the tiles and the tallest stop's words need, at least the "Never sent" list.
    const float dh = detail.Measure(LayoutConstraints{}, LayoutContext{s_, theme_}).height / s_;
    const float h = (std::max)(208.0f, 48 + dh + pad);
    DrawCard(24, top, lw, h);
    p.Text(L"Share with the developer", TypeRole::Label, R(24 + pad, top + 14, lw - 2 * pad, 18), c.text);
    Put(detail, 24 + pad, top + 48);

    const float rx = 24 + lw + gap;
    DrawCard(rx, top, rw, h);
    p.Text(L"Never sent", TypeRole::Label, R(rx + pad, top + 14, rw - 2 * pad, 18), c.text);
    const wchar_t* never[] = {L"Your clips, screenshots and sound", L"What's on your screen or what you type",
                              L"What's in your files"};
    float y = top + 44;
    for (const wchar_t* line : never) {
        const auto mid = R(rx + pad + 9, y + 10, 0, 0);
        p.Circle(mid.left, mid.top, 9 * s_, c.raised);
        p.Line(mid.left - 3.5f * s_, mid.top - 3.5f * s_, mid.left + 3.5f * s_, mid.top + 3.5f * s_, c.text2, 1.5f);
        p.Line(mid.left - 3.5f * s_, mid.top + 3.5f * s_, mid.left + 3.5f * s_, mid.top - 3.5f * s_, c.text2, 1.5f);
        p.Text(line, TypeRole::Body, R(rx + pad + 28, y, rw - 2 * pad - 28, 20), c.text, TextOpts{TextAlign::Leading, false, true});
        y += 32;
    }
    p.Text(L"Logs and resource tracking are always kept on this PC, so you can send them with a bug report yourself.",
           TypeRole::Caption, R(rx + pad, y + 8, rw - 2 * pad, 48), c.text2, TextOpts{TextAlign::Leading, true});

    DrawFootButtons(L"Start Instant Replay", [this] { Finish(); }, true);
}

}  // namespace ombra_ui
