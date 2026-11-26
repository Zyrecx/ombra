// The encoded-AU ring holds bitstream, not raw frames — at 1440p60/40Mbps
// that's ~5 MB/s versus ~316 MB/s for raw NV12.
//
// Single writer (the pacer thread, via Append), any number of readers (saves, each holding a
// RingPin). Eviction always removes whole GOPs from the front, never a partial one: dropping a
// frame a surviving frame references corrupts the buffer, and it stops at the oldest reader.
// A pin protects its range from eviction by qpc, not by deque index, since Append can pop_front
// (evict) and push_back (append) concurrently with a pin being held — indices shift, qpc doesn't.
//
// The byte storage is a swappable arena (IRingArena); descriptors, barriers, and pin
// bookkeeping stay backend-agnostic.
#pragma once

#include <algorithm>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ombra/video_codec.h"

namespace ombra {

// The circular byte store Ring writes AUs into. Everything that differs between a RAM buffer and
// a memory-mapped file lives here; wrap handling is the arena's job so Ring::Append/ReadInto don't
// need to know which backend they're talking to.
class IRingArena {
public:
    virtual ~IRingArena() = default;

    // Writes `size` bytes starting at `offset`, wrapping around Size() if the write crosses the
    // end. Caller guarantees size <= Size() (Ring::Append already dies on that before calling in).
    // False when the storage is gone (FileArena: its drive was removed); Ring moves to RAM.
    virtual bool Write(size_t offset, const uint8_t* data, size_t size) = 0;

    // Reads `length` bytes starting at `offset`, wrapping the same way, into `out` (resized to fit).
    // False when the storage is gone, as for Write.
    virtual bool ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const = 0;

    // Physical size of the arena — capacity_bytes + overshoot_bytes, not the soft eviction budget.
    // Name deliberately not Capacity(): Ring::Capacity() already means the soft budget, and every
    // wrap computation in the arena needs the physical size — a name collision would silently wrap
    // early and corrupt pinned data the overshoot headroom exists to protect.
    virtual size_t Size() const = 0;

    // Memory committed per chunk as footage needs it (RamArena); a file mapping has nothing to
    // commit and keeps these defaults. Ranges are circular, like Write's.
    //   EnsureCommitted: makes [offset, offset + size) writable; false if the system refused.
    //   DecommitFree: returns up to `max_chunks` committed chunks lying wholly inside
    //     [begin, begin + length) to the system, bytes freed. The caller vouches nothing live is there.
    //   Decommit: returns every page to the system. Nothing may be live.
    virtual bool   EnsureCommitted(size_t /*offset*/, size_t /*size*/) { return true; }
    virtual size_t CommittedBytes() const { return Size(); }
    virtual size_t DecommitFree(size_t /*begin*/, size_t /*length*/, size_t /*max_chunks*/) { return 0; }
    virtual void   Decommit() {}
};

// Reserves its whole size up front and commits it in kCommitChunk steps as the write position
// reaches them, so private memory follows the footage actually held: a fresh or idle ring costs
// address space, not RAM. Chunks the ring has evicted are handed back (Ring::TrimCommitted), so
// commit tracks live footage instead of growing to the whole arena on the first lap. One commit
// per 4 MB of footage runs on the pacer thread inside Append; ring_tests measures its cost.
class RamArena : public IRingArena {
public:
    static constexpr size_t kCommitChunk = 4u << 20;

    explicit RamArena(size_t physical_bytes);
    ~RamArena() override;
    RamArena(const RamArena&) = delete;
    RamArena& operator=(const RamArena&) = delete;

    bool Write(size_t offset, const uint8_t* data, size_t size) override;
    bool ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const override;
    size_t Size() const override { return size_; }

    bool   EnsureCommitted(size_t offset, size_t size) override;
    size_t CommittedBytes() const override { return committed_; }
    size_t DecommitFree(size_t begin, size_t length, size_t max_chunks) override;
    void   Decommit() override;

