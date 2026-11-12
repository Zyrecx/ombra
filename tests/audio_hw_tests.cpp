// Real WASAPI: AudioCapture against the default output's loopback, alone and two at once into one
// AudioTrackHub muxed to MP4. No GPU and nothing on screen, but it opens the real audio
// device, so the "audio-hw" suite runs only with `ombra-tests --audio` (or OMBRA_TEST_AUDIO).
//
// Loopback delivers nothing while no program plays audio, so each test plays digital silence on
// the default output for its duration (SilentRender): inaudible, and it keeps the loopback stream
// delivering whether or not anything else is playing.

#include <doctest/doctest.h>

#include "support/media_probe.h"
#include "support/test_env.h"

#include "ombra/audio.h"
#include "ombra/audio_tracks.h"
#include "ombra/mux.h"
#include "ombra/util.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ombra::test {

namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

// Zeros written to the default render endpoint in shared mode, refilled every 10 ms.
class SilentRender {
public:
    SilentRender() {
        com_ok_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice>           device;
        WAVEFORMATEX*               format = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_)) ||
            FAILED(client_->GetMixFormat(&format))) {
            return;
        }
        frame_bytes_ = format->nBlockAlign;
        const HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, format, nullptr);
        CoTaskMemFree(format);
        if (FAILED(hr) || FAILED(client_->GetBufferSize(&buffer_frames_)) ||
            FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
            return;
        }
        Fill();
        if (FAILED(client_->Start())) return;
        ok_ = true;
        thread_ = std::thread([this] {
            while (running_.load()) {
                std::this_thread::sleep_for(10ms);
                Fill();
            }
        });
    }

    ~SilentRender() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
        render_.Reset();
        client_.Reset();
        if (com_ok_) CoUninitialize();
    }

    bool ok() const { return ok_; }

private:
    void Fill() {
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return;
        const UINT32 frames = buffer_frames_ - padding;
        BYTE*        data   = nullptr;
        if (frames == 0 || FAILED(render_->GetBuffer(frames, &data))) return;
        std::memset(data, 0, size_t(frames) * frame_bytes_);
        render_->ReleaseBuffer(frames, 0);
    }

    bool                       com_ok_ = false;
    bool                       ok_     = false;
    ComPtr<IAudioClient>       client_;
    ComPtr<IAudioRenderClient> render_;
    UINT32                     buffer_frames_ = 0;
    UINT32                     frame_bytes_   = 0;
    std::atomic<bool>          running_{true};
    std::thread                thread_;
};

std::vector<uint8_t> FakeSpsPps() {
    return {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1E, 0x8B, 0x40, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80};
}

std::vector<MuxAu> FakeAus(int frame_count, int gop_size) {
    std::vector<MuxAu> aus;
    for (int i = 0; i < frame_count; ++i) {
        MuxAu au;
        au.is_keyframe = (i % gop_size) == 0;
        au.tick_index  = static_cast<uint64_t>(i);
        au.annexb = {0, 0, 0, 1, uint8_t(au.is_keyframe ? 0x65 : 0x41), uint8_t(i), uint8_t(i >> 8)};
        aus.push_back(std::move(au));
    }
    return aus;
}

}  // namespace

