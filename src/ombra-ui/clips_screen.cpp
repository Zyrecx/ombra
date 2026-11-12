#include "ombra-ui/clips_screen.h"

#include "ombra-ui/ui/draw_probe.h"
#include "ombra-ui/ui/motion.h"
#include "ombra-ui/ui/paint.h"
#include "ombra/brand.h"
#include "ombra/clip_file.h"
#include "ombra/clip_ops.h"
#include "ombra/log.h"
#include "ombra/paths.h"
#include "ombra/util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>

namespace ombra_ui {

namespace {

ComPtr<ID2D1SolidColorBrush> Solid(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c) {
    ComPtr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(c, &b);
    return b;
}

void Text(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
          const D2D1_COLOR_F& c) {
    if (!fmt || s.empty()) return;
    DrawString(dc, s.c_str(), static_cast<UINT32>(s.size()), fmt, r, Solid(dc, c).Get());
}

std::wstring FormatDuration(double seconds) {
    const long total = static_cast<long>(std::floor((std::max)(0.0, seconds)));
    wchar_t buf[32];
    if (total >= 3600) swprintf_s(buf, L"%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    else swprintf_s(buf, L"%ld:%02ld", total / 60, total % 60);
    return buf;
}

// "0:07.40": to the hundredth, so a frame step visibly moves it.
std::wstring FormatPrecise(double seconds) {
    const double t = (std::max)(0.0, seconds);
    const long cs = static_cast<long>(std::floor(t * 100.0 + 1e-6));
    wchar_t buf[16];
    swprintf_s(buf, L".%02ld", cs % 100);
    return FormatDuration(t) + buf;
}

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes >= (uint64_t{1} << 30)) swprintf_s(buf, L"%.1f GB", double(bytes) / double(uint64_t{1} << 30));
    else swprintf_s(buf, L"%.0f MB", (std::max)(1.0, double(bytes) / double(1 << 20)));
    return buf;
}

// The per-game folder a save went into, else the file name up to its date ("Desktop 2026-10-15
// 17-49-42" -> "Desktop").
std::wstring GameOf(const std::filesystem::path& file, const std::filesystem::path& root) {
    std::error_code ec;
    const auto rel = std::filesystem::relative(file.parent_path(), root, ec);
    if (!ec && !rel.empty() && rel != L".") return rel.begin()->wstring();
    std::wstring stem = file.stem().wstring();
    for (size_t i = 0; i + 1 < stem.size(); ++i) {
        if (stem[i] == L' ' && std::iswdigit(stem[i + 1])) {
            stem.resize(i);
            break;
        }
    }
    return stem.empty() ? L"Clips" : stem;
}

// Explorer's own thumbnail (its cache answers in a few ms per clip, measured 2026-10-19), as
// top-down BGRA.
bool ShellThumbnail(const std::wstring& path, UINT px_w, UINT px_h, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP bmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{static_cast<LONG>(px_w), static_cast<LONG>(px_h)},
                                 SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp)) || !bmp) {
        return false;
    }
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bgra.resize(size_t(bm.bmWidth) * bm.bmHeight * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, bm.bmHeight, bgra.data(), &bi, DIB_RGB_COLORS) == bm.bmHeight;
        ReleaseDC(nullptr, dc);
        w = static_cast<UINT>(bm.bmWidth);
        h = static_cast<UINT>(bm.bmHeight);
    }
    DeleteObject(bmp);
    return ok;
}

// A new file beside the original, never over it: "<name> (trim).mp4", "<name> (edit 2).mp4".
std::wstring EditedPath(const std::wstring& src, const std::wstring& word) {
    const std::filesystem::path p(src);
    for (int n = 1; n < 1000; ++n) {
        const std::wstring suffix = n == 1 ? L" (" + word + L")" : L" (" + word + L" " + std::to_wstring(n) + L")";
        const auto candidate = p.parent_path() / (p.stem().wstring() + suffix + p.extension().wstring());
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate.wstring();
    }
    return {};
}

constexpr float kTileMinWidthDip = 250.0f;  // the grid's column count, not a size token

// Heights in DIPs, from the theme so density reaches them.
float TitleBlockDip(const Sizes& z) { return LineHeight(TypeRole::Label) + LineHeight(TypeRole::Caption) + z.space_sm(); }
float HeaderDip(const Sizes& z) {
    return (std::max)(LineHeight(TypeRole::Title) + LineHeight(TypeRole::Caption), z.control()) + z.space_md();
}
float TileMetaDip(const Sizes& z) { return z.space_sm() + LineHeight(TypeRole::Label) + LineHeight(TypeRole::Caption) + z.space_sm(); }
float TimelineDip(const Sizes& z) { return z.control(); }
float ControlsDip(const Sizes& z) { return z.control() + z.space_sm(); }
float StatusDip(const Sizes& z) { return LineHeight(TypeRole::Caption) + z.space_xs(); }
float PlayerBelowDip(const Sizes& z) { return TimelineDip(z) + ControlsDip(z) + StatusDip(z) + z.space_sm(); }

const Theme& DefaultTheme() {
    static const Theme t;
    return t;
}

}  // namespace

// Space, the arrows (5 s, as YouTube and Medal), I/O and comma/period for frames (YouTube,
// LosslessCut).
const ClipShortcut kClipShortcuts[] = {
    {VK_SPACE,      L"Space",  ClipAction::PlayPause, L"Play / pause"},
    {VK_LEFT,       L"\u2190", ClipAction::Back5,     L"Back 5 s"},
    {VK_RIGHT,      L"\u2192", ClipAction::Forward5,  L"Forward 5 s"},
    {VK_OEM_COMMA,  L",",      ClipAction::PrevFrame, L"Previous frame"},
    {VK_OEM_PERIOD, L".",      ClipAction::NextFrame, L"Next frame"},
    {'I',           L"I",      ClipAction::StartHere, L"Start here"},
    {'O',           L"O",      ClipAction::EndHere,   L"End here"},
    {VK_ESCAPE,     L"Esc",    ClipAction::Close,     L"Back to clips"},
};
const size_t kClipShortcutCount = sizeof(kClipShortcuts) / sizeof(kClipShortcuts[0]);

// speed_select_'s options, in order.
static constexpr double kPreviewRates[] = {0.5, 1.0, 1.5, 2.0};

struct ClipsScreen::Hit {
    enum Kind { None, Tile, Timeline, InHandle, OutHandle, Video } kind = None;
    int index = -1;
};