    // What EnsureCommitted(offset, size) would add (tests that model a system out of commit).
    size_t UncommittedBytesIn(size_t offset, size_t size) const;

private:
    size_t ChunkBytes(size_t chunk) const { return (std::min)(kCommitChunk, size_ - chunk * kCommitChunk); }

    uint8_t* base_      = nullptr;
    size_t   size_      = 0;
    size_t   committed_ = 0;
    std::vector<bool> chunk_committed_;
};

// A memory-mapped file backend (buffer.storage=file). The ring is "current
// buffer only" — restart-survives-the-buffer is not a stated goal, so Init always creates the
// file fresh (CREATE_ALWAYS), truncating anything already at file_path; there is no stale-file
// recovery path here. No FILE_FLAG_NO_BUFFERING and no explicit flush on Write — neither buys
// anything for a scratch buffer nobody reads outside this process, and a
// flush per Append would blow the pacer thread's 16.67ms per-tick budget for no benefit.
class FileArena : public IRingArena {
public:
    // Null, with `error` saying why, when the file can't be created at this size: the folder is
    // gone (a removed drive), the disk is full or the path is invalid. The user chose the path, so
    // none of that may die().
    static std::unique_ptr<FileArena> Create(const std::wstring& file_path, size_t physical_bytes,
                                             std::string* error);
    ~FileArena() override;

    FileArena(const FileArena&) = delete;
    FileArena& operator=(const FileArena&) = delete;

    // A mapped view whose drive was removed raises EXCEPTION_IN_PAGE_ERROR on the next page it
    // touches; these catch it and return false instead of taking the engine down.
    bool Write(size_t offset, const uint8_t* data, size_t size) override;
    bool ReadInto(size_t offset, size_t length, std::vector<uint8_t>& out) const override;
    size_t Size() const override { return size_; }

private:
    FileArena() = default;

    void*  file_handle_    = nullptr;  // HANDLE, void* so windows.h stays out of this header
    void*  mapping_handle_ = nullptr;  // HANDLE
    uint8_t* view_         = nullptr;
    size_t size_           = 0;
};

// Whether a ring file of this size can live at file_path: creates its folder, checks that the disk
// has room (a file already there counts as room, it gets replaced) and that the folder takes a new
// file. Leaves no file behind. False, with `error` for the user, otherwise.
bool CheckRingFile(const std::wstring& file_path, size_t physical_bytes, std::string* error);

struct AuDescriptor {
    int64_t  qpc         = 0;   // QueryPerformanceCounter ticks at pacer-tick time
    uint64_t index       = 0;   // Tick::index (CFR frame count) — what the muxer's stts needs
    size_t   offset      = 0;   // into the circular arena
    size_t   length      = 0;
    bool     is_keyframe = false;  // from NV_ENC_LOCK_BITSTREAM::pictureType == NV_ENC_PIC_TYPE_IDR
};

// The encoder parameters one *segment* of the ring was encoded with — everything the muxer's
// avcC and track header need, and the thing a resolution change invalidates. A segment is the
// span between two barriers; a barrier both cuts the save
// window and carries the parameters of the segment that *starts* at it.
//
// Shared by pointer, not copied: a save holds one for the duration of its mux while the ring goes
// on evicting and appending, and the blob is a few hundred bytes either way.
struct SegmentParams {
    std::vector<uint8_t> codec_header;  // IVideoEncoder::SequenceParams(): H.264 SPS+PPS, or the AV1 sequence header
    VideoCodec            codec  = VideoCodec::H264;
    uint32_t              width  = 0;
    uint32_t              height = 0;
    // The picture inside the coded size (AV1 padded 1080 -> 1082 keeps 1080): the mux's clap box.
    // 0: the coded size.
    uint32_t              picture_width  = 0;
    uint32_t              picture_height = 0;
    // The rate this segment's ticks count at; the mux times samples by it. 0: use SaveContext::fps.
    uint32_t              fps    = 0;
};

class Ring;

// Whether a pin stops at the save.overlap_policy=truncate cut (Ring::Truncate). Only the user's
// hotkey saves do; a tool's clip or a recording reaches past it. Hard barriers bind every pin.
enum class TruncateFloor { kIgnore, kRespect };

// Where a tailing pin stands after RingPin::Follow.
enum class TailState {
    kOpen,           // more footage before `until` may still arrive
    kReached,        // the ring holds footage at or past `until`: everything before it is taken
    kSegmentEnded,   // a barrier was cut after the pin was taken: the window ends at it
    kClosing,        // Ring::Init is waiting for this pin: finish with what was taken
    kBroken,         // the pin broke (headroom ran out, storage lost) or holds nothing
};

// One reader's hold on a window of the ring (Ring::Pin). Eviction never takes what a live pin
// still covers; the pin moves forward as the reader calls ReleaseThrough, and lets go when it is
// destroyed or Unpin()ned. An empty window holds nothing. Must not outlive its Ring; one thread
// uses it at a time.
//
// A pin also protects everything appended after it, so a reader can tail live footage (an open
// clip): Follow() adds what arrived since, the reader writes and releases behind itself, and the
// headroom bounds how far it may lag, as for a save. Nothing on the append path wakes a tail; the
// reader polls.
class RingPin {
public:
    RingPin() = default;
    RingPin(RingPin&& other) noexcept;
    RingPin& operator=(RingPin&& other) noexcept;
    RingPin(const RingPin&) = delete;
    RingPin& operator=(const RingPin&) = delete;
    ~RingPin() { Unpin(); }

