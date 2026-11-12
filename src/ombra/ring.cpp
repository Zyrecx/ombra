#include "ombra/ring.h"

#include "ombra/die.h"
#include "ombra/log.h"
#include "ombra/util.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ombra {

RamArena::RamArena(size_t physical_bytes) : size_(physical_bytes) {
    if (physical_bytes == 0) return;
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, physical_bytes, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_) die("RamArena: could not reserve address space for the ring");
    chunk_committed_.assign((physical_bytes + kCommitChunk - 1) / kCommitChunk, false);
}

RamArena::~RamArena() {
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}

bool RamArena::EnsureCommitted(size_t offset, size_t size) {
    while (size > 0) {
        const size_t chunk = offset / kCommitChunk;
        if (!chunk_committed_[chunk]) {
            if (!VirtualAlloc(base_ + chunk * kCommitChunk, ChunkBytes(chunk), MEM_COMMIT, PAGE_READWRITE)) return false;
            chunk_committed_[chunk] = true;
            committed_ += ChunkBytes(chunk);
        }
        const size_t step = (std::min)(size, chunk * kCommitChunk + ChunkBytes(chunk) - offset);
        size  -= step;
        offset = (offset + step) % size_;
    }
    return true;
}

size_t RamArena::UncommittedBytesIn(size_t offset, size_t size) const {
    size_t missing = 0;
    while (size > 0) {
        const size_t chunk = offset / kCommitChunk;
        if (!chunk_committed_[chunk]) missing += ChunkBytes(chunk);
        const size_t step = (std::min)(size, chunk * kCommitChunk + ChunkBytes(chunk) - offset);
        size  -= step;
        offset = (offset + step) % size_;
    }
    return missing;
}

size_t RamArena::DecommitFree(size_t begin, size_t length, size_t max_chunks) {
    if (size_ == 0 || length == 0) return 0;
    // The first chunk boundary at or after `begin`, as a distance into the range.
    size_t chunk = (begin + kCommitChunk - 1) / kCommitChunk;
    size_t dist  = (std::min)(chunk * kCommitChunk, size_) - begin;
    if (chunk == chunk_committed_.size()) chunk = 0;
    size_t freed = 0, count = 0;
    for (size_t seen = 0; seen < chunk_committed_.size() && count < max_chunks; ++seen) {
        const size_t bytes = ChunkBytes(chunk);
        if (dist + bytes > length) break;
        if (chunk_committed_[chunk]) {
            VirtualFree(base_ + chunk * kCommitChunk, bytes, MEM_DECOMMIT);
            chunk_committed_[chunk] = false;
            committed_ -= bytes;
            freed += bytes;
            ++count;
        }
        dist += bytes;
        chunk = (chunk + 1) % chunk_committed_.size();
    }
    return freed;
}

void RamArena::Decommit() {
    if (base_ && committed_ > 0) VirtualFree(base_, size_, MEM_DECOMMIT);  // uncommitted pages are fine
    chunk_committed_.assign(chunk_committed_.size(), false);
    committed_ = 0;
}

bool RamArena::Write(size_t offset, const uint8_t* data, size_t size) {
    const size_t first_chunk = (std::min)(size, size_ - offset);
    std::memcpy(base_ + offset, data, first_chunk);
    if (first_chunk < size) std::memcpy(base_, data + first_chunk, size - first_chunk);
    return true;
}

bool RamArena::ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const {
    out.resize(length);
    const size_t first_chunk = (std::min)(length, size_ - offset);
    std::memcpy(out.data(), base_ + offset, first_chunk);
    if (first_chunk < length)
        std::memcpy(out.data() + first_chunk, base_, length - first_chunk);
    return true;
}