ClipsScreen::ClipsScreen(ScreenHost* host) : host_(host) {
    open_folder_btn_.label = L"Open folder";
    open_folder_btn_.icon = IconType::Folder;
    open_folder_btn_.on_click = [this] {
        if (root_.empty()) return;
        CreateDirectoryW(root_.c_str(), nullptr);
        if (!NativeWindowBlocked(L"Explorer at " + root_))
            ShellExecuteW(nullptr, L"open", root_.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
        if (host_) host_->CloseOverlay();
    };
    for (IconButtonControl* b : {&close_btn_, &play_btn_}) b->glyph = IconButtonControl::Glyph::Icon;
    close_btn_.icon = IconType::Close;
    close_btn_.tooltip = L"Back to clips";
    close_btn_.on_click = [this] { CloseClip(); };
    play_btn_.icon = IconType::Play;
    play_btn_.on_click = [this] { TogglePlay(); };
    volume_.on_mute = [this] {
        if (!player_) return;
        player_->SetMuted(!player_->Muted());
        if (host_) host_->RequestRepaint();
    };
    save_split_.label = L"Save trimmed";
    save_split_.icon = IconType::Scissors;
    save_split_.on_click = [this] { SaveTrimmed(SaveMode::Copy); };
    save_split_.on_menu = [this] {
        if (!host_) return;
        save_split_.open = true;
        // A pick runs that action once; the main area stays Save trimmed.
        host_->ShowDropdown(save_split_.bounds, {save_split_.label, L"Replace original"}, 0,
                            [this](int i) {
                                if (i == 0) SaveTrimmed(SaveMode::Copy);
                                else ReplaceOriginal();
                            },
                            [this] {
                                save_split_.open = false;
                                if (host_) host_->RequestRepaint();
                            });
    };
    sort_select_.options = {L"Newest", L"Oldest", L"Largest", L"Longest", L"Game"};
    sort_select_.width_dip = 130.0f;
    sort_select_.on_change = [this](int i) {
        sort_select_.selected = i;
        sort_ = static_cast<Sort>(i);
        scroll_y_ = 0.0f;
        if (host_) host_->RequestRepaint();
    };
    speed_select_.options = {L"0.5×", L"1×", L"1.5×", L"2×"};
    speed_select_.width_dip = 64.0f;
    speed_select_.fit_options = true;
    speed_select_.selected = 1;
    speed_select_.on_change = [this](int i) {
        speed_select_.selected = i;
        if (player_) player_->SetRate(kPreviewRates[i]);
        if (host_) host_->RequestRepaint();
    };
    for (SelectControl* sel : {&sort_select_, &speed_select_}) {
        sel->on_open = [this](SelectControl& s) {
            if (!host_) {
                s.open = false;
                return;
            }
            host_->ShowDropdown(s.bounds, s.options, s.selected, s.on_change, [this, &s] {
                s.open = false;
                if (host_) host_->RequestRepaint();
            });
        };
    }
    cancel_export_btn_.label = L"Cancel";
    cancel_export_btn_.visible = false;
    cancel_export_btn_.on_click = [this] { CancelExport(); };
    copy_btn_.label = L"Copy";
    copy_btn_.icon = IconType::Copy;
    copy_btn_.on_click = [this] {
        if (selected_ < 0) return;
        // The range just saved needn't be written again.
        const bool edited = Trimmed() || TracksOff();
        if (edited && last_trim_.src == clips_[selected_].path && last_trim_.in == trim_in_ && last_trim_.out == trim_out_ &&
            last_trim_.tracks == track_on_ && GetFileAttributesW(last_trim_.dst.c_str()) != INVALID_FILE_ATTRIBUTES)
            CopyToClipboard(last_trim_.dst);
        else if (edited) SaveTrimmed(SaveMode::CopyThenClipboard);
        else CopyToClipboard(clips_[selected_].path);
    };
    show_btn_.label = L"Show in folder";
    show_btn_.icon = IconType::Folder;
    show_btn_.on_click = [this] { ShowInFolder(); };
    delete_btn_.tooltip = L"Delete clip";
    delete_btn_.icon = IconType::Trash;
    delete_btn_.kind = ButtonControl::Kind::Danger;
    delete_btn_.on_click = [this] { DeleteClip(); };
    filter_chips_.on_change = [this](int i) {
        filter_ = i - 1;
        scroll_y_ = 0.0f;
        if (host_) host_->RequestRepaint();
    };
    volume_.on_change = [this](double v) {
        if (!player_) return;
        player_->SetVolume(v);
        if (player_->Muted()) player_->SetMuted(false);
        if (host_) host_->RequestRepaint();
    };
    store_btn_.label = L"Get the AV1 Video Extension";
    store_btn_.kind = ButtonControl::Kind::Primary;
    store_btn_.on_click = [this] {
        const wchar_t* url = L"ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V";
        if (!NativeWindowBlocked(L"Microsoft Store (AV1 Video Extension)"))
            ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        if (host_) host_->CloseOverlay();
    };
}

ClipsScreen::~ClipsScreen() = default;

const std::vector<ClipsScreen::KeyRow>& ClipsScreen::KeyRows() {
    auto key = [](ClipAction a) {
        for (size_t i = 0; i < kClipShortcutCount; ++i)
            if (kClipShortcuts[i].action == a) return std::wstring(kClipShortcuts[i].key);
        return std::wstring();
    };
    static const std::vector<KeyRow> rows = {
        {{key(ClipAction::PlayPause)}, L"Play / pause", ClipAction::PlayPause, true, L"Playback"},
        {{key(ClipAction::Back5), key(ClipAction::Forward5)}, L"Skip 5 s", ClipAction::Forward5, false, L""},
        {{key(ClipAction::PrevFrame), key(ClipAction::NextFrame)}, L"Step a frame", ClipAction::NextFrame, false, L""},
        {{key(ClipAction::StartHere)}, L"Start here", ClipAction::StartHere, true, L"Trim"},
        {{key(ClipAction::EndHere)}, L"End here", ClipAction::EndHere, false, L""},
        {{key(ClipAction::Close)}, L"Back to clips", ClipAction::Close, true, L""},
    };
    return rows;
}

// One flow measures and draws, as MemoryCard's.
float ClipsScreen::KeysCardFlow(ID2D1DeviceContext* dc, const Theme& t, float s, bool draw) {
    const paint::Paint p{dc, t, s};
    const ColorRoles& c = t.color;
    const Sizes& z = t.size;
    const float pad = z.card_padding() * s;
    const float x0 = keys_card_.left + pad, x1 = keys_card_.right - pad;
    const float label = LineHeight(TypeRole::Label) * s, cap = LineHeight(TypeRole::Caption) * s;
    const float row_h = z.control_sm() * s;
    const float key_h = row_h - z.space_xs() * s;
    float y = keys_card_.top + pad;
    if (draw) p.Text(L"Shortcuts", TypeRole::Label, D2D1::RectF(x0, y, x1, y + label), c.text);
    y += label;
    if (draw) p.Text(L"Click or press", TypeRole::Caption,
                     D2D1::RectF(x0, y, x1, y + cap), c.text3);
    y += cap + z.space_sm() * s;
    key_row_rects_.clear();
    const auto& rows = KeyRows();
    for (size_t i = 0; i < rows.size(); ++i) {
        const KeyRow& r = rows[i];
        if (r.group_start && i > 0) {
            y += z.space_sm() * s;
            if (draw) p.Fill(D2D1::RectF(x0, y, x1, y + 1 * s), 0, c.line);
            y += 1 * s + z.space_sm() * s;
        }
        if (r.group_start && !r.group.empty()) {
            if (draw) p.Text(r.group, TypeRole::Caption, D2D1::RectF(x0, y, x1, y + cap), c.text3);
            y += cap + z.space_xxs() * s;
        }
        const D2D1_RECT_F row = D2D1::RectF(x0 - z.space_sm() * s, y, x1 + z.space_sm() * s, y + row_h);
        key_row_rects_.push_back(row);
        if (draw) {
            if (key_hover_ == static_cast<int>(i)) p.Fill(row, z.radius_control() * s, c.HoverLayer());
            const float kw = p.Keys(r.keys, 0, 0, key_h, false, true);
            p.Keys(r.keys, x1 - kw, (row.top + row.bottom) * 0.5f, key_h, true, true);
            p.Text(r.label, TypeRole::Body, D2D1::RectF(x0, row.top, x1 - kw - z.space_sm() * s, row.bottom), c.text,
                   {TextAlign::Leading, true, true});
        }
        y += row_h;
    }
    return y + pad;
}

std::vector<Control*> ClipsScreen::Buttons() {
    if (selected_ < 0) return {&open_folder_btn_, &sort_select_, &filter_chips_};
    std::vector<Control*> out{&close_btn_, &play_btn_, &volume_, &speed_select_, &save_split_, &copy_btn_, &show_btn_,
                              &delete_btn_, &store_btn_, &cancel_export_btn_};
    for (const auto& c : track_chips_) out.push_back(c.get());
    return out;
}

// ---- list ----------------------------------------------------------------------------------

void ClipsScreen::Open(const std::wstring& root) {
    const ombra::InstancePaths& inst = ombra::Instance();
    root_ = inst.isolated ? inst.data_dir + L"\\clips" : root;
    selected_ = -1;
    scroll_y_ = 0.0f;
    LoadList();
}

void ClipsScreen::Leave() {
    CloseClip();
    if (host_) host_->SetFrameTimer(false);
}

void ClipsScreen::ReleaseDeviceResources() {
    if (player_) player_->Close();
    player_.reset();
    player_failed_init_ = false;
    frame_bitmap_.Reset();
    for (auto& c : clips_) c.thumb.Reset();  // bound to the D2D device; rebuilt from bytes below
    clips_.clear();
    selected_ = -1;
}

void ClipsScreen::WaitForWrites(DWORD timeout_ms) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (writes_->load() > 0 && GetTickCount64() < until) Sleep(20);
}

void ClipsScreen::LoadList() {
    const uint64_t gen = ++generation_;
    loading_ = true;
    // The fallback folder too: a clip saved there when output.root's drive was missing (save.cpp).
    std::vector<std::wstring> roots{root_};
    const std::wstring fallback = ombra::FallbackClipsDir();
    if (!fallback.empty() && _wcsicmp(fallback.c_str(), root_.c_str()) != 0) roots.push_back(fallback);
    host_->RunAsync([this, gen, roots, post = host_->UiPoster()] {
        std::vector<ClipEntry> found;
        namespace fs = std::filesystem;
        for (const std::wstring& root : roots) {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                std::wstring ext = it->path().extension().wstring();
                for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
                if (ext != L".mp4") continue;
                // One root inside the other lists a clip once.
                if (std::any_of(found.begin(), found.end(), [&](const ClipEntry& c) {
                        return _wcsicmp(c.path.c_str(), it->path().c_str()) == 0;
                    })) continue;
                WIN32_FILE_ATTRIBUTE_DATA a{};
                if (!GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &a)) continue;
                ClipEntry e;
                e.path = it->path().wstring();
                e.game = GameOf(it->path(), root);
                e.written = a.ftLastWriteTime;
                e.bytes = (uint64_t{a.nFileSizeHigh} << 32) | a.nFileSizeLow;
                found.push_back(std::move(e));
            }
        }
        std::sort(found.begin(), found.end(), [](const ClipEntry& a, const ClipEntry& b) {
            return CompareFileTime(&a.written, &b.written) > 0;
        });
        auto shared = std::make_shared<std::vector<ClipEntry>>(std::move(found));
        post([this, gen, shared] {
            if (gen != generation_) return;
            ApplyList(std::move(*shared));
            loading_ = false;
            games_.clear();
            for (const auto& c : clips_) {
                if (std::find(games_.begin(), games_.end(), c.game) == games_.end()) games_.push_back(c.game);
            }
            if (filter_ >= static_cast<int>(games_.size())) filter_ = -1;
            std::vector<std::wstring> paths;
            for (const auto& c : clips_) paths.push_back(c.path);
            LoadDetails(gen, std::move(paths));
            host_->RequestRepaint();
        });
    });
}

// A tile from before this refresh can be open by now: follow it to its new position (keeping its
// loaded details), or close the player if the file is gone, so selected_ never outlives its entry.
void ClipsScreen::ApplyList(std::vector<ClipEntry> next) {
    int follow = -1;
    if (selected_ >= 0) {
        for (int i = 0; i < static_cast<int>(next.size()); ++i) {
            if (next[i].path == clips_[selected_].path) {
                next[i] = std::move(clips_[selected_]);
                follow = i;
                break;
            }
        }
        if (follow < 0) CloseClip();
    }
    clips_ = std::move(next);
    selected_ = follow;
}

