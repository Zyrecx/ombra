#include "ombra/audio.h"
#include "ombra/audio_format.h"
#include "ombra/brand.h"
#include "ombra/controller.h"
#include "ombra/crash_upload.h"
#include "ombra/die.h"
#include "ombra/log.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace ombra {

namespace {

// `source` is mic, loopback or process; never the device's name (it can hold a person's name).
void ReportAudioError(const char* source, const char* call, HRESULT hr) {
    char code[160];
    std::snprintf(code, sizeof(code), "%s:%s:0x%08lX", source, call, static_cast<unsigned long>(hr));
    crash_upload::ReportError(crash_upload::ErrorKind::Audio, code, "", "audio stream failed");
}

std::string Utf8FromWideLocal(const std::wstring& s) {
    if (s.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), count, nullptr, nullptr);
    return out;
}

int64_t QpcFrequency() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
}

int64_t QpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// WASAPI's pu64QPCPosition is documented in fixed 100-nanosecond units — *not* raw QPC ticks,
// regardless of what QueryPerformanceFrequency() actually reports on this machine. Convert here,
// once, at the capture boundary, so every AuDescriptor-shaped qpc field in
// the codebase (ring.h's video AUs and, later, PcmRing's packets) is directly comparable in the
// same unit without a second conversion at every call site.
int64_t Qpc100nsToRawTicks(uint64_t qpc_100ns, int64_t qpc_freq) {
    // 100ns units are always base-10,000,000/sec by definition of the unit, independent of
    // qpc_freq — do the multiply-before-divide in double to avoid the 64-bit overflow a very long
    // uptime could hit doing it in pure integer math, precision loss here is well under a sample.
    return static_cast<int64_t>(
        static_cast<double>(qpc_100ns) * static_cast<double>(qpc_freq) / 10'000'000.0);
}

// "Audio device changes are unhandled" fix: a minimal IMMNotificationClient that only cares about
// OnDefaultDeviceChanged for one data flow (eRender for desktop loopback, eCapture for mic) — the
// half of device-change recovery that AUDCLNT_E_DEVICE_INVALIDATED alone can't catch, because
// switching the system default output/input does *not* invalidate a stream already bound to the
// previously-default IMMDevice; Windows expects the app to notice via this callback and reopen
// against the new default itself. Only registered when the source is configured to follow the
// default endpoint (AudioEndpoint::device_id empty) — a source pinned to one specific device_id has
// nothing to "follow." Same plain-classic-COM/manual-refcount shape as
// ProcessLoopbackActivationHandler below; runs on an MMDevice-internal thread, so the only thing it
// touches is a std::atomic flag the capture loop polls.
class DefaultDeviceWatcher : public IMMNotificationClient {
public:
    DefaultDeviceWatcher(EDataFlow flow, std::atomic<bool>* changed_flag)
        : flow_(flow), changed_flag_(changed_flag) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            static_cast<ULONG>(ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == flow_ && role == eConsole) changed_flag_->store(true, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    EDataFlow flow_;
    std::atomic<bool>* changed_flag_;
    std::atomic<ULONG> ref_count_{1};
};

// Opens (or reopens) the WASAPI stream for `endpoint`/`is_mic` against `enumerator`, reusing the
// event handle already created by the caller. Never dies — every failure returns false and Run()
// retries. Carries the same "stale device_id falls back to default" behavior the original inline
// setup had. A free function, not a member, so this stays out of audio.h -- Run() is the only caller.
bool OpenStream(IMMDeviceEnumerator* enumerator, bool is_mic, const std::wstring& device_id,
                 HANDLE event, winrt::com_ptr<IAudioClient>& out_client,
                 winrt::com_ptr<IAudioCaptureClient>& out_capture_client,
                 FormatConverter& out_converter, NegotiatedFormat& out_negotiated) {
    winrt::com_ptr<IMMDevice> device;
    if (!device_id.empty() && FAILED(enumerator->GetDevice(device_id.c_str(), device.put()))) {
        Log::Warn("audio: configured device id no longer exists, falling back to the default "
                  "endpoint");
        device = nullptr;
    }
    if (!device && FAILED(enumerator->GetDefaultAudioEndpoint(is_mic ? eCapture : eRender, eConsole,
                                                                device.put())))
        return false;

    winrt::com_ptr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(client.put()))))
        return false;

    WAVEFORMATEX* mix_format = nullptr;
    if (FAILED(client->GetMixFormat(&mix_format))) return false;
    FormatConverter converter;
    if (!converter.Configure(mix_format)) {
        // Run() retries every 500 ms, so say this once rather than per retry.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log::Warn("audio: can't read this endpoint's mix format (tag 0x%04X, %u bits, %u channels)",
                      mix_format->wFormatTag, mix_format->wBitsPerSample, mix_format->nChannels);
        CoTaskMemFree(mix_format);
        return false;
    }

    constexpr REFERENCE_TIME kBufferDuration = 200 * 10'000;  // 100ns units, same as initial open
    const DWORD stream_flags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (is_mic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK);
    const HRESULT init_hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                                kBufferDuration, 0, mix_format, nullptr);
    CoTaskMemFree(mix_format);
    if (FAILED(init_hr)) return false;

    if (FAILED(client->SetEventHandle(event))) return false;

    winrt::com_ptr<IAudioCaptureClient> capture_client;
    if (FAILED(client->GetService(IID_PPV_ARGS(capture_client.put())))) return false;

    if (FAILED(client->Start())) return false;

    out_negotiated.sample_rate_hz  = converter.NegotiatedSampleRate();
    out_negotiated.channels        = converter.NegotiatedChannels();
    out_negotiated.bits_per_sample = converter.NegotiatedBits();
    out_negotiated.is_float        = converter.NegotiatedIsFloat();
    out_negotiated.was_resampled   = converter.IsResampling();

    out_client         = client;
    out_capture_client = capture_client;
    out_converter      = converter;
    return true;
}