std::unique_ptr<FileArena> FileArena::Create(const std::wstring& file_path, size_t physical_bytes,
                                             std::string* error) {
    auto fail = [&](const char* what) -> std::unique_ptr<FileArena> {
        if (error) *error = std::string(what) + " failed, error " + std::to_string(GetLastError());
        return nullptr;
    };
    if (!EnsureParentDir(file_path)) return fail("creating the folder");

    std::unique_ptr<FileArena> arena(new FileArena());
    arena->size_ = physical_bytes;
    HANDLE h = CreateFileW(file_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("CreateFileW");
    arena->file_handle_ = h;

    LARGE_INTEGER size_li;
    size_li.QuadPart = static_cast<LONGLONG>(physical_bytes);
    HANDLE mapping = CreateFileMappingW(h, nullptr, PAGE_READWRITE,
                                         static_cast<DWORD>(size_li.HighPart),
                                         static_cast<DWORD>(size_li.LowPart), nullptr);
    if (!mapping) return fail("CreateFileMappingW");
    arena->mapping_handle_ = mapping;

    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
    if (!view) return fail("MapViewOfFile");
    arena->view_ = static_cast<uint8_t*>(view);
    return arena;
}

bool CheckRingFile(const std::wstring& file_path, size_t physical_bytes, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!EnsureParentDir(file_path)) return fail("couldn't create the buffer's folder");

    const size_t slash = file_path.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? L"." : file_path.substr(0, slash + 1);
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(folder.c_str(), &free_bytes, nullptr, nullptr))
        return fail("couldn't read the free space of the buffer's disk");
    uint64_t room = free_bytes.QuadPart;
    WIN32_FILE_ATTRIBUTE_DATA existing{};
    if (GetFileAttributesExW(file_path.c_str(), GetFileExInfoStandard, &existing))
        room += (uint64_t{existing.nFileSizeHigh} << 32) | existing.nFileSizeLow;
    if (room < physical_bytes) {
        return fail("the buffer needs " + std::to_string(physical_bytes / (1024 * 1024)) +
                    " MB and its disk has " + std::to_string(room / (1024 * 1024)) + " MB free");
    }

    // A probe beside it, not the file itself: the running ring may hold that one open.
    HANDLE probe = CreateFileW((file_path + L".probe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return fail("can't write to the buffer's folder");
    CloseHandle(probe);
    return true;
}

FileArena::~FileArena() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_handle_) CloseHandle(static_cast<HANDLE>(mapping_handle_));
    if (file_handle_) CloseHandle(static_cast<HANDLE>(file_handle_));
}

// Its own function: __try can't share one with objects that need unwinding. Only a failed page-in
// is caught, so a real access violation still crashes where it can be seen.
static bool CopyMapped(void* to, const void* from, size_t size) {
    __try {
        std::memcpy(to, from, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                              : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool FileArena::Write(size_t offset, const uint8_t* data, size_t size) {
    const size_t first_chunk = (std::min)(size, size_ - offset);
    if (!CopyMapped(view_ + offset, data, first_chunk)) return false;
    return first_chunk == size || CopyMapped(view_, data + first_chunk, size - first_chunk);
}

bool FileArena::ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const {
    out.resize(length);
    const size_t first_chunk = (std::min)(length, size_ - offset);
    if (!CopyMapped(out.data(), view_ + offset, first_chunk)) return false;
    return first_chunk == length || CopyMapped(out.data() + first_chunk, view_, length - first_chunk);
}

void Ring::Init(size_t capacity_bytes, size_t overshoot_bytes) {
    std::unique_lock<std::mutex> lock(mutex_);
    pin_released_.wait(lock, [this] { return !pinned_; });
    arena_.reset();  // release any prior (e.g. file) arena before building the new one
    InitLocked(std::make_unique<RamArena>(capacity_bytes + overshoot_bytes), capacity_bytes);  // physical size; capacity_ is the soft budget
}

bool Ring::Init(size_t capacity_bytes, size_t overshoot_bytes, const std::wstring& file_path) {
    std::unique_lock<std::mutex> lock(mutex_);
    pin_released_.wait(lock, [this] { return !pinned_; });
    arena_.reset();  // close/unmap any prior arena (same file_path) before CREATE_ALWAYS truncates it
    std::string error;
    std::unique_ptr<IRingArena> arena = FileArena::Create(file_path, capacity_bytes + overshoot_bytes, &error);
    const bool on_file = arena != nullptr;
    if (!on_file) {
        Log::Warn("ring: can't use buffer.file_path (%s); keeping the buffer in RAM", error.c_str());
        arena = std::make_unique<RamArena>(capacity_bytes + overshoot_bytes);
    }
    InitLocked(std::move(arena), capacity_bytes);
    storage_lost_ = !on_file;
    return on_file;
}

void Ring::InitWithArena(std::unique_ptr<IRingArena> arena, size_t capacity_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    arena_.reset();
    InitLocked(std::move(arena), capacity_bytes);
}

void Ring::InitLocked(std::unique_ptr<IRingArena> arena, size_t capacity_bytes) {
    overshoot_           = arena->Size() - capacity_bytes;
    arena_               = std::move(arena);
    capacity_            = capacity_bytes;
    capped_              = false;
    dropping_gop_        = false;
    released_            = false;
    storage_lost_        = false;
    read_failed_         = false;
    last_append_qpc_     = 0;
    live_bytes_          = 0;
    write_pos_           = 0;
    descriptors_.clear();
    barriers_.clear();
    pinned_     = false;
    pin_broken_ = false;
}

void Ring::ReleaseLocked() {
    descriptors_.clear();
    // Every barrier cut something now gone. The params that matter next arrive with the barrier
    // the supervisor drops when it re-arms (its disarm latched one).
    barriers_.clear();
    live_bytes_   = 0;
    write_pos_    = 0;
    dropping_gop_ = false;
    arena_->Decommit();
    capped_    = false;
    released_  = true;
}

bool Ring::ReleaseIfIdle(int64_t now_qpc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (released_ || pinned_ || max_age_qpc_ <= 0 || !arena_) return false;
    if (now_qpc - last_append_qpc_ < max_age_qpc_) return false;
    ReleaseLocked();
    return true;
}

bool Ring::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pinned_ || !arena_) return false;
    if (!released_) ReleaseLocked();
    return true;
}

bool Ring::Released() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return released_;
}