// Duration, keyframes and a thumbnail per clip, posted back in small batches so the grid fills in
// as it goes rather than after the slowest file.
void ClipsScreen::LoadDetails(uint64_t gen, std::vector<std::wstring> paths) {
    const UINT thumb_w = static_cast<UINT>(std::lround(320.0f * s_)), thumb_h = static_cast<UINT>(std::lround(180.0f * s_));
    host_->RunAsync([this, gen, paths = std::move(paths), thumb_w, thumb_h, post = host_->UiPoster()] {
        ombra::ComInit com;
        struct Detail {
            std::wstring path;
            ombra::ClipInfo info;
            bool info_ok = false;
            std::vector<uint8_t> bgra;
            UINT w = 0, h = 0;
        };
        auto batch = std::make_shared<std::vector<Detail>>();
        auto flush = [&] {
            if (batch->empty()) return;
            post([this, gen, batch] {
                if (gen != generation_) return;
                for (auto& d : *batch) {
                    for (auto& c : clips_) {
                        if (c.path != d.path) continue;
                        c.info_loaded = true;
                        if (d.info_ok) {
                            c.duration_s = d.info.duration_s;
                            c.keyframes_s = std::move(d.info.keyframes_s);
                        }
                        c.thumb_bgra = std::move(d.bgra);
                        c.thumb_w = d.w;
                        c.thumb_h = d.h;
                    }
                }
                host_->RequestRepaint();
            });
            batch = std::make_shared<std::vector<Detail>>();
        };
        for (const auto& p : paths) {
            Detail d;
            d.path = p;
            d.info_ok = ombra::ReadClipInfo(p, d.info);
            ShellThumbnail(p, thumb_w, thumb_h, d.bgra, d.w, d.h);
            batch->push_back(std::move(d));
            if (batch->size() >= 6) flush();
        }
        flush();
    });
}

std::vector<int> ClipsScreen::Visible() const {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(clips_.size()); ++i) {
        if (filter_ < 0 || clips_[i].game == games_[filter_]) out.push_back(i);
    }
    // clips_ is newest first and stays that way (selected_ indexes it); ties keep newest first.
    const auto by = [&](auto before) {
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return before(clips_[a], clips_[b]); });
    };
    switch (sort_) {
        case Sort::Newest: break;
        case Sort::Oldest: std::reverse(out.begin(), out.end()); break;
        case Sort::Largest: by([](const ClipEntry& a, const ClipEntry& b) { return a.bytes > b.bytes; }); break;
        // A duration not read yet is 0: those clips wait at the end.
        case Sort::Longest: by([](const ClipEntry& a, const ClipEntry& b) { return a.duration_s > b.duration_s; }); break;
        case Sort::Game: by([](const ClipEntry& a, const ClipEntry& b) { return _wcsicmp(a.game.c_str(), b.game.c_str()) < 0; }); break;
    }
    return out;
}

std::vector<std::wstring> ClipsScreen::GridNamesForTest(int sort) {
    sort_ = static_cast<Sort>(sort);
    std::vector<std::wstring> names;
    for (int i : Visible()) names.push_back(std::filesystem::path(clips_[i].path).filename().wstring());
    return names;
}

bool ClipsScreen::QueryHoverTooltip(std::wstring* text, D2D1_POINT_2F* anchor) {
    for (Control* b : Buttons())
        if (b->QueryTooltip(text, anchor)) return true;
    return false;
}

// ---- player --------------------------------------------------------------------------------

void ClipsScreen::OpenClip(int index) {
    if (index < 0 || index >= static_cast<int>(clips_.size())) return;
    // An export started from an earlier overlay is still working on it: opening it would hold the
    // file the export may be about to replace.
    if (ombra::ClipBusy(clips_[index].path) && !(export_.active && export_.src == clips_[index].path)) {
        if (host_) host_->ShowModal(L"This clip is being trimmed", L"It opens again when the trim is saved.", false,
                                    nullptr, nullptr, ModalButtons{L"OK", L""});
        return;
    }
    selected_ = index;
    status_.clear();
    drag_ = Drag::None;
    const ClipEntry& c = clips_[index];
    trim_in_ = 0.0;
    trim_out_ = c.duration_s;
    needs_av1_extension_ = false;

    if (!player_ && !player_failed_init_) {
        player_ = std::make_unique<ClipPlayer>();
        if (!player_->Init(host_ ? host_->VideoDevice() : nullptr)) {
            player_.reset();
            player_failed_init_ = true;
        } else {
            // --drive and tests never play sound on the user's speakers.
            if (ombra::Instance().isolated) player_->SetMuted(true);
        }
    }
    ombra::ClipInfo info;
    const bool info_ok = ombra::ReadClipInfo(c.path, info);
    fps_ = info_ok ? info.fps : 0.0;
    step_frame_ = -1;
    track_names_.clear();
    for (const auto& n : info.audio_track_names) {  // hdlr names are UTF-8
        std::wstring w(n.size(), L'\0');
        w.resize(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, n.data(), static_cast<int>(n.size()), w.data(), static_cast<int>(w.size()))));
        track_names_.push_back(w.empty() ? L"Audio" : w);
    }
    track_on_.assign(track_names_.size(), true);
    if (player_) {
        if (info_ok && info.codec == ombra::VideoCodec::AV1 && !ClipPlayer::HasAv1Decoder()) {
            needs_av1_extension_ = true;
            player_->Close();
        } else {
            player_->Open(c.path);
            player_->SetLoop(0.0, 0.0);
        }
    }
    if (host_) {
        host_->SetFrameTimer(player_ != nullptr && !needs_av1_extension_);
        host_->RequestRepaint();
    }
}

void ClipsScreen::CloseClip() {
    if (player_) player_->Close();
    frame_bitmap_.Reset();
    selected_ = -1;
    drag_ = Drag::None;
    if (host_) {
        host_->SetFrameTimer(false);
        host_->RequestRepaint();
    }
}

void ClipsScreen::SetStatus(const std::wstring& text) {
    status_ = text;
    status_until_ = motion::NowMs() + 5000;
    if (host_) host_->RequestRepaint();
}

void ClipsScreen::TogglePlay() {
    if (!player_) return;
    step_frame_ = -1;
    if (player_->Playing()) player_->Pause();
    else {
        // Play from the trim start when the playhead sits outside the kept range.
        const double pos = player_->Position();
        if (Trimmed() && (pos < trim_in_ || pos >= trim_out_ - 0.05)) player_->Seek(trim_in_);
        player_->Play();
    }
    if (host_) host_->RequestRepaint();
}

void ClipsScreen::SeekTo(double seconds) {
    step_frame_ = -1;
    if (player_) player_->Seek(seconds);
}

void ClipsScreen::RunAction(ClipAction a) {
    switch (a) {
        case ClipAction::PlayPause: TogglePlay(); break;
        case ClipAction::Back5: if (player_) SeekTo(player_->Position() - 5.0); break;
        case ClipAction::Forward5: if (player_) SeekTo(player_->Position() + 5.0); break;
        case ClipAction::PrevFrame: StepFrame(-1); break;
        case ClipAction::NextFrame: StepFrame(+1); break;
        case ClipAction::StartHere: SetStartHere(); break;
        case ClipAction::EndHere: SetEndHere(); break;
        case ClipAction::Close: CloseClip(); return;
    }
    if (host_) host_->RequestRepaint();
}

int64_t ClipsScreen::CurrentFrame() const {
    if (!player_ || fps_ <= 0.0) return 0;
    if (step_frame_ >= 0) return step_frame_;  // a step's seek may not have shown its frame yet
    // The clock, not the decoded frame's timestamp: after a seek the clock is the target at once,
    // and an exact seek shows the frame under it. (Frame timestamps from OnVideoStreamTick read 0
    // after a paused seek on an H.264 clip, 2026-11-06.)
    return FrameAt(player_->Position(), fps_);
}

// One frame from the frame on screen, counted as an index so repeated keys never round onto the
// same frame or skip one while an earlier seek is still decoding.
void ClipsScreen::StepFrame(int dir) {
    if (!player_ || player_->GetState() != ClipPlayer::State::Ready || fps_ <= 0.0 || selected_ < 0) return;
    player_->Pause();
    const int64_t last = std::max<int64_t>(0, FrameAt(clips_[selected_].duration_s, fps_) - 1);
    const int64_t target = std::clamp<int64_t>(CurrentFrame() + dir, 0, last);
    player_->Seek(FrameSeekTime(target, fps_));
    step_frame_ = target;
}

// Start at the playhead, on the frame on screen (a start between keyframes re-encodes on save).
void ClipsScreen::SetStartHere() {
    if (!player_ || selected_ < 0) return;
    trim_in_ = fps_ > 0.0 ? CurrentFrame() / fps_ : player_->Position();
    if (trim_in_ > trim_out_ - 0.5) trim_out_ = clips_[selected_].duration_s;
    player_->SetLoop(Trimmed() ? trim_in_ : 0.0, Trimmed() ? trim_out_ : 0.0);
}

// End at the playhead, keeping the frame on screen: a trim keeps the frames that start before its
// end time (TrimClip), so the end is that frame's end.
void ClipsScreen::SetEndHere() {
    if (!player_ || selected_ < 0) return;
    const double dur = clips_[selected_].duration_s;
    const double end = fps_ > 0.0 ? (CurrentFrame() + 1) / fps_ : player_->Position();
    trim_out_ = std::clamp(end, trim_in_ + 0.5, dur);
    player_->SetLoop(Trimmed() ? trim_in_ : 0.0, Trimmed() ? trim_out_ : 0.0);
}

void ClipsScreen::ReplaceOriginal() {
    if (selected_ < 0 || busy_ || !(Trimmed() || TracksOff()) || !host_) return;
    const std::wstring name = std::filesystem::path(clips_[selected_].path).filename().wstring();
    host_->ShowModal(L"Replace the original?",
                     L"The trimmed clip takes the name " + name + L". The original goes to the Recycle Bin.", true,
                     [this] { SaveTrimmed(SaveMode::Replace); }, nullptr, ModalButtons{L"Replace", L"Cancel"});
}

// The kept range can only start on a keyframe: that is where a cut without re-encoding starts.
double ClipsScreen::SnapToKeyframe(double seconds) const {
    if (selected_ < 0) return seconds;
    const auto& keys = clips_[selected_].keyframes_s;
    if (keys.empty()) return 0.0;
    double best = keys.front();
    for (double k : keys) {
        if (std::abs(k - seconds) < std::abs(best - seconds)) best = k;
    }
    return best;
}

bool ClipsScreen::Trimmed() const {
    if (selected_ < 0) return false;
    const double dur = clips_[selected_].duration_s;
    return trim_in_ > 0.01 || trim_out_ < dur - 0.01;
}

bool ClipsScreen::TracksOff() const {
    return std::find(track_on_.begin(), track_on_.end(), false) != track_on_.end();
}