// The completion-handler side of
// ActivateAudioInterfaceAsync's async-callback contract. Allocated with `new`, refcount starts at
// 1 (the caller's own reference) — ActivateAudioInterfaceAsync internally AddRefs it for the
// duration of the async operation and Releases it once ActivateCompleted returns, so the caller's
// own Release() (after the synchronous wait below observes completion) is what actually frees it.
// Not a winrt::implements type: this is a plain classic-COM object, not a WinRT one, and the
// interface it implements (IActivateAudioInterfaceCompletionHandler) has no WinRT ABI.
//
// Also implements IAgileObject (a marker interface, no methods beyond IUnknown) — without it,
// ActivateAudioInterfaceAsync fails outright with E_ILLEGAL_METHOD_CALL (0x8000000E), confirmed
// against this exact failure on real hardware this session: COM tries to marshal the completion
// handler across apartments unless it's declared agile (free-threaded), and this handler has no
// proxy/stub to marshal through. Every real sample of this API (including Microsoft's own
// LoopbackCapture sample) derives from an agile/free-threaded base for exactly this reason.
class ProcessLoopbackActivationHandler : public IActivateAudioInterfaceCompletionHandler,
                                          public IAgileObject {
public:
    explicit ProcessLoopbackActivationHandler(HANDLE done_event) : done_event_(done_event) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            static_cast<ULONG>(ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activate_hr = E_FAIL;
        IUnknown* unk = nullptr;
        op->GetActivateResult(&activate_hr, &unk);
        activate_hr_ = activate_hr;
        if (SUCCEEDED(activate_hr) && unk) {
            unk->QueryInterface(IID_PPV_ARGS(&client_));
            unk->Release();
        }
        SetEvent(done_event_);
        return S_OK;
    }

    HRESULT       ActivateHr() const { return activate_hr_; }
    IAudioClient* Client() const { return client_; }  // caller must AddRef before this object is Release()d

private:
    ~ProcessLoopbackActivationHandler() {
        if (client_) client_->Release();
    }

    std::atomic<int> ref_count_{1};
    HANDLE           done_event_;
    HRESULT          activate_hr_ = E_FAIL;
    IAudioClient*    client_      = nullptr;  // owned
};

void SetRepairCapacity(LoopbackStampRepair& repair, IAudioClient* client, uint32_t device_rate_hz,
                       int64_t qpc_freq) {
    UINT32 frames = 0;
    const bool known = client && device_rate_hz != 0 && SUCCEEDED(client->GetBufferSize(&frames));
    repair.SetCapacity(known ? static_cast<int64_t>(frames) * qpc_freq / device_rate_hz : 0, qpc_freq);
}

// One line per restamped run: rare, and on a capture thread, not the pacer's.
void LogRepairedRun(LoopbackStampRepair& repair, const char* what, int64_t qpc_freq) {
    LoopbackStampRepair::Run r;
    if (!repair.TakeFinishedRun(r)) return;
    Log::Info("audio: %s: restamped %lld ms in %d packets over %d passes %lld ms apart (their own "
              "stamps were %lld ms old)",
              what, static_cast<long long>(r.frames * 1000 / 48000), r.packets, r.passes,
              static_cast<long long>(r.span_ticks * 1000 / qpc_freq),
              static_cast<long long>(r.raw_age_ticks * 1000 / qpc_freq));
}

}  // namespace

void AudioCapture::Start(PcmCallback cb, const AudioEndpoint& endpoint) {
    negotiated_ = {};  // an instance is restarted by the audio target watcher; don't report the last open
    ready_or_failed_ = CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr);
    if (!ready_or_failed_) die("audio: CreateEvent for capture-thread readiness failed");

    running_.store(true);
    thread_ = std::thread(&AudioCapture::Run, this, std::move(cb), endpoint);

    // Block until Run() has either opened the endpoint or found none to open (it keeps retrying).
    WaitForSingleObject(ready_or_failed_, INFINITE);
    CloseHandle(ready_or_failed_);
    ready_or_failed_ = nullptr;
}

