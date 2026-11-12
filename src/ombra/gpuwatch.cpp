#include "ombra/gpuwatch.h"
#include "ombra/encode_target.h"
#include "ombra/gputopo.h"
#include "ombra/log.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace ombra {

namespace {
constexpr size_t kMaxInstances     = 512;
// Two collects this far apart, once per cycle -- short enough that the watcher thread's own
// footprint stays negligible, long enough for PDH's rate counters to produce a real number.
constexpr DWORD  kSampleGapMs      = 300;
constexpr auto   kCycleInterval    = std::chrono::milliseconds(2000);
}  // namespace

GpuLoadWatcher::GpuLoadWatcher() {
    thread_ = std::thread(&GpuLoadWatcher::Run, this);
}

GpuLoadWatcher::~GpuLoadWatcher() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

double GpuLoadWatcher::Utilization3D(DWORD pid) const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    auto it = snapshot_.find(pid);
    return it == snapshot_.end() ? 0.0 : it->second.util_3d;
}

bool GpuLoadWatcher::RenderAdapter(DWORD pid, LUID* luid) const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    auto it = snapshot_.find(pid);
    if (it == snapshot_.end() || it->second.busiest_util <= 0) return false;
    *luid = it->second.busiest;
    return true;
}

void GpuLoadWatcher::Run() {
    Log::SetThreadName("gpuwatch");
    while (running_.load(std::memory_order_acquire)) {
        const auto cycle_start = std::chrono::steady_clock::now();

        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            std::vector<PDH_HCOUNTER> counters;
            std::vector<std::wstring> instance_paths;

            std::vector<wchar_t> path_buf(8192);
            DWORD                path_len = static_cast<DWORD>(path_buf.size());
            PDH_STATUS           expand_st;
            for (;;) {
                expand_st = PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                                                    path_buf.data(), &path_len, 0);
                if (expand_st == PDH_MORE_DATA && path_buf.size() < (1u << 22)) {
                    path_buf.resize(path_len);
                    continue;
                }
                break;
            }
            if (expand_st == ERROR_SUCCESS) {
                for (wchar_t* p = path_buf.data(); *p && instance_paths.size() < kMaxInstances;) {
                    std::wstring instance_path(p);
                    // Only engtype_3D instances matter for "is this process rendering" -- skip the
                    // rest so a busy Copy/VideoDecode-only process doesn't read as a game.
                    if (instance_path.find(L"engtype_3d") != std::wstring::npos) {
                        PDH_HCOUNTER c = nullptr;
                        if (PdhAddEnglishCounterW(query, instance_path.c_str(), 0, &c) == ERROR_SUCCESS) {
                            counters.push_back(c);
                            instance_paths.push_back(instance_path);
                        }
                    }
                    p += instance_path.size() + 1;
                }
            }

            if (!instance_paths.empty()) {
                PdhCollectQueryData(query);
                Sleep(kSampleGapMs);
                PdhCollectQueryData(query);

                std::unordered_map<DWORD, Sample> fresh;
                struct PerAdapter {
                    DWORD  pid;
                    LUID   luid;
                    double util;
                };
                std::vector<PerAdapter> per_adapter;
                for (size_t i = 0; i < instance_paths.size(); ++i) {
                    PDH_FMT_COUNTERVALUE val{};
                    double                util = 0;
                    if (PdhGetFormattedCounterValue(counters[i], PDH_FMT_DOUBLE, nullptr, &val) ==
                            ERROR_SUCCESS &&
                        val.CStatus == PDH_CSTATUS_VALID_DATA) {
                        util = val.doubleValue;
                    }

                    const size_t pid_pos = instance_paths[i].find(L"pid_");
                    if (pid_pos == std::wstring::npos) continue;
                    wchar_t* end = nullptr;
                    unsigned long pid = wcstoul(instance_paths[i].c_str() + pid_pos + 4, &end, 10);
                    if (!end || pid == 0) continue;

                    fresh[static_cast<DWORD>(pid)].util_3d += util;
                    LUID luid{};
                    if (!ParseEngineInstanceLuid(instance_paths[i], &luid)) continue;
                    auto same = std::find_if(per_adapter.begin(), per_adapter.end(), [&](const PerAdapter& e) {
                        return e.pid == pid && SameLuid(e.luid, luid);
                    });
                    if (same == per_adapter.end()) per_adapter.push_back(PerAdapter{static_cast<DWORD>(pid), luid, util});
                    else same->util += util;
                }
                for (const PerAdapter& e : per_adapter) {
                    Sample& s = fresh[e.pid];
                    if (e.util > s.busiest_util) {
                        s.busiest      = e.luid;
                        s.busiest_util = e.util;
                    }
                }

                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_ = std::move(fresh);
            }

            PdhCloseQuery(query);
        }

        const auto elapsed = std::chrono::steady_clock::now() - cycle_start;
        const auto remaining = kCycleInterval - elapsed;
        // Sleep in short slices so Stop() (running_ = false) is picked up promptly rather than
        // blocking a full cycle interval on shutdown.
        auto slept = std::chrono::milliseconds(0);
        while (slept < remaining && running_.load(std::memory_order_acquire)) {
            auto slice = (std::min)(std::chrono::milliseconds(200),
                                     std::chrono::duration_cast<std::chrono::milliseconds>(remaining - slept));
            if (slice.count() <= 0) break;
            std::this_thread::sleep_for(slice);
            slept += slice;
        }
    }
}

}  // namespace ombra
