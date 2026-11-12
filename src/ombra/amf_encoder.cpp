#include "ombra/amf_encoder.h"

#include "ombra/die.h"
#include "ombra/log.h"
#include "ombra/util.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace ombra {

namespace {

// Our own surface property carrying the pacer's tick. AMF copies custom surface properties onto
// the output, and OBS pairs frames the same way (its L"PTS"); the output's own GetPts() is the
// encoder's to rewrite, so it isn't what we compare.
constexpr const wchar_t* kTickProperty = L"OmbraTick";

// How long QueryOutput may block for the frame just submitted. The encode itself takes a few ms; a
// wait this long means the session is stuck, and the supervisor's rebuild is the right answer.
constexpr amf_int64 kQueryTimeoutMs = 500;
// How long EncodeSlot keeps asking after that before it faults.
constexpr int64_t kLateFrameBudgetMs = 2000;

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    std::string out;
    for (; *w; ++w) out.push_back(*w < 128 ? static_cast<char>(*w) : '?');
    return out;
}

}  // namespace

std::string AmfVersionString(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", static_cast<unsigned>(AMF_GET_MAJOR_VERSION(v)),
                  static_cast<unsigned>(AMF_GET_MINOR_VERSION(v)),
                  static_cast<unsigned>(AMF_GET_SUBMINOR_VERSION(v)));
    return buf;
}

const AmfRuntime* LoadAmfRuntime(std::string* error) {
    static std::once_flag once;
    static AmfRuntime     runtime;
    static std::string    load_error;
    std::call_once(once, [] {
        HMODULE dll = LoadLibraryW(AMF_DLL_NAME);
        if (!dll) {
            load_error = "amfrt64.dll not found (no AMD driver?)";
            return;
        }
        auto query_version = reinterpret_cast<AMFQueryVersion_Fn>(
            GetProcAddress(dll, AMF_QUERY_VERSION_FUNCTION_NAME));
        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll, AMF_INIT_FUNCTION_NAME));
        if (!query_version || !init) {
            load_error = "amfrt64.dll is missing AMFInit/AMFQueryVersion";
            return;
        }
        amf_uint64 version = 0;
        if (query_version(&version) != AMF_OK) {
            load_error = "AMFQueryVersion failed";
            return;
        }
        // Ask for what both sides have: the runtime refuses a request newer than itself, and
        // everything this encoder sets predates 1.4.
        const amf_uint64 requested = version < AMF_FULL_VERSION ? version : AMF_FULL_VERSION;
        if (init(requested, &runtime.factory) != AMF_OK || !runtime.factory) {
            load_error = "AMFInit failed (runtime " + AmfVersionString(version) + ")";
            runtime.factory = nullptr;
            return;
        }
        runtime.version = version;
        if (runtime.factory->GetTrace(&runtime.trace) == AMF_OK && runtime.trace) {
            runtime.trace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, false);
            runtime.trace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, false);
        }
        Log::Info("amf runtime       %s, headers %s", AmfVersionString(version).c_str(),
                  AmfVersionString(AMF_FULL_VERSION).c_str());
    });
    if (!runtime.factory) {
        if (error) *error = load_error;
        return nullptr;
    }
    return &runtime;
}

int AmfH264Level(uint32_t width, uint32_t height, uint32_t fps) {
    // H.264 Table A-1: max frame size (MaxFS) and macroblock rate (MaxMBPS). AMF defaults to 4.2,
    // which is too small for 1440p and above; a level too low for the stream is a spec violation
    // that some decoders reject.
    struct Level { int level; uint64_t max_fs, max_mbps; };
    static constexpr Level kLevels[] = {
        {AMF_H264_LEVEL__4_2, 8704, 522240},   {AMF_H264_LEVEL__5_1, 36864, 983040},
        {AMF_H264_LEVEL__5_2, 36864, 2073600}, {AMF_H264_LEVEL__6, 139264, 4177920},
        {AMF_H264_LEVEL__6_1, 139264, 8355840}, {AMF_H264_LEVEL__6_2, 139264, 16711680},
    };
    const uint64_t mbs  = uint64_t((width + 15) / 16) * ((height + 15) / 16);
    const uint64_t mbps = mbs * fps;
    for (const auto& l : kLevels)
        if (mbs <= l.max_fs && mbps <= l.max_mbps) return l.level;
    return AMF_H264_LEVEL__6_2;
}