void AudioCapture::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::Run(PcmCallback cb, AudioEndpoint endpoint) {
    Log::SetThreadName("audio-capture");
    // Everything WASAPI-related — device enumeration through the capture loop — happens on this
    // one thread, in the same COM apartment, so no interface pointer ever crosses an apartment
    // boundary. This is the standard pattern for a WASAPI capture thread; splitting setup onto the
    // caller's thread and the loop onto this one would need marshaling for no benefit.
    OMBRA_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    OMBRA_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(enumerator.put())));

    const bool is_mic = endpoint.kind == AudioEndpointKind::kMicrophone;
    // "Audio device changes are unhandled" fix: device_id.empty() means this source follows
    // whichever endpoint is the system default, so it's the case that needs to react to
    // OnDefaultDeviceChanged below. A source pinned to one specific device_id doesn't get moved by
    // a default-device switch (Windows only reroutes default-following streams) -- if its own
    // endpoint later disappears, that surfaces as AUDCLNT_E_DEVICE_INVALIDATED instead, handled the
    // same way in the packet loop below, and OpenStream's fallback-to-default kicks in on reopen.
    const bool following_default = endpoint.device_id.empty();

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) die("audio: CreateEvent for WASAPI buffer-ready notification failed");

    winrt::com_ptr<IAudioClient>        client;
    winrt::com_ptr<IAudioCaptureClient> capture_client;
    FormatConverter                     converter;
    // A failed first open doesn't die(): the mic can be switched on live (audio.sources[].enabled)
    // with no microphone plugged in, and one missing endpoint must not kill the engine. It goes
    // straight to the reopen loop below, which retries every 500 ms like a lost stream.
    const bool opened = OpenStream(enumerator.get(), is_mic, endpoint.device_id, event, client,
                                   capture_client, converter, negotiated_);
    if (!opened) Log::Warn("audio: %s endpoint open failed, retrying until one appears", is_mic ? "microphone" : "loopback");

    // Registered only when following the default endpoint (see following_default above). Refcount
    // starts at 1 for this owning reference; RegisterEndpointNotificationCallback AddRefs its own,
    // so Release() after Unregister below is what actually frees it, not this call.
    std::atomic<bool>    default_changed{false};
    DefaultDeviceWatcher* watcher = nullptr;
    if (following_default) {
        watcher = new DefaultDeviceWatcher(is_mic ? eCapture : eRender, &default_changed);
        enumerator->RegisterEndpointNotificationCallback(watcher);
    }

    const int64_t qpc_freq = QpcFrequency();
    const char*   kind     = is_mic ? "microphone" : "loopback";
    LoopbackStampRepair repair;
    SetRepairCapacity(repair, client.get(), negotiated_.sample_rate_hz, qpc_freq);

    // Setup succeeded — release the caller waiting in Start().
    SetEvent(ready_or_failed_);

    // Set by either the notifier above (default endpoint switched) or AUDCLNT_E_DEVICE_INVALIDATED
    // below (this stream's own endpoint was unplugged/disabled/reconfigured) -- both mean "the
    // current client is dead or stale, reopen against whatever's current." Reopen failure doesn't
    // die(): it logs and leaves need_reopen set so the loop just keeps retrying every 500ms
    // (WaitForSingleObject's own timeout paces the retry, no extra sleep needed) until it succeeds
    // or Stop() flips running_ false. A die() here would kill the entire engine over one lost
    // audio endpoint, not just this source.
    std::atomic<bool> need_reopen{!opened};
    bool reopen_failing = !opened;  // warn once per failing streak, not every 500 ms
    // Any failure reopens, not only an invalidated device: a restart of the Windows Audio service
    // reports AUDCLNT_E_SERVICE_NOT_RUNNING here, and no audio error may stop the engine.
    auto check_invalidated = [&](HRESULT hr, const char* what) -> bool {
        if (SUCCEEDED(hr)) return false;
        if (hr != AUDCLNT_E_DEVICE_INVALIDATED) {
            Log::Warn("audio: %s failed  hr=0x%08lX, reopening", what, static_cast<unsigned long>(hr));
            ReportAudioError(is_mic ? "mic" : "loopback", what, hr);
        }
        need_reopen.store(true, std::memory_order_relaxed);
        return true;
    };

    while (running_.load(std::memory_order_relaxed)) {
        if (default_changed.exchange(false, std::memory_order_relaxed))
            need_reopen.store(true, std::memory_order_relaxed);

        if (need_reopen.exchange(false, std::memory_order_relaxed)) {
            if (client) client->Stop();
            winrt::com_ptr<IAudioClient>        new_client;
            winrt::com_ptr<IAudioCaptureClient> new_capture_client;
            FormatConverter                     new_converter;
            if (OpenStream(enumerator.get(), is_mic, endpoint.device_id, event, new_client,
                            new_capture_client, new_converter, negotiated_)) {
                client         = new_client;
                capture_client = new_capture_client;
                converter      = new_converter;
                SetRepairCapacity(repair, client.get(), negotiated_.sample_rate_hz, qpc_freq);
                Log::Info(reopen_failing ? "audio: endpoint opened" : "audio: endpoint reopened after a device change");
                reopen_failing = false;
            } else {
                if (!reopen_failing) Log::Warn("audio: endpoint reopen failed, retrying");
                reopen_failing = true;
                need_reopen.store(true, std::memory_order_relaxed);
                Sleep(500);
                continue;
            }
        }

        const DWORD wait = WaitForSingleObject(event, 500);
        if (wait == WAIT_TIMEOUT) continue;  // nothing playing on a loopback endpoint: no packets
        if (wait != WAIT_OBJECT_0) die("audio: WaitForSingleObject on WASAPI event failed");

        UINT32 packet_frames = 0;
        if (check_invalidated(capture_client->GetNextPacketSize(&packet_frames),
                               "capture_client->GetNextPacketSize")) continue;
        repair.BeginPass(QpcNow());
        while (packet_frames != 0) {
            BYTE*  data        = nullptr;
            UINT32 frame_count = 0;
            DWORD  flags       = 0;
            UINT64 device_position = 0;
            UINT64 qpc_100ns       = 0;
            if (check_invalidated(capture_client->GetBuffer(&data, &frame_count, &flags,
                                                              &device_position, &qpc_100ns),
                                   "capture_client->GetBuffer")) break;

            PcmPacket packet;
            packet.qpc_ticks     = Qpc100nsToRawTicks(qpc_100ns, qpc_freq);
            packet.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            // AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR: qpc_100ns is documented unreliable for this
            // packet, and nothing better can be substituted from inside one GetBuffer call.
            packet.discontinuity |= (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;

            const uint8_t* src = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr
                                                                       : reinterpret_cast<const uint8_t*>(data);
            converter.Convert(src, frame_count, packet.samples);

            repair.Push(std::move(packet), cb);

            if (check_invalidated(capture_client->ReleaseBuffer(frame_count),
                                   "capture_client->ReleaseBuffer")) break;
            if (check_invalidated(capture_client->GetNextPacketSize(&packet_frames),
                                   "capture_client->GetNextPacketSize")) break;
        }
        repair.EndPass(cb);
        LogRepairedRun(repair, kind, qpc_freq);
    }

    if (watcher) {
        enumerator->UnregisterEndpointNotificationCallback(watcher);
        watcher->Release();
    }

    if (client) client->Stop();
    CloseHandle(event);
    CoUninitialize();
}