std::wstring ClipsScreen::OffTrackNames() const {
    std::wstring out;
    for (size_t i = 0; i < track_on_.size() && i < track_names_.size(); ++i) {
        if (track_on_[i]) continue;
        if (!out.empty()) out += L", ";
        out += track_names_[i];
    }
    return out;
}

// The buttons say what a save or copy will contain, so a switched-off track never reads as a
// preview-only setting.
void ClipsScreen::UpdateActionLabels() {
    size_t off = 0;
    for (bool on : track_on_) off += on ? 0 : 1;
    if (Trimmed()) save_split_.label = L"Save trimmed";
    else if (off == 1) save_split_.label = L"Save without " + OffTrackNames();
    else if (off > 1) save_split_.label = L"Save without " + std::to_wstring(off) + L" tracks";
    else save_split_.label = L"Save trimmed";
    copy_btn_.label = Trimmed() || off ? L"Copy edited" : L"Copy";
}

void ClipsScreen::SaveTrimmed(SaveMode mode) {
    if (selected_ < 0 || busy_ || export_.active || !(Trimmed() || TracksOff())) return;
    const bool then_copy = mode == SaveMode::CopyThenClipboard;
    const bool replace = mode == SaveMode::Replace;
    const std::wstring src = clips_[selected_].path;
    // Replace writes beside the original too: the original goes only once the new file is whole.
    const std::wstring dst = EditedPath(src, Trimmed() ? L"trim" : L"edit");
    if (dst.empty()) return;
    const double a = trim_in_, b = Trimmed() ? trim_out_ : 1e9;  // untrimmed: to the end
    // What the timeline shows now, for Copy to compare with later. The timeline stays editable
    // during the write, so the completion below must not read it again.
    SaveDone d;
    d.src = src;
    d.dst = dst;
    d.a = a;
    d.out_saved = trim_out_;
    d.tracks = track_on_;
    d.then_copy = then_copy;
    d.replace = replace;
    d.gen = generation_;
    if (NeedsReencode()) {
        StartExport(d, b);
        return;
    }
    busy_ = true;
    SetStatus(replace ? L"Replacing the original…" : L"Saving a copy…");
    writes_->fetch_add(1);
    host_->RunAsync([this, d, b, post = host_->UiPoster(), writes = writes_]() mutable {
        d.r = ombra::TrimClip(d.src, d.dst, d.a, b, &d.tracks);
        if (d.r.ok && d.replace) {
            ombra::ClipInfo check;  // never trade the original for a file that doesn't read back
            std::string err;
            if (!ombra::ReadClipInfo(d.dst, check, &err) || check.duration_s <= 0.0) {
                DeleteFileW(d.dst.c_str());
                d.r.ok = false;
                d.r.error = "the trimmed file doesn't read back: " + err;
            }
        }
        writes->fetch_sub(1);
        post([this, d] { SaveFinished(d); });
    });
}

// A save's result, on the UI thread: the stream copy's (which still has to replace the original)
// or the export child's (which replaced it already, `replaced` >= 0).
void ClipsScreen::SaveFinished(const SaveDone& d) {
    busy_ = false;
    const ombra::TrimResult& r = d.r;
    if (!r.ok) {
        ombra::Log::Warn("clips: trim failed: %s", r.error.c_str());
        if (r.error == "cancelled") SetStatus(L"Cancelled");
        else SetStatus(d.replace ? L"Couldn't replace the original" : L"Couldn't save the copy");
        if (d.replace && d.gen == generation_ && selected_ >= 0 && clips_[selected_].path == d.src && player_)
            player_->Open(d.src);  // closed for the export
        return;
    }
    const std::wstring name = std::filesystem::path(d.dst).filename().wstring();
    const bool on_src = d.gen == generation_ && selected_ >= 0 && clips_[selected_].path == d.src;
    if (d.replace) {
        const bool replaced = d.replaced >= 0 ? d.replaced == 1 : ReplaceWithTrim(d.src, d.dst);
        if (replaced) {
            last_trim_ = {};
            if (on_src) {
                ClipEntry& c = clips_[selected_];
                WIN32_FILE_ATTRIBUTE_DATA attr{};
                if (GetFileAttributesExW(d.src.c_str(), GetFileExInfoStandard, &attr)) {
                    c.bytes = (uint64_t{attr.nFileSizeHigh} << 32) | attr.nFileSizeLow;
                    c.written = attr.ftLastWriteTime;
                }
                ombra::ClipInfo info;
                if (ombra::ReadClipInfo(d.src, info)) {
                    c.duration_s = info.duration_s;
                    c.keyframes_s = std::move(info.keyframes_s);
                }
                c.thumb.Reset();
                LoadDetails(d.gen, {d.src});
                OpenClip(selected_);
            }
            SetStatus(L"Replaced the original (" + FormatDuration(r.end_s - r.start_s) + L"); the old one is in the Recycle Bin");
            return;
        }
        // Both files stay when the original couldn't go: the trim under its own name.
        if (player_ && on_src) player_->Open(d.src);
        SetStatus(L"Saved as " + name + L": the original couldn't go to the Recycle Bin, so it's kept");
    } else {
        last_trim_ = {d.src, d.a, d.out_saved, d.tracks, d.dst};
        if (d.then_copy) CopyToClipboard(d.dst);
        else SetStatus(L"Saved " + name + L" (" + FormatDuration(r.end_s - r.start_s) + L")");
    }
    // List it, keeping the player on the clip it was cut from.
    if (d.gen == generation_ && selected_ >= 0) {
        ClipEntry e;
        e.path = d.dst;
        e.game = clips_[selected_].game;
        GetSystemTimeAsFileTime(&e.written);
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(d.dst.c_str(), GetFileExInfoStandard, &attr))
            e.bytes = (uint64_t{attr.nFileSizeHigh} << 32) | attr.nFileSizeLow;
        clips_.insert(clips_.begin(), std::move(e));
        ++selected_;
        LoadDetails(d.gen, {d.dst});
    }
}

// `ombra.exe export` beside this exe, its stdout read on a worker: progress lines while it runs,
// then "done START END OUTCOME" or "error MESSAGE" (engine.cpp RunExport). It is windowless and
// runs in the same OMBRA_HOME, so --drive exercises it for real; it inherits only its stdout pipe
// and the cancel event. When the overlay closes the child carries on: the reader just dies with
// this process, and a reopened overlay finds the source busy (ombra::ClipBusy).
void ClipsScreen::StartExport(const SaveDone& d, double end_s) {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe(self, n);
    exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"" OMBRA_ENGINE_EXE;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    HANDLE cancel = CreateEventW(&sa, TRUE, FALSE, nullptr);
    // This process, so the child can tell the overlay closed and toast the result itself.
    HANDLE self_proc = OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId());
    if (!cancel || !self_proc || !CreatePipe(&rd, &wr, &sa, 0)) {
        if (cancel) CloseHandle(cancel);
        if (self_proc) CloseHandle(self_proc);
        SetStatus(L"Couldn't start the trim");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring tracks;
    for (bool on : d.tracks) tracks += on ? L'1' : L'0';
    wchar_t times[96];
    swprintf_s(times, L" %.6f %.6f", d.a, end_s);
    std::wstring cmd = L"\"" + exe + L"\" export \"" + d.src + L"\" \"" + d.dst + L"\"" + times + L" --cancel " +
                       std::to_wstring(reinterpret_cast<uintptr_t>(cancel)) + L" --overlay " +
                       std::to_wstring(reinterpret_cast<uintptr_t>(self_proc));
    if (!tracks.empty()) cmd += L" --tracks " + tracks;
    if (d.replace) cmd += L" --replace";

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[3] = {wr, cancel, self_proc};
    bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    // Replace: the player must not hold the original open when the child recycles it.
    if (d.replace && player_) player_->Close();
    ok = ok && CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                              EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                              nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    CloseHandle(self_proc);
    if (!ok) {
        ombra::Log::Warn("clips: could not start %ls (%lu)", exe.c_str(), GetLastError());
        CloseHandle(rd);
        CloseHandle(cancel);
        if (d.replace && player_) player_->Open(d.src);
        SetStatus(L"Couldn't start the trim");
        return;
    }
    CloseHandle(pi.hThread);
    ombra::Log::Info("clips: export %ls %.3f-%.3f s%s", d.src.c_str(), d.a, end_s, d.replace ? " (replace)" : "");

    export_ = {};
    export_.active = true;
    export_.replace = d.replace;
    export_.src = d.src;
    export_.dst = d.dst;
    export_.cancel = cancel;
    export_.id = ++export_ids_;
    const uint64_t id = export_.id;
    host_->RunAsync([this, d = SaveDone(d), id, rd, process = pi.hProcess, post = host_->UiPoster()]() mutable {
        std::string pending, line;
        char buf[512];
        DWORD got = 0;
        bool finished = false;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            pending.append(buf, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                unsigned done = 0, total = 0;
                double s0 = 0, s1 = 0;
                char outcome[16] = {};
                if (sscanf_s(line.c_str(), "progress %u %u", &done, &total) == 2) {
                    post([this, id, done, total] {
                        if (export_.id != id) return;
                        export_.done = done;
                        export_.total = total;
                    });
                } else if (line == "phase finish") {
                    post([this, id] { if (export_.id == id) export_.finishing = true; });
                } else if (sscanf_s(line.c_str(), "done %lf %lf %15s", &s0, &s1, outcome, static_cast<unsigned>(sizeof(outcome))) >= 2) {
                    finished = true;
                    d.r.ok = true;
                    d.r.start_s = s0;
                    d.r.end_s = s1;
                    const std::string o = outcome;
                    d.replaced = !d.replace ? -1 : (o == "replaced" ? 1 : 0);
                } else if (line.rfind("error ", 0) == 0) {
                    finished = true;
                    d.r.ok = false;
                    d.r.error = line.substr(6);
                }
            }
        }
        CloseHandle(rd);
        if (!finished) {
            DWORD code = 0;
            WaitForSingleObject(process, 5000);
            GetExitCodeProcess(process, &code);
            d.r.ok = false;
            d.r.error = "the export stopped (exit code " + std::to_string(code) + ")";
        }
        CloseHandle(process);
        post([this, d, id] {
            if (export_.id == id) {
                if (export_.cancel) CloseHandle(export_.cancel);
                export_ = {};
            }
            SaveFinished(d);
        });
    });
    SetStatus(d.replace ? L"Replacing the original…" : L"Saving a copy…");
}

