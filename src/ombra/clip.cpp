#include "ombra/clip.h"

#include "ombra/controller.h"
#include "ombra/log.h"
#include "ombra/mux.h"
#include "ombra/supervisor.h"
#include "ombra/telemetry.h"
#include "ombra/util.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace ombra {

namespace {

// How long a live clip sleeps when it has written everything there is: a few frames at 60 fps,
// so the disk keeps pace without a wakeup per frame.
constexpr DWORD kClipPollMs = 50;

int64_t SecondsToQpc(double seconds, int64_t freq) {
    return static_cast<int64_t>(std::llround(seconds * static_cast<double>(freq)));
}

// The whole action's outcome, the same rule as SaveReplay's: any failed or busy monitor fails it.
SaveOutcome FoldOutcome(const std::vector<ReplaySaveResult>& monitors) {
    bool saved = false, incomplete = false;
    for (const auto& m : monitors) {
        saved |= m.outcome == SaveOutcome::kSaved;
        incomplete |= m.outcome == SaveOutcome::kFailed || m.outcome == SaveOutcome::kBusy;
    }
    return incomplete ? SaveOutcome::kFailed : saved ? SaveOutcome::kSaved : SaveOutcome::kNothingNew;
}

}  // namespace

const char* ClipEndName(ClipEnd end) {
    switch (end) {
        case ClipEnd::kReached:      return "reached";
        case ClipEnd::kSegmentEnded: return "segment_ended";
        case ClipEnd::kClosing:      return "closing";
        case ClipEnd::kStalled:      return "stalled";
        case ClipEnd::kStopped:      return "stopped";
        case ClipEnd::kBroken:       return "broken";
    }
    return "stopped";
}

std::shared_ptr<ClipSession> ClipSession::Open(SaveContext* ctx, const ClipRequest& request,
                                               const std::wstring* group_folder_name, SaveOutcome* why) {
    const auto refuse = [why](SaveOutcome outcome) {
        if (why) *why = outcome;
        return std::shared_ptr<ClipSession>{};
    };
    std::shared_ptr<ClipSession> s(new ClipSession());
    s->ctx_      = ctx;
    s->qpc_freq_ = QpcFrequency();
    {
        // Listed before the pin is taken, so a reallocation either sees this clip and refuses, or
        // has already blocked it here.
        std::lock_guard<std::mutex> lock(ctx->clip_mutex);
        if (ctx->clips_blocked) return refuse(SaveOutcome::kBusy);
        ctx->open_clips.push_back(s.get());
        s->listed_ = true;
    }

    const double before = std::clamp(request.before_seconds, 0.0, ctx->buffer_seconds);
    const double after  = std::clamp(request.after_seconds, 0.0, kClipMaxAfterSeconds);
    s->until_   = request.event_qpc + SecondsToQpc(after, s->qpc_freq_);
    s->cap_qpc_ = request.event_qpc + SecondsToQpc(kClipMaxAfterSeconds, s->qpc_freq_);
    s->pin_ = ctx->ring->PinCovering(request.event_qpc - SecondsToQpc(before, s->qpc_freq_), s->until_,
                                     TruncateFloor::kIgnore);
    if (s->pin_.empty()) return refuse(SaveOutcome::kNothingNew);
    // Held from the pin on, before the disk checks: a marker added meanwhile mustn't prune ours.
    s->first_qpc_ = s->pin_.front().qpc;
    if (ctx->markers) s->marker_hold_ = ctx->markers->HoldFrom(s->first_qpc_);

    s->segment_ = s->pin_.Params();
    if (!s->segment_) {
        if (!ctx->supervisor) return refuse(SaveOutcome::kFailed);
        auto live = ctx->supervisor->CurrentParams();
        auto fallback = std::make_shared<SegmentParams>();
        fallback->codec_header = std::move(live.codec_header);
        fallback->codec        = live.codec;
        fallback->width        = live.width;
        fallback->height       = live.height;
        s->segment_ = fallback;
    }
    s->fps_ = s->segment_->fps ? s->segment_->fps : ctx->fps.load();

    // The clip's size, from the footage already pinned when it spans long enough to say, never under
    // the configured bitrate: a quiet scene before the event mustn't promise room for a busy one.
    uint64_t pinned_bytes = 0;
    for (const AuDescriptor& d : s->pin_) pinned_bytes += d.length;
    const double span = static_cast<double>(s->pin_.back().qpc - s->pin_.front().qpc) / s->qpc_freq_;
    double rate = ctx->bitrate_kbps.load() * 1000.0 / 8.0;
    if (span >= 1.0) rate = (std::max)(rate, static_cast<double>(pinned_bytes) / span);
    s->bytes_per_second_ = rate;
    const double seconds = static_cast<double>(s->until_ - s->pin_.front().qpc) / s->qpc_freq_;
    const uint64_t estimate = (std::max)(pinned_bytes, static_cast<uint64_t>(rate * seconds));
    s->dest_ = PickClipPath(ctx, request.event_qpc, group_folder_name, request.title, ClipRoomNeeded(estimate), "clip");
    if (s->dest_.path.empty()) return refuse(SaveOutcome::kFailed);

    s->queue_       = s->pin_.Aus();
    s->clock_.emplace(s->qpc_freq_, s->fps_);
    s->newest_qpc_  = s->queue_.back().qpc;
    s->first_index_ = s->queue_.front().index;
    s->written_qpc_.store(s->queue_.front().qpc, std::memory_order_relaxed);
    if (ctx->audio) s->audio_ = ctx->audio->BeginClip(s->queue_.front().qpc);
    return s;
}