bool AudioCapture::StartProcessLoopback(PcmCallback cb, const ProcessLoopbackTarget& target) {
    ready_or_failed_ = CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr);
    if (!ready_or_failed_) return false;

    running_.store(true);
    stream_lost_.store(false, std::memory_order_release);
    bool ok = false;
    thread_ = std::thread(&AudioCapture::RunProcessLoopback, this, std::move(cb), target, &ok);

    // Same wait-then-join-on-failure contract as Start(), except this path can fail in ordinary
    // operation (old driver, pre-20348 Windows build, activation refused) rather than only at
    // startup — so a false return here is expected to happen sometimes, not a die().
    WaitForSingleObject(ready_or_failed_, INFINITE);
    CloseHandle(ready_or_failed_);
    ready_or_failed_ = nullptr;

    if (!ok) {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }
    return ok;
}

void AudioCapture::RunProcessLoopback(PcmCallback cb, ProcessLoopbackTarget target, bool* ok) {
    Log::SetThreadName("audio-capture");
    *ok = false;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        SetEvent(ready_or_failed_);
        return;
    }

    HANDLE activate_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!activate_done) {
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    // Refcount starts at 1 (this function's own reference); ActivateAudioInterfaceAsync AddRefs it
    // for the duration of the async call, so `handler->Release()` below (once the wait confirms
    // ActivateCompleted has run) is the matching release for the reference created here.
    auto* handler = new ProcessLoopbackActivationHandler(activate_done);

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = target.target_pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        target.exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                        : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    // VT_BLOB pointing at a stack struct is fine here: ActivateAudioInterfaceAsync copies the
    // params synchronously before returning (documented behavior) — this PROPVARIANT is never
    // touched again after the call below, and must NOT be PropVariantClear()'d (that would attempt
    // to CoTaskMemFree a pointer to our own stack memory).
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    IActivateAudioInterfaceAsyncOperation* async_op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                               __uuidof(IAudioClient), &pv, handler, &async_op);
    if (FAILED(hr)) {
        Log::Warn("audio: ActivateAudioInterfaceAsync failed  hr=0x%08lX", static_cast<unsigned long>(hr));
        ReportAudioError("process", "ActivateAudioInterfaceAsync", hr);
        handler->Release();
        CloseHandle(activate_done);
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    WaitForSingleObject(activate_done, INFINITE);
    if (async_op) async_op->Release();
    CloseHandle(activate_done);

    if (FAILED(handler->ActivateHr()) || !handler->Client()) {
        Log::Warn("audio: process-loopback ActivateCompleted failed  hr=0x%08lX  client=%p",
                  static_cast<unsigned long>(handler->ActivateHr()),
                  static_cast<void*>(handler->Client()));
        ReportAudioError("process", "ActivateCompleted", handler->ActivateHr());
        handler->Release();
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    IAudioClient* client_raw = handler->Client();
    client_raw->AddRef();  // keep the client alive past handler->Release() below
    handler->Release();
    winrt::com_ptr<IAudioClient> client;
    client.attach(client_raw);

    // GetMixFormat()/IsFormatSupported() return E_NOTIMPL on the process-loopback virtual device
    // (backed by AudioSes!CMixerClient, which doesn't implement them) — confirmed against
    // Microsoft's own sample, whose comment suggesting GetMixFormat as an alternative is a known,
    // reported bug in that sample. Request 48000/2ch/s16 directly; this also means the format
    // converter below never actually resamples (needs_resample_ is false for this exact rate).
    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = static_cast<WORD>(AudioCapture::kChannels);
    fmt.nSamplesPerSec  = AudioCapture::kSampleRateHz;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    FormatConverter converter;
    (void)converter.Configure(&fmt);  // the fixed format above always reads
    negotiated_.sample_rate_hz  = converter.NegotiatedSampleRate();
    negotiated_.channels        = converter.NegotiatedChannels();
    negotiated_.bits_per_sample = converter.NegotiatedBits();
    negotiated_.is_float        = converter.NegotiatedIsFloat();
    negotiated_.was_resampled   = converter.IsResampling();

    // AUDCLNT_STREAMFLAGS_LOOPBACK is still required here even though the activation params already
    // select process-based loopback -- confirmed against Microsoft's own ApplicationLoopback sample
    // and real hardware this session: omitting it fails Initialize with
    // AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021).
    constexpr REFERENCE_TIME kBufferDuration = 200 * 10'000;  // 100ns units, same as Run()
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             kBufferDuration, 0, &fmt, nullptr);
    if (FAILED(hr)) {
        Log::Warn("audio: process-loopback IAudioClient::Initialize failed  hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        ReportAudioError("process", "IAudioClient::Initialize", hr);
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }
    if (FAILED(client->SetEventHandle(event))) {
        CloseHandle(event);
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    winrt::com_ptr<IAudioCaptureClient> capture_client;
    if (FAILED(client->GetService(IID_PPV_ARGS(capture_client.put())))) {
        CloseHandle(event);
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    const int64_t qpc_freq = QpcFrequency();
    if (FAILED(client->Start())) {
        CloseHandle(event);
        SetEvent(ready_or_failed_);
        CoUninitialize();
        return;
    }

    // A target with no render streams yields silence or no packets at all, not an error
    // (documented); the hub treats a source that delivers nothing as idle (audio_tracks.h).
    LoopbackStampRepair repair;
    SetRepairCapacity(repair, client.get(), negotiated_.sample_rate_hz, qpc_freq);

    *ok = true;
    SetEvent(ready_or_failed_);  // setup succeeded -- release the caller waiting in StartProcessLoopback()

    while (running_.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForSingleObject(event, 500);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;  // unexpected wait failure -- stop rather than die()

        // Asked on a timeout too: a dead stream stops signalling, so this is what notices it.
        UINT32 packet_frames = 0;
        if (FAILED(capture_client->GetNextPacketSize(&packet_frames))) break;
        if (wait == WAIT_TIMEOUT && packet_frames == 0) continue;
        repair.BeginPass(QpcNow());
        while (packet_frames != 0) {
            BYTE*  data        = nullptr;
            UINT32 frame_count = 0;
            DWORD  flags       = 0;
            UINT64 device_position = 0;
            UINT64 qpc_100ns       = 0;
            if (FAILED(capture_client->GetBuffer(&data, &frame_count, &flags, &device_position,
                                                  &qpc_100ns)))
                break;

            PcmPacket packet;
            packet.qpc_ticks     = Qpc100nsToRawTicks(qpc_100ns, qpc_freq);
            packet.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            packet.discontinuity |= (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;

            const uint8_t* src = (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                                     ? nullptr
                                     : reinterpret_cast<const uint8_t*>(data);
            converter.Convert(src, frame_count, packet.samples);
            repair.Push(std::move(packet), cb);

            capture_client->ReleaseBuffer(frame_count);
            if (FAILED(capture_client->GetNextPacketSize(&packet_frames))) break;
        }
        repair.EndPass(cb);
        LogRepairedRun(repair, "process loopback", qpc_freq);
    }
    if (running_.load(std::memory_order_relaxed)) stream_lost_.store(true, std::memory_order_release);

    client->Stop();
    CloseHandle(event);
    CoUninitialize();
}

DWORD FindTargetRootPid(const std::vector<ProcessEntry>& processes, const std::wstring& exe) {
    if (exe.empty()) return 0;
    auto matches = [&](const ProcessEntry& p) { return _wcsicmp(p.exe.c_str(), exe.c_str()) == 0; };
    DWORD first = 0;
    for (const auto& p : processes) {
        if (!matches(p)) continue;
        if (first == 0) first = p.pid;
        bool parent_matches = false;
        for (const auto& q : processes) {
            if (q.pid == p.parent_pid && q.pid != p.pid && matches(q)) { parent_matches = true; break; }
        }
        if (!parent_matches) return p.pid;
    }
    return first;  // every match has a same-named parent (PID reuse): any match beats none
}

struct OutputActivityProbe::Impl {
    // Peak (0..1, post-mix) above which an output counts as audible: about -60 dBFS, clear of the
    // noise floor a powered-but-idle USB DAC can show, well under quiet speech.
    static constexpr float kAudiblePeak = 0.001f;
    // The endpoint list is re-read this often so a device plugged in (earphones) is noticed.
    static constexpr ULONGLONG kRefreshMs = 5000;

    struct Endpoint {
        winrt::com_ptr<IAudioMeterInformation> meter;     // render endpoints only
        winrt::com_ptr<IAudioSessionManager2>  sessions;
    };
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    std::vector<Endpoint>               endpoints;
    ULONGLONG                           refreshed_ms = 0;
    bool                                refreshed    = false;
    const DWORD                         own_pid      = GetCurrentProcessId();

    bool Refresh() {
        if (!enumerator &&
            FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(enumerator.put()))))
            return false;
        std::vector<Endpoint> fresh;
        for (const EDataFlow flow : {eRender, eCapture}) {
            winrt::com_ptr<IMMDeviceCollection> devices;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.put()))) return false;
            UINT count = 0;
            if (FAILED(devices->GetCount(&count))) return false;
            for (UINT i = 0; i < count; ++i) {
                winrt::com_ptr<IMMDevice> device;
                Endpoint                  ep;
                if (FAILED(devices->Item(i, device.put()))) return false;
                if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(ep.sessions.put()))))
                    return false;
                if (flow == eRender &&
                    FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(ep.meter.put()))))
                    return false;
                fresh.push_back(std::move(ep));
            }
        }
        endpoints    = std::move(fresh);
        refreshed_ms = GetTickCount64();
        refreshed    = true;
        return true;
    }

    // Another process has a running stream on this endpoint. A call holds one open even while
    // nobody speaks, which the peak meter alone would read as silence.
    bool OtherProcessStreaming(IAudioSessionManager2* sessions) const {
        winrt::com_ptr<IAudioSessionEnumerator> list;
        if (FAILED(sessions->GetSessionEnumerator(list.put()))) return true;
        int count = 0;
        if (FAILED(list->GetCount(&count))) return true;
        for (int i = 0; i < count; ++i) {
            winrt::com_ptr<IAudioSessionControl> control;
            if (FAILED(list->GetSession(i, control.put()))) return true;
            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(control->GetState(&state))) return true;
            if (state != AudioSessionStateActive) continue;
            auto control2 = control.try_as<IAudioSessionControl2>();
            DWORD pid = 0;
            if (!control2 || FAILED(control2->GetProcessId(&pid))) return true;
            if (pid != own_pid) return true;  // our own capture sessions don't count
        }
        return false;
    }
};