void ClipsScreen::CancelExport() {
    if (!export_.active || export_.finishing || !export_.cancel) return;
    SetEvent(export_.cancel);
}

bool ClipsScreen::NeedsReencode() const {
    if (selected_ < 0 || trim_in_ <= 0.0) return false;
    const double eps = fps_ > 0.0 ? 0.5 / fps_ : 0.01;
    for (double k : clips_[selected_].keyframes_s)
        if (std::abs(k - trim_in_) <= eps) return false;
    return true;
}

void ClipsScreen::SetExportForTest(uint32_t done, uint32_t total) {
    if (selected_ < 0) return;
    export_ = {};
    export_.active = true;
    export_.done = done;
    export_.total = total;
    export_.src = clips_[selected_].path;
}

// The original to the Recycle Bin, then the trim under its name with its times (so it keeps its
// place in the grid and in Explorer). False with both files where they were if the original
// couldn't be recycled. The player must not hold `src` open.
bool ClipsScreen::ReplaceWithTrim(const std::wstring& src, const std::wstring& trim) {
    if (player_ && selected_ >= 0 && clips_[selected_].path == src) player_->Close();  // the engine holds it open
    NativeWindowBlocked(L"Recycle Bin: " + src);  // --drive reports it; an isolated instance deletes
    return ombra::ReplaceWithEdited(src, trim);
}

void ClipsScreen::CopyToClipboard(const std::wstring& path) {
    if (NativeWindowBlocked(L"clipboard: " + path)) {
        SetStatus(L"Copied");
        return;
    }
    // CF_HDROP: the file itself, which Discord, Explorer and chat apps paste as an attachment.
    const size_t chars = path.size() + 2;  // double-null-terminated list of one
    HGLOBAL drop = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    HGLOBAL effect = GlobalAlloc(GHND, sizeof(DWORD));
    bool ok = false;
    if (drop && effect) {
        auto* df = static_cast<DROPFILES*>(GlobalLock(drop));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
        GlobalUnlock(drop);
        *static_cast<DWORD*>(GlobalLock(effect)) = DROPEFFECT_COPY;
        GlobalUnlock(effect);
        if (OpenClipboard(host_ ? host_->OwnerWindow() : nullptr)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_HDROP, drop) != nullptr;
            if (ok) drop = nullptr;  // the clipboard owns it now
            if (SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect)) effect = nullptr;
            CloseClipboard();
        }
    }
    if (drop) GlobalFree(drop);
    if (effect) GlobalFree(effect);
    SetStatus(ok ? L"Copied " + std::filesystem::path(path).filename().wstring() + L": paste it into Discord or a folder"
                 : L"Couldn't copy to the clipboard");
}

void ClipsScreen::ShowInFolder() {
    if (selected_ < 0) return;
    const std::wstring path = clips_[selected_].path;
    if (!NativeWindowBlocked(L"Explorer selecting " + path)) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        }
    }
    if (host_) host_->CloseOverlay();
}

void ClipsScreen::DeleteClip() {
    if (selected_ < 0 || !host_) return;
    const std::wstring path = clips_[selected_].path;
    const std::wstring name = std::filesystem::path(path).filename().wstring();
    host_->ShowModal(L"Delete this clip?", name + L" goes to the Recycle Bin.", true, [this, path] {
        if (player_) player_->Close();  // the engine holds the file open
        NativeWindowBlocked(L"Recycle Bin: " + path);  // --drive reports it; an isolated instance deletes
        const bool ok = ombra::RecycleClip(path, host_->OwnerWindow());
        if (!ok) {
            if (player_ && selected_ >= 0) player_->Open(path);
            SetStatus(L"Couldn't delete the clip");
            return;
        }
        const int idx = selected_;
        CloseClip();
        if (idx >= 0 && idx < static_cast<int>(clips_.size()) && clips_[idx].path == path) clips_.erase(clips_.begin() + idx);
        host_->RequestRepaint();
    }, nullptr, ModalButtons{L"Delete", L"Cancel"});
}

// ---- layout and drawing ------------------------------------------------------------------

float ClipsScreen::PanelHeightFor(float width, const Theme& theme, float dpi_scale) {
    const Sizes& z = theme.size;
    const float pad = z.card_padding() * dpi_scale;
    const float chrome = (TitleBlockDip(z) + PlayerBelowDip(z)) * dpi_scale + 2.0f * pad;
    return (width - 2.0f * pad) * 9.0f / 16.0f + chrome;
}

void ClipsScreen::Layout(const D2D1_RECT_F& outer, float dpi_scale) {
    // One panel around everything, as Settings': the screen reads over a game either way,
    // floating or attached.
    const Theme& t = host_ ? host_->GetTheme() : DefaultTheme();
    const Sizes& z = t.size;
    const float s = dpi_scale;
    card_rect_ = outer;
    const float pad = z.card_padding() * s;
    const D2D1_RECT_F bounds = D2D1::RectF(outer.left + pad, outer.top + pad, outer.right - pad, outer.bottom - pad);
    bounds_ = bounds;
    s_ = dpi_scale;
    const float W = bounds.right - bounds.left;
    for (Control* b : Buttons()) b->focused = b->focus_visible = false;
    LayoutContext ctx;
    ctx.dpi_scale = s;
    ctx.theme = &t;

    if (selected_ < 0) {
        // Header: title and count left, Open folder right; chips under it when there's more than one game.
        const D2D1_SIZE_F of = open_folder_btn_.Measure(LayoutConstraints{}, ctx);
        const float text_h = (LineHeight(TypeRole::Title) + LineHeight(TypeRole::Caption)) * s;
        const float oy = bounds.top + (text_h - of.height) * 0.5f;
        open_folder_btn_.bounds = D2D1::RectF(bounds.right - of.width, oy, bounds.right, oy + of.height);
        sort_select_.visible = clips_.size() > 1;
        {
            const D2D1_SIZE_F m = sort_select_.Measure(LayoutConstraints{}, ctx);
            const float sr = open_folder_btn_.bounds.left - z.space_sm() * s;
            sort_select_.bounds = D2D1::RectF(sr - m.width, oy, sr, oy + of.height);
        }
        float y = bounds.top + HeaderDip(z) * s;
        filter_chips_.visible = games_.size() > 1;
        if (filter_chips_.visible) {
            filter_chips_.options = {L"All"};
            filter_chips_.options.insert(filter_chips_.options.end(), games_.begin(), games_.end());
            filter_chips_.selected = filter_ + 1;
            const D2D1_SIZE_F m = filter_chips_.Measure(LayoutConstraints{}, ctx);
            filter_chips_.bounds = D2D1::RectF(bounds.left, y, (std::min)(bounds.right, bounds.left + m.width), y + m.height);
            y += m.height + z.space_md() * s;
        }
        grid_rect_ = D2D1::RectF(bounds.left, y, bounds.right, bounds.bottom);

        const float gap = z.space_lg() * s;
        const float tpad = z.space_sm() * s;
        const int cols = (std::max)(1, static_cast<int>((W + gap) / (kTileMinWidthDip * s + gap)));
        const float tile_w = (W - gap * (cols - 1)) / cols;
        const float tile_h = tpad + (tile_w - 2.0f * tpad) * 9.0f / 16.0f + TileMetaDip(z) * s;
        const std::vector<int> vis = Visible();
        tile_rects_.clear();
        for (size_t i = 0; i < vis.size(); ++i) {
            const int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
            const float x = bounds.left + c * (tile_w + gap);
            const float ty = grid_rect_.top + r * (tile_h + gap);
            tile_rects_.push_back(D2D1::RectF(x, ty, x + tile_w, ty + tile_h));
        }
        const int rows = (static_cast<int>(vis.size()) + cols - 1) / cols;
        content_h_ = rows > 0 ? rows * tile_h + (rows - 1) * gap : 0.0f;
        scroll_y_ = std::clamp(scroll_y_, 0.0f, (std::max)(0.0f, content_h_ - (grid_rect_.bottom - grid_rect_.top)));
        return;
    }

    // Player: title row, video, timeline, transport + actions, status line.
    const float title_h = TitleBlockDip(z) * s;
    const D2D1_SIZE_F cb = close_btn_.Measure(LayoutConstraints{}, ctx);
    const float text_h = (LineHeight(TypeRole::Label) + LineHeight(TypeRole::Caption)) * s;
    const float close_y = bounds.top + (text_h - cb.height) * 0.5f;
    close_btn_.bounds = D2D1::RectF(bounds.right - cb.width, close_y, bounds.right, close_y + cb.height);
    const float avail_h = (bounds.bottom - bounds.top) - title_h - PlayerBelowDip(z) * s;
    float vw = W, vh = W * 9.0f / 16.0f;
    if (vh > avail_h) { vh = avail_h; vw = vh * 16.0f / 9.0f; }
    const float vx = bounds.left + (W - vw) * 0.5f;
    video_rect_ = D2D1::RectF(vx, bounds.top + title_h, vx + vw, bounds.top + title_h + vh);
    float y = video_rect_.bottom + z.space_sm() * s;
    // Inset by the handle's half-width so a handle at either end stays inside the panel.
    const float inset = z.space_sm() * s;
    timeline_rect_ = D2D1::RectF(bounds.left + inset, y, bounds.right - inset, y + TimelineDip(z) * s);
    y = timeline_rect_.bottom;

    const float bh = z.control() * s;
    const float cy = y + (ControlsDip(z) * s - bh) * 0.5f;
    const float gap_xs = z.space_xs() * s;
    play_btn_.bounds = D2D1::RectF(bounds.left, cy, bounds.left + bh, cy + bh);
    const float vol_w = volume_.Measure(LayoutConstraints{}, ctx).width;
    volume_.bounds = D2D1::RectF(play_btn_.bounds.right + gap_xs, cy, play_btn_.bounds.right + gap_xs + vol_w, cy + bh);
    UpdateActionLabels();

    // Actions right-aligned, measured so a label never clips.
    float x = bounds.right;
    for (Control* b : std::initializer_list<Control*>{&delete_btn_, &show_btn_, &copy_btn_, &save_split_}) {
        const float w = b->Measure(LayoutConstraints{}, ctx).width;
        b->bounds = D2D1::RectF(x - w, cy, x, cy + bh);
        x -= w + z.space_sm() * s;
    }
    {  // A running export: Cancel takes Copy's slot (Copy is disabled then), right-aligned in it.
        const float w = cancel_export_btn_.Measure(LayoutConstraints{}, ctx).width;
        cancel_export_btn_.bounds = D2D1::RectF(copy_btn_.bounds.right - w, cy, copy_btn_.bounds.right, cy + bh);
    }
    // Track chips after the time readout, then the speed, as far as the action buttons leave room.
    // Speed gives way first: a chip changes what gets saved, speed is only the preview.
    {
        while (track_chips_.size() < track_names_.size()) {
            const size_t i = track_chips_.size();
            auto chip = std::make_unique<ToggleChipControl>();
            chip->on_change = [this, i](bool on) {
                if (i >= track_on_.size()) return;
                track_on_[i] = on;
                if (player_) player_->SetAudioTrackEnabled(i, on);
                UpdateActionLabels();
                if (host_) host_->RequestRepaint();
            };
            track_chips_.push_back(std::move(chip));
        }
        track_chips_.resize(track_names_.size());
        // The readout's widest form for this clip, so a short clip doesn't reserve room for an
        // hour-long one. Every digit becomes 0: digit widths differ. Length not known yet: assume long.
        const double known = selected_ >= 0 ? clips_[selected_].duration_s : 0.0;
        const double readout_dur = known > 0 ? known : (player_ ? player_->Duration() : 0.0);
        std::wstring readout = readout_dur > 0 ? FormatPrecise(readout_dur) + L" / " + FormatDuration(readout_dur)
                                               : std::wstring(L"00:00.00 / 00:00");
        for (wchar_t& c : readout)
            if (c >= L'0' && c <= L'9') c = L'0';
        float tx = volume_.bounds.right + z.space_md() * s + t.MeasureText(t.Font(TypeRole::Body), readout).width +
                   z.space_md() * s;
        const float limit = save_split_.bounds.left - z.space_sm() * s;
        const float speed_w = speed_select_.Measure(LayoutConstraints{}, ctx).width;
        bool fits = true;
        for (size_t i = 0; i < track_chips_.size(); ++i) {
            ToggleChipControl& chip = *track_chips_[i];
            chip.label = track_names_[i];
            chip.icon = track_names_[i].find(L"Mic") != std::wstring::npos ? IconType::Mic : IconType::Volume;
            chip.on = track_on_[i];
            const float w = chip.Measure(LayoutConstraints{}, ctx).width;
            fits = fits && tx + w <= limit;
            chip.visible = fits;
            chip.bounds = D2D1::RectF(tx, cy, tx + w, cy + bh);
            if (fits) tx += w + z.space_xs() * s;
        }
        speed_select_.visible = tx + speed_w <= limit;
        speed_select_.bounds = D2D1::RectF(tx, cy, tx + speed_w, cy + bh);
        const double rate = player_ ? player_->Rate() : 1.0;
        for (int i = 0; i < static_cast<int>(std::size(kPreviewRates)); ++i)
            if (kPreviewRates[i] == rate) speed_select_.selected = i;
    }
    store_btn_.visible = needs_av1_extension_;
    const D2D1_SIZE_F sb = store_btn_.Measure(LayoutConstraints{}, ctx);
    const float scx = (video_rect_.left + video_rect_.right) * 0.5f;
    const float scy = (video_rect_.top + video_rect_.bottom) * 0.5f + LineHeight(TypeRole::Body) * s + z.space_md() * s;
    store_btn_.bounds = D2D1::RectF(scx - sb.width * 0.5f, scy, scx + sb.width * 0.5f, scy + sb.height);

    // Shortcuts card, right of the player card: every row is also a button.
    const float cw = 224.0f * s, gap = 20.0f * s;
    keys_card_ = D2D1::RectF(card_rect_.right + gap, card_rect_.top, card_rect_.right + gap + cw, card_rect_.bottom);
}