    // The pinned access units, in order.
    const std::vector<AuDescriptor>& Aus() const { return aus_; }
    bool   empty() const { return aus_.empty(); }
    size_t size() const { return aus_.size(); }
    const AuDescriptor& operator[](size_t i) const { return aus_[i]; }
    const AuDescriptor& front() const { return aus_.front(); }
    const AuDescriptor& back() const { return aus_.back(); }
    std::vector<AuDescriptor>::const_iterator begin() const { return aus_.begin(); }
    std::vector<AuDescriptor>::const_iterator end() const { return aus_.end(); }

    // The parameters of the segment the window lies in, taken at Pin() time: Pin() never crosses a
    // barrier, so the newest barrier's params (or the base segment's) are the complete answer. Null
    // if none were ever supplied (synthetic tests, the audio rings).
    const std::shared_ptr<const SegmentParams>& Params() const { return params_; }

    // Copies a pinned AU's bytes into `out` (handles the arena-wrap case). Valid for descriptors this
    // pin holds that ReleaseThrough() hasn't passed (false for one it has). False once this pin
    // broke (see Ring::Append) or the storage failed: the bytes may have been overwritten.
    bool ReadInto(const AuDescriptor& d, std::vector<uint8_t>& out) const;
    // The reader is done with `d` and everything before it: eviction may take them. A save calls it
    // per AU as it writes, so a slow disk holds back only what it hasn't written yet.
    void ReleaseThrough(const AuDescriptor& d);
    // Lets go of the window; the descriptors stay readable as data, not as ring bytes.
    void Unpin();

