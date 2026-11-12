#include "ombra-ui/audio_levels.h"

#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <utility>

namespace ombra_ui {

namespace {

LevelReaderFactory& Factory() {
    static LevelReaderFactory f;
    return f;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring ExeName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
    CloseHandle(h);
    if (!ok) return {};
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

class WasapiLevelReader final : public LevelReader {
public:
    WasapiLevelReader() {
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         enumerator_.put_void());
    }

    void Read(const std::vector<LevelTarget>& targets, std::vector<float>* peaks) override {
        peaks->assign(targets.size(), 0.0f);
        if (!enumerator_) return;
        const ULONGLONG now = GetTickCount64();
        // Sessions come and go as apps start and stop playing: look again once a second, not per read.
        if (now - last_scan_ms_ >= 1000) {
            last_scan_ms_ = now;
            exe_names_.clear();  // a pid can be reused
        }
        for (size_t i = 0; i < targets.size(); ++i) {
            const LevelTarget& t = targets[i];
            Endpoint& ep = EndpointFor(t, now);
            if (!ep.device) continue;
            float peak = 0.0f;
            if (t.app.empty()) {
                if (ep.meter) ep.meter->GetPeakValue(&peak);
            } else {
                if (ep.scanned_ms != last_scan_ms_) ScanSessions(ep);  // once per endpoint per rescan
                if (const auto it = ep.apps.find(Lower(Wide(t.app))); it != ep.apps.end())
                    for (const auto& m : it->second) {
                        float p = 0.0f;
                        if (SUCCEEDED(m->GetPeakValue(&p))) peak = std::max(peak, p);
                    }
            }
            (*peaks)[i] = peak;
        }
    }

private:
    struct Endpoint {
        winrt::com_ptr<IMMDevice> device;
        winrt::com_ptr<IAudioMeterInformation> meter;
        winrt::com_ptr<IAudioSessionManager2> sessions;
        // Each app's session meters by lower-case exe name, from one enumeration per rescan.
        std::map<std::wstring, std::vector<winrt::com_ptr<IAudioMeterInformation>>> apps;
        ULONGLONG scanned_ms = 0;  // the rescan it belongs to
        ULONGLONG opened_ms = 0;
    };

    // One per (flow, device). A default endpoint is looked up again on rescan, so it follows Windows.
    Endpoint& EndpointFor(const LevelTarget& t, ULONGLONG now) {
        const bool follows_default = t.device.empty() || t.device == "default";
        const std::string key = (t.input ? "in:" : "out:") + (follows_default ? std::string() : t.device);
        Endpoint& ep = endpoints_[key];
        if (ep.device && !(follows_default && now - ep.opened_ms >= 1000)) return ep;
        winrt::com_ptr<IMMDevice> device;
        // A pinned device that's gone reads the default, as the engine records it (OpenStream).
        if (!follows_default) enumerator_->GetDevice(Wide(t.device).c_str(), device.put());
        if (!device) enumerator_->GetDefaultAudioEndpoint(t.input ? eCapture : eRender, eConsole, device.put());
        ep.opened_ms = now;
        if (ep.device && device && SameDevice(ep.device.get(), device.get())) return ep;
        ep = Endpoint{};
        ep.opened_ms = now;
        ep.device = device;
        if (!device) return ep;
        device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, ep.meter.put_void());
        if (!t.input) device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, ep.sessions.put_void());
        return ep;
    }

    static bool SameDevice(IMMDevice* a, IMMDevice* b) {
        LPWSTR ia = nullptr, ib = nullptr;
        const bool same = SUCCEEDED(a->GetId(&ia)) && SUCCEEDED(b->GetId(&ib)) && std::wcscmp(ia, ib) == 0;
        CoTaskMemFree(ia);
        CoTaskMemFree(ib);
        return same;
    }

    void ScanSessions(Endpoint& ep) {
        ep.scanned_ms = last_scan_ms_;
        ep.apps.clear();
        if (!ep.sessions) return;
        winrt::com_ptr<IAudioSessionEnumerator> list;
        int count = 0;
        if (FAILED(ep.sessions->GetSessionEnumerator(list.put())) || FAILED(list->GetCount(&count))) return;
        for (int i = 0; i < count; ++i) {
            winrt::com_ptr<IAudioSessionControl> control;
            if (FAILED(list->GetSession(i, control.put()))) continue;
            AudioSessionState state = AudioSessionStateExpired;
            if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) continue;
            auto control2 = control.try_as<IAudioSessionControl2>();
            DWORD pid = 0;
            if (!control2 || FAILED(control2->GetProcessId(&pid)) || pid == 0) continue;
            auto it = exe_names_.find(pid);
            if (it == exe_names_.end()) it = exe_names_.emplace(pid, Lower(ExeName(pid))).first;
            if (it->second.empty()) continue;
            if (auto meter = control.try_as<IAudioMeterInformation>()) ep.apps[it->second].push_back(std::move(meter));
        }
    }

    winrt::com_ptr<IMMDeviceEnumerator> enumerator_;
    std::map<std::string, Endpoint> endpoints_;
    std::map<DWORD, std::wstring> exe_names_;
    ULONGLONG last_scan_ms_ = 0;
};

}  // namespace

void SetLevelReaderFactory(LevelReaderFactory factory) { Factory() = std::move(factory); }

std::unique_ptr<LevelReader> MakeLevelReader() {
    return Factory() ? Factory()() : nullptr;
}

std::unique_ptr<LevelReader> MakeWasapiLevelReader() { return std::make_unique<WasapiLevelReader>(); }

float PeakToDb(float linear) {
    return linear > 0.0f ? 20.0f * std::log10(linear) : -INFINITY;
}

}  // namespace ombra_ui
