// The real WGC Capture's readback paths: the screenshot path (stage texture -> ReadSlotRegionBGRA
// -> PNG) and the Settings pickers' one-shot thumbnails. Captures the primary monitor and a window
// that is already open; draws nothing and never changes focus.
//
// "gpu" suite: runs only with `ombra-tests --gpu`.

#include <doctest/doctest.h>

#include "support/test_env.h"

#include "ombra/capture.h"
#include "ombra/encode_target.h"
#include "ombra/screenshot.h"
#include "ombra/util.h"

#include <winrt/base.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace ombra::test {

namespace {

void CheckPngSize(const std::wstring& path, uint32_t width, uint32_t height) {
    uint32_t             decoded_w = 0, decoded_h = 0;
    std::vector<uint8_t> decoded;
    REQUIRE(ReadPngAsBgra(path, decoded_w, decoded_h, decoded));
    CHECK(decoded_w == width);
    CHECK(decoded_h == height);
}

}  // namespace

// This runs on the pacer: a refused readback must come back empty, not die() and take the engine.
// A zero-size staging texture is a refusal any device gives; a real device reset takes the same path.
TEST_CASE("gpu: a readback the GPU refuses comes back empty" * doctest::test_suite("gpu")) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto adapter = ResolveEncodeAdapter("auto");
    REQUIRE(adapter.has_value());
    Capture capture;
    REQUIRE(capture.Open(/*capture_cursor=*/true, CaptureTarget{}, adapter->luid));

    std::vector<uint8_t> bgra(16, 0xAB);
    uint32_t             row_pitch = 123;
    capture.ReadSlotRegionBGRA(0, 0, 0, 0, 0, bgra, row_pitch);
    CHECK(bgra.empty());
    CHECK(row_pitch == 0);

    // And the next request still works.
    capture.ReadSlotRegionBGRA(0, 0, 0, 16, 16, bgra, row_pitch);
    CHECK_FALSE(bgra.empty());
    capture.ReleaseReadbackStaging();
    capture.Close();
}

TEST_CASE("gpu: a frame-pool recreate exception returns false and the injection is one-shot" *
          doctest::test_suite("gpu")) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto adapter = ResolveEncodeAdapter("auto");
    REQUIRE(adapter.has_value());
    Capture capture;
    REQUIRE(capture.Open(true, CaptureTarget{}, adapter->luid));
    capture.TestFailNextFramePoolRecreate();
    CHECK_FALSE(capture.Recreate(640, 360));  // an escaped exception fails the test
    CHECK_FALSE(capture.HasFrame());
    CHECK(capture.Recreate(640, 360));
    CHECK(capture.Width() == 640);
    CHECK(capture.Height() == 360);
    capture.Close();
}

// A colour whose channels are all distinct and none 0 or 255, so an R/B swap or a clamp shows up
// as a wrong value rather than a plausible one.
TEST_CASE("gpu: a screenshot of the stage texture writes a PNG that decodes to the same colour" *
          doctest::test_suite("gpu")) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto adapter = ResolveEncodeAdapter("auto");
    REQUIRE(adapter.has_value());
    Capture capture;
    REQUIRE(capture.Open(/*capture_cursor=*/true, CaptureTarget{}, adapter->luid));

    const float rgba[4] = {0.20f, 0.55f, 0.85f, 1.0f};
    capture.FillStageColor(rgba);
    std::vector<uint8_t> bgra;
    uint32_t             row_pitch = 0;
    capture.ReadSlotRegionBGRA(0, 0, 0, capture.Width(), capture.Height(), bgra, row_pitch);
    capture.ReleaseReadbackStaging();
    const uint32_t width = capture.Width(), height = capture.Height();
    capture.Close();
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    REQUIRE_FALSE(bgra.empty());

    const std::wstring path = TestOutputPath(L"capture\\shot.png");
    REQUIRE(EnsureParentDir(path));
    REQUIRE(WriteBgraPng(path, width, height, row_pitch, bgra));

    uint32_t             decoded_w = 0, decoded_h = 0;
    std::vector<uint8_t> decoded;
    REQUIRE(ReadPngAsBgra(path, decoded_w, decoded_h, decoded));
    REQUIRE(decoded_w == width);
    REQUIRE(decoded_h == height);
    const size_t px = (size_t(height / 2) * width + width / 2) * 4;
    // 8-bit quantisation of the float fill, not a colour error.
    CHECK(std::abs(int(decoded[px + 2]) - int(rgba[0] * 255.0f + 0.5f)) <= 2);
    CHECK(std::abs(int(decoded[px + 1]) - int(rgba[1] * 255.0f + 0.5f)) <= 2);
    CHECK(std::abs(int(decoded[px + 0]) - int(rgba[2] * 255.0f + 0.5f)) <= 2);
    CHECK(decoded[px + 3] == 255);  // forced opaque
}

TEST_CASE("gpu: a monitor thumbnail is a PNG no larger than the requested size" *
          doctest::test_suite("gpu")) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const std::wstring    path = TestOutputPath(L"capture\\monitor-thumb.png");
    REQUIRE(EnsureParentDir(path));
    const ThumbnailResult r = CaptureMonitorThumbnailToPng("primary", 320, path);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.width <= 320);
    CHECK(r.height <= 320);
    CHECK((r.width == 320 || r.height == 320));
    CheckPngSize(path, r.width, r.height);
}

// Whatever capturable window is open and not minimized: the test opens none of its own.
TEST_CASE("gpu: a window thumbnail of an open window is a PNG no larger than the requested size" *
          doctest::test_suite("gpu")) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const WindowInfo* target = nullptr;
    const auto        windows = EnumerateCapturableWindows();
    for (const WindowInfo& w : windows)
        if (!w.minimized && w.hwnd) {
            target = &w;
            break;
        }
    if (!target) {
        MESSAGE("no open, unminimized window to capture: skipped");
        return;
    }
    MESSAGE("window: " << target->process);

    const std::wstring    path = TestOutputPath(L"capture\\window-thumb.png");
    REQUIRE(EnsureParentDir(path));
    const ThumbnailResult r =
        CaptureWindowThumbnailToPng(reinterpret_cast<HWND>(static_cast<uintptr_t>(target->hwnd)), 320, path);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.width <= 320);
    CHECK(r.height <= 320);
    CheckPngSize(path, r.width, r.height);
}

}  // namespace ombra::test