    // Replaces `fresh` with the access units appended since the newest one this pin has handed
    // out (Aus() at first, then earlier Follows), up to (not including) `until` and never past a
    // barrier cut after the pin was taken, which ends the window (kSegmentEnded) because the next
    // segment needs other codec parameters. Empty once the pin broke. Aus() itself never grows, so
    // a long tail costs only what it hasn't written; pin it with max_qpc = `until` so Aus() doesn't
    // reach past the end. Relies on the ring's qpc rising with every Append, as Pin does.
    TailState Follow(int64_t until, std::vector<AuDescriptor>& fresh);

private:
    friend class Ring;
    Ring*                                ring_ = nullptr;  // null: holds nothing
    uint64_t                             id_   = 0;
    std::vector<AuDescriptor>            aus_;
    std::shared_ptr<const SegmentParams> params_;
    // The newest barrier when the pin was taken; a newer one ends a tail. INT64_MIN: none.
    int64_t                              segment_floor_ = (std::numeric_limits<int64_t>::min)();
    int64_t                              tail_qpc_ = 0;  // newest AU handed out (Aus().back() at first)
};

class Ring {
public:
    // `capacity_bytes` is the soft budget eviction targets. `overshoot_bytes` is *physical*
    // headroom on top of it: the ring is allowed to overshoot its budget while a pin blocks
    // eviction, which only works if the arena actually has spare bytes to write into — sizing the
    // arena to exactly capacity_bytes and calling that "overshoot room" just wraps the write around
    // and silently corrupts the pinned data being saved. A save releases its pin behind it as it
    // writes (ReleaseThrough), so the headroom covers how far the writer lags the encoder, not the
    // whole save. Both Inits wait for every held pin to be released before rebuilding the ring; a
    // tailing reader learns it should finish from Follow (kClosing).
    void Init(size_t capacity_bytes, size_t overshoot_bytes);

    // File-backed variant (buffer.storage=file) — same soft budget/overshoot semantics, backed by
    // FileArena instead of RamArena. See FileArena's comment: always recreates file_path fresh.
    // False when the file can't be made: the ring falls back to RAM and logs why, so recording goes
    // on. Check a new path with CheckRingFile first when the user can still be told.
    bool Init(size_t capacity_bytes, size_t overshoot_bytes, const std::wstring& file_path);

    // Any arena, its Size() the physical size (capacity_bytes + overshoot): tests use one that
    // refuses commits past a limit.
    void InitWithArena(std::unique_ptr<IRingArena> arena, size_t capacity_bytes);

    // Also evict whole GOPs older than `qpc_ticks` behind the newest keyframe (0 = bytes only, the
    // default). The byte budget alone is sized for a busy screen: on an idle desktop the AUs are
    // ~75 bytes, so the ring took days to reach its budget while it held hours of footage no save
    // window reaches, plus one descriptor per frame (~64 bytes each with MSVC's one-element deque
    // blocks). That was the engine's slow "Simple mode" memory growth, ~7 MB/h at 30 fps.
    void SetMaxAge(int64_t qpc_ticks);
    int64_t MaxAge() const;

    // Appends one access unit, copying its bytes into the arena. May evict whole GOPs from the
    // front to make room; never evicts a descriptor a live pin still covers — the ring overshoots
    // its soft byte budget instead, into the physical headroom above. When the headroom runs out
    // (a save's writer stalled on a hung disk), the oldest pin breaks, then the next oldest if that
    // isn't enough: eviction resumes, footage keeps coming and a broken pin's ReadInto() fails from
    // then on. Readers behind it are untouched, so the headroom bounds memory however many there
    // are. Dies only if a single access unit is larger than the whole arena.
    //
    // Memory is committed as the write position reaches it (RamArena). If the system refuses a
    // commit, the ring evicts its oldest GOPs until a whole chunk is free and reuses that chunk's
    // commit, so it keeps the newest footage and saves reach less far back (CommitCapped()). An
    // access unit that still can't be committed is dropped with the rest of its GOP, never stored
    // as a fragment.
    void Append(const uint8_t* data, size_t size, int64_t qpc, uint64_t index, bool is_keyframe);

