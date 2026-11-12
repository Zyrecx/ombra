#include "ombra/encoder.h"

#include "ombra/amf_encoder.h"
#include "ombra/x264_encoder.h"

#include "ombra/die.h"
#include "ombra/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace ombra {

namespace {
std::atomic<bool> g_av1_unsupported_for_test{false};
}  // namespace

void SetAv1UnsupportedForTest(bool unsupported) { g_av1_unsupported_for_test.store(unsupported); }
bool Av1UnsupportedForTest() { return g_av1_unsupported_for_test.load(); }

void NvencEncoder::DieNv(const char* what, NVENCSTATUS s) {
    const char* msg = (enc_ && fn_.nvEncGetLastErrorString) ? fn_.nvEncGetLastErrorString(enc_) : "";
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s  (NVENCSTATUS=%d) %s", what, static_cast<int>(s), msg ? msg : "");
    die(buf);
}

// "NVENC session lost" is recoverable at runtime, not a die() — this is the
// non-fatal counterpart used by EncodeSlot/UnlockLast (the pacer-thread, per-tick path), never by
// Open() (still fatal-at-startup via DieNv/die above). Logged once per fault, which is fine here
// since a fault ends this Encoder's useful life — there is no repeat-every-tick cost to worry
// about (the rule is about steady per-tick logging, not a single terminal event).
void NvencEncoder::ReportFault(const char* what, NVENCSTATUS s) {
    const char* msg = (enc_ && fn_.nvEncGetLastErrorString) ? fn_.nvEncGetLastErrorString(enc_) : "";
    faulted_ = true;
    fault_code_   = std::string(what) + ":" + std::to_string(static_cast<int>(s));
    fault_reason_ = std::string(what) + "  (NVENCSTATUS=" + std::to_string(static_cast<int>(s)) +
                    ") " + (msg ? msg : "");
    Log::Warn("encoder fault  %s", fault_reason_.c_str());
}

