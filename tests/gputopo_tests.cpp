// PickEncodeAdapter and ClassifyForEncoder: pure, headless, no GPU. AdapterForMonitor: read-only DXGI.

#include <doctest/doctest.h>

#include "ombra/config.h"
#include "ombra/encode_target.h"
#include "ombra/gputopo.h"
#include "ombra-ui/monitor_adapter.h"

#include <algorithm>

namespace ombra::test {

namespace {

AdapterFacts Adapter(UINT vendor, DWORD luid_low, const wchar_t* name, bool software = false,
                     bool has_output = false) {
    AdapterFacts a;
    a.vendor_id     = vendor;
    a.luid.LowPart  = luid_low;
    a.description   = name;
    a.software      = software;
    if (has_output) a.output_devices.push_back(L"\\\\.\\DISPLAY1");
    return a;
}

// What a passed AMF probe reports for one adapter.
AmfAdapterCaps Amf(DWORD luid_low, bool av1 = false) {
    AmfAdapterCaps c;
    c.luid.LowPart = luid_low;
    c.h264         = true;
    c.av1          = av1;
    if (av1) c.av1_align_w = 64, c.av1_align_h = 16;
    return c;
}

}  // namespace

TEST_CASE("PickEncodeAdapter: auto with no recorded monitor prefers NVIDIA, then AMD once AMF passed its probe") {
    const std::vector<AdapterFacts> hybrid = {Adapter(kVendorNvidia, 1, L"RTX"),
                                              Adapter(kVendorAmd, 2, L"780M"),
                                              Adapter(0x1414, 3, L"WARP", /*software=*/true)};
    auto picked = PickEncodeAdapter(hybrid, "auto", {Amf(2)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
    CHECK(picked->luid.LowPart == 1);

    const std::vector<AdapterFacts> amd_only = {Adapter(kVendorAmd, 2, L"780M")};
    picked = PickEncodeAdapter(amd_only, "auto", {Amf(2)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    // An AMD adapter whose AMF runtime didn't pass the probe is not a hardware encoder: the CPU
    // records, capturing on that adapter.
    picked = PickEncodeAdapter(amd_only, "auto", {});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::X264);
    CHECK(picked->luid.LowPart == 2);
    // A software adapter never hardware-encodes, but the CPU can still record through it.
    picked = PickEncodeAdapter({Adapter(0x1414, 3, L"WARP", true)}, "auto", {Amf(3)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::X264);
    CHECK_FALSE(PickEncodeAdapter({}, "auto", {}));
}

TEST_CASE("PickEncodeAdapter: auto records a game on the NVIDIA GPU it renders on") {
    std::vector<AdapterFacts> panel = {Adapter(kVendorNvidia, 1, L"RTX"),
                                       Adapter(kVendorAmd, 2, L"780M", false, /*has_output=*/true)};
    panel[1].owns_target_monitor = true;
    const LUID rtx = panel[0].luid, igpu = panel[1].luid;

    auto picked = PickEncodeAdapter(panel, "auto", {Amf(2)}, nullptr, &rtx);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
    CHECK(picked->luid.LowPart == 1);
    // A game on the display's own GPU, or no game known: the display's GPU.
    picked = PickEncodeAdapter(panel, "auto", {Amf(2)}, nullptr, &igpu);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    picked = PickEncodeAdapter(panel, "auto", {Amf(2)}, nullptr, nullptr);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    // The RTX left out (its encoder refused): the game's GPU isn't in the list, AMF records.
    picked = PickEncodeAdapter({panel[1]}, "auto", {Amf(2)}, nullptr, &rtx);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    // Explicit choices ignore the game.
    picked = PickEncodeAdapter(panel, "amd", {Amf(2)}, nullptr, &rtx);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    picked = PickEncodeAdapter(panel, "cpu", {Amf(2)}, nullptr, &rtx);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::X264);
    // The RTX driving the display: NVENC either way.
    std::vector<AdapterFacts> external = panel;
    external[0].owns_target_monitor = true;
    external[1].owns_target_monitor = false;
    picked = PickEncodeAdapter(external, "auto", {Amf(2)}, nullptr, &igpu);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
}

TEST_CASE("ParseEngineInstanceLuid reads the LUID out of a GPU Engine instance name") {
    LUID luid{};
    REQUIRE(ParseEngineInstanceLuid(L"\\GPU Engine(pid_1234_luid_0x00000000_0x00013413_phys_0_eng_0_engtype_3D)\\Utilization Percentage", &luid));
    CHECK(luid.HighPart == 0);
    CHECK(luid.LowPart == 0x13413u);
    REQUIRE(ParseEngineInstanceLuid(L"pid_8_luid_0x00000001_0x0000ABCD_phys_0", &luid));
    CHECK(luid.HighPart == 1);
    CHECK(luid.LowPart == 0xABCDu);
    CHECK_FALSE(ParseEngineInstanceLuid(L"pid_8_phys_0", &luid));
}

TEST_CASE("PickEncodeAdapter: auto records on the GPU that drives the recorded monitor when it can encode") {
    // Hybrid laptop on its panel: the 780M owns the monitor, the RTX enumerates first.
    std::vector<AdapterFacts> panel = {Adapter(kVendorNvidia, 1, L"RTX"),
                                       Adapter(kVendorAmd, 2, L"780M", false, /*has_output=*/true),
                                       Adapter(0x1414, 3, L"WARP", true)};
    panel[1].owns_target_monitor = true;
    auto picked = PickEncodeAdapter(panel, "auto", {Amf(2, true)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    CHECK(picked->luid.LowPart == 2);
    CHECK(picked->av1_align_w == 64);
    // Its AMF didn't pass the probe: NVIDIA next, not the CPU on the owner.
    picked = PickEncodeAdapter(panel, "auto", {});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
    // Explicit choices don't follow the display.
    picked = PickEncodeAdapter(panel, "nvidia", {Amf(2)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
    picked = PickEncodeAdapter(panel, "cpu", {Amf(2)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::X264);
    CHECK(picked->luid.LowPart == 2);
    // "nvidia" that isn't there falls back to auto, which follows the display too.
    std::string note;
    picked = PickEncodeAdapter({panel[1]}, "nvidia", {Amf(2)}, &note);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);

    // The external monitor wired to the RTX: NVENC.
    std::vector<AdapterFacts> external = panel;
    external[1].owns_target_monitor = false;
    external[0].owns_target_monitor = true;
    external[0].output_devices.push_back(L"\\\\.\\DISPLAY2");
    picked = PickEncodeAdapter(external, "auto", {Amf(2, true)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
    CHECK(picked->luid.LowPart == 1);

    // A Ryzen iGPU drives the monitor beside a Radeon dGPU: the owner, though PickAmd's own
    // preference (more memory) is the dGPU.
    AdapterFacts igpu = Adapter(kVendorAmd, 1, L"Radeon 780M", false, true);
    AdapterFacts dgpu = Adapter(kVendorAmd, 2, L"Radeon RX 7800M");
    igpu.dedicated_mb        = 512;
    dgpu.dedicated_mb        = 12288;
    igpu.owns_target_monitor = true;
    picked = PickEncodeAdapter({igpu, dgpu}, "auto", {Amf(1, true), Amf(2, true)});
    REQUIRE(picked);
    CHECK(picked->luid.LowPart == 1);
    // A software adapter that owns it (a remote session) is skipped.
    std::vector<AdapterFacts> remote = {Adapter(kVendorNvidia, 1, L"RTX"), Adapter(0x1414, 3, L"WARP", true)};
    remote[1].owns_target_monitor = true;
    picked = PickEncodeAdapter(remote, "auto", {});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
}

TEST_CASE("PickRecordingGpu mirrors auto: the GPU that drives the display, else NVIDIA, AMD, CPU") {
    std::vector<RecordingGpuCaps> gpus = {{"nvidia", true}, {"amd", true, 64, 16, true}, {"cpu"}};
    const RecordingGpuCaps* g = PickRecordingGpu(gpus, "auto");
    REQUIRE(g);
    CHECK(g->vendor == "amd");
    CHECK(PickRecordingGpu(gpus, "nvidia")->vendor == "nvidia");
    // The panel at 1080p with AV1 and the h264 rule: the codec the AMD GPU records, not NVIDIA's AV1.
    CHECK(RecordedCodec(gpus, "auto", "av1", "h264", {1920, 1080}) == VideoCodec::H264);
    CHECK(RecordedCodec(gpus, "nvidia", "av1", "h264", {1920, 1080}) == VideoCodec::AV1);
    gpus[1].drives_display = false;
    CHECK(PickRecordingGpu(gpus, "auto")->vendor == "nvidia");
    // The CPU never counts as the display's GPU.
    gpus = {{"amd", true}, {"cpu", false, 1, 1, true}};
    CHECK(PickRecordingGpu(gpus, "auto")->vendor == "amd");
}

TEST_CASE("PickEncodeAdapter: cpu captures on the adapter that drives a display") {
    // Hybrid laptop: the RTX enumerates first but the panel hangs off the iGPU.
    const std::vector<AdapterFacts> hybrid = {Adapter(kVendorNvidia, 1, L"RTX"),
                                              Adapter(kVendorAmd, 2, L"780M", false, /*has_output=*/true),
                                              Adapter(0x1414, 3, L"WARP", true)};
    auto picked = PickEncodeAdapter(hybrid, "cpu", {Amf(2)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::X264);
    CHECK(picked->luid.LowPart == 2);
    // Both GPUs drive a display (an HDMI port wired to the dGPU): the one owning the captured monitor.
    std::vector<AdapterFacts> both = hybrid;
    both[0].output_devices.push_back(L"\\\\.\\DISPLAY2");
    both[0].owns_target_monitor = true;
    CHECK(MixedOutputOwnership(both));
    CHECK_FALSE(MixedOutputOwnership(hybrid));
    picked = PickCpuEncodeAdapter(both);
    REQUIRE(picked);
    CHECK(picked->luid.LowPart == 1);
    // No adapter reports an output (a headless box): the first hardware one, never WARP over it.
    picked = PickCpuEncodeAdapter({Adapter(0x1414, 3, L"WARP", true), Adapter(kVendorNvidia, 1, L"RTX")});
    REQUIRE(picked);
    CHECK(picked->luid.LowPart == 1);
}

TEST_CASE("PickEncodeAdapter: an explicit vendor wins, and a missing one falls back to auto with a note") {
    const std::vector<AdapterFacts> hybrid = {Adapter(kVendorNvidia, 1, L"RTX"),
                                              Adapter(kVendorAmd, 2, L"780M")};
    std::string note;
    auto picked = PickEncodeAdapter(hybrid, "amd", {Amf(2)}, &note);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    CHECK(note.empty());

    // "amd" with a failed probe: falls back rather than refusing to record.
    picked = PickEncodeAdapter(hybrid, "amd", {}, &note);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Nvenc);
    CHECK(note.find("AMF probe failed") != std::string::npos);

    // "nvidia" after a MUX switch or on an AMD-only machine.
    note.clear();
    picked = PickEncodeAdapter({Adapter(kVendorAmd, 2, L"780M")}, "nvidia", {Amf(2)}, &note);
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    CHECK(note.find("no such adapter") != std::string::npos);
}

TEST_CASE("PickEncodeAdapter: AMF records on an adapter its probe passed, preferring the discrete one") {
    // Ryzen iGPU + Radeon dGPU, the iGPU enumerating first.
    AdapterFacts igpu = Adapter(kVendorAmd, 1, L"Radeon 780M", false, /*has_output=*/true);
    AdapterFacts dgpu = Adapter(kVendorAmd, 2, L"Radeon RX 7800M");
    igpu.dedicated_mb = 512;
    dgpu.dedicated_mb = 12288;
    const std::vector<AdapterFacts> both = {igpu, dgpu};

    // Both passed: the discrete one, with its own AV1 alignment.
    auto picked = PickEncodeAdapter(both, "auto", {Amf(1, true), Amf(2, true)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    CHECK(picked->luid.LowPart == 2);
    CHECK(picked->av1_align_w == 64);
    // Only the iGPU passed (dGPU driver broken): never the adapter the probe didn't clear.
    picked = PickEncodeAdapter(both, "amd", {Amf(1)});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::Amf);
    CHECK(picked->luid.LowPart == 1);
    CHECK(picked->av1_align_w == 1);
    // AV1 outranks memory: the codec default is AV1.
    picked = PickEncodeAdapter(both, "auto", {Amf(1, true), Amf(2, false)});
    REQUIRE(picked);
    CHECK(picked->luid.LowPart == 1);
    // A probe entry without H.264 isn't usable.
    AmfAdapterCaps no_h264 = Amf(2);
    no_h264.h264 = false;
    picked = PickEncodeAdapter(both, "auto", {no_h264});
    REQUIRE(picked);
    CHECK(picked->backend == EncoderBackend::X264);
}

TEST_CASE("ClassifyForEncoder: topology follows the recording adapter, not the vendor") {
    StartupGpuSummary summary;
    summary.encoder_available  = true;
    summary.display_owner_luid = LUID{2, 0};
    CHECK(ClassifyForEncoder(summary, LUID{2, 0}) == Topology::T1);  // e.g. AMF on the panel's iGPU
    CHECK(ClassifyForEncoder(summary, LUID{1, 0}) == Topology::T2);  // NVENC with the panel on the iGPU
    CHECK(ClassifyForEncoder(summary, LUID{}) == Topology::T5);      // nothing built yet / no encoder
    summary.encoder_available = false;
    CHECK(ClassifyForEncoder(summary, LUID{2, 0}) == Topology::T5);
}

}  // namespace ombra::test

// The overlay and toast draw on the GPU that drives their monitor (monitor_adapter.h). Read-only:
// enumerates this machine's adapters, opens no device.
TEST_CASE("monitor adapter: the primary monitor's adapter is the one the engine calls its owner") {
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const auto adapter = ombra_ui::AdapterForMonitor(primary);
    const auto facts = ombra::EnumerateAdapters(primary);
    const auto owner = std::find_if(facts.begin(), facts.end(), [](const ombra::AdapterFacts& a) { return a.owns_target_monitor; });
    if (owner == facts.end()) {
        CHECK_FALSE(adapter);  // no adapter reports an output (a remote session): default device
        return;
    }
    REQUIRE(adapter);
    DXGI_ADAPTER_DESC1 desc{};
    REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
    CHECK(desc.AdapterLuid.LowPart == owner->luid.LowPart);
    CHECK(desc.AdapterLuid.HighPart == owner->luid.HighPart);
    CHECK_FALSE(ombra_ui::AdapterForMonitor(nullptr));
}