    // Frees the ring once nothing has been appended for the max age (SetMaxAge): at that point no
    // save can reach anything it holds (a save reaches buffer.seconds back from now). Drops every
    // descriptor and barrier and returns the arena's memory to the system; the next Append commits
    // afresh. False (and nothing done) while a save holds a pin, with no max age set, or when the
    // ring is already released. The supervisor calls it while disarmed.
    bool ReleaseIfIdle(int64_t now_qpc);
    // The same, unconditionally except for a pin: the audio hub's suspend. False while pinned.
    bool Release();
    // Released and not appended to since.
    bool Released() const;
    // qpc of the oldest and newest frame held; nullopt while empty.
    struct Span { int64_t oldest_qpc; int64_t newest_qpc; };
    std::optional<Span> HeldSpan() const;
    // Private memory the arena holds now; 0 when released.
    size_t CommittedBytes() const;
    // A commit was refused: the ring holds what CommittedBytes() fits. Reset by a release or Init.
    bool CommitCapped() const;
    // True once each time the buffer's file became unusable and the ring moved to RAM: the file
    // couldn't be made at Init, or its drive went away while recording, which loses the footage held
    // (Append keeps going from the next keyframe; a save in progress fails). Also destroys the old
    // arena, whose unmap can wait on the missing drive. Called off the pacer thread (the watchdog).
    bool TakeStorageLost();
    // Returns one committed chunk that holds nothing live to the system; bytes freed (0 when there
    // is none). Called off the pacer thread (the supervisor's watchdog): a 4 MB VirtualFree holds
    // the ring's mutex, which Append waits on. The chunk just ahead of the write position is kept.
    size_t TrimCommitted();

    // T_start = oldest keyframe with qpc >= min_qpc. Pins everything from there up to
    // (not including) the first descriptor with qpc >= max_qpc, and returns the descriptor list,
    // in order. `max_qpc` defaults to unbounded (the original single-track behavior — T_end
    // and "newest" were close enough with one producer). An explicit bound is
    // required once a second, independently-clocked ring (PcmRing, pcmring.h) is pinned against
    // the same T_end — pinning each to its own "newest" would silently give tracks of different
    // lengths. Pins are independent: several readers may hold overlapping windows at once.
    RingPin Pin(int64_t min_qpc, int64_t max_qpc = (std::numeric_limits<int64_t>::max)(),
                TruncateFloor truncate = TruncateFloor::kIgnore);
    // Pin() that starts at the newest keyframe at or before `min_qpc` when the ring still holds one
    // (never past the newest barrier, nor the truncate cut under kRespect), so the window covers all of [min_qpc, max_qpc) instead of
    // losing up to a GOP at the front. Otherwise the same as Pin(). The video save uses it: a 120 s
    // buffer saves at least 120 s, not 118-120.
    RingPin PinCovering(int64_t min_qpc, int64_t max_qpc = (std::numeric_limits<int64_t>::max)(),
                        TruncateFloor truncate = TruncateFloor::kIgnore);

    // Any reader holds a pin.
    bool IsPinned() const;
    // An Init waits for its readers: a tail that saw kClosing waits for this to clear before it
    // pins again, or it could hold Init off for good.
    bool Closing() const;
    size_t Capacity() const;
    size_t LiveBytes() const { std::lock_guard<std::mutex> lock(mutex_); return live_bytes_; }

    // Marks `qpc` as a point no future Pin() may reach past: a resolution change or a
    // fault-rebuild, so a save window can never straddle two different SPS/PPS segments. Pin()'s
    // T_start is clamped to at least the newest barrier on record — an older barrier can't matter
    // once a newer one exists, since the newer one is already at least as restrictive. Kept in qpc
    // order whatever order they arrive in.
    void Barrier(int64_t qpc, std::shared_ptr<const SegmentParams> params = nullptr);

    // The save.overlap_policy=truncate cut: a pin with TruncateFloor::kRespect starts at or after
    // `qpc`, so the user's next hotkey save doesn't repeat footage. Erases nothing and starts no
    // segment; other pins reach past it. Only ever moves forward: a save drops its cut at the press's
    // time once its file is written, after a later press may already have dropped its own.
    void Truncate(int64_t qpc);
    // The current cut; INT64_MIN when none. Cleared with the footage (Init, a release, a lost file).
    int64_t TruncateQpc() const;

