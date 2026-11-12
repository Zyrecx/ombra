#include "ombra/telemetry.h"
#include "ombra/brand.h"

#include "ombra/log.h"
#include "ombra/paths.h"

#include <knownfolders.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <mutex>
#include <share.h>
#include <vector>

namespace ombra {

namespace {

constexpr uint64_t kMaxTotalBytes = 50ull * 1024 * 1024;
// Utilization counters are rates: PDH needs two collects this far apart to produce a value.
constexpr DWORD    kGpuSampleGapMs = 500;
constexpr auto     kPollInterval   = std::chrono::seconds(1);

constexpr const char* kSamplesHeader =
    "time,uptime_s,interval_s,"
    "private_mb,ws_mb,handles,threads,gdi,user,cpu_pct,sampler_cpu_ms,sys_cpu_pct,sys_mem_load_pct,"
    "gpu_3d_pct,gpu_encode_pct,gpu_dedicated_mb,gpu_shared_mb,"
    "ui_processes,ui_private_mb,ui_ws_mb,ui_handles,ui_threads,ui_gdi,ui_user,ui_gpu_3d_pct,"
    "ui_gpu_dedicated_mb,"
    "fg_exe,fg_gpu_3d_pct,armed,game,game_gpu_3d_pct,"
    "pipelines,video_ring_mb,video_ring_committed_mb,video_ring_capacity_mb,audio_sources,audio_ring_mb,"
    "ticks,duplicate_ticks,late_ticks,resyncs,encode_p50_ms,encode_p99_ms,encode_max_ms,"
    "wait_overshoot_p99_ms,rebuilds,resolution_changes,saves,"
    "audio_dropped_ms,audio_late_ms,audio_encoder_errors";

constexpr const char* kEventsHeader = "time,uptime_s,type,detail";

std::atomic<bool> g_enabled{false};
std::atomic<double> g_last_dedicated_mb{-1.0};

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::string Format(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += (c == '\r' || c == '\n') ? ' ' : c;
    }
    return out + "\"";
}

std::string Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Format("%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
}

uint64_t FileTimeTo100ns(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

double UptimeSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return static_cast<double>(FileTimeTo100ns(now) - FileTimeTo100ns(created)) / 1e7;
}

// Deletes the oldest telemetry CSVs until the directory is under kMaxTotalBytes. Names end in
// YYYYMMDD.csv, so sorting on the date part orders both file kinds chronologically together.
void PruneDirectory(const std::wstring& dir, const std::wstring& today_suffix) {
    struct Entry { std::wstring name; std::wstring date; uint64_t size; };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.csv").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        const size_t dash = name.find(L'-');
        entries.push_back({name, dash == std::wstring::npos ? name : name.substr(dash + 1),
                           (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.date < b.date; });
    uint64_t total = 0;
    for (const auto& e : entries) total += e.size;
    for (const auto& e : entries) {
        if (total <= kMaxTotalBytes) break;
        if (e.date == today_suffix) continue;
        if (DeleteFileW((dir + L"\\" + e.name).c_str())) total -= e.size;
    }
}

// One CSV per local day, reopened (with a header when new) on the first write after midnight.
class DailyCsv {
public:
    DailyCsv(const wchar_t* prefix, const char* header) : prefix_(prefix), header_(header) {}
    ~DailyCsv() { Close(); }

    void WriteRow(const std::string& row) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const int day = st.wYear * 10000 + st.wMonth * 100 + st.wDay;
        if (!file_ || day != day_) Open(day);
        if (!file_) return;
        std::fputs(row.c_str(), file_);
        std::fputc('\n', file_);
        std::fflush(file_);
    }

    void Close() {
        if (file_) std::fclose(file_);
        file_ = nullptr;
    }

private:
    void Open(int day) {
        Close();
        day_ = day;
        const std::wstring dir = Telemetry::Directory();
        if (dir.empty()) return;
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        wchar_t suffix[16];
        swprintf_s(suffix, L"%08d.csv", day);
        PruneDirectory(dir, suffix);
        const std::wstring path = dir + L"\\" + prefix_ + L"-" + suffix;
        SetAsideIfOtherHeader(path, day);
        // _wfsopen with _SH_DENYNO, not _wfopen_s: _wfopen_s opens non-shareable, which locks
        // everyone else (including a debugging session) out of the file while the engine runs.
        file_ = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
        if (!file_) {
            Log::WarnW(L"telemetry: could not open %s", path.c_str());
            return;
        }
        std::fseek(file_, 0, SEEK_END);
        if (std::ftell(file_) == 0) {
            std::fputs(header_, file_);
            std::fputc('\n', file_);
        }
    }

    // Today's file written by a build with other columns is renamed to <prefix>-<day>-<n>.csv, so
    // one file never mixes two layouts. The '-' sorts it before today's file for pruning.
    void SetAsideIfOtherHeader(const std::wstring& path, int day) {
        FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
        if (!f) return;
        const std::string expected = std::string(header_) + "\n";  // Open writes it in binary mode
        std::string       first(expected.size(), '\0');
        const size_t      got = std::fread(first.data(), 1, first.size(), f);
        std::fclose(f);
        if (got == 0 || (got == first.size() && first == expected)) return;
        for (int n = 1; n < 100; ++n) {
            wchar_t name[64];
            swprintf_s(name, L"-%08d-%d.csv", day, n);
            const std::wstring aside = Telemetry::Directory() + L"\\" + prefix_ + name;
            if (MoveFileW(path.c_str(), aside.c_str())) return;
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) return;
        }
    }