OutputActivityProbe::OutputActivityProbe() : impl_(new Impl) {}
OutputActivityProbe::~OutputActivityProbe() { delete impl_; }

bool OutputActivityProbe::Audible() {
    Impl& s = *impl_;
    if (!s.refreshed || GetTickCount64() - s.refreshed_ms >= Impl::kRefreshMs) {
        if (!s.Refresh()) { s.refreshed = false; return true; }  // can't look: behave as before the gate existed
    }
    for (const auto& ep : s.endpoints) {
        if (ep.meter) {
            float peak = 0.f;
            if (FAILED(ep.meter->GetPeakValue(&peak))) { s.refreshed = false; return true; }
            if (peak > Impl::kAudiblePeak) return true;
        }
        if (s.OtherProcessStreaming(ep.sessions.get())) return true;
    }
    return false;
}

AudioTargetCapture WantedTargetCapture(const std::string& mode, DWORD target_pid) {
    if (mode == "all_except") {
        return target_pid ? AudioTargetCapture::kAllExceptTarget : AudioTargetCapture::kAllDesktop;
    }
    if (mode == "only" || mode == "game_only") {
        return target_pid ? AudioTargetCapture::kTargetOnly : AudioTargetCapture::kNothing;
    }
    return AudioTargetCapture::kAllDesktop;
}

