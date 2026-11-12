// The Clips preview's player (clip_player.cpp) on a real Media Foundation engine, on the hardware
// adapter (WARP has no video decode), so it runs with --gpu. The clip is ffmpeg's, with two audio
// tracks in one alternate group like a save's Desktop + Mic, and no B-frames like a save (with
// them, a seek right after loading didn't land in this test).

#include <doctest/doctest.h>

#include "support/process.h"
#include "support/test_env.h"

#include "ombra-ui/clip_player.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>

namespace ombra::test {

namespace {

using Microsoft::WRL::ComPtr;

// Ticks the player like the overlay's frame timer for `ms`.
void Pump(ombra_ui::ClipPlayer& p, int ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        p.Tick(320, 180);
        Sleep(8);
    }
}

}  // namespace

TEST_CASE("clip player: the first Play after a seek starts there, not over from 0" * doctest::test_suite("gpu")) {
    const std::wstring clip = TestOutputPath(L"clip_player\\two_tracks.mp4");
    std::filesystem::create_directories(std::filesystem::path(clip).parent_path());
    const ProcessResult made = RunProcess(
        L"ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=30:duration=12 "
        L"-f lavfi -i sine=frequency=440:duration=12 -f lavfi -i sine=frequency=660:duration=12 "
        L"-map 0:v -map 1:a -map 2:a -c:v libx264 -bf 0 -g 30 -pix_fmt yuv420p -c:a aac " + Quote(clip));
    REQUIRE(made.exit_code == 0);

    ComPtr<ID3D11Device> device;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    ombra_ui::ClipPlayer player;
    REQUIRE(player.Init(device.Get()));
    player.SetMuted(true);
    player.Open(clip);
    for (int i = 0; i < 300 && player.GetState() == ombra_ui::ClipPlayer::State::Loading; ++i) Pump(player, 16);
    REQUIRE(player.GetState() == ombra_ui::ClipPlayer::State::Ready);
    CHECK(player.AudioTrackCount() == 2);
    Pump(player, 1500);  // a person looks before seeking

    // Selecting every audio track takes effect on the first Play, which restarted the clip from 0.
    player.Seek(7.0);
    Pump(player, 300);
    REQUIRE(player.Position() > 6.9);
    player.Play();
    Pump(player, 500);
    CHECK(player.Position() > 6.5);

    // A track switched off while paused applies the same way.
    player.Pause();
    player.Seek(3.0);
    Pump(player, 300);
    player.SetAudioTrackEnabled(1, false);
    player.Play();
    Pump(player, 500);
    CHECK(player.Position() > 2.5);
    player.Close();
}

}  // namespace ombra::test
