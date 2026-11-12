#include "ombra/aac_encoder.h"

#include "ombra/log.h"

#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>

#include <winrt/base.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace ombra {

namespace {

// AAC Encoder MFT (C:\Windows\System32\mfAACEnc.dll). Hardcoded rather than pulled from
// wmcodecdsp.h to avoid another import lib. Checked against the registry 2026-08-26 after a
// mis-remembered GUID gave REGDB_E_CLASSNOTREG: re-check there if it ever needs re-deriving.
// {93AF0C51-2275-45D2-A35B-F2BA21CAED00}
constexpr GUID kClsidAacMftEncoder = {
    0x93af0c51, 0x2275, 0x45d2, {0xa3, 0x5b, 0xf2, 0xba, 0x21, 0xca, 0xed, 0x00}};

IMFTransform* Xform(void* p) { return static_cast<IMFTransform*>(p); }

bool Failed(const char* what, HRESULT hr) {
    Log::Warn("aac: %s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return false;
}

#define AAC_TRY(expr)                                     \
    do {                                                  \
        const HRESULT aac_hr_ = (expr);                   \
        if (FAILED(aac_hr_)) return Failed(#expr, aac_hr_); \
    } while (0)

// Once per process and never shut down: encoders live on the audio thread and (in tests) others,
// and balanced per-instance MFStartup/MFShutdown pairs would race each other.
bool EnsureMediaFoundation() {
    static std::once_flag once;
    static HRESULT        hr = E_FAIL;
    std::call_once(once, [] { hr = MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    if (FAILED(hr)) return Failed("MFStartup", hr);
    return true;
}

bool MakeInputType(winrt::com_ptr<IMFMediaType>& type) {
    AAC_TRY(MFCreateMediaType(type.put()));
    AAC_TRY(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    AAC_TRY(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    AAC_TRY(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
    AAC_TRY(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    AAC_TRY(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    AAC_TRY(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4));
    AAC_TRY(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000));
    AAC_TRY(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    return true;
}

}  // namespace

bool AacEncoder::Open(uint32_t bitrate_kbps) {
    Close();
    if (!EnsureMediaFoundation()) return false;

    winrt::com_ptr<IMFTransform> transform;
    AAC_TRY(CoCreateInstance(kClsidAacMftEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(transform.put())));

    winrt::com_ptr<IMFMediaType> input_type;
    if (!MakeInputType(input_type)) return false;
    AAC_TRY(transform->SetInputType(0, input_type.get(), 0));

    // The MFT offers a fixed list of output byte rates, 32..320 kbps for 48 kHz stereo (HE-AAC below
    // 128, AAC-LC from 128), each in raw and ADTS form. Only raw types qualify: nearest-first over
    // every type picked the ADTS one for 256 kbps, a tie on rate. Listing a type is necessary but not
    // sufficient either: the closest candidate once failed SetOutputType with MF_E_INVALIDMEDIATYPE
    // while another succeeded (2026-08-26), so try them nearest-first until one is accepted.
    const uint32_t target_bytes_per_sec = bitrate_kbps * 1000 / 8;
    auto abs_diff = [](int64_t a, int64_t b) { return a > b ? a - b : b - a; };
    std::vector<std::pair<uint32_t, winrt::com_ptr<IMFMediaType>>> candidates;
    for (DWORD i = 0;; ++i) {
        winrt::com_ptr<IMFMediaType> candidate;
        const HRESULT hr = transform->GetOutputAvailableType(0, i, candidate.put());
        if (hr == MF_E_NO_MORE_TYPES) break;
        AAC_TRY(hr);
        UINT32 payload_type = 0;
        candidate->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload_type);  // absent means raw
        if (payload_type != 0) continue;
        UINT32 bytes_per_sec = 0;
        AAC_TRY(candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes_per_sec));
        candidates.emplace_back(bytes_per_sec, std::move(candidate));
    }
    if (candidates.empty()) return Failed("GetOutputAvailableType (no types offered)", E_FAIL);
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        return abs_diff(a.first, target_bytes_per_sec) < abs_diff(b.first, target_bytes_per_sec);
    });
    HRESULT  set_output_hr = E_FAIL;
    uint32_t chosen        = 0;
    for (auto& [bytes_per_sec, type] : candidates) {
        set_output_hr = transform->SetOutputType(0, type.get(), 0);
        if (SUCCEEDED(set_output_hr)) {
            chosen = bytes_per_sec;
            break;
        }
    }
    AAC_TRY(set_output_hr);
    negotiated_bitrate_bps_ = chosen * 8;

    winrt::com_ptr<IMFMediaType> negotiated;
    AAC_TRY(transform->GetOutputCurrentType(0, negotiated.put()));
    UINT32 payload_type = 0xFFFFFFFF;
    negotiated->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload_type);
    if (payload_type != 0) return Failed("negotiated ADTS output (MF_MT_AAC_PAYLOAD_TYPE != 0)", E_FAIL);
    UINT32 blob_size = 0;
    if (SUCCEEDED(negotiated->GetBlobSize(MF_MT_USER_DATA, &blob_size)) && blob_size > 0) {
        user_data_blob_.resize(blob_size);
        AAC_TRY(negotiated->GetBlob(MF_MT_USER_DATA, user_data_blob_.data(), blob_size, nullptr));
    }

    MFT_OUTPUT_STREAM_INFO info{};
    AAC_TRY(transform->GetOutputStreamInfo(0, &info));
    provides_output_samples_ =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    min_output_buffer_size_ = info.cbSize;

    AAC_TRY(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    AAC_TRY(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    transform_    = transform.detach();
    input_frames_ = 0;
    return true;
}

std::vector<uint8_t> AacEncoder::AudioSpecificConfig() const {
    // HEAACWAVEINFO's 12 bytes after the WAVEFORMATEX, then the ASC.
    if (user_data_blob_.size() <= 12) return {};
    return std::vector<uint8_t>(user_data_blob_.begin() + 12, user_data_blob_.end());
}

// Pulls every output sample the MFT has ready. The AAC MFT doesn't allocate its own output
// samples, so this does unless the stream info says otherwise.
bool AacEncoder::DrainAvailable(std::vector<Frame>& out) {
    IMFTransform* transform = Xform(transform_);
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER         output{};
        winrt::com_ptr<IMFSample>      sample;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (!provides_output_samples_) {
            AAC_TRY(MFCreateSample(sample.put()));
            AAC_TRY(MFCreateMemoryBuffer(min_output_buffer_size_, buffer.put()));
            AAC_TRY(sample->AddBuffer(buffer.get()));
            output.pSample = sample.get();
        }
        DWORD         status = 0;
        const HRESULT hr     = transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        AAC_TRY(hr);

        winrt::com_ptr<IMFSample> owned;  // an MFT-provided sample becomes ours to release
        if (provides_output_samples_ && output.pSample) owned.attach(output.pSample);
        IMFSample* emitted = provides_output_samples_ ? owned.get() : sample.get();

        winrt::com_ptr<IMFMediaBuffer> contiguous;
        AAC_TRY(emitted->ConvertToContiguousBuffer(contiguous.put()));
        BYTE* data = nullptr;
        DWORD len  = 0;
        AAC_TRY(contiguous->Lock(&data, nullptr, &len));
        Frame frame;
        frame.data.assign(data, data + len);
        contiguous->Unlock();
        out.push_back(std::move(frame));
    }
}

bool AacEncoder::Encode(const int16_t* interleaved, size_t frames, std::vector<Frame>& out) {
    if (!transform_) return false;
    IMFTransform* transform = Xform(transform_);
    // Bounded chunks: the MFT's input queue is small and expects ProcessOutput between inputs.
    constexpr size_t kChunkFrames = 8192;
    for (size_t offset = 0; offset < frames; offset += kChunkFrames) {
        const size_t chunk = (std::min)(kChunkFrames, frames - offset);
        const DWORD  bytes = static_cast<DWORD>(chunk * 2 * sizeof(int16_t));

        winrt::com_ptr<IMFMediaBuffer> buffer;
        AAC_TRY(MFCreateMemoryBuffer(bytes, buffer.put()));
        BYTE* dst = nullptr;
        AAC_TRY(buffer->Lock(&dst, nullptr, nullptr));
        std::memcpy(dst, interleaved + offset * 2, bytes);
        buffer->Unlock();
        AAC_TRY(buffer->SetCurrentLength(bytes));

        winrt::com_ptr<IMFSample> sample;
        AAC_TRY(MFCreateSample(sample.put()));
        AAC_TRY(sample->AddBuffer(buffer.get()));
        // MF's 100 ns units; this only round-trips through the MFT.
        AAC_TRY(sample->SetSampleTime(static_cast<LONGLONG>(input_frames_) * 10'000'000 / 48000));
        AAC_TRY(sample->SetSampleDuration(static_cast<LONGLONG>(chunk) * 10'000'000 / 48000));
        AAC_TRY(transform->ProcessInput(0, sample.get(), 0));
        input_frames_ += static_cast<int64_t>(chunk);
        if (!DrainAvailable(out)) return false;
    }
    return true;
}

bool AacEncoder::Drain(std::vector<Frame>& out) {
    if (!transform_) return false;
    AAC_TRY(Xform(transform_)->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
    return DrainAvailable(out);
}

void AacEncoder::Close() {
    if (transform_) {
        Xform(transform_)->Release();
        transform_ = nullptr;
    }
}

}  // namespace ombra