AudioTargetCapture WantedSourceCapture(bool enabled, const std::string& kind, const std::string& mode,
                                       DWORD target_pid) {
    if (!enabled) return AudioTargetCapture::kNothing;
    if (kind == "input") return AudioTargetCapture::kAllDesktop;
    return WantedTargetCapture(mode, target_pid);
}

bool AudioSourceHasTarget(const std::string& kind, const std::string& mode) {
    return kind == "output_loopback" && (mode == "only" || mode == "all_except" || mode == "game_only");
}

const char* AudioTargetCaptureName(AudioTargetCapture capture) {
    switch (capture) {
        case AudioTargetCapture::kNothing:         return "nothing";
        case AudioTargetCapture::kTargetOnly:      return "target_only";
        case AudioTargetCapture::kAllExceptTarget: return "all_except_target";
        case AudioTargetCapture::kAllDesktop:      return "all_desktop_audio";
    }
    return "unknown";
}

TargetWatcherDecision DecideTargetWatcherAction(const TargetWatcherState& cur, AudioTargetCapture want,
                                                 DWORD want_pid, int poll, int snapshot_every_polls) {
    if (cur.started && cur.capture == want && cur.pid == want_pid) return {false, false};
    const bool retrying = want_pid != 0 && cur.failed_pid == want_pid;
    if (retrying && poll % snapshot_every_polls != 0) return {false, true};
    return {true, retrying};
}