    const wchar_t* prefix_;
    const char*    header_;
    FILE*          file_ = nullptr;
    int            day_  = 0;
};

std::mutex g_files_mutex;
DailyCsv   g_samples_csv(L"samples", kSamplesHeader);
DailyCsv   g_events_csv(L"events", kEventsHeader);

struct ProcessStats {
    double private_mb = 0, ws_mb = 0;
    DWORD  handles = 0, gdi = 0, user = 0;
};

ProcessStats ReadProcessStats(HANDLE process) {
    ProcessStats s;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        s.ws_mb      = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    GetProcessHandleCount(process, &s.handles);
    s.gdi  = GetGuiResources(process, GR_GDIOBJECTS);
    s.user = GetGuiResources(process, GR_USEROBJECTS);
    return s;
}

std::wstring ExeBaseName(DWORD pid) {
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t image[MAX_PATH]{};
        DWORD   len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, image, &len)) {
            name.assign(image, len);
            const size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name.erase(0, slash + 1);
        }
        CloseHandle(h);
    }
    return name;
}

struct GpuUsage {
    double util_3d = 0, util_encode = 0, dedicated_mb = 0, shared_mb = 0;
};

std::vector<std::wstring> ExpandCounterPath(const wchar_t* wildcard) {
    std::vector<wchar_t> buf(16384);
    DWORD len = static_cast<DWORD>(buf.size());
    PDH_STATUS st;
    for (;;) {
        st = PdhExpandWildCardPathW(nullptr, wildcard, buf.data(), &len, 0);
        if (st == PDH_MORE_DATA && buf.size() < (1u << 22)) {
            buf.resize(len);
            continue;
        }
        break;
    }
    std::vector<std::wstring> out;
    if (st != ERROR_SUCCESS) return out;
    for (wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

// Per-process GPU engine utilization and GPU memory, the same PDH counters Task Manager reads.
// One short-lived query per sample: at a 30 s interval that's cheaper than holding one open.
std::map<DWORD, GpuUsage> SampleGpu(const std::vector<DWORD>& pids) {
    std::map<DWORD, GpuUsage> out;
    if (pids.empty()) return out;
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return out;

    enum class Kind { k3d, kEncode, kDedicated, kShared };
    struct Counter { PDH_HCOUNTER handle; DWORD pid; Kind kind; };
    std::vector<Counter> counters;

    auto add_matching = [&](const wchar_t* wildcard, bool is_engine) {
        for (const std::wstring& path : ExpandCounterPath(wildcard)) {
            std::wstring lower = path;
            for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
            const size_t pid_pos = lower.find(L"pid_");
            if (pid_pos == std::wstring::npos) continue;
            const DWORD pid = static_cast<DWORD>(wcstoul(lower.c_str() + pid_pos + 4, nullptr, 10));
            if (std::find(pids.begin(), pids.end(), pid) == pids.end()) continue;

            Kind kind;
            if (is_engine) {
                if (lower.find(L"engtype_3d") != std::wstring::npos) kind = Kind::k3d;
                // NVIDIA names its encoder "VideoEncode"; AMD's VCN is one "Video Codec N" engine
                // for encode and decode. Per-PID, and the engine never decodes, so it's encode load.
                else if (lower.find(L"engtype_videoencode") != std::wstring::npos ||
                         lower.find(L"engtype_video codec") != std::wstring::npos) kind = Kind::kEncode;
                else continue;
            } else {
                kind = lower.find(L"dedicated usage") != std::wstring::npos ? Kind::kDedicated
                                                                             : Kind::kShared;
            }
            PDH_HCOUNTER handle = nullptr;
            if (PdhAddEnglishCounterW(query, path.c_str(), 0, &handle) == ERROR_SUCCESS)
                counters.push_back({handle, pid, kind});
        }
    };
    add_matching(L"\\GPU Engine(*)\\Utilization Percentage", true);
    add_matching(L"\\GPU Process Memory(*)\\Dedicated Usage", false);
    add_matching(L"\\GPU Process Memory(*)\\Shared Usage", false);

    if (!counters.empty()) {
        PdhCollectQueryData(query);
        Sleep(kGpuSampleGapMs);
        PdhCollectQueryData(query);
        for (const Counter& c : counters) {
            PDH_FMT_COUNTERVALUE value{};
            if (PdhGetFormattedCounterValue(c.handle, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS ||
                value.CStatus != PDH_CSTATUS_VALID_DATA)
                continue;
            GpuUsage& u = out[c.pid];
            switch (c.kind) {
                case Kind::k3d:        u.util_3d += value.doubleValue; break;
                case Kind::kEncode:    u.util_encode += value.doubleValue; break;
                case Kind::kDedicated: u.dedicated_mb += value.doubleValue / (1024.0 * 1024.0); break;
                case Kind::kShared:    u.shared_mb += value.doubleValue / (1024.0 * 1024.0); break;
            }
        }
    }
    PdhCloseQuery(query);
    return out;
}

// State carried between sample rows: CPU clocks and the previous cumulative pacer counters.
class SampleWriter {
public:
    void Write(const EngineSample& engine) {
        const auto now = std::chrono::steady_clock::now();
        const double interval_s = have_prev_
            ? std::chrono::duration<double>(now - prev_wall_).count() : 0.0;

        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const uint64_t proc_cpu = FileTimeTo100ns(kernel) + FileTimeTo100ns(user);
        GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
        const uint64_t thread_cpu = FileTimeTo100ns(kernel) + FileTimeTo100ns(user);
        FILETIME sys_idle{}, sys_kernel{}, sys_user{};
        GetSystemTimes(&sys_idle, &sys_kernel, &sys_user);
        const uint64_t sys_idle_t = FileTimeTo100ns(sys_idle);
        const uint64_t sys_total  = FileTimeTo100ns(sys_kernel) + FileTimeTo100ns(sys_user);

        const double ncpu = static_cast<double>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        double cpu_pct = 0, sampler_cpu_ms = 0, sys_cpu_pct = 0;
        if (have_prev_ && interval_s > 0) {
            cpu_pct = static_cast<double>(proc_cpu - prev_proc_cpu_) / 1e7 / interval_s / ncpu * 100.0;
            sampler_cpu_ms = static_cast<double>(thread_cpu - prev_thread_cpu_) / 1e4;
            const uint64_t total_delta = sys_total - prev_sys_total_;
            if (total_delta > 0)
                sys_cpu_pct = 100.0 * static_cast<double>(total_delta - (sys_idle_t - prev_sys_idle_)) /
                              static_cast<double>(total_delta);
        }

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);

        // One process snapshot answers both "our thread count" and "is the overlay running".
        const DWORD self_pid = GetCurrentProcessId();
        DWORD self_threads = 0, ui_pid = 0, ui_threads = 0, ui_processes = 0;
        ProcessStats ui;
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
                if (pe.th32ProcessID == self_pid) self_threads = pe.cntThreads;
                if (_wcsicmp(pe.szExeFile, L"" OMBRA_UI_EXE) != 0) continue;
                ++ui_processes;
                // Toasts spawn short-lived ombra-ui instances; report the largest one, which is the
                // resident overlay process whenever it's running.
                if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
                                           pe.th32ProcessID)) {
                    const ProcessStats s = ReadProcessStats(h);
                    CloseHandle(h);
                    if (ui_pid == 0 || s.private_mb > ui.private_mb) {
                        ui = s;
                        ui_pid = pe.th32ProcessID;
                        ui_threads = pe.cntThreads;
                    }
                }
            }
            CloseHandle(snap);
        }
        if ((ui_pid != 0) != (prev_ui_pid_ != 0)) {
            Telemetry::Event(ui_pid ? "ui_started" : "ui_exited", "pid=%lu",
                             static_cast<unsigned long>(ui_pid ? ui_pid : prev_ui_pid_));
        }
        prev_ui_pid_ = ui_pid;

        DWORD fg_pid = 0;
        if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fg_pid);
        const std::string fg_exe = fg_pid ? WideToUtf8(ExeBaseName(fg_pid)) : std::string();

        std::vector<DWORD> gpu_pids{self_pid};
        for (DWORD pid : {ui_pid, fg_pid, engine.game_pid})
            if (pid && std::find(gpu_pids.begin(), gpu_pids.end(), pid) == gpu_pids.end())
                gpu_pids.push_back(pid);
        const std::map<DWORD, GpuUsage> gpu = SampleGpu(gpu_pids);
        auto gpu_of = [&](DWORD pid) {
            auto it = gpu.find(pid);
            return it == gpu.end() ? GpuUsage{} : it->second;
        };
        const GpuUsage self_gpu = gpu_of(self_pid), ui_gpu = gpu_of(ui_pid), fg_gpu = gpu_of(fg_pid),
                       game_gpu = gpu_of(engine.game_pid);

        g_last_dedicated_mb.store(self_gpu.dedicated_mb, std::memory_order_relaxed);

        const ProcessStats self = ReadProcessStats(GetCurrentProcess());
        const TickCounters ticks = have_prev_ ? engine.ticks.Since(prev_ticks_) : TickCounters{};
        const double mb = 1024.0 * 1024.0;

        std::string row;
        row.reserve(512);
        row += Timestamp() + Format(",%.0f,%.1f", UptimeSeconds(), interval_s);
        row += Format(",%.1f,%.1f,%lu,%lu,%lu,%lu,%.2f,%.1f,%.1f,%lu", self.private_mb, self.ws_mb,
                      self.handles, self_threads, self.gdi, self.user, cpu_pct, sampler_cpu_ms,
                      sys_cpu_pct, mem.dwMemoryLoad);
        row += Format(",%.1f,%.1f,%.1f,%.1f", self_gpu.util_3d, self_gpu.util_encode,
                      self_gpu.dedicated_mb, self_gpu.shared_mb);
        row += Format(",%lu,%.1f,%.1f,%lu,%lu,%lu,%lu,%.1f,%.1f", ui_processes, ui.private_mb, ui.ws_mb,
                      ui.handles, ui_threads, ui.gdi, ui.user, ui_gpu.util_3d, ui_gpu.dedicated_mb);
        row += "," + CsvField(fg_exe) + Format(",%.1f,%d,", fg_gpu.util_3d, engine.armed ? 1 : 0) +
               CsvField(engine.game) + Format(",%.1f", game_gpu.util_3d);
        row += Format(",%lu,%.1f,%.1f,%.1f,%lu,%.1f", engine.pipelines, engine.video_ring_bytes / mb,
                      engine.video_ring_committed / mb, engine.video_ring_capacity / mb, engine.audio_sources,
                      engine.audio_ring_bytes / mb);
        row += Format(",%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f", ticks.ticks, ticks.duplicates,
                      ticks.late, ticks.resyncs, ticks.encode.Percentile(0.50),
                      ticks.encode.Percentile(0.99), ticks.encode.Max(),
                      ticks.wait_overshoot.Percentile(0.99));
        row += Format(",%llu,%llu,%llu", engine.rebuilds, engine.resolution_changes, engine.saves);
        row += Format(",%.0f,%.0f,%llu", engine.audio_dropped_frames / 48.0, engine.audio_late_frames / 48.0,
                      engine.audio_encoder_errors);

        {
            std::lock_guard<std::mutex> lock(g_files_mutex);
            g_samples_csv.WriteRow(row);
        }

        if (have_prev_) prev_ticks_.KeepMax(engine.ticks);
        else prev_ticks_ = engine.ticks;
        have_prev_       = true;
        prev_wall_       = now;
        prev_proc_cpu_   = proc_cpu;
        prev_thread_cpu_ = thread_cpu;
        prev_sys_idle_   = sys_idle_t;
        prev_sys_total_  = sys_total;
    }

