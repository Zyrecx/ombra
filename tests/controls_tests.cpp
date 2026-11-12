// Controls (ui/controls.h): behaviour that doesn't need a device.
#include <doctest/doctest.h>

#include "ombra-ui/ui/controls.h"

#include <windows.h>

using namespace ombra_ui;

namespace {

TemplateFieldControl NameField(const std::wstring& text) {
    TemplateFieldControl f;
    f.tokens = {{L"{game}", L"Game", L"Valorant"}, {L"{date}", L"Date", L"2026-10-29"},
                {L"{time}", L"Time", L"00-21-33"}, {L"{monitor_suffix}", L"Monitor", L""}};
    f.text = text;
    f.focused = true;
    return f;
}

}  // namespace

TEST_CASE("template field: known placeholders are chips, unknown ones stay text") {
    auto f = NameField(L"{game} {foo} {date}{monitor_suffix}");
    const auto chips = f.Chips();
    REQUIRE(chips.size() == 3);
    CHECK(chips[0].token == 0);
    CHECK(chips[1].pos == 13);
    CHECK(chips[2].token == 3);
    CHECK(f.Preview() == L"Valorant {foo} 2026-10-29.mp4");
}

TEST_CASE("template field: the caret and Backspace treat a chip as one character") {
    auto f = NameField(L"a{game}b");
    f.SetCaret(4);  // inside {game}: snapped past it
    CHECK(f.Caret() == 7);
    f.OnKeyDown(VK_LEFT);
    CHECK(f.Caret() == 1);  // one step jumps the whole chip
    f.OnKeyDown(VK_RIGHT);
    CHECK(f.Caret() == 7);
    f.OnKeyDown(VK_BACK);
    CHECK(f.text == L"ab");
    CHECK(f.Caret() == 1);
    f.SetCaret(1);
    f.text = L"a{date}b";
    f.OnKeyDown(VK_DELETE);
    CHECK(f.text == L"ab");
}

TEST_CASE("template field: typing the closing brace makes a chip; inserting puts one at the caret") {
    auto f = NameField(L"x");
    std::wstring last;
    f.on_change = [&](const std::wstring& t) { last = t; };
    f.SetCaret(0);
    for (wchar_t ch : std::wstring(L"{time}")) f.OnChar(ch);
    CHECK(f.text == L"{time}x");
    CHECK(f.Chips().size() == 1);
    CHECK(f.Caret() == 6);
    f.InsertToken(0);
    CHECK(f.text == L"{time}{game}x");
    CHECK(last == f.text);
    f.selected_chip = 0;  // a clicked chip: Backspace removes it, wherever the caret is
    f.OnKeyDown(VK_BACK);
    CHECK(f.text == L"{game}x");
}

TEST_CASE("template field: refused characters, the empty fallback and a typed .mp4 in the preview") {
    auto f = NameField(L"a");
    f.accept_char = [](wchar_t c) { return c != L':'; };
    f.SetCaret(1);
    CHECK(f.OnChar(L':'));  // consumed, not typed
    CHECK(f.text == L"a");
    f.text = L"{game}.MP4";
    CHECK(f.Preview() == L"Valorant.mp4");  // the engine swaps the extension, it doesn't add a second
    f.text.clear();
    f.fallback = L"{game} {date}";
    CHECK(f.Preview() == L"Valorant 2026-10-29.mp4");
}

TEST_CASE("colour and position pickers: hit tests follow density and DPI") {
    for (Density d : {Density::Compact, Density::Default, Density::Comfortable}) {
        for (float s : {1.0f, 1.5f}) {
            CAPTURE(static_cast<int>(d));
            CAPTURE(s);
            Theme t;
            t.size.density = DensityScale(d);
            const LayoutContext ctx{s, &t};
            const Sizes& z = t.size;

            ColorPickerControl cp;
            cp.presets = {0xeeeeee, 0xa78bfa, 0x22d3ee, 0x5b9bff, 0x76b900};
            const D2D1_SIZE_F m = cp.Measure(LayoutConstraints{}, ctx);
            cp.Arrange(D2D1::RectF(100, 50, 100 + m.width, 50 + m.height), ctx);
            const float step = (z.control_xs() + z.space_sm()) * s, left = 100 + z.space_xs() * s;
            const float cy = 50 + (z.space_xs() + z.control_xs() / 2) * s;
            for (int i = 0; i < 5; ++i) {
                cp.OnMouseMove(D2D1::Point2F(left + i * step + z.control_xs() / 2 * s, cy));
                CHECK(cp.hovered_index == i);
            }
            const float custom_x = left + 5 * step + z.space_sm() * s + z.control_xs() / 2 * s;
            cp.OnMouseMove(D2D1::Point2F(custom_x, cy));
            CHECK(cp.hovered_index == 5);
            cp.OnMouseMove(D2D1::Point2F(custom_x + z.control_xs() * s, cy));  // the hex text
            CHECK(cp.hovered_index == -1);

            PositionPickerControl pp;
            pp.spots = {{0.18f, 0.14f}, {0.82f, 0.14f}, {0.18f, 0.86f}, {0.82f, 0.86f}};
            const D2D1_SIZE_F pm = pp.Measure(LayoutConstraints{}, ctx);
            pp.Arrange(D2D1::RectF(10, 10, 10 + pm.width, 10 + pm.height), ctx);
            const float mw = pm.width, mh = pm.width * 9 / 16;
            for (int i = 0; i < 4; ++i) {
                pp.OnMouseMove(D2D1::Point2F(10 + mw * pp.spots[i].fx, 10 + mh * pp.spots[i].fy));
                CHECK(pp.hovered_index == i);
            }
            pp.OnMouseMove(D2D1::Point2F(10 + mw / 2, 10 + mh / 2));  // the middle of the screen
            CHECK(pp.hovered_index == -1);
        }
    }
}