int AmfQualityPresetFor(const std::string& preset) {
    // EncoderOptions carries NVENC-style p1..p7 (the persisted encode.preset). AMF has three
    // speed/quality steps for H.264; map by position rather than exposing a second preset key.
    if (preset == "p1" || preset == "p2" || preset == "p3") return AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED;
    if (preset == "p6" || preset == "p7") return AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY;
    return AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
}

int AmfAv1QualityPresetFor(const std::string& preset) {
    if (preset == "p1" || preset == "p2" || preset == "p3") return AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED;
    if (preset == "p6" || preset == "p7") return AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY;
    return AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED;
}

bool AmfAv1Alignment(const AmfRuntime& rt, amf::AMFContext* context, uint32_t& align_w, uint32_t& align_h) {
    align_w = align_h = 1;
    amf::AMFComponentPtr encoder;
    if (rt.factory->CreateComponent(context, AMFVideoEncoder_AV1, &encoder) != AMF_OK) return false;
    amf::AMFCapsPtr caps;
    if (encoder->GetCaps(&caps) == AMF_OK && caps) {
        amf_int64 w = 1, h = 1;
        caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_WIDTH_ALIGNMENT_FACTOR, &w);
        caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_HEIGHT_ALIGNMENT_FACTOR, &h);
        align_w = static_cast<uint32_t>((std::max)(w, amf_int64{1}));
        align_h = static_cast<uint32_t>((std::max)(h, amf_int64{1}));
    }
    return true;
}

int64_t AmfHeaderSpacing(uint32_t fps, uint32_t keyframe_sec) {
    // HeaderInsertionSpacing is capped at 1000 frames, and a GOP can be longer (144 fps x 10 s).
    // The GOP is a whole number of seconds, so one second of frames always divides it: headers
    // then still land on every IDR, plus some frames in between.
    const int64_t gop = int64_t(fps) * keyframe_sec;
    if (gop <= 1000) return gop;
    return fps <= 1000 ? int64_t(fps) : 1000;
}

bool IsAnnexB(const std::vector<uint8_t>& b) {
    return (b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) ||
           (b.size() >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1);
}

std::string AmfEncoder::ResultText(AMF_RESULT res) const {
    if (runtime_ && runtime_->trace) return Narrow(runtime_->trace->GetResultText(res));
    return "AMF_RESULT " + std::to_string(static_cast<int>(res));
}

bool AmfEncoder::OpenFailed(const std::string& what, AMF_RESULT res) {
    std::string reason = what;
    if (res != AMF_OK) reason += "  (" + ResultText(res) + ")";
    if (!nonfatal_open_) die(reason.c_str());
    faulted_      = true;
    fault_reason_ = reason;
    fault_code_   = what + ":" + std::to_string(static_cast<int>(res));
    Log::Warn("encoder open failed  %s", reason.c_str());
    return false;
}

// Runtime faults on the pacer's tick path: logged once, since a faulted encoder is rebuilt, never
// reused (same contract as NvencEncoder::ReportFault).
void AmfEncoder::ReportFault(const char* what, AMF_RESULT res) {
    faulted_      = true;
    fault_reason_ = std::string(what) + "  (" + ResultText(res) + ")";
    fault_code_   = std::string(what) + ":" + std::to_string(static_cast<int>(res));
    Log::Warn("encoder fault  %s", fault_reason_.c_str());
}