TargetWatcherState NextTargetWatcherState(AudioTargetCapture want, DWORD want_pid, bool activation_failed) {
    using C = AudioTargetCapture;
    C     got        = want;
    DWORD failed_pid = 0;
    if ((want == C::kTargetOnly || want == C::kAllExceptTarget) && activation_failed) {
        got        = want == C::kAllExceptTarget ? C::kAllDesktop : C::kNothing;
        failed_pid = want_pid;
    }
    return TargetWatcherState{true, got, got == want ? want_pid : 0, failed_pid};
}

void LoopbackStampRepair::SetCapacity(int64_t capacity_ticks, int64_t qpc_freq) {
    qpc_freq_ = qpc_freq;
    // 100 ms of margin over the buffer: a true stamp never gets near it, even after an overflow.
    // Unknown capacity turns the repair off rather than guessing.
    max_age_ticks_ = capacity_ticks > 0 ? capacity_ticks + qpc_freq / 10 : 0;
}

void LoopbackStampRepair::BeginPass(int64_t now_qpc) {
    pass_qpc_       = now_qpc;
    pass_had_stale_ = false;
}

void LoopbackStampRepair::Push(PcmPacket&& packet, const Emit& emit) {
    if (max_age_ticks_ > 0 && pass_qpc_ - packet.qpc_ticks > max_age_ticks_) {
        if (!run_active_) {
            run_active_         = true;
            run_                = {};
            run_.raw_age_ticks  = pass_qpc_ - packet.qpc_ticks;
            run_first_pass_qpc_ = pass_qpc_;
        }
        if (!pass_had_stale_) {
            pass_had_stale_  = true;
            run_.passes     += 1;
            run_.span_ticks  = pass_qpc_ - run_first_pass_qpc_;
        }
        held_.push_back(std::move(packet));
        return;
    }
    if (!held_.empty()) Flush((std::min)(pass_qpc_, packet.qpc_ticks), emit);
    if (run_active_) FinishRun();
    emit(packet);
}

void LoopbackStampRepair::EndPass(const Emit& emit) {
    if (!held_.empty()) Flush(pass_qpc_, emit);
    if (run_active_ && !pass_had_stale_) FinishRun();
}

void LoopbackStampRepair::Flush(int64_t end_qpc, const Emit& emit) {
    int64_t total = 0;
    for (const PcmPacket& p : held_) total += static_cast<int64_t>(p.samples.size() / 2);
    const int64_t start = end_qpc - total * qpc_freq_ / 48000;
    int64_t       done  = 0;
    for (PcmPacket& p : held_) {
        p.qpc_ticks = start + done * qpc_freq_ / 48000;
        done += static_cast<int64_t>(p.samples.size() / 2);
        emit(p);
    }
    run_.frames  += total;
    run_.packets += static_cast<int>(held_.size());
    held_.clear();
}

void LoopbackStampRepair::FinishRun() {
    run_active_   = false;
    finished_     = true;
    finished_run_ = run_;
}

bool LoopbackStampRepair::TakeFinishedRun(Run& out) {
    if (!finished_) return false;
    finished_ = false;
    out       = finished_run_;
    return true;
}

const char* AudioSourceStateName(AudioSourceState state) {
    switch (state) {
        case AudioSourceState::kLive:    return "live";
        case AudioSourceState::kQuiet:   return "quiet";
        case AudioSourceState::kWaiting: return "waiting";
        case AudioSourceState::kNoGame:  return "no_game";
        case AudioSourceState::kFailed:  return "failed";
        case AudioSourceState::kOff:     return "off";
        case AudioSourceState::kPaused:  return "paused";
    }
    return "quiet";
}

AudioSourceState ClassifyAudioSource(bool enabled, const std::string& mode, const TargetWatcherState* watch,
                                     int64_t last_audible_qpc, int64_t now_qpc, int64_t qpc_freq) {
    if (!enabled) return AudioSourceState::kOff;
    if (watch) {
        // all_except falls back to the whole desktop on a failed activation, so it records, but
        // not what was asked: that's a failure too.
        if (watch->failed_pid != 0) return AudioSourceState::kFailed;
        if (!watch->started || watch->capture == AudioTargetCapture::kNothing)
            return mode == "game_only" ? AudioSourceState::kNoGame : AudioSourceState::kWaiting;
    }
    const auto hold = static_cast<int64_t>(kAudibleHoldSeconds * static_cast<double>(qpc_freq));
    if (last_audible_qpc > 0 && now_qpc - last_audible_qpc <= hold) return AudioSourceState::kLive;
    return AudioSourceState::kQuiet;
}

void AudioSourceBoard::Init(std::vector<Entry> entries, std::vector<TargetWatcherState> watch) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(entries);
    watch_   = std::move(watch);
    watch_.resize(entries_.size());
}

void AudioSourceBoard::SetWatch(size_t source, const TargetWatcherState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source < watch_.size()) watch_[source] = state;
}

void AudioSourceBoard::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

