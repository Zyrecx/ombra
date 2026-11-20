// The NVENC encoder and session probe, compiled once per vendored NVENC header: nvenc13.cpp and
// nvenc11.cpp include their header, define OMBRA_NVENC_NS and include this file, which puts
// everything in ombra::OMBRA_NVENC_NS so the two copies never share a symbol. The only differences
// between the APIs are under NVENCAPI_MAJOR_VERSION.
//
// Synchronous mode, one bitstream buffer, D3D11 input textures registered once per capture slot.

#ifndef OMBRA_NVENC_NS
#error "include nvenc_impl.inl from nvenc13.cpp or nvenc11.cpp"
#endif

#include "ombra/die.h"
#include "ombra/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

// API 12.0 brought AV1 and turned the VUI colour fields into enums.
#define OMBRA_NVENC_HAS_AV1 (NVENCAPI_MAJOR_VERSION >= 12)

namespace ombra::OMBRA_NVENC_NS {

namespace {

// This header's API, in NvEncodeAPIGetMaxSupportedVersion's form.
constexpr uint32_t kHeaderApi = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;

// NV_ENC_CODEC_AV1_GUID, which 11.1 doesn't define: a driver lists it for a GPU that has AV1 whatever
// API the session asked for.
constexpr GUID kAv1CodecGuid = {0x0a352289, 0x0aa7, 0x4759, {0x86, 0x2d, 0x5d, 0x15, 0xcd, 0x16, 0xd2, 0x54}};

// BT.709 in the VUI's colour fields: an enum from 12.0, the plain ITU-T code (1) before.
template <typename Field>
constexpr Field Bt709() {
    return static_cast<Field>(1);
}
#if OMBRA_NVENC_HAS_AV1
static_assert(NV_ENC_VUI_COLOR_PRIMARIES_BT709 == 1 && NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709 == 1 &&
              NV_ENC_VUI_MATRIX_COEFFS_BT709 == 1);
#endif

GUID PresetGuidFromStr(const std::string& p) {
    if (p == "p1") return NV_ENC_PRESET_P1_GUID;
    if (p == "p2") return NV_ENC_PRESET_P2_GUID;
    if (p == "p3") return NV_ENC_PRESET_P3_GUID;
    if (p == "p4") return NV_ENC_PRESET_P4_GUID;
    if (p == "p5") return NV_ENC_PRESET_P5_GUID;
    if (p == "p6") return NV_ENC_PRESET_P6_GUID;
    if (p == "p7") return NV_ENC_PRESET_P7_GUID;
    return NV_ENC_PRESET_P4_GUID;
}

// The codec GUIDs a session lists.
std::vector<GUID> EncodeGuids(const NV_ENCODE_API_FUNCTION_LIST& fn, void* enc) {
    uint32_t count = 0;
    if (fn.nvEncGetEncodeGUIDCount(enc, &count) != NV_ENC_SUCCESS || count == 0) return {};
    std::vector<GUID> guids(count);
    uint32_t          got = 0;
    if (fn.nvEncGetEncodeGUIDs(enc, guids.data(), count, &got) != NV_ENC_SUCCESS) return {};
    guids.resize(got);
    return guids;
}

bool Lists(const std::vector<GUID>& guids, const GUID& codec) {
    return std::any_of(guids.begin(), guids.end(), [&](const GUID& g) { return IsEqualGUID(g, codec) != 0; });
}

// NVENC implementation of IVideoEncoder (H.264, and AV1 from API 12.0).
class NvencEncoder final : public IVideoEncoder {
public:
    bool Open(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
              uint32_t keyframe_sec, const EncoderOptions& options = {}) override;
    bool TryOpen(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
                 uint32_t keyframe_sec, const EncoderOptions& options = {}) override;
    VideoCodec                  Codec() const override { return codec_; }
#if !OMBRA_NVENC_HAS_AV1
    VideoCodec CodecFor(VideoCodec) const override { return VideoCodec::H264; }
#endif
    const std::vector<uint8_t>& SequenceParams() const override { return sequence_params_; }
    bool       RegisterSlot(ID3D11Texture2D* tex, uint32_t width, uint32_t height) override;
    AccessUnit EncodeSlot(uint32_t slot, uint32_t width, uint32_t height, uint64_t timestamp) override;
    void       UnlockLast() override;
    bool               Faulted() const override { return faulted_.load(std::memory_order_acquire); }
    const std::string& FaultReason() const override { return fault_reason_; }
    std::string        FaultCode() const override { return fault_code_; }
    void               Close() override;