private:
    bool         have_prev_ = false;
    TickCounters prev_ticks_;
    std::chrono::steady_clock::time_point prev_wall_{};
    uint64_t     prev_proc_cpu_ = 0, prev_thread_cpu_ = 0, prev_sys_idle_ = 0, prev_sys_total_ = 0;
    DWORD        prev_ui_pid_ = 0;
};

void EmitTransitions(const EngineSample& prev, const EngineSample& cur) {
    if (cur.armed != prev.armed) Telemetry::Event(cur.armed ? "armed" : "disarmed", "");
    if (cur.game != prev.game) {
        Telemetry::Event("game", "name=%s pid=%lu", cur.game.c_str(),
                         static_cast<unsigned long>(cur.game_pid));
    }
    if (cur.rebuilds > prev.rebuilds)
        Telemetry::Event("pipeline_rebuild", "total=%llu", cur.rebuilds);
    if (cur.resolution_changes > prev.resolution_changes)
        Telemetry::Event("resolution_change", "total=%llu", cur.resolution_changes);
}

}  // namespace

bool GetProcessRssMb(double& ws_mb, double& private_mb) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                 sizeof(pmc)))
        return false;
    ws_mb      = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    return true;
}

void Telemetry::SetEnabled(bool enabled) {
    const bool was = g_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (was == enabled) return;
    if (enabled) {
        Event("telemetry_enabled", "");
    } else {
        std::lock_guard<std::mutex> lock(g_files_mutex);
        g_samples_csv.Close();
        g_events_csv.Close();
    }
}