// The loopback path end to end: the device's mix format negotiated down to 48 kHz stereo s16, and
// packets stamped on QPC whose span agrees with the frames delivered (a drifting resampler or a
// mis-converted timestamp shows up as the two durations parting).
TEST_CASE("audio-hw: a desktop loopback capture delivers 48 kHz stereo on QPC without drift" *
          doctest::test_suite("audio-hw")) {
    SilentRender silence;
    REQUIRE_MESSAGE(silence.ok(), "could not play silence on the default output");

    std::mutex mutex;
    uint64_t   packets = 0, discontinuities = 0, frames = 0, last_frames = 0;
    int64_t    first_qpc = 0, last_qpc = 0;
    AudioCapture capture;
    capture.Start([&](const PcmPacket& p) {
        std::lock_guard<std::mutex> lock(mutex);
        if (packets == 0) first_qpc = p.qpc_ticks;
        ++packets;
        if (p.discontinuity) ++discontinuities;
        last_frames = p.samples.size() / AudioCapture::kChannels;
        frames += last_frames;
        last_qpc = p.qpc_ticks;
    });
    std::this_thread::sleep_for(3000ms);
    capture.Stop();

    const NegotiatedFormat& fmt = capture.Negotiated();
    const std::string sample_kind = std::string(fmt.is_float ? "float" : "int") +
                                    (fmt.was_resampled ? ", resampled" : "");
    MESSAGE("device format " << fmt.sample_rate_hz << " Hz, " << fmt.channels << " ch, "
                             << fmt.bits_per_sample << "-bit " << sample_kind);
    CHECK(fmt.sample_rate_hz > 0);

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(packets > 0);
    const double by_frames = double(frames) / AudioCapture::kSampleRateHz;
    const double by_qpc    = double(last_qpc - first_qpc) / QpcFrequency() +
                          double(last_frames) / AudioCapture::kSampleRateHz;
    MESSAGE(packets << " packets, " << discontinuities << " discontinuities, " << by_frames
                    << " s by frame count, " << by_qpc << " s by QPC");
    CHECK(by_frames > 2.0);
    CHECK(std::abs(by_frames - by_qpc) < 0.020);
    CHECK(discontinuities <= 1);  // the stream's first packet may carry the flag
}

// Two real captures on one endpoint at once, each its own hub slot and AAC track, muxed to both
// containers; ffprobe reads the tracks back by name and each decodes to about the captured length.
TEST_CASE("audio-hw: two loopback captures feed one hub and mux to two named AAC tracks" *
          doctest::test_suite("audio-hw")) {
    SilentRender silence;
    REQUIRE_MESSAGE(silence.ok(), "could not play silence on the default output");

    const int64_t start_qpc = QpcNow();
    AudioTrackHub hub(start_qpc, QpcFrequency(), 192, AudioTrackHub::SizeFor(5.0, 192, 0));
    const std::vector<size_t> slots =
        hub.SetSources({{"Desktop", "desktop"}, {"Desktop2", "desktop2"}}, /*full_mix=*/false);
    REQUIRE(slots.size() == 2);

    AudioCapture          captures[2];
    std::atomic<uint64_t> packets[2] = {};
    for (size_t i = 0; i < 2; ++i) {
        captures[i].Start([&, i](const PcmPacket& p) {
            hub.Append(slots[i], p.samples.data(), p.samples.size(), p.qpc_ticks);
            packets[i].fetch_add(1);
        });
    }
    std::this_thread::sleep_for(2000ms);
    const int64_t end_qpc = QpcNow();
    for (auto& c : captures) c.Stop();
    CHECK(packets[0].load() > 0);
    CHECK(packets[1].load() > 0);

    const std::vector<MuxAudioTrack> tracks = hub.Collect(start_qpc, end_qpc);
    REQUIRE(tracks.size() == 2);

    const std::wstring mp4 = TestOutputPath(L"audio-hw\\two-loopbacks.mp4");
    REQUIRE(EnsureParentDir(mp4));
    const std::vector<MuxAu> aus = FakeAus(60, 30);
    REQUIRE(MuxWriteMp4(mp4, FakeSpsPps(), 1920, 1080, 30, aus, tracks, VideoCodec::H264));

    for (const std::wstring& file : {mp4}) {
        CAPTURE(NarrowUtf8(file));
        const ProbeInfo probe = Probe(file);
        REQUIRE_MESSAGE(probe.ok, probe.error);
        const auto audio = probe.Of("audio");
        REQUIRE(audio.size() == 2);
        CHECK(audio[0].title == "Desktop");
        CHECK(audio[1].title == "Desktop2");
        for (int i = 0; i < 2; ++i) {
            CHECK(audio[i].codec_name == "aac");
            const Pcm pcm = DecodeAudio(file, i);
            MESSAGE("track " << i << ": " << pcm.Seconds() << " s decoded");
            CHECK(pcm.Seconds() > 1.5);
            CHECK(pcm.Seconds() < 2.5);
        }
    }
}

}  // namespace ombra::test