std::vector<AudioSourceBoard::Status> AudioSourceBoard::Snapshot(
    const std::function<int64_t(size_t)>& last_audible_qpc, int64_t now_qpc, int64_t qpc_freq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Status> out;
    out.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        AudioSourceState state = ClassifyAudioSource(e.enabled, e.mode, e.watched ? &watch_[i] : nullptr,
                                                     last_audible_qpc(e.slot), now_qpc, qpc_freq);
        // "No game running" still says the true thing; "Quiet" or "Not running" wouldn't.
        if (paused_ && state != AudioSourceState::kOff && state != AudioSourceState::kNoGame)
            state = AudioSourceState::kPaused;
        out.push_back({e.id, state});
    }
    return out;
}

std::vector<AudioSessionInfo> ListActiveAudioSessions() {
    std::vector<AudioSessionInfo> out;

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(enumerator.put()))))
        return out;

    winrt::com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) return out;

    winrt::com_ptr<IAudioSessionManager2> session_mgr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(session_mgr.put()))))
        return out;

    winrt::com_ptr<IAudioSessionEnumerator> session_enum;
    if (FAILED(session_mgr->GetSessionEnumerator(session_enum.put()))) return out;

    int count = 0;
    if (FAILED(session_enum->GetCount(&count))) return out;

    std::vector<DWORD> seen_pids;
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IAudioSessionControl> control;
        if (FAILED(session_enum->GetSession(i, control.put())) || !control) continue;

        auto control2 = control.try_as<IAudioSessionControl2>();
        if (!control2) continue;

        // IsSystemSoundsSession() returns S_OK (is one) / S_FALSE (isn't) -- not a BOOL out-param.
        if (control2->IsSystemSoundsSession() == S_OK) continue;

        // GetState() distinguishes a session that's actually rendering right now (Active) from one
        // that's merely registered (Inactive: opened a stream but isn't playing -- many apps do
        // this eagerly at startup for notification sounds) or orphaned (Expired: owning process
        // exited but WASAPI hasn't dropped the session yet). This call backs the "live now" card
        // list (header comment above), so only Active belongs here -- Inactive/Expired is exactly
        // the closed-app and never-made-sound clutter this list shouldn't show. A muted-but-running
        // app the user wants to pre-group still reaches its group via the separate "Add app"
        // picker (ListRunningProcessesUtf8, main.cpp), not this call.
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(control2->GetState(&state)) || state != AudioSessionStateActive) continue;

        DWORD pid = 0;
        if (FAILED(control2->GetProcessId(&pid)) || pid == 0) continue;
        if (std::find(seen_pids.begin(), seen_pids.end(), pid) != seen_pids.end()) continue;
        seen_pids.push_back(pid);

        AudioSessionInfo info;
        info.pid = pid;
        if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t image[MAX_PATH]{};
            DWORD   len = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, image, &len)) {
                std::wstring image_w(image, len);
                const size_t slash = image_w.find_last_of(L'\\');
                info.exe_name = Utf8FromWideLocal(slash == std::wstring::npos ? image_w : image_w.substr(slash + 1));
                info.path     = Utf8FromWideLocal(image_w);
                const std::wstring friendly = ReadVersionResourceName(image_w);
                info.display_name = friendly.empty() ? info.exe_name : Utf8FromWideLocal(friendly);
            }
            CloseHandle(proc);
        }
        // Same self-exclusion ListRunningProcessesUtf8 (main.cpp) applies to the "Add app" picker --
        // ombra-ui.exe's own toast/UI sounds otherwise show up as a "Live now" draggable card, and
        // dropping it onto another card synthesizes a real output_loopback/mode:only source that
        // records Ombra's own audio session.
        if (_stricmp(info.exe_name.c_str(), OMBRA_ENGINE_EXE) == 0 ||
            _stricmp(info.exe_name.c_str(), OMBRA_UI_EXE) == 0)
            continue;
        out.push_back(std::move(info));
    }

    return out;
}

std::vector<AudioDeviceInfo> ListAudioDevices(bool input) {
    std::vector<AudioDeviceInfo> out;
    const EDataFlow flow = input ? eCapture : eRender;

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(enumerator.put()))))
        return out;

    // Same eConsole role AudioCapture::Run/ListActiveAudioSessions use for "the" default render
    // endpoint -- eMultimedia/eCommunications can legitimately point elsewhere, but this project has
    // never distinguished them (nothing documents roles), so eConsole is the one "default" the
    // picker marks.
    std::wstring default_id;
    if (winrt::com_ptr<IMMDevice> default_device;
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, default_device.put()))) {
        wchar_t* id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id)) && id) {
            default_id = id;
            CoTaskMemFree(id);
        }
    }

    winrt::com_ptr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())))
        return out;

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return out;

    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put())) || !device) continue;

        wchar_t* id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;
        AudioDeviceInfo info;
        info.id         = Utf8FromWideLocal(id);
        info.is_default = !default_id.empty() && default_id == id;
        CoTaskMemFree(id);

        winrt::com_ptr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT name_var;
            PropVariantInit(&name_var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name_var)) &&
                name_var.vt == VT_LPWSTR && name_var.pwszVal) {
                info.name = Utf8FromWideLocal(name_var.pwszVal);
            }
            PropVariantClear(&name_var);
        }
        if (info.name.empty()) info.name = info.id;  // never show a blank row

        out.push_back(std::move(info));
    }

    return out;
}

}  // namespace ombra