size_t Ring::CommittedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return arena_ ? arena_->CommittedBytes() : 0;
}

bool Ring::CommitCapped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capped_;
}

bool Ring::TakeStorageLost() {
    std::unique_ptr<IRingArena> lost;
    bool was_lost;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lost = std::move(lost_arena_);
        was_lost = storage_lost_;
        storage_lost_ = false;
    }
    return was_lost;
}

// The bytes are gone with the drive, so everything that describes them goes too. Same size, so
// nothing the pipeline computed from the ring changes; the newest params stay for the next save,
// as EvictLocked would have promoted them. A held pin breaks: its offsets mean nothing in the new
// arena, and reading them would write a corrupt clip rather than fail. Runs on the pacer's path, but
// only once: a reserve, no commit, no log.
void Ring::MoveToRamLocked() {
    for (auto it = barriers_.rbegin(); it != barriers_.rend(); ++it) {
        if (it->params) { base_segment_params_ = it->params; break; }
    }
    barriers_.clear();
    descriptors_.clear();
    live_bytes_ = 0;
    write_pos_  = 0;
    capped_     = false;
    read_failed_ = false;
    if (pinned_) pin_broken_ = true;
    const size_t physical = arena_->Size();
    lost_arena_ = std::move(arena_);
    arena_ = std::make_unique<RamArena>(physical);
    storage_lost_ = true;
}

// Frees one whole GOP from the front — the leading keyframe and everything up to, not including,
// the next keyframe — unless a live pin still needs it. Never evicts a partial GOP: if a pin's
// boundary falls mid-GOP, stops there and leaves the rest for a later call once the pin lifts.
// Append() calls this in a loop until enough room is freed or eviction stalls.
void Ring::EvictLocked() {
    if (descriptors_.empty()) return;
    const bool protecting = pinned_ && !pin_broken_;
    if (protecting && descriptors_.front().qpc >= pin_start_qpc_) return;  // would strand pinned data

    do {
        const auto& d = descriptors_.front();
        live_bytes_ -= d.length;
        descriptors_.pop_front();
    } while (!descriptors_.empty() && !descriptors_.front().is_keyframe &&
             !(protecting && descriptors_.front().qpc >= pin_start_qpc_));

    // A barrier with no surviving descriptor before it can no longer clamp anything a future Pin()
    // would otherwise reach — see Barrier()'s comment. If the ring emptied out entirely, every
    // barrier is dead the same way.
    const int64_t oldest_surviving_qpc =
        descriptors_.empty() ? (std::numeric_limits<int64_t>::max)() : descriptors_.front().qpc;
    while (!barriers_.empty() && barriers_.front().qpc < oldest_surviving_qpc) {
        // Everything before this barrier is gone, so every surviving descriptor now belongs to the
        // segment it started — promote its params before dropping it, or a save after a long
        // enough wrap would mux the *previous* segment's SPS/PPS.
        if (barriers_.front().params) base_segment_params_ = barriers_.front().params;
        barriers_.pop_front();
    }
}