    // The parameters of the segment that predates every surviving barrier. Set once at startup for
    // the first generation (PipelineSupervisor); after that it is maintained automatically, by
    // EvictLocked() promoting a barrier's params here as that barrier is pruned — at which point
    // every surviving descriptor belongs to the segment that barrier started.
    void SetBaseSegmentParams(std::shared_ptr<const SegmentParams> params);

    // A verify clause needs "the expected barrier count in the ring" to be
    // observable from outside — barriers_ itself stays private.
    size_t BarrierCount() const;

private:
    friend class RingPin;
    struct Reader {
        uint64_t id;
        int64_t  start_qpc;  // descriptors with qpc >= this are protected unless broken
        bool     broken;     // the headroom ran out under it: protects nothing until Unpin
    };
    Reader* FindReaderLocked(uint64_t id);
    // Oldest qpc an unbroken reader protects; INT64_MAX when none.
    int64_t ProtectedFromLocked() const;
    void BreakOldestReaderLocked();
    bool ReadPinned(uint64_t id, const AuDescriptor& d, std::vector<uint8_t>& out);
    TailState FollowPinned(RingPin& pin, int64_t until, std::vector<AuDescriptor>& fresh);
    void WaitForReadersLocked(std::unique_lock<std::mutex>& lock);
    void ReleasePinnedThrough(uint64_t id, int64_t qpc);
    void Unpin(uint64_t id);

    void EvictLocked();
    void EvictAgedLocked(int64_t newest_qpc);
    RingPin PinImpl(int64_t min_qpc, int64_t max_qpc, bool covering, TruncateFloor truncate);
    void InitLocked(std::unique_ptr<IRingArena> arena, size_t capacity_bytes);
    void ReleaseLocked();
    bool ReclaimCommitLocked(size_t size);
    void MoveToRamLocked();

    mutable std::mutex   mutex_;
    std::unique_ptr<IRingArena> arena_;
    std::unique_ptr<IRingArena> lost_arena_;  // waits for TakeStorageLost, off the pacer thread
    bool                  storage_lost_ = false;  // TakeStorageLost
    mutable bool          read_failed_  = false;  // a save hit the fault first: Append moves to RAM
    size_t                capacity_   = 0;
    size_t                overshoot_  = 0;
    bool                  capped_     = false;  // CommitCapped
    bool                  dropping_gop_ = false;  // an AU of this GOP didn't fit: drop to the next keyframe
    bool                  released_   = false;
    int64_t               last_append_qpc_ = 0;
    size_t                write_pos_  = 0;  // next write offset, circular
    size_t                live_bytes_ = 0;  // sole authority on how much of the arena is live
    int64_t               max_age_qpc_ = 0; // SetMaxAge; 0 = byte budget only

    std::deque<AuDescriptor> descriptors_;

    struct BarrierRec {
        int64_t                              qpc;
        std::shared_ptr<const SegmentParams> params;  // params of the segment starting here; may be null
    };
    // Ascending by construction (see Barrier()'s ordering assumption above). Pruned in
    // EvictLocked() as its corresponding descriptors are evicted — see Barrier()'s comment on why
    // a barrier older than the oldest surviving descriptor is dead weight, not something to keep.
    std::deque<BarrierRec> barriers_;

    std::shared_ptr<const SegmentParams> base_segment_params_;
    int64_t truncate_qpc_ = (std::numeric_limits<int64_t>::min)();  // Truncate

    // A handful at most (one per save in flight); scanned per eviction on the pacer path. Only
    // Pin and Unpin resize it, off the pacer thread.
    std::vector<Reader>     readers_;
    uint64_t                next_reader_id_ = 1;
    std::condition_variable pin_released_;  // Init waits on it for readers_ to empty
    int                     closing_ = 0;  // Inits waiting: tails finish (Follow: kClosing)
};

}  // namespace ombra