void ClipsScreen::Draw(ID2D1DeviceContext* dc, const Theme& theme, float dpi_scale) {
    const float s = dpi_scale;
    const paint::Paint p{dc, theme, s};
    const ColorRoles& c = theme.color;
    const Sizes& z = theme.size;
    {
        const float r = z.radius_panel() * s;
        p.Fill(card_rect_, r, c.panel);
        p.CardEdge(card_rect_, r);
    }

    if (selected_ < 0) {
        const std::vector<int> vis = Visible();
        uint64_t total = 0;
        for (const auto& clip : clips_) total += clip.bytes;
        const float text_r = (sort_select_.visible ? sort_select_.bounds.left : open_folder_btn_.bounds.left) - z.space_sm() * s;
        const float title_b = bounds_.top + LineHeight(TypeRole::Title) * s;
        p.Text(L"Clips", TypeRole::Title, D2D1::RectF(bounds_.left, bounds_.top, text_r, title_b), c.text);
        const std::wstring sub = loading_ && clips_.empty()
                                     ? L"Looking for clips…"
                                     : std::to_wstring(clips_.size()) + (clips_.size() == 1 ? L" clip · " : L" clips · ") + FormatBytes(total);
        p.Text(sub, TypeRole::Caption, D2D1::RectF(bounds_.left, title_b, text_r, title_b + LineHeight(TypeRole::Caption) * s), c.text3);
        open_folder_btn_.Draw(dc, theme, s);
        sort_select_.Draw(dc, theme, s);

        filter_chips_.Draw(dc, theme, s);

        if (clips_.empty() && !loading_) {
            const float cy = paint::Cy(grid_rect_);
            const float icon = z.space_xxl() * s;
            DrawIcon(dc, theme, IconType::Film, (bounds_.left + bounds_.right) * 0.5f, cy - icon, icon, p.B(c.text3).Get());
            const float lb = LineHeight(TypeRole::Label) * s;
            p.Text(L"No clips yet", TypeRole::Label, D2D1::RectF(bounds_.left, cy, bounds_.right, cy + lb), c.text, {TextAlign::Center});
            p.Text(L"Saved replays show up here.", TypeRole::Caption,
                   D2D1::RectF(bounds_.left, cy + lb, bounds_.right, cy + lb + LineHeight(TypeRole::Caption) * s), c.text3,
                   {TextAlign::Center});
            return;
        }

        // Tiles as the Capture page's: raised, a hairline edge, the hover layer; the picture inset.
        tile_fade_.Track(hover_tile_, kFadeMs);
        const float tpad = z.space_sm() * s;
        dc->PushAxisAlignedClip(grid_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < vis.size() && i < tile_rects_.size(); ++i) {
            D2D1_RECT_F r = tile_rects_[i];
            r.top -= scroll_y_;
            r.bottom -= scroll_y_;
            if (r.bottom < grid_rect_.top || r.top > grid_rect_.bottom) continue;
            ClipEntry& clip = clips_[vis[i]];
            const float rad = z.radius_card() * s;
            p.Fill(r, rad, c.raised);
            p.Fill(r, rad, c.HoverLayer(tile_fade_.Amount(static_cast<int>(i))));
            p.Stroke(r, rad, c.line_strong);

            const D2D1_RECT_F thumb = D2D1::RectF(r.left + tpad, r.top + tpad, r.right - tpad,
                                                  r.top + tpad + (r.right - r.left - 2.0f * tpad) * 9.0f / 16.0f);
            const float ir = z.radius_control() * s;
            p.Fill(thumb, ir, c.panel);
            if (!clip.thumb && !clip.thumb_bgra.empty()) {
                const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                dc->CreateBitmap(D2D1::SizeU(clip.thumb_w, clip.thumb_h), clip.thumb_bgra.data(), clip.thumb_w * 4, &props, &clip.thumb);
                clip.thumb_bgra.clear();
                clip.thumb_bgra.shrink_to_fit();
            }
            if (clip.thumb) {
                // Cover the 16:9 box (crop, never letterbox): a thumbnail of another shape still fills it.
                const D2D1_SIZE_F bs = clip.thumb->GetSize();
                const float bw = thumb.right - thumb.left, bh = thumb.bottom - thumb.top;
                const float k = (std::max)(bw / bs.width, bh / bs.height);
                const float dw = bs.width * k, dh = bs.height * k;
                const D2D1_RECT_F dst = D2D1::RectF(thumb.left + (bw - dw) * 0.5f, thumb.top + (bh - dh) * 0.5f,
                                                    thumb.left + (bw + dw) * 0.5f, thumb.top + (bh + dh) * 0.5f);
                ComPtr<ID2D1RoundedRectangleGeometry> clip_geo;
                ComPtr<ID2D1Factory> factory;
                dc->GetFactory(&factory);
                factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(thumb, ir, ir), &clip_geo);
                ComPtr<ID2D1Layer> layer;
                dc->CreateLayer(&layer);
                dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip_geo.Get()), layer.Get());
                dc->DrawBitmap(clip.thumb.Get(), dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                dc->PopLayer();
            } else {
                DrawIcon(dc, theme, IconType::Film, (thumb.left + thumb.right) * 0.5f, paint::Cy(thumb), z.space_xl() * s, p.B(c.text3).Get());
            }
            if (clip.duration_s > 0.0) {
                // Over the picture in either theme: a dark scrim with white text, not a theme colour.
                const std::wstring d = FormatDuration(clip.duration_s);
                const float bw = p.TextW(d, TypeRole::Caption) + 2.0f * z.space_xs() * s;
                const float bh = LineHeight(TypeRole::Caption) * s + z.space_xxs() * s;
                const float m = z.space_xs() * s;
                const D2D1_RECT_F badge = D2D1::RectF(thumb.right - m - bw, thumb.bottom - m - bh, thumb.right - m, thumb.bottom - m);
                p.Fill(badge, z.radius_inner() * s, D2D1::ColorF(0, 0, 0, 0.72f));
                p.Text(d, TypeRole::Caption, badge, D2D1::ColorF(1, 1, 1, 1), {TextAlign::Center});
            }
            const float tx = r.left + tpad, my = thumb.bottom + z.space_sm() * s;
            const float lb = LineHeight(TypeRole::Label) * s;
            p.Text(clip.game, TypeRole::Label, D2D1::RectF(tx, my, r.right - tpad, my + lb), c.text, {TextAlign::Leading, false, true});
            p.Text(FormatWhen(clip.written) + L"  ·  " + FormatBytes(clip.bytes), TypeRole::Caption,
                   D2D1::RectF(tx, my + lb, r.right - tpad, my + lb + LineHeight(TypeRole::Caption) * s), c.text3,
                   {TextAlign::Leading, false, true});
        }
        dc->PopAxisAlignedClip();

        // Scrollbar when the grid overflows, centred in the card padding beside it.
        p.ScrollThumb(grid_rect_.right + z.card_padding() * s * 0.5f, grid_rect_.top, grid_rect_.bottom - grid_rect_.top,
                      content_h_, scroll_y_);
        return;
    }

    // ---- player
    const ClipEntry& clip = clips_[selected_];
    const float lb = LineHeight(TypeRole::Label) * s;
    const D2D1_RECT_F title_r = D2D1::RectF(bounds_.left, bounds_.top, close_btn_.bounds.left - z.space_sm() * s, bounds_.top + lb);
    p.Text(clip.game, TypeRole::Label, title_r, c.text, {TextAlign::Leading, false, true});
    p.Text(FormatWhen(clip.written) + L"  ·  " + FormatBytes(clip.bytes) + L"  ·  " + std::filesystem::path(clip.path).filename().wstring(),
           TypeRole::Caption, D2D1::RectF(title_r.left, title_r.bottom, title_r.right, title_r.bottom + LineHeight(TypeRole::Caption) * s),
           c.text3, {TextAlign::Leading, false, true});
    close_btn_.Draw(dc, theme, s);

    const float rad = z.radius_card() * s;
    p.Fill(video_rect_, rad, D2D1::ColorF(0, 0, 0, 1));
    const ClipPlayer::State st = player_ ? player_->GetState() : ClipPlayer::State::Failed;
    if (player_ && player_->Frame() && st == ClipPlayer::State::Ready) {
        if (!frame_bitmap_ || frame_bitmap_generation_ != player_->FrameGeneration()) {
            frame_bitmap_.Reset();
            ComPtr<IDXGISurface> surface;
            if (SUCCEEDED(player_->Frame()->QueryInterface(IID_PPV_ARGS(&surface)))) {
                const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &frame_bitmap_);
            }
            frame_bitmap_generation_ = player_->FrameGeneration();
        }
        if (frame_bitmap_) {
            const D2D1_SIZE_F bs = frame_bitmap_->GetSize();
            const float bw = video_rect_.right - video_rect_.left, bh = video_rect_.bottom - video_rect_.top;
            const float k = (std::min)(bw / bs.width, bh / bs.height);
            const float dw = bs.width * k, dh = bs.height * k;
            const D2D1_RECT_F dst = D2D1::RectF(video_rect_.left + (bw - dw) * 0.5f, video_rect_.top + (bh - dh) * 0.5f,
                                                video_rect_.left + (bw + dw) * 0.5f, video_rect_.top + (bh + dh) * 0.5f);
            dc->DrawBitmap(frame_bitmap_.Get(), dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    } else {
        // On the black video box in either theme: light text, not a theme colour.
        std::wstring msg;
        if (export_.active && export_.replace) msg = L"Replacing the original…";
        else if (needs_av1_extension_) msg = L"This clip is AV1. Windows plays it with Microsoft's free AV1 Video Extension.";
        else if (!player_) msg = L"Preview isn't available on this graphics device.";
        else if (st == ClipPlayer::State::Failed) msg = player_->Error();
        else msg = L"Loading…";
        const float cy = paint::Cy(video_rect_), m = z.space_lg() * s;
        p.Text(msg, TypeRole::Body, D2D1::RectF(video_rect_.left + m, cy - LineHeight(TypeRole::Body) * s, video_rect_.right - m, cy),
               D2D1::ColorF(1, 1, 1, 0.75f), {TextAlign::Center});
        if (needs_av1_extension_) store_btn_.Draw(dc, theme, s);
    }

    // Timeline: kept range, keyframe ticks, trim handles, playhead.
    const double dur = clip.duration_s > 0 ? clip.duration_s : (player_ ? player_->Duration() : 0.0);
    const float tcy = paint::Cy(timeline_rect_);
    const float th = 3.0f * s;
    const D2D1_RECT_F track = D2D1::RectF(timeline_rect_.left, tcy - th, timeline_rect_.right, tcy + th);
    p.Fill(track, th, c.HoverLayer(2.0f));
    if (dur > 0.0) {
        const float x_in = TimelineX(trim_in_), x_out = TimelineX(trim_out_);
        p.Fill(D2D1::RectF(x_in, track.top, x_out, track.bottom), 0.0f, c.accent_line);
        float last_x = -1e9f;
        for (double k : clip.keyframes_s) {
            const float x = TimelineX(k);
            if (x - last_x < 6.0f * s) continue;
            p.Line(x, track.bottom + th, x, track.bottom + th + z.space_xs() * s, c.text_off, 1.0f);
            last_x = x;
        }
        const float hw = z.space_xs() * s;
        for (float hx : {x_in, x_out}) {
            const D2D1_RECT_F h = D2D1::RectF(hx - hw, timeline_rect_.top + hw, hx + hw, timeline_rect_.bottom - hw);
            p.Fill(h, z.radius_inner() * s, c.accent);
            p.Line(hx, h.top + 2.0f * hw, hx, h.bottom - 2.0f * hw, c.on_accent, 1.0f);
        }
        const double pos = player_ ? player_->Position() : 0.0;
        p.Circle(TimelineX(pos), tcy, 6.0f * s, c.text);
    }

    // Transport and actions.
    play_btn_.icon = player_ && player_->Playing() ? IconType::Pause : IconType::Play;
    play_btn_.enabled = player_ && st == ClipPlayer::State::Ready;
    play_btn_.Draw(dc, theme, s);

    // Volume: one level for every track (the engine has no per-track volume).
    volume_.value = player_ ? player_->Volume() : 1.0;
    volume_.muted = player_ && player_->Muted();
    volume_.enabled = play_btn_.enabled;
    volume_.Draw(dc, theme, s);

    // Paused, the readout shows hundredths: a frame step has to visibly move it.
    const bool paused = player_ && !player_->Playing() && fps_ > 0.0;
    const double pos = paused ? CurrentFrame() / fps_ : (player_ ? player_->Position() : 0.0);
    p.Text((paused ? FormatPrecise(pos) : FormatDuration(pos)) + L" / " + FormatDuration(dur), TypeRole::Body,
           D2D1::RectF(volume_.bounds.right + z.space_md() * s, play_btn_.bounds.top, save_split_.bounds.left, play_btn_.bounds.bottom),
           c.text);

    for (const auto& chip : track_chips_) {
        chip->enabled = play_btn_.enabled;
        chip->Draw(dc, theme, s);
    }
    speed_select_.enabled = play_btn_.enabled;
    speed_select_.Draw(dc, theme, s);

    const bool exporting = export_.active;
    save_split_.enabled = (Trimmed() || TracksOff()) && !busy_ && !exporting;
    copy_btn_.enabled = !busy_ && !exporting;
    delete_btn_.enabled = !exporting;
    save_split_.visible = copy_btn_.visible = !exporting;
    cancel_export_btn_.visible = exporting;
    cancel_export_btn_.enabled = exporting && !export_.finishing;
    const uint32_t pct = export_.total ? export_.done * 100 / export_.total : 0;
    if (exporting) {
        // Save becomes a bar that fills as the frames are encoded; timeline fill read as playback
        // and shrank with the kept range.
        const D2D1_RECT_F bar = D2D1::RectF(save_split_.bounds.left, save_split_.bounds.top,
                                            cancel_export_btn_.bounds.left - z.space_sm() * s, save_split_.bounds.bottom);
        const float r = z.radius_inner() * s;
        p.Fill(bar, r, c.accent_soft);
        const float fill_x = bar.left + (bar.right - bar.left) * (export_.total ? static_cast<float>(export_.done) / export_.total : 0.0f);
        if (export_.total) p.Fill(D2D1::RectF(bar.left, bar.top, fill_x, bar.bottom), r, c.accent);
        const std::wstring what = export_.finishing ? std::wstring(L"Finishing…")
                                                    : (export_.replace ? L"Replacing " : L"Saving ") + std::to_wstring(pct) + L"%";
        // The label in on_accent over the fill and in text over the rest, so it reads across the edge.
        dc->PushAxisAlignedClip(D2D1::RectF(bar.left, bar.top, fill_x, bar.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        p.Text(what, TypeRole::Label, bar, c.on_accent, {TextAlign::Center});
        dc->PopAxisAlignedClip();
        dc->PushAxisAlignedClip(D2D1::RectF(fill_x, bar.top, bar.right, bar.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        p.Text(what, TypeRole::Label, bar, c.text, {TextAlign::Center});
        dc->PopAxisAlignedClip();
        for (ButtonControl* b : {&show_btn_, &delete_btn_, &cancel_export_btn_}) b->Draw(dc, theme, s);
    } else {
        save_split_.Draw(dc, theme, s);
        for (ButtonControl* b : {&copy_btn_, &show_btn_, &delete_btn_}) b->Draw(dc, theme, s);
    }

    // What a save or copy will contain, then (right) the last action's result.
    const float y = play_btn_.bounds.bottom + z.space_sm() * s;
    const float cb = y + LineHeight(TypeRole::Caption) * s;
    std::wstring range = Trimmed() ? L"Keeps " + FormatDuration(trim_in_) + L" – " + FormatDuration(trim_out_) + L"  (" +
                                         FormatDuration(trim_out_ - trim_in_) + L")"
                                   : L"Drag the ends of the timeline to trim";
    if (!exporting && NeedsReencode())
        range += L"  ·  starts between keyframes: saving re-encodes";
    p.Text(range, TypeRole::Caption, D2D1::RectF(bounds_.left, y, bounds_.right, cb), c.text3);
    {  // the card is as tall as its rows
        keys_card_.bottom = KeysCardFlow(dc, theme, s, false);
        const float r = z.radius_panel() * s;
        p.Fill(keys_card_, r, c.panel);
        p.CardEdge(keys_card_, r);
        KeysCardFlow(dc, theme, s, true);
    }
    if (TracksOff()) {
        const float x = bounds_.left + p.TextW(range, TypeRole::Caption) + z.space_lg() * s;
        p.Text(L"Saves and copies leave out: " + OffTrackNames(), TypeRole::Caption, D2D1::RectF(x, y, bounds_.right, cb), c.warning);
    }
    if (!status_.empty() && motion::NowMs() < status_until_) {
        const float tw = p.TextW(status_, TypeRole::Caption);
        p.Text(status_, TypeRole::Caption, D2D1::RectF(bounds_.right - tw - 1.0f, y, bounds_.right, cb), c.text2);
    }
}

float ClipsScreen::TimelineX(double seconds) const {
    const double dur = selected_ >= 0 ? clips_[selected_].duration_s : 0.0;
    if (dur <= 0.0) return timeline_rect_.left;
    const double f = std::clamp(seconds / dur, 0.0, 1.0);
    return timeline_rect_.left + static_cast<float>(f) * (timeline_rect_.right - timeline_rect_.left);
}

double ClipsScreen::TimelineTime(float x) const {
    const double dur = selected_ >= 0 ? clips_[selected_].duration_s : 0.0;
    const float w = timeline_rect_.right - timeline_rect_.left;
    if (dur <= 0.0 || w <= 0.0f) return 0.0;
    return std::clamp(static_cast<double>((x - timeline_rect_.left) / w), 0.0, 1.0) * dur;
}

// ---- input ---------------------------------------------------------------------------------

ClipsScreen::Hit ClipsScreen::HitAt(D2D1_POINT_2F pt) const {
    Hit h;
    if (selected_ < 0) {
        if (!PointIn(grid_rect_, pt)) return h;
        const D2D1_POINT_2F p = D2D1::Point2F(pt.x, pt.y + scroll_y_);
        for (size_t i = 0; i < tile_rects_.size(); ++i) {
            if (PointIn(tile_rects_[i], p)) return {Hit::Tile, static_cast<int>(i)};
        }
        return h;
    }
    const float grab = 10.0f * s_;
    if (pt.y >= timeline_rect_.top && pt.y <= timeline_rect_.bottom) {
        if (std::abs(pt.x - TimelineX(trim_in_)) <= grab) return {Hit::InHandle, 0};
        if (std::abs(pt.x - TimelineX(trim_out_)) <= grab) return {Hit::OutHandle, 0};
        if (pt.x >= timeline_rect_.left - grab && pt.x <= timeline_rect_.right + grab) return {Hit::Timeline, 0};
    }
    if (PointIn(video_rect_, pt)) return {Hit::Video, 0};
    return h;
}

bool ClipsScreen::OnMouseMove(D2D1_POINT_2F pt) {
    bool rep = false;
    for (Control* b : Buttons()) rep |= b->OnMouseMove(pt);
    if (selected_ >= 0 && drag_ != Drag::None) {
        const double t = TimelineTime(pt.x);
        const double dur = clips_[selected_].duration_s;
        const double min_len = 0.5;
        if (drag_ == Drag::Scrub) {
            SeekTo(t);
        } else if (drag_ == Drag::In) {
            // Any frame; a keyframe within a few pixels pulls the handle onto it, since a start
            // there saves as a stream copy instead of re-encoding.
            double k = fps_ > 0.0 ? std::floor(t * fps_ + 0.5) / fps_ : t;
            const double key = SnapToKeyframe(t);
            if (std::abs(TimelineX(key) - pt.x) <= 6.0f * s_) k = key;
            k = std::clamp(k, 0.0, (std::max)(0.0, trim_out_ - min_len));
            if (k != trim_in_) { trim_in_ = k; SeekTo(trim_in_); }
        } else if (drag_ == Drag::Out) {
            trim_out_ = std::clamp(t, trim_in_ + min_len, dur);
            SeekTo(trim_out_);
        }
        if (player_) player_->SetLoop(Trimmed() ? trim_in_ : 0.0, Trimmed() ? trim_out_ : 0.0);
        return true;
    }
    if (selected_ >= 0) {
        int kh = -1;
        for (size_t i = 0; i < key_row_rects_.size(); ++i) if (PointIn(key_row_rects_[i], pt)) kh = static_cast<int>(i);
        rep |= kh != key_hover_;
        key_hover_ = kh;
    }
    const Hit h = HitAt(pt);
    const int tile = h.kind == Hit::Tile ? h.index : -1;
    rep |= tile != hover_tile_;
    hover_tile_ = tile;
    return rep;
}

CursorKind ClipsScreen::CursorAt(D2D1_POINT_2F pt) const {
    if (drag_ == Drag::In || drag_ == Drag::Out) return CursorKind::Move;
    for (Control* b : const_cast<ClipsScreen*>(this)->Buttons()) {
        if (b->visible && b->CursorAt(pt) != CursorKind::Arrow) return b->CursorAt(pt);
    }
    switch (HitAt(pt).kind) {
        case Hit::Tile: case Hit::Timeline: case Hit::Video:
            return CursorKind::Hand;
        case Hit::InHandle: case Hit::OutHandle: return CursorKind::Move;
        default: return CursorKind::Arrow;
    }
}

bool ClipsScreen::OnMouseDown(D2D1_POINT_2F pt) {
    if (selected_ < 0) {
        for (Control* b : Buttons()) {
            if (b->OnMouseDown(pt)) return true;
        }
        const Hit h = HitAt(pt);
        if (h.kind == Hit::Tile) {
            const std::vector<int> vis = Visible();
            if (h.index < static_cast<int>(vis.size())) OpenClip(vis[h.index]);
            return true;
        }
        return false;
    }
    for (Control* b : Buttons()) {
        if (b->OnMouseDown(pt)) return true;
    }
    for (size_t i = 0; i < key_row_rects_.size(); ++i) {
        if (PointIn(key_row_rects_[i], pt)) {
            RunAction(KeyRows()[i].action);
            return true;
        }
    }
    if (!player_ || player_->GetState() != ClipPlayer::State::Ready) return false;
    const Hit h = HitAt(pt);
    switch (h.kind) {
        case Hit::InHandle: drag_ = Drag::In; break;
        case Hit::OutHandle: drag_ = Drag::Out; break;
        case Hit::Timeline:
            drag_ = Drag::Scrub;
            resume_after_scrub_ = player_->Playing();
            player_->Pause();
            SeekTo(TimelineTime(pt.x));
            break;
        case Hit::Video: TogglePlay(); return true;
        default: return false;
    }
    if (drag_ == Drag::In || drag_ == Drag::Out) player_->Pause();
    if (host_) host_->RequestRepaint();
    return true;
}

bool ClipsScreen::OnMouseUp(D2D1_POINT_2F pt) {
    bool released = false;
    for (Control* b : Buttons()) released |= b->OnMouseUp(pt);
    if (drag_ == Drag::None) {
        if (released && host_) host_->RequestRepaint();
        return released;
    }
    if (drag_ == Drag::Scrub && resume_after_scrub_ && player_) player_->Play();
    if ((drag_ == Drag::In || drag_ == Drag::Out) && player_) SeekTo(trim_in_);
    drag_ = Drag::None;
    if (host_) host_->RequestRepaint();
    return true;
}

bool ClipsScreen::OnKeyDown(UINT vk) {
    if (selected_ < 0) {
        if (vk == VK_ESCAPE) {
            if (host_) host_->CloseOverlay();
            return true;
        }
        return false;
    }
    for (const ClipShortcut& k : kClipShortcuts) {
        if (k.vk == vk) {
            RunAction(k.action);
            return true;
        }
    }
    return false;
}

void ClipsScreen::OnMouseWheel(float delta) {
    if (selected_ >= 0) return;
    scroll_y_ -= delta * 120.0f * s_;  // `delta` is in notches: about half a tile row each
    const float view_h = grid_rect_.bottom - grid_rect_.top;
    scroll_y_ = std::clamp(scroll_y_, 0.0f, (std::max)(0.0f, content_h_ - view_h));
    if (host_) host_->RequestRepaint();
}

bool ClipsScreen::OnFrameTimer() {
    if (selected_ < 0 || !player_) return false;
    const UINT w = static_cast<UINT>(video_rect_.right - video_rect_.left);
    const UINT h = static_cast<UINT>(video_rect_.bottom - video_rect_.top);
    // Tick runs once per display refresh; repaint only for a new video frame (the time readout and
    // playhead move with it), not every refresh: 120 renders/s for a 30 fps clip on a 120 Hz panel.
    bool changed = player_->Tick(w, h);
    if (player_->GetState() == ClipPlayer::State::Ready) {
        ClipEntry& c = clips_[selected_];
        // A clip whose details hadn't loaded yet when it was opened: take the engine's duration.
        if (c.duration_s <= 0.0 && player_->Duration() > 0.0) {
            c.duration_s = player_->Duration();
            trim_out_ = c.duration_s;
            changed = true;
        }
    }
    if (!status_.empty() && motion::NowMs() >= status_until_) {
        status_.clear();
        changed = true;
    }
    return changed;
}

}  // namespace ombra_ui