// Drops front GOPs that no save can reach: a GOP whose *next* keyframe is at or before the cutoff
// lies entirely before it, and Pin() starts at a keyframe >= its floor. Runs once per keyframe, so
// the scan to the next keyframe (one GOP) is amortized over the GOP's ticks.
void Ring::EvictAgedLocked(int64_t newest_qpc) {
    const int64_t cutoff = newest_qpc - max_age_qpc_;
    while (!descriptors_.empty() && descriptors_.front().qpc < cutoff) {
        size_t next_kf = 1;
        while (next_kf < descriptors_.size() && !descriptors_[next_kf].is_keyframe) ++next_kf;
        if (next_kf == descriptors_.size() || descriptors_[next_kf].qpc > cutoff) return;
        const size_t before = descriptors_.size();
        EvictLocked();
        if (descriptors_.size() == before) return;  // a pin holds it
    }
}

void Ring::SetMaxAge(int64_t qpc_ticks) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_age_qpc_ = (std::max)(int64_t{0}, qpc_ticks);
}

int64_t Ring::MaxAge() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_age_qpc_;
}

void Ring::Barrier(int64_t qpc, std::shared_ptr<const SegmentParams> params) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto at = std::upper_bound(barriers_.begin(), barriers_.end(), qpc,
                                     [](int64_t q, const BarrierRec& b) { return q < b.qpc; });
    barriers_.insert(at, BarrierRec{qpc, std::move(params)});
}

void Ring::SetBaseSegmentParams(std::shared_ptr<const SegmentParams> params) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_segment_params_ = std::move(params);
}

std::shared_ptr<const SegmentParams> Ring::PinnedSegmentParams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pinned_segment_params_;
}

size_t Ring::BarrierCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return barriers_.size();
}

void Ring::Append(const uint8_t* data, size_t size, int64_t qpc, uint64_t index, bool is_keyframe) {
    std::lock_guard<std::mutex> lock(mutex_);
    released_        = false;
    last_append_qpc_ = qpc;
    if (read_failed_) {
        MoveToRamLocked();
        dropping_gop_ = true;  // this AU's GOP started in the lost arena
    }
    if (is_keyframe) dropping_gop_ = false;
    if (dropping_gop_) return;
    if (size > arena_->Size()) die("Ring::Append: access unit larger than the ring's entire physical arena");

    // Evict toward the soft budget first; a pin can block this short of the target.
    while (live_bytes_ + size > capacity_) {
        const size_t before = descriptors_.size();
        EvictLocked();
        if (descriptors_.size() == before) break;  // pin (or an empty ring) blocked further eviction
    }
    // Past the soft budget and still not enough room (pin blocked eviction): overshoot into the
    // physical headroom rather than drop a frame. A physical-capacity overflow (the pinned reader
    // fell further behind than the headroom allows) breaks the pin: that save fails, the engine
    // keeps recording.
    if (arena_->Size() - live_bytes_ < size) {
        pin_broken_ = true;
        while (arena_->Size() - live_bytes_ < size && !descriptors_.empty()) EvictLocked();
    }

    if (!arena_->EnsureCommitted(write_pos_, size) && !ReclaimCommitLocked(size)) {
        dropping_gop_ = true;  // nothing to reuse (empty or pinned): drop this GOP, retry at the next keyframe
        return;
    }

    if (!arena_->Write(write_pos_, data, size)) {
        MoveToRamLocked();
        // Only a keyframe can start the new arena; anything else references footage just lost.
        if (!is_keyframe || !arena_->EnsureCommitted(0, size) || !arena_->Write(0, data, size)) {
            dropping_gop_ = true;
            return;
        }
    }

    AuDescriptor desc{qpc, index, write_pos_, size, is_keyframe};
    write_pos_ = (write_pos_ + size) % arena_->Size();
    live_bytes_ += size;
    descriptors_.push_back(desc);
    if (is_keyframe && max_age_qpc_ > 0) EvictAgedLocked(qpc);
}