bool AmfEncoder::TryOpen(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps,
                         uint32_t bitrate_kbps, uint32_t keyframe_sec,
                         const EncoderOptions& options) {
    nonfatal_open_ = true;
    return Open(device, w, h, fps, bitrate_kbps, keyframe_sec, options);
}

namespace {

struct Prop {
    const wchar_t*  name;
    amf::AMFVariant value;
};

struct RateControl {
    bool    cqp = false, cbr = false;
    int64_t bps = 0, peak = 0;
    int64_t qp  = 0;  // in the codec's own scale
};

RateControl RateControlFor(const EncoderOptions& options, uint32_t bitrate_kbps, int64_t qp_scale) {
    RateControl rc;
    rc.cqp  = options.rate_control == "cqp";
    rc.cbr  = options.rate_control == "cbr";
    rc.bps  = static_cast<int64_t>(bitrate_kbps) * 1000;
    rc.peak = rc.cbr ? rc.bps
            : options.max_bitrate_kbps > 0 ? static_cast<int64_t>(options.max_bitrate_kbps) * 1000
                                           : rc.bps + rc.bps / 4;  // 1.25x, NvencEncoder's VBR ceiling
    rc.qp   = static_cast<int64_t>(options.cq) * qp_scale;
    return rc;
}

// H.264 (AMFVideoEncoderVCE_AVC). USAGE first: it resets every other property to that usage's defaults.
std::vector<Prop> AvcProps(uint32_t w, uint32_t h, uint32_t fps, uint32_t keyframe_sec,
                           const EncoderOptions& options, uint32_t bitrate_kbps, bool bframes_supported) {
    const int64_t gop = static_cast<int64_t>(fps) * keyframe_sec;
    std::vector<Prop> props = {
        {AMF_VIDEO_ENCODER_USAGE, amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_USAGE_TRANSCODING))},
        {AMF_VIDEO_ENCODER_FRAMESIZE, amf::AMFVariant(AMFConstructSize(amf_int32(w), amf_int32(h)))},
        {AMF_VIDEO_ENCODER_FRAMERATE, amf::AMFVariant(AMFConstructRate(fps, 1))},
        {AMF_VIDEO_ENCODER_PROFILE, amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_PROFILE_HIGH))},
        {AMF_VIDEO_ENCODER_PROFILE_LEVEL, amf::AMFVariant(amf_int64(AmfH264Level(w, h, fps)))},
        {AMF_VIDEO_ENCODER_QUALITY_PRESET, amf::AMFVariant(amf_int64(AmfQualityPresetFor(options.preset)))},
        // One frame in, one frame out: the pacer pairs each AU with the tick that submitted it.
        // Pre-analysis looks ahead (frame delay); pre-encode has never been verified not to.
        {AMF_VIDEO_ENCODER_LOWLATENCY_MODE, amf::AMFVariant(false)},
        {AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, amf::AMFVariant(false)},
        {AMF_VIDEO_ENCODER_PREENCODE_ENABLE, amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_PREENCODE_DISABLED))},
        {AMF_VIDEO_ENCODER_IDR_PERIOD, amf::AMFVariant(gop)},
        // SPS/PPS in-band on every IDR, like NVENC's repeatSPSPPS.
        {AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, amf::AMFVariant(AmfHeaderSpacing(fps, keyframe_sec))},
        {AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, amf::AMFVariant(true)},
        {AMF_VIDEO_ENCODER_ENFORCE_HRD, amf::AMFVariant(true)},
        {AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE, amf::AMFVariant(false)},
        // BGRA in, BT.709 limited out, and say so in the VUI (same as NvencEncoder).
        {AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, amf::AMFVariant(amf_int64(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709))},
        {AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, amf::AMFVariant(amf_int64(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709))},
        {AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, amf::AMFVariant(amf_int64(AMF_COLOR_PRIMARIES_BT709))},
        {AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, amf::AMFVariant(false)},
        {AMF_VIDEO_ENCODER_QUERY_TIMEOUT, amf::AMFVariant(kQueryTimeoutMs)},
    };
    if (bframes_supported) props.push_back({AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf::AMFVariant(amf_int64(0))});

    const RateControl rc = RateControlFor(options, bitrate_kbps, 1);
    if (rc.cqp) {
        props.push_back({AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                         amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP))});
        props.push_back({AMF_VIDEO_ENCODER_QP_I, amf::AMFVariant(rc.qp)});
        props.push_back({AMF_VIDEO_ENCODER_QP_P, amf::AMFVariant(rc.qp)});
    } else {
        props.push_back({AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                         amf::AMFVariant(amf_int64(rc.cbr ? AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR
                                                          : AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR))});
        props.push_back({AMF_VIDEO_ENCODER_TARGET_BITRATE, amf::AMFVariant(amf_int64(rc.bps))});
        props.push_back({AMF_VIDEO_ENCODER_PEAK_BITRATE, amf::AMFVariant(amf_int64(rc.peak))});
        props.push_back({AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, amf::AMFVariant(amf_int64(rc.bps))});
        // VBAQ off. On the 780M it cost quality that grew through each GOP (likely P-frames
        // starved, inferred from the pattern): 1080p60 CSGO at 11 Mbps scored VMAF 76.8 with it and
        // 87.4 without at 11% fewer bits (scripts/encbench.py, 2026-10-31), and moving frames
        // visibly smeared away the crosshair and players.
        props.push_back({AMF_VIDEO_ENCODER_ENABLE_VBAQ, amf::AMFVariant(false)});
        if (rc.cbr) props.push_back({AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, amf::AMFVariant(true)});
    }
    return props;
}