ClipSession::~ClipSession() { Close(); }

void ClipSession::Close() {
    if (!listed_) return;
    std::lock_guard<std::mutex> lock(ctx_->clip_mutex);
    auto& open = ctx_->open_clips;
    open.erase(std::remove(open.begin(), open.end(), this), open.end());
    listed_ = false;
}

bool ClipSession::Extend(int64_t until_qpc) {
    ClipSession* self = this;
    return ExtendAll(&self, 1, until_qpc);
}

bool ClipSession::ExtendAll(ClipSession* const* sessions, size_t count, int64_t until_qpc) {
    // The disk first, outside the locks: the clips keep writing while it's asked. Room for what each
    // clip hasn't written yet up to the new end, its index and audio included.
    for (size_t i = 0; i < count; ++i) {
        ClipSession& s = *sessions[i];
        if (until_qpc > s.cap_qpc_) return false;
        {
            std::lock_guard<std::mutex> lock(s.mutex_);
            if (s.ended_ || s.stop_) return false;  // no disk probe for a clip that's done
        }
        const int64_t unwritten =
            (std::max)(int64_t{0}, until_qpc - s.written_qpc_.load(std::memory_order_relaxed));
        const auto bytes = static_cast<uint64_t>(s.bytes_per_second_ * static_cast<double>(unwritten) / s.qpc_freq_);
        if (!FolderTakesFile(s.dest_.path, ClipRoomNeeded(bytes))) {
            Log::Warn("clip: \"%ls\" has no room to run longer; not extended", s.dest_.path.c_str());
            return false;
        }
    }
    // Every clip's lock at once (no other path holds two), so none ends between the check and the
    // change: all of them run longer, or none does.
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(count);
    for (size_t i = 0; i < count; ++i) locks.emplace_back(sessions[i]->mutex_);
    for (size_t i = 0; i < count; ++i)
        if (sessions[i]->ended_ || sessions[i]->stop_) return false;
    for (size_t i = 0; i < count; ++i) sessions[i]->until_ = (std::max)(sessions[i]->until_, until_qpc);
    return true;
}

void ClipSession::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
}

ClipSession::Pull ClipSession::EndLocked(ClipEnd why) {
    ended_ = true;
    end_   = why;
    // A clip that reached its end runs to it; one cut short ends one frame past its last.
    end_qpc_ = why == ClipEnd::kReached ? until_ : written_->qpc + qpc_freq_ / fps_;
    return why == ClipEnd::kBroken ? Pull::kFail : Pull::kEnd;
}

ClipSession::Pull ClipSession::Step(int64_t now, MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
    if (next_ == queue_.size()) {
        // Held across Follow and the decision, so an Extend lands either before (and counts) or
        // after (and is refused).
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return EndLocked(ClipEnd::kStopped);
        const TailState state = pin_.Follow(until_, queue_);
        next_ = 0;
        if (queue_.empty()) {
            switch (state) {
                case TailState::kOpen:
                    if (now - newest_qpc_ < SecondsToQpc(kClipStallSeconds, qpc_freq_)) return Pull::kWait;
                    return EndLocked(ClipEnd::kStalled);
                case TailState::kReached:      return EndLocked(ClipEnd::kReached);
                case TailState::kSegmentEnded: return EndLocked(ClipEnd::kSegmentEnded);
                case TailState::kClosing:      return EndLocked(ClipEnd::kClosing);
                case TailState::kBroken:
                    broken_ = true;
                    return EndLocked(ClipEnd::kBroken);
            }
        }
        newest_qpc_ = queue_.back().qpc;
    }

    const AuDescriptor d = queue_[next_++];
    if (written_) pin_.ReleaseThrough(*written_);
    if (!pin_.ReadInto(d, bytes)) {
        std::lock_guard<std::mutex> lock(mutex_);
        broken_ = true;
        ended_  = true;
        end_    = ClipEnd::kBroken;
        return Pull::kFail;
    }
    sample   = {d.is_keyframe, d.index};
    clock_->Add(d.qpc, d.index);
    written_ = d;
    written_qpc_.store(d.qpc, std::memory_order_relaxed);
    if (audio_ && now - last_drain_ >= SecondsToQpc(kClipDrainSeconds, qpc_freq_)) {
        ctx_->audio->DrainClip(*audio_, d.qpc);
        last_drain_ = now;
    }
    return Pull::kSample;
}