#define OMBRA_NVC(expr)                                               \
    do {                                                              \
        NVENCSTATUS s_ = (expr);                                     \
        if (s_ != NV_ENC_SUCCESS) DieNv(#expr, s_);                  \
    } while (0)

// Open()/RegisterSlot()'s variant: fatal by default (startup), reported-and-returned when this
// Encoder was created via TryOpen() -- PipelineSupervisor's rebuild and resolution-reopen paths,
// see encoder.h for why those must not exit the process.
#define OMBRA_NVC_OPEN(expr)                                          \
    do {                                                              \
        NVENCSTATUS s_ = (expr);                                      \
        if (s_ != NV_ENC_SUCCESS) {                                   \
            if (!nonfatal_open_) DieNv(#expr, s_);                    \
            ReportFault(#expr, s_);                                   \
            return false;                                             \
        }                                                             \
    } while (0)

// The non-NVENCSTATUS failures in Open() (LoadLibrary, GetProcAddress, driver too old) get the
// same treatment -- a rebuild must not exit the process for them either.
#define OMBRA_OPEN_FAIL(msg)                                          \
    do {                                                              \
        if (!nonfatal_open_) die(msg);                                \
        faulted_      = true;                                         \
        fault_reason_ = (msg);                                                fault_code_   = (msg);                                        \
        Log::Warn("encoder open failed  %s", (msg));                 \
        return false;                                                 \
    } while (0)

namespace {
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
}  // namespace

// Whether this GPU's NVENC lists the codec (AV1 needs Ada or newer; OBS checks the same list).
bool NvencEncoder::SupportsCodec(const GUID& codec_guid) {
    uint32_t count = 0;
    if (fn_.nvEncGetEncodeGUIDCount(enc_, &count) != NV_ENC_SUCCESS || count == 0) return false;
    std::vector<GUID> guids(count);
    uint32_t got = 0;
    if (fn_.nvEncGetEncodeGUIDs(enc_, guids.data(), count, &got) != NV_ENC_SUCCESS) return false;
    for (uint32_t i = 0; i < got; ++i)
        if (IsEqualGUID(guids[i], codec_guid)) return true;
    return false;
}

bool NvencEncoder::TryOpen(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps,
                       uint32_t bitrate_kbps, uint32_t keyframe_sec,
                       const EncoderOptions& options) {
    nonfatal_open_ = true;
    return Open(device, w, h, fps, bitrate_kbps, keyframe_sec, options);
}

bool NvencEncoder::Open(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps,
                    uint32_t bitrate_kbps, uint32_t keyframe_sec,
                    const EncoderOptions& options) {
    dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll_) OMBRA_OPEN_FAIL("LoadLibraryW(nvEncodeAPI64.dll) — no NVIDIA driver?");

    using GetMaxVer_t  = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using CreateInst_t = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

    auto get_max = reinterpret_cast<GetMaxVer_t>(
        GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
    auto create_inst = reinterpret_cast<CreateInst_t>(
        GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
    if (!get_max || !create_inst) OMBRA_OPEN_FAIL("GetProcAddress on nvEncodeAPI64.dll");

    uint32_t driver_max = 0;
    OMBRA_NVC_OPEN(get_max(&driver_max));
    const uint32_t header_ver = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    Log::Info("nvenc api         driver supports %u.%u, header wants %u.%u%s",
              driver_max >> 4, driver_max & 0xF,
              NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION,
              driver_max >= header_ver ? "" : "   <-- DRIVER TOO OLD");
    if (driver_max < header_ver) OMBRA_OPEN_FAIL("driver older than the vendored nvEncodeAPI.h");

    fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    OMBRA_NVC_OPEN(create_inst(&fn_));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = device;
    open.apiVersion = NVENCAPI_VERSION;
    OMBRA_NVC_OPEN(fn_.nvEncOpenEncodeSessionEx(&open, &enc_));

    codec_ = options.codec;
    if (codec_ == VideoCodec::AV1 && (Av1UnsupportedForTest() || !SupportsCodec(NV_ENC_CODEC_AV1_GUID))) {
        Log::Info("nvenc             no AV1 on this GPU, encoding H.264");
        codec_ = VideoCodec::H264;
    }
    const bool av1        = codec_ == VideoCodec::AV1;
    const GUID codec_guid = av1 ? NV_ENC_CODEC_AV1_GUID : NV_ENC_CODEC_H264_GUID;

    // Preset config first, then override only what we care about — the preset knows more about
    // this silicon than we do.
    GUID preset_guid = PresetGuidFromStr(options.preset);
    NV_ENC_PRESET_CONFIG preset{};
    preset.version           = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    OMBRA_NVC_OPEN(fn_.nvEncGetEncodePresetConfigEx(enc_, codec_guid, preset_guid,
                                               NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset));

    NV_ENC_CONFIG cfg = preset.presetCfg;
    cfg.version        = NV_ENC_CONFIG_VER;
    cfg.gopLength      = fps * keyframe_sec;
    // One frame in, one frame out (the pacer pairs each AU with its tick): no B-frames, no
    // lookahead. Either makes nvEncEncodePicture return NV_ENC_ERR_NEED_MORE_INPUT.
    cfg.frameIntervalP = 1;
    cfg.rcParams.enableLookahead = 0;  // H.264's p6 and p7 turn it on in their own config
    cfg.rcParams.lookaheadDepth  = 0;
    if (options.rate_control == "cbr") {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.averageBitRate  = bitrate_kbps * 1000;
        cfg.rcParams.maxBitRate      = bitrate_kbps * 1000;
    } else if (options.rate_control == "cqp") {
        // AV1's quantizer index runs 0-255 against H.264's 0-51; OBS scales its CQP by 4 the same way.
        const uint32_t qp = av1 ? (std::min)(options.cq * 4, 255u) : options.cq;
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        cfg.rcParams.constQP.qpInterP = qp;
        cfg.rcParams.constQP.qpInterB = qp;
        cfg.rcParams.constQP.qpIntra  = qp;
    } else {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        cfg.rcParams.averageBitRate  = bitrate_kbps * 1000;
        const uint32_t max_br = (options.max_bitrate_kbps > 0)
                                    ? (options.max_bitrate_kbps * 1000)
                                    : static_cast<uint32_t>(bitrate_kbps * 1000 * 1.25);
        cfg.rcParams.maxBitRate      = max_br;
    }
    // NVENC encodes BT.709 limited range and, left alone, signals none of it — every player then
    // guesses, which is why a clip looked washed out until a player was forced to full range.
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
    } else {
        cfg.encodeCodecConfig.h264Config.idrPeriod    = cfg.gopLength;
        cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;  // every IDR carries SPS/PPS in-band
        auto& vui                          = cfg.encodeCodecConfig.h264Config.h264VUIParameters;
        vui.videoSignalTypePresentFlag     = 1;
        vui.videoFullRangeFlag             = 0;   // limited (tv) range — matches what NVENC actually writes
        vui.colourDescriptionPresentFlag   = 1;
        vui.colourPrimaries                = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics        = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix                   = NV_ENC_VUI_MATRIX_COEFFS_BT709;
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
    rr.bufferFormat       = NV_ENC_BUFFER_FORMAT_ARGB;
    rr.bufferUsage        = NV_ENC_INPUT_IMAGE;
    OMBRA_NVC_OPEN(fn_.nvEncRegisterResource(enc_, &rr));
    reg_.push_back(rr.registeredResource);
    return true;
}

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
        // Would mean the encoder is buffering (B-frames or lookahead), which Open never enables — a real
        // encoder-state problem, not a transient one, but "NVENC session lost" is treated as
        // recoverable-at-runtime generically; a rebuild (fresh NvencEncoder::Open) is the
        // right response either way, so this is reported the same as any other EncodePicture
        // failure rather than dying.
        ReportFault("NV_ENC_ERR_NEED_MORE_INPUT with frameIntervalP=1 -- encoder buffering unexpectedly",
                    s);
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
        // The bitstream itself is already locked and valid at this point, but a resource that
        // won't unmap means this Encoder's NVENC session is in a bad state — surface the fault so
        // the caller rebuilds, rather than pretending everything is fine because this one AU's
        // bytes happen to be readable.
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
    // EncodeSlot sets locked_ = true
    // right after a successful nvEncLockBitstream, but a caller whose *next* call fails
    // (nvEncUnmapInputResource, or the pacer stopping the tick loop on au.ok == false without ever
    // reaching UnlockLast() -- pacer.cpp) can leave this buffer locked with nothing left to call
    // UnlockLast() again before Close() tears the whole session down. NVENC's own docs don't
    // document destroying a still-locked bitstream buffer as safe; unlock it here first,
    // best-effort, rather than assume it's harmless because the session is about to die anyway.
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

std::unique_ptr<IVideoEncoder> CreateVideoEncoder(EncoderBackend backend) {
    switch (backend) {
        case EncoderBackend::Nvenc: return std::make_unique<NvencEncoder>();
        case EncoderBackend::Amf: return std::make_unique<AmfEncoder>();
        case EncoderBackend::X264: return std::make_unique<X264Encoder>();
    }
    return std::make_unique<NvencEncoder>();
}

}  // namespace ombra