// AV1 (AMFVideoEncoderHW_AV1), following OBS's amf_av1_create_internal. Same one-in-one-out rules
// as H.264: no pre-analysis, no pre-encode, no B-frames.
std::vector<Prop> Av1Props(uint32_t w, uint32_t h, uint32_t fps, uint32_t keyframe_sec,
                           const EncoderOptions& options, uint32_t bitrate_kbps, bool bframes_supported) {
    const int64_t gop = static_cast<int64_t>(fps) * keyframe_sec;
    std::vector<Prop> props = {
        {AMF_VIDEO_ENCODER_AV1_USAGE, amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING))},
        {AMF_VIDEO_ENCODER_AV1_FRAMESIZE, amf::AMFVariant(AMFConstructSize(amf_int32(w), amf_int32(h)))},
        {AMF_VIDEO_ENCODER_AV1_FRAMERATE, amf::AMFVariant(AMFConstructRate(fps, 1))},
        {AMF_VIDEO_ENCODER_AV1_PROFILE, amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN))},
        {AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, amf::AMFVariant(amf_int64(AMF_COLOR_BIT_DEPTH_8))},
        {AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, amf::AMFVariant(amf_int64(AmfAv1QualityPresetFor(options.preset)))},
        {AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
         amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_NONE))},
        {AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, amf::AMFVariant(false)},
        {AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_PREENCODE, amf::AMFVariant(false)},
        {AMF_VIDEO_ENCODER_AV1_GOP_SIZE, amf::AMFVariant(gop)},
        // Sequence header on every key frame, like NVENC's repeatSeqHdr.
        {AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
         amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED))},
        {AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, amf::AMFVariant(true)},
        {AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, amf::AMFVariant(amf_int64(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709))},
        {AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC, amf::AMFVariant(amf_int64(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709))},
        {AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES, amf::AMFVariant(amf_int64(AMF_COLOR_PRIMARIES_BT709))},
        {AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, amf::AMFVariant(kQueryTimeoutMs)},
    };
    if (bframes_supported) props.push_back({AMF_VIDEO_ENCODER_AV1_B_PIC_PATTERN, amf::AMFVariant(amf_int64(0))});

    // AV1's quantizer index runs 0-255 against H.264's 0-51; OBS scales its CQP by 4 the same way.
    const RateControl rc = RateControlFor(options, bitrate_kbps, 4);
    if (rc.cqp) {
        props.push_back({AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                         amf::AMFVariant(amf_int64(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP))});
        props.push_back({AMF_VIDEO_ENCODER_AV1_Q_INDEX_INTRA, amf::AMFVariant((std::min)(rc.qp, int64_t{255}))});
        props.push_back({AMF_VIDEO_ENCODER_AV1_Q_INDEX_INTER, amf::AMFVariant((std::min)(rc.qp, int64_t{255}))});
    } else {
        props.push_back({AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                         amf::AMFVariant(amf_int64(rc.cbr ? AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR
                                                          : AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR))});
        props.push_back({AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, amf::AMFVariant(amf_int64(rc.bps))});
        props.push_back({AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, amf::AMFVariant(amf_int64(rc.peak))});
        props.push_back({AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, amf::AMFVariant(amf_int64(rc.bps))});
        if (rc.cbr) props.push_back({AMF_VIDEO_ENCODER_AV1_FILLER_DATA, amf::AMFVariant(true)});
    }
    return props;
}

}  // namespace