ClipSession::Result ClipSession::Run(const std::function<int64_t()>& now, const std::function<void()>& wait) {
    ComInit com;  // the AAC encoder FinishClip may open
    const auto started = std::chrono::steady_clock::now();
    Result result;
    result.path      = dest_.path;
    result.fell_back = dest_.fell_back;
    last_drain_      = now();

    const MuxPullSample next = [&](MuxVideoSample& sample, std::vector<uint8_t>& bytes) {
        for (;;) {
            switch (Step(now(), sample, bytes)) {
                case Pull::kSample: return MuxPull::kSample;
                case Pull::kEnd:    return MuxPull::kEnd;
                case Pull::kFail:   return MuxPull::kFail;
                case Pull::kWait:   wait(); break;
            }
        }
    };
    const MuxTakeTail take_tail = [&](MuxTail& tail) {
        if (audio_) audio_tracks_ = ctx_->audio->FinishClip(*audio_, end_qpc_);
        audio_.reset();
        pin_.Unpin();  // the last AU is written: the mux's finalize must not hold back eviction
        tail.audio = &audio_tracks_;
        if (ctx_->markers) tail.markers = MarkersForFile(ctx_->markers->ForFile(first_qpc_, end_qpc_), *clock_);
        marker_hold_.Reset();
        return true;
    };
    const bool ok = MuxWriteMp4Stream(dest_.path, segment_->codec_header, segment_->width, segment_->height, fps_,
                                      next, take_tail, segment_->codec, 1,
                                      {segment_->picture_width, segment_->picture_height});
    pin_.Unpin();
    audio_.reset();
    marker_hold_.Reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ended_     = true;  // a mux that failed mid-stream never reached EndLocked
        result.end = broken_ ? ClipEnd::kBroken : end_;
    }

    const uint64_t ticks = written_ ? written_->index - first_index_ + 1 : 0;
    if (ok && ticks < static_cast<uint64_t>(std::ceil(kMinClipSeconds * fps_))) {
        DeleteFileW(dest_.path.c_str());
        result.outcome = SaveOutcome::kNothingNew;
    } else {
        result.outcome = ok ? SaveOutcome::kSaved : SaveOutcome::kFailed;
        if (ok) result.seconds = static_cast<double>(ticks) / fps_;
    }
    if (!ok)
        Log::Error("clip: \"%ls\" not written (%s)", dest_.path.c_str(),
                   broken_ ? "the disk fell behind the encoder by more than the ring's headroom"
                           : "couldn't write the file");
    if (Telemetry::Enabled()) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const bool have_size = result.outcome == SaveOutcome::kSaved &&
                               GetFileAttributesExW(dest_.path.c_str(), GetFileExInfoStandard, &info);
        const double output_mb = have_size
            ? static_cast<double>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) /
                  (1024.0 * 1024.0)
            : -1.0;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started).count();
        Telemetry::Event("clip", "outcome=%s end=%s duration_ms=%lld output_mb=%.2f clip_s=%.1f monitor=%s "
                                 "codec=%s fell_back=%d path=%s",
                         SaveOutcomeName(result.outcome), ClipEndName(result.end), static_cast<long long>(ms),
                         output_mb, result.seconds, NarrowUtf8(ctx_->monitor_label).c_str(),
                         VideoCodecName(segment_->codec), result.fell_back ? 1 : 0,
                         NarrowUtf8(dest_.path).c_str());
    }
    return result;
}

bool ClipGroup::Extend(int64_t until_qpc) {
    std::vector<ClipSession*> all;
    for (const auto& s : sessions_) all.push_back(s.get());
    return !all.empty() && ClipSession::ExtendAll(all.data(), all.size(), until_qpc);
}