    ~NvencEncoder() override { Close(); }

private:
    [[noreturn]] void DieNv(const char* what, NVENCSTATUS s);
    // Non-fatal counterpart to DieNv, for the runtime-reachable calls in EncodeSlot/UnlockLast.
    void ReportFault(const char* what, NVENCSTATUS s);

    bool                               nonfatal_open_ = false;  // set by TryOpen
    HMODULE                            dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST        fn_{};
    void*                              enc_ = nullptr;
    std::vector<NV_ENC_REGISTERED_PTR> reg_;
    NV_ENC_OUTPUT_PTR                  bs_     = nullptr;
    bool                               locked_ = false;
    VideoCodec                         codec_  = VideoCodec::H264;
    std::vector<uint8_t>               sequence_params_;

    std::atomic<bool> faulted_{false};
    std::string       fault_reason_;
    std::string       fault_code_;
};

void NvencEncoder::DieNv(const char* what, NVENCSTATUS s) {
    const char* msg = (enc_ && fn_.nvEncGetLastErrorString) ? fn_.nvEncGetLastErrorString(enc_) : "";
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s  (NVENCSTATUS=%d) %s", what, static_cast<int>(s), msg ? msg : "");
    die(buf);
}

// "NVENC session lost" is recoverable at runtime, not a die(): this is the non-fatal counterpart used
// by EncodeSlot/UnlockLast (the pacer-thread, per-tick path), never by a fatal Open(). Logged once:
// a fault ends this Encoder's useful life.
void NvencEncoder::ReportFault(const char* what, NVENCSTATUS s) {
    const char* msg = (enc_ && fn_.nvEncGetLastErrorString) ? fn_.nvEncGetLastErrorString(enc_) : "";
    faulted_      = true;
    fault_code_   = std::string(what) + ":" + std::to_string(static_cast<int>(s));
    fault_reason_ = std::string(what) + "  (NVENCSTATUS=" + std::to_string(static_cast<int>(s)) + ") " + (msg ? msg : "");
    Log::Warn("encoder fault  %s", fault_reason_.c_str());
}

#define OMBRA_NVC_OPEN(expr)                       \
    do {                                           \
        NVENCSTATUS s_ = (expr);                   \
        if (s_ != NV_ENC_SUCCESS) {                \
            if (!nonfatal_open_) DieNv(#expr, s_); \
            ReportFault(#expr, s_);                \
            return false;                          \
        }                                          \
    } while (0)

// Open()/RegisterSlot()'s non-NVENCSTATUS failures (LoadLibrary, GetProcAddress, driver too old):
// fatal at startup, reported and returned when opened through TryOpen (a rebuild must not exit).
#define OMBRA_OPEN_FAIL(msg)                       \
    do {                                           \
        if (!nonfatal_open_) die(msg);             \
        faulted_      = true;                      \
        fault_reason_ = (msg);                     \
        fault_code_   = (msg);                     \
        Log::Warn("encoder open failed  %s", (msg)); \
        return false;                              \
    } while (0)

bool NvencEncoder::TryOpen(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate_kbps,
                           uint32_t keyframe_sec, const EncoderOptions& options) {
    nonfatal_open_ = true;
    return Open(device, w, h, fps, bitrate_kbps, keyframe_sec, options);
}

bool NvencEncoder::Open(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate_kbps,
                        uint32_t keyframe_sec, const EncoderOptions& options) {
    dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll_) OMBRA_OPEN_FAIL("LoadLibraryW(nvEncodeAPI64.dll) — no NVIDIA driver?");

    using GetMaxVer_t  = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using CreateInst_t = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto get_max     = reinterpret_cast<GetMaxVer_t>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
    auto create_inst = reinterpret_cast<CreateInst_t>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
    if (!get_max || !create_inst) OMBRA_OPEN_FAIL("GetProcAddress on nvEncodeAPI64.dll");

    uint32_t reported = 0;
    OMBRA_NVC_OPEN(get_max(&reported));
    const uint32_t driver_max = NvencEffectiveMaxApi(reported);
    Log::Info("nvenc api         driver supports %u.%u%s, using %u.%u%s", driver_max >> 4, driver_max & 0xF,
              driver_max != reported ? " (test cap)" : "", NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION,
              driver_max >= kHeaderApi ? "" : "   <-- DRIVER TOO OLD");
    if (driver_max < kHeaderApi) OMBRA_OPEN_FAIL(kFaultDriverTooOld);

    fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    OMBRA_NVC_OPEN(create_inst(&fn_));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = device;
    open.apiVersion = NVENCAPI_VERSION;
    OMBRA_NVC_OPEN(fn_.nvEncOpenEncodeSessionEx(&open, &enc_));

    codec_ = options.codec;
#if OMBRA_NVENC_HAS_AV1
    // AV1 needs Ada or newer; OBS checks the same list.
    if (codec_ == VideoCodec::AV1 && (Av1UnsupportedForTest() || !Lists(EncodeGuids(fn_, enc_), kAv1CodecGuid))) {
        Log::Info("nvenc             no AV1 on this GPU, encoding H.264");
        codec_ = VideoCodec::H264;
    }
#else
    if (codec_ == VideoCodec::AV1) {
        Log::Info("nvenc             NVENC API %u.%u has no AV1, encoding H.264", NVENCAPI_MAJOR_VERSION,
                  NVENCAPI_MINOR_VERSION);
        codec_ = VideoCodec::H264;
    }
#endif
    const bool av1 = codec_ == VideoCodec::AV1;
#if OMBRA_NVENC_HAS_AV1
    const GUID codec_guid = av1 ? NV_ENC_CODEC_AV1_GUID : NV_ENC_CODEC_H264_GUID;
#else
    const GUID codec_guid = NV_ENC_CODEC_H264_GUID;
#endif

    // Preset config first, then override only what we care about: the preset knows more about
    // this silicon than we do.
    GUID                 preset_guid = PresetGuidFromStr(options.preset);
    NV_ENC_PRESET_CONFIG preset{};
    preset.version           = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    OMBRA_NVC_OPEN(fn_.nvEncGetEncodePresetConfigEx(enc_, codec_guid, preset_guid, NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset));

    NV_ENC_CONFIG cfg = preset.presetCfg;
    cfg.version       = NV_ENC_CONFIG_VER;
    cfg.gopLength     = fps * keyframe_sec;
    // One frame in, one frame out (the pacer pairs each AU with its tick): no B-frames, no
    // lookahead. Either makes nvEncEncodePicture return NV_ENC_ERR_NEED_MORE_INPUT.
    cfg.frameIntervalP           = 1;
    cfg.rcParams.enableLookahead = 0;  // H.264's p6 and p7 turn it on in their own config
    cfg.rcParams.lookaheadDepth  = 0;
    if (options.rate_control == "cbr") {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.averageBitRate  = bitrate_kbps * 1000;
        cfg.rcParams.maxBitRate      = bitrate_kbps * 1000;
    } else if (options.rate_control == "cqp") {
        // AV1's quantizer index runs 0-255 against H.264's 0-51; OBS scales its CQP by 4 the same way.
        const uint32_t qp = av1 ? (std::min)(options.cq * 4, 255u) : options.cq;
        cfg.rcParams.rateControlMode  = NV_ENC_PARAMS_RC_CONSTQP;
        cfg.rcParams.constQP.qpInterP = qp;
        cfg.rcParams.constQP.qpInterB = qp;
        cfg.rcParams.constQP.qpIntra  = qp;
    } else {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        cfg.rcParams.averageBitRate  = bitrate_kbps * 1000;
        cfg.rcParams.maxBitRate      = options.max_bitrate_kbps > 0 ? options.max_bitrate_kbps * 1000
                                                                    : static_cast<uint32_t>(bitrate_kbps * 1000 * 1.25);
    }
    // NVENC encodes BT.709 limited range and, left alone, signals none of it: every player then
    // guesses, which is why a clip looked washed out until a player was forced to full range.
#if OMBRA_NVENC_HAS_AV1
    if (av1) {
        // Sequence header on every key frame (like repeatSPSPPS), Main profile 8-bit 4:2:0, level
        // picked by the driver (the muxer parses it back out of the header for av1C).
        auto& a                   = cfg.encodeCodecConfig.av1Config;
        a.idrPeriod               = cfg.gopLength;
        a.repeatSeqHdr            = 1;
        a.chromaFormatIDC         = 1;
        a.tier                    = NV_ENC_TIER_AV1_0;
        a.level                   = NV_ENC_LEVEL_AV1_AUTOSELECT;
        a.colorPrimaries          = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        a.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        a.matrixCoefficients      = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        a.colorRange              = 0;  // limited (tv)
        a.useBFramesAsRef         = NV_ENC_BFRAME_REF_MODE_DISABLED;
        if (cfg.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR) a.enableBitstreamPadding = 1;
        cfg.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
    } else
#endif
    {
        cfg.encodeCodecConfig.h264Config.idrPeriod    = cfg.gopLength;
        cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;  // every IDR carries SPS/PPS in-band
        auto& vui                        = cfg.encodeCodecConfig.h264Config.h264VUIParameters;
        vui.videoSignalTypePresentFlag   = 1;
        vui.videoFullRangeFlag           = 0;  // limited (tv) range: matches what NVENC actually writes
        vui.colourDescriptionPresentFlag = 1;
        vui.colourPrimaries              = Bt709<decltype(vui.colourPrimaries)>();
        vui.transferCharacteristics      = Bt709<decltype(vui.transferCharacteristics)>();
        vui.colourMatrix                 = Bt709<decltype(vui.colourMatrix)>();
    }

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version           = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID        = codec_guid;
    init.presetGUID        = preset_guid;
    init.tuningInfo        = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    init.encodeWidth       = w;
    init.encodeHeight      = h;
    init.darWidth          = w;
    init.darHeight         = h;
    init.maxEncodeWidth    = w;
    init.maxEncodeHeight   = h;
    init.frameRateNum      = fps;
    init.frameRateDen      = 1;
    init.enablePTD         = 1;
    init.enableEncodeAsync = 0;  // synchronous
    init.encodeConfig      = &cfg;
    OMBRA_NVC_OPEN(fn_.nvEncInitializeEncoder(enc_, &init));

    NV_ENC_CREATE_BITSTREAM_BUFFER cb{};
    cb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    OMBRA_NVC_OPEN(fn_.nvEncCreateBitstreamBuffer(enc_, &cb));
    bs_ = cb.bitstreamBuffer;

    sequence_params_.resize(1024);
    NV_ENC_SEQUENCE_PARAM_PAYLOAD seq{};
    seq.version              = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    seq.inBufferSize         = static_cast<uint32_t>(sequence_params_.size());
    seq.spsppsBuffer         = sequence_params_.data();
    uint32_t out_size        = 0;
    seq.outSPSPPSPayloadSize = &out_size;
    OMBRA_NVC_OPEN(fn_.nvEncGetSequenceParams(enc_, &seq));
    sequence_params_.resize(out_size);
    return true;
}

bool NvencEncoder::RegisterSlot(ID3D11Texture2D* tex, uint32_t w, uint32_t h) {
    NV_ENC_REGISTER_RESOURCE rr{};
    rr.version            = NV_ENC_REGISTER_RESOURCE_VER;
    rr.resourceType       = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.width              = w;
    rr.height             = h;
    rr.pitch              = 0;
    rr.resourceToRegister = tex;
    rr.bufferFormat       = NV_ENC_BUFFER_FORMAT_ARGB;  // pairs with DXGI_FORMAT_B8G8R8A8_UNORM
    rr.bufferUsage        = NV_ENC_INPUT_IMAGE;
    OMBRA_NVC_OPEN(fn_.nvEncRegisterResource(enc_, &rr));
    reg_.push_back(rr.registeredResource);
    return true;
}

#undef OMBRA_NVC_OPEN
#undef OMBRA_OPEN_FAIL

NvencEncoder::AccessUnit NvencEncoder::EncodeSlot(uint32_t slot, uint32_t w, uint32_t h, uint64_t timestamp) {
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version            = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = reg_[slot];
    NVENCSTATUS map_s = fn_.nvEncMapInputResource(enc_, &map);
    if (map_s != NV_ENC_SUCCESS) {
        ReportFault("nvEncMapInputResource", map_s);
        return AccessUnit{.ok = false};
    }

    NV_ENC_PIC_PARAMS pic{};
    pic.version         = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer     = map.mappedResource;
    pic.bufferFmt       = map.mappedBufferFmt;
    pic.inputWidth      = w;
    pic.inputHeight     = h;
    pic.outputBitstream = bs_;
    pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp  = timestamp;

    NVENCSTATUS s = fn_.nvEncEncodePicture(enc_, &pic);
    if (s == NV_ENC_ERR_NEED_MORE_INPUT) {
        // Would mean the encoder is buffering (B-frames or lookahead), which Open never enables: a real
        // encoder-state problem. A rebuild (a fresh encoder) is the right response either way, so it
        // is reported like any other EncodePicture failure.
        ReportFault("NV_ENC_ERR_NEED_MORE_INPUT with frameIntervalP=1 -- encoder buffering unexpectedly", s);
        fn_.nvEncUnmapInputResource(enc_, map.mappedResource);  // best-effort; rebuild follows anyway
        return AccessUnit{.ok = false};
    }
    if (s != NV_ENC_SUCCESS) {
        ReportFault("nvEncEncodePicture", s);
        fn_.nvEncUnmapInputResource(enc_, map.mappedResource);  // best-effort; rebuild follows anyway
        return AccessUnit{.ok = false};
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version         = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bs_;
    NVENCSTATUS lock_s = fn_.nvEncLockBitstream(enc_, &lock);
    if (lock_s != NV_ENC_SUCCESS) {
        ReportFault("nvEncLockBitstream", lock_s);
        fn_.nvEncUnmapInputResource(enc_, map.mappedResource);  // best-effort; rebuild follows anyway
        return AccessUnit{.ok = false};
    }
    locked_ = true;

    NVENCSTATUS unmap_s = fn_.nvEncUnmapInputResource(enc_, map.mappedResource);
    if (unmap_s != NV_ENC_SUCCESS) {
        // The bitstream is locked and valid, but a resource that won't unmap means the session is in
        // a bad state: surface the fault so the caller rebuilds.
        ReportFault("nvEncUnmapInputResource", unmap_s);
        return AccessUnit{.ok = false};
    }

    AccessUnit au;
    au.data        = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
    au.size        = lock.bitstreamSizeInBytes;
    au.is_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
    return au;
}

void NvencEncoder::UnlockLast() {
    if (!locked_) return;
    NVENCSTATUS s = fn_.nvEncUnlockBitstream(enc_, bs_);
    locked_ = false;
    if (s != NV_ENC_SUCCESS) ReportFault("nvEncUnlockBitstream", s);
}

void NvencEncoder::Close() {
    if (!enc_) return;
    // A caller whose next call failed (an unmap, or the pacer stopping on au.ok == false) can leave
    // the bitstream locked with nothing left to call UnlockLast(). NVENC doesn't document destroying a
    // locked buffer as safe: unlock it first, best-effort.
    if (locked_ && bs_) {
        fn_.nvEncUnlockBitstream(enc_, bs_);
        locked_ = false;
    }
    NV_ENC_PIC_PARAMS eos{};
    eos.version        = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    fn_.nvEncEncodePicture(enc_, &eos);
    for (auto r : reg_) fn_.nvEncUnregisterResource(enc_, r);
    if (bs_) fn_.nvEncDestroyBitstreamBuffer(enc_, bs_);
    fn_.nvEncDestroyEncoder(enc_);
    enc_ = nullptr;
}

}  // namespace

std::unique_ptr<IVideoEncoder> CreateEncoder() { return std::make_unique<NvencEncoder>(); }

// `create_instance` is NvEncodeAPICreateInstance from a loaded nvEncodeAPI64.dll whose effective
// maximum API is at least this header's (ProbeNvencSession checks).
void ProbeSession(void* create_instance, ID3D11Device* device, NvencSessionProbe& out) {
    using CreateInst_t = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto create_inst   = reinterpret_cast<CreateInst_t>(create_instance);

    NV_ENCODE_API_FUNCTION_LIST fn{};
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create_inst(&fn) != NV_ENC_SUCCESS || !fn.nvEncOpenEncodeSessionEx) {
        out.fail_reason = "NvEncodeAPICreateInstance failed";
        return;
    }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = device;
    open.apiVersion = NVENCAPI_VERSION;
    void*       enc = nullptr;
    NVENCSTATUS s   = fn.nvEncOpenEncodeSessionEx(&open, &enc);
    if (s != NV_ENC_SUCCESS || !enc) {
        out.fail_reason = "nvEncOpenEncodeSessionEx failed (not an NVIDIA adapter, or no NVENC on it)";
        return;
    }
    out.session_ok = true;

    const std::vector<GUID> guids = EncodeGuids(fn, enc);
    out.h264 = Lists(guids, NV_ENC_CODEC_H264_GUID);
    out.hevc = Lists(guids, NV_ENC_CODEC_HEVC_GUID);
    const bool lists_av1 = Lists(guids, kAv1CodecGuid);
#if OMBRA_NVENC_HAS_AV1
    out.av1 = lists_av1;
#else
    out.av1_needs_newer_driver = lists_av1;
#endif

    // The engine's real session opens after this one: sessions are limited per process and adapter.
    if (fn.nvEncDestroyEncoder) fn.nvEncDestroyEncoder(enc);
}

}  // namespace ombra::OMBRA_NVENC_NS

#undef OMBRA_NVENC_HAS_AV1