bool Telemetry::Enabled() { return g_enabled.load(std::memory_order_acquire); }

double Telemetry::LastGpuDedicatedMb() {
    return Enabled() ? g_last_dedicated_mb.load(std::memory_order_relaxed) : -1.0;
}

void Telemetry::Event(const char* type, const char* fmt, ...) {
    char detail[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, args);
    va_end(args);
    // Every event is also a log line, whether or not telemetry is on, so the
    // log tells the whole story from one call site. Before the files mutex: Log takes its own.
    Log::Info("event %s %s", type, detail);
    if (!Enabled()) return;
    const std::string row = Timestamp() + Format(",%.0f,", UptimeSeconds()) + CsvField(type) + "," +
                            CsvField(detail);
    std::lock_guard<std::mutex> lock(g_files_mutex);
    g_events_csv.WriteRow(row);
}

std::wstring Telemetry::Directory() {
    const std::wstring& base = Instance().data_dir;
    return base.empty() ? std::wstring() : base + L"\\telemetry";
}

TelemetrySampler::TelemetrySampler(Probe probe, std::chrono::seconds interval)
    : probe_(std::move(probe)), interval_(interval), thread_(&TelemetrySampler::Run, this) {}

TelemetrySampler::~TelemetrySampler() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void TelemetrySampler::Run() {
    Log::SetThreadName("telemetry");
    SampleWriter writer;
    EngineSample prev;
    bool have_prev = false;
    auto next_sample = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        if (Telemetry::Enabled()) {
            const EngineSample cur = probe_();
            if (have_prev) EmitTransitions(prev, cur);
            prev = cur;
            have_prev = true;
            if (std::chrono::steady_clock::now() >= next_sample) {
                writer.Write(cur);
                next_sample = std::chrono::steady_clock::now() + interval_;
            }
        } else {
            have_prev = false;
        }
        // Short slices so shutdown doesn't wait out a full poll.
        for (auto slept = std::chrono::milliseconds(0);
             slept < kPollInterval && running_.load(std::memory_order_acquire);
             slept += std::chrono::milliseconds(200)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

}  // namespace ombra