std::shared_ptr<ClipGroup> StartClip(const std::vector<SaveContext*>& contexts, const ClipRequest& request,
                                     SaveFeedback feedback) {
    struct Operation {
        std::promise<ReplaySaveResults> promise;
        ReplaySaveResults               result{SaveOutcome::kNothingNew, {}};
        std::atomic<size_t>             remaining{0};
        SaveFeedback                    feedback;
    };
    auto op = std::make_shared<Operation>();
    op->feedback = std::move(feedback);
    std::shared_ptr<ClipGroup> group(new ClipGroup());
    group->done_ = op->promise.get_future().share();
    const auto finish = [op, contexts, at = request.event_qpc, app = request.app, id = request.id] {
        op->result.outcome = FoldOutcome(op->result.monitors);
        const bool saved = std::any_of(op->result.monitors.begin(), op->result.monitors.end(),
                                       [](const ReplaySaveResult& m) { return m.outcome == SaveOutcome::kSaved; });
        if (saved) {
            PlaceClipSavedMarker(contexts, at, app);
            PublishClipSaved(contexts, op->result, app, id);
        }
        if (op->feedback) op->feedback(op->result);
        op->promise.set_value(std::move(op->result));
    };

    std::wstring folder;
    if (contexts.size() > 1) {
        const auto* controller = contexts.front()->controller;
        const SYSTEMTIME at = LocalTimeAtQpc(request.event_qpc);  // as a single monitor's file is named
        folder = FormatFilename("{game} {date} {time}", controller ? controller->CurrentGameFolder() : L"", L"", &at);
        const std::wstring titled = ClipTitleForName(request.title);
        if (!titled.empty()) folder += L" - " + titled;
        folder = SanitizeFileName(folder);
    }
    op->result.monitors.resize(contexts.size());
    std::vector<size_t> opened;
    for (size_t i = 0; i < contexts.size(); ++i) {
        SaveOutcome why = SaveOutcome::kFailed;
        auto session = ClipSession::Open(contexts[i], request, folder.empty() ? nullptr : &folder, &why);
        if (!session) {
            op->result.monitors[i].outcome = why;
            continue;
        }
        group->sessions_.push_back(std::move(session));
        opened.push_back(i);
    }
    if (opened.empty()) {
        finish();
        return group;
    }
    op->remaining = opened.size();  // fixed before any clip thread can finish
    for (size_t k = 0; k < opened.size(); ++k) {
        std::thread([op, finish, session = group->sessions_[k], i = opened[k]] {
            Log::SetThreadName("clip");
            const ClipSession::Result r = session->Run(QpcNow, [] { Sleep(kClipPollMs); });
            op->result.monitors[i] = {r.outcome, r.path, r.fell_back, r.seconds};
            if (op->remaining.fetch_sub(1) == 1) finish();
            session->Close();  // last: the engine's exit waits for it before tearing the context down
        }).detach();
    }
    return group;
}

std::optional<AppClips::Started> AppClips::Request(const std::string& app,
                                                   const std::vector<SaveContext*>& contexts,
                                                   const ClipRequest& request,
                                                   const StartSaveFeedback& feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return std::nullopt;
    // A finished clip's group holds no footage (Run unpinned it), only its result: drop it here.
    std::erase_if(open_, [](const auto& entry) {
        return entry.second.group->Done().wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    const auto it = std::find_if(open_.begin(), open_.end(), [&](const auto& e) { return e.first == app; });
    if (it != open_.end()) {
        const int64_t until = request.event_qpc +
            SecondsToQpc(std::clamp(request.after_seconds, 0.0, kClipMaxAfterSeconds), QpcFrequency());
        if (it->second.group->Extend(until)) return Started{it->second.id, true, it->second.group->Done()};
        open_.erase(it);  // past the cap or already ended: this request gets a clip of its own
    }
    const uint64_t id = next_id_++;
    ClipRequest numbered = request;
    numbered.id = id;
    auto group = StartClip(contexts, numbered, feedback ? feedback() : SaveFeedback{});
    Started started{id, false, group->Done()};
    if (started.done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        open_.emplace_back(app, Open{id, std::move(group)});
    return started;
}

void AppClips::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    open_.clear();  // each clip's own thread keeps it running; nothing here outlives the engine's contexts
}

void StopClipsAndWait(SaveContext* ctx) {
    {
        std::lock_guard<std::mutex> lock(ctx->clip_mutex);
        ctx->clips_blocked = true;  // and none opens behind this
        for (ClipSession* s : ctx->open_clips) s->Stop();
    }
    while (ClipsOpen(ctx)) Sleep(10);
}

bool ClipsOpen(SaveContext* ctx) {
    std::lock_guard<std::mutex> lock(ctx->clip_mutex);
    return !ctx->open_clips.empty();
}

}  // namespace ombra