// The system refused a commit: evict the oldest GOPs until a whole chunk outside the write is
// free, hand it back and commit again. Commit stays what the ring already had, and the GOPs it
// evicted are footage the write would have overtaken first anyway.
bool Ring::ReclaimCommitLocked(size_t size) {
    capped_ = true;
    for (;;) {
        const size_t free_bytes = arena_->Size() - live_bytes_;
        if (free_bytes > size &&
            arena_->DecommitFree((write_pos_ + size) % arena_->Size(), free_bytes - size, SIZE_MAX) > 0)
            return arena_->EnsureCommitted(write_pos_, size);
        const size_t before = descriptors_.size();
        EvictLocked();
        if (descriptors_.size() == before) return false;  // empty, or a pin holds the rest
    }
}

size_t Ring::TrimCommitted() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!arena_ || released_) return 0;
    // Free space runs from write_pos_ to the oldest live byte; keep the next chunk the write needs.
    const size_t free_bytes = arena_->Size() - live_bytes_;
    if (free_bytes <= RamArena::kCommitChunk) return 0;
    return arena_->DecommitFree((write_pos_ + RamArena::kCommitChunk) % arena_->Size(),
                                free_bytes - RamArena::kCommitChunk, 1);
}

std::vector<AuDescriptor> Ring::Pin(int64_t min_qpc, int64_t max_qpc) {
    return PinImpl(min_qpc, max_qpc, /*covering=*/false);
}

std::vector<AuDescriptor> Ring::PinCovering(int64_t min_qpc, int64_t max_qpc) {
    return PinImpl(min_qpc, max_qpc, /*covering=*/true);
}

std::vector<AuDescriptor> Ring::PinImpl(int64_t min_qpc, int64_t max_qpc, bool covering) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pinned_) die("Ring::Pin: a pin is already held (one save in flight at a time)");

    // A save window may never reach past the newest barrier — clamp the
    // requested floor up to it before searching. If nothing survives at or after the barrier, the
    // loop below finds no keyframe and Pin() returns an empty window, same as any other
    // out-of-range request; that's correct, not a bug to special-case.
    const int64_t barrier_qpc = barriers_.empty() ? (std::numeric_limits<int64_t>::min)() : barriers_.back().qpc;
    min_qpc = (std::max)(min_qpc, barrier_qpc);

    // The window can only lie inside the newest barrier's segment (that is what the clamp above
    // guarantees), so its params are the newest barrier's — walking back past a null only matters
    // for callers that drop bare barriers with no params at all (the synthetic ring tests, and
    // PipelineSupervisor's audio ring, which has no SPS/PPS concept).
    pinned_segment_params_ = base_segment_params_;
    for (auto it = barriers_.rbegin(); it != barriers_.rend(); ++it) {
        if (it->params) { pinned_segment_params_ = it->params; break; }
    }

    // T_start = oldest keyframe with qpc >= min_qpc; covering: the newest keyframe at or before it,
    // if one lies inside the newest barrier's segment.
    auto start = descriptors_.end();
    auto covering_start = descriptors_.end();
    for (auto it = descriptors_.begin(); it != descriptors_.end(); ++it) {
        if (!it->is_keyframe) continue;
        if (it->qpc >= min_qpc) { start = it; break; }
        if (it->qpc >= barrier_qpc) covering_start = it;
    }
    if (covering && covering_start != descriptors_.end() && (start == descriptors_.end() || start->qpc > min_qpc))
        start = covering_start;
    if (start == descriptors_.end()) return {};  // nothing in range — buffer shorter than requested

    auto end = start;
    while (end != descriptors_.end() && end->qpc < max_qpc) ++end;

    pinned_        = true;
    pin_broken_    = false;
    pin_start_qpc_ = start->qpc;
    return std::vector<AuDescriptor>(start, end);
}

void Ring::Unpin() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pinned_     = false;
        pin_broken_ = false;
    }
    pin_released_.notify_all();
}

void Ring::ReleaseThrough(const AuDescriptor& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pinned_) pin_start_qpc_ = (std::max)(pin_start_qpc_, d.qpc + 1);
}

bool Ring::IsPinned() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pinned_;
}

size_t Ring::Capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}

bool Ring::ReadInto(const AuDescriptor& d, std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pin_broken_) return false;
    if (!arena_->ReadInto(d.offset, d.length, out)) {
        read_failed_ = true;
        return false;
    }
    return true;
}

}  // namespace ombra