bool AmfEncoder::Open(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t fps,
                      uint32_t bitrate_kbps, uint32_t keyframe_sec,
                      const EncoderOptions& options) {
    std::string error;
    runtime_ = LoadAmfRuntime(&error);
    if (!runtime_) {
        OpenFailed(error, AMF_OK);
        fault_code_ = "LoadAmfRuntime";  // `error` holds a path
        return false;
    }

    AMF_RESULT res = runtime_->factory->CreateContext(&context_);
    if (res != AMF_OK) return OpenFailed("AMFFactory::CreateContext", res);
    // The capture device, so the encoder reads the stage textures with no copy. It is already
    // multithread-protected (Capture::Open), which AMF's own submissions on this thread rely on.
    res = context_->InitDX11(device);
    if (res != AMF_OK) return OpenFailed("AMFContext::InitDX11", res);

    codec_ = options.codec;
    if (codec_ == VideoCodec::AV1) {
        res = Av1UnsupportedForTest() ? AMF_NOT_SUPPORTED
                                      : runtime_->factory->CreateComponent(context_, AMFVideoEncoder_AV1, &encoder_);
        if (res != AMF_OK) {
            Log::Info("amf               no AV1 on this GPU (%s), encoding H.264", ResultText(res).c_str());
            codec_ = VideoCodec::H264;
        }
    }
    const bool av1 = codec_ == VideoCodec::AV1;
    if (!av1) {
        res = runtime_->factory->CreateComponent(context_, AMFVideoEncoderVCE_AVC, &encoder_);
        if (res != AMF_OK) return OpenFailed("CreateComponent(AMFVideoEncoderVCE_AVC)", res);
    }

    bool query_timeout_ok = false;
    bool bframes_supported = false;
    {
        amf::AMFCapsPtr caps;
        if (encoder_->GetCaps(&caps) == AMF_OK && caps) {
            // AV1 has no cap for it: its component postdates the timeout, and setting
            // Av1QueryTimeout below fails the open on a driver that lacks it.
            if (av1) query_timeout_ok = true;
            else caps->GetProperty(AMF_VIDEO_ENCODER_CAP_QUERY_TIMEOUT_SUPPORT, &query_timeout_ok);
            caps->GetProperty(av1 ? AMF_VIDEO_ENCODER_AV1_CAP_BFRAMES : AMF_VIDEO_ENCODER_CAP_BFRAMES,
                              &bframes_supported);
        }
    }
    // Without a blocking QueryOutput, waiting for the frame means an Ombra-side sleep loop on the
    // pacer's tick path. Every current driver supports it; refuse rather than poll.
    if (!query_timeout_ok) return OpenFailed("AMF driver lacks QueryOutput timeout support", AMF_OK);

    const std::vector<Prop> props = av1 ? Av1Props(w, h, fps, keyframe_sec, options, bitrate_kbps, bframes_supported)
                                        : AvcProps(w, h, fps, keyframe_sec, options, bitrate_kbps, bframes_supported);
    for (const auto& p : props) {
        res = encoder_->SetProperty(p.name, p.value);
        if (res != AMF_OK) return OpenFailed("SetProperty(" + Narrow(p.name) + ")", res);
    }
    if (av1) {
        // Asks for exact sizes. The 780M (driver of 2026-10) accepts it and pads anyway (1080 lines
        // code as 1082), so the supervisor's PlanEncode decides by the probe's alignment caps, and
        // the segment takes its size from the sequence header. Kept for hardware that honours it.
        res = encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE,
                                    amf_int64(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS));
        if (res != AMF_OK) Log::Info("amf               AV1 alignment mode not accepted (%s)", ResultText(res).c_str());
    }

    res = encoder_->Init(amf::AMF_SURFACE_BGRA, amf_int32(w), amf_int32(h));
    if (res != AMF_OK) return OpenFailed("AMFComponent::Init(BGRA)", res);
    if (av1) {
        amf_int64 mode = -1, wa = -1, ha = -1;
        encoder_->GetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, &mode);
        amf::AMFCapsPtr caps;
        if (encoder_->GetCaps(&caps) == AMF_OK && caps) {
            caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_WIDTH_ALIGNMENT_FACTOR, &wa);
            caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_HEIGHT_ALIGNMENT_FACTOR, &ha);
        }
        Log::Info("amf               AV1 alignment mode %lld, caps align %lldx%lld", mode, wa, ha);
    }

    amf::AMFVariant extradata;
    res = encoder_->GetProperty(av1 ? AMF_VIDEO_ENCODER_AV1_EXTRA_DATA : AMF_VIDEO_ENCODER_EXTRADATA, &extradata);
    if (res != AMF_OK || extradata.type != amf::AMF_VARIANT_INTERFACE || !extradata.pInterface)
        return OpenFailed("GetProperty(ExtraData)", res);
    amf::AMFBufferPtr header(extradata.pInterface);
    if (!header) return OpenFailed("ExtraData is not an AMFBuffer", AMF_OK);
    const auto* bytes = static_cast<const uint8_t*>(header->GetNative());
    sequence_params_.assign(bytes, bytes + header->GetSize());
    // H.264: the mux splits these at start codes into avcC (same shape NVENC hands back). The
    // header says Annex B; a runtime that disagrees would corrupt every save, so refuse it here.
    // AV1's is a sequence header OBU, which the mux parses itself.
    if (!av1 && !IsAnnexB(sequence_params_)) return OpenFailed("ExtraData is not Annex B SPS/PPS", AMF_OK);

    pts_per_frame_ = AMF_SECOND / static_cast<int64_t>(fps ? fps : 60);
    Log::Info("amf encoder       %s %ux%u @%u  %s  %u kbps  preset %s", VideoCodecName(codec_), w, h, fps,
              options.rate_control.c_str(), bitrate_kbps, options.preset.c_str());
    return true;
}

bool AmfEncoder::RegisterSlot(ID3D11Texture2D* tex, uint32_t, uint32_t) {
    // Nothing to register: each tick wraps the slot's texture in a surface (CreateSurfaceFromDX11Native
    // is a wrapper, not a copy), as OBS and ffmpeg's amfenc do.
    if (!tex) {
        if (!nonfatal_open_) die("AmfEncoder::RegisterSlot: null texture");
        faulted_      = true;
        fault_reason_ = "RegisterSlot: null texture";
        fault_code_   = fault_reason_;
        return false;
    }
    slots_.push_back(tex);
    return true;
}

IVideoEncoder::AccessUnit AmfEncoder::EncodeSlot(uint32_t slot, uint32_t, uint32_t,
                                                 uint64_t timestamp) {
    amf::AMFSurfacePtr surface;
    AMF_RESULT res = context_->CreateSurfaceFromDX11Native(slots_[slot], &surface, nullptr);
    if (res != AMF_OK) {
        ReportFault("CreateSurfaceFromDX11Native", res);
        return AccessUnit{.ok = false};
    }
    surface->SetPts(static_cast<int64_t>(timestamp) * pts_per_frame_);
    surface->SetProperty(kTickProperty, static_cast<amf_int64>(timestamp));

    res = encoder_->SubmitInput(surface);
    if (res != AMF_OK) {
        // AMF_INPUT_FULL / AMF_NEED_MORE_INPUT would mean the encoder is holding frames, which
        // this configuration rules out: the same "rebuild it" answer as any other failure.
        ReportFault("SubmitInput", res);
        return AccessUnit{.ok = false};
    }

    amf::AMFDataPtr data;
    const int64_t   asked = QpcNow();
    res = encoder_->QueryOutput(&data);  // blocks up to kQueryTimeoutMs
    // Other work on the same GPU can hold a frame past one timeout. Waiting for it costs a stall
    // the pacer resyncs over; a fault costs a rebuild and a barrier, so a save afterwards loses
    // everything before it.
    int64_t waited_ms = 0;
    while (!data && res == AMF_REPEAT) {
        waited_ms = (QpcNow() - asked) * 1000 / QpcFrequency();
        if (waited_ms >= kLateFrameBudgetMs) break;
        if (waited_ms < kQueryTimeoutMs / 2) Sleep(1);  // a driver that returns at once: don't spin
        res = encoder_->QueryOutput(&data);
    }
    if (data && waited_ms > 0) {
        waited_ms = (QpcNow() - asked) * 1000 / QpcFrequency();
        late_frames_.fetch_add(1, std::memory_order_relaxed);
        if (waited_ms > longest_wait_ms_.load(std::memory_order_relaxed))
            longest_wait_ms_.store(waited_ms, std::memory_order_relaxed);
    }
    if (!data) {
        ReportFault("QueryOutput returned no frame within the timeout", res);
        return AccessUnit{.ok = false};
    }
    amf_int64 tick = -1;
    data->GetProperty(kTickProperty, &tick);
    if (tick != static_cast<amf_int64>(timestamp)) {
        // An AU for an earlier frame: the encoder is running behind and every later AU would be
        // paired with the wrong tick. Rebuild rather than mislabel the stream.
        const std::string what = "QueryOutput returned tick " + std::to_string(tick) +
                                 " for submitted tick " + std::to_string(timestamp);
        ReportFault(what.c_str(), AMF_OK);
        fault_code_ = "QueryOutput tick mismatch";
        return AccessUnit{.ok = false};
    }
    amf::AMFBufferPtr buffer(data);
    if (!buffer) {
        ReportFault("QueryOutput data is not an AMFBuffer", AMF_OK);
        return AccessUnit{.ok = false};
    }
    // Key frames only: an AV1 intra-only frame or an H.264 non-IDR I frame can't start a save.
    amf_int64 type = -1;
    bool      key  = false;
    if (codec_ == VideoCodec::AV1) {
        data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
        key = type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
    } else {
        data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
        key = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
    }

    last_output_ = buffer;
    AccessUnit au;
    au.data        = static_cast<const uint8_t*>(buffer->GetNative());
    au.size        = buffer->GetSize();
    au.is_keyframe = key;
    return au;
}

void AmfEncoder::UnlockLast() { last_output_.Release(); }

void AmfEncoder::Close() {
    last_output_.Release();
    // Component, then context, while the D3D11 device they were built on is still alive (the
    // supervisor closes the encoder before the capture source).
    if (encoder_) {
        encoder_->Terminate();
        encoder_.Release();
    }
    if (context_) {
        context_->Terminate();
        context_.Release();
    }
    slots_.clear();
}

}  // namespace ombra
