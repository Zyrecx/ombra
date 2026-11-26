#include "ombra/recording.h"

#include "ombra/audio_tracks.h"
#include "ombra/clip.h"
#include "ombra/crash_upload.h"
#include "ombra/events.h"
#include "ombra/json.h"
#include "ombra/log.h"
#include "ombra/marker.h"
#include "ombra/mux.h"
#include "ombra/open_recording.h"
#include "ombra/ring.h"
#include "ombra/supervisor.h"
#include "ombra/telemetry.h"
#include "ombra/util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace ombra {

namespace {

// How often the audio encoded so far goes into the file, well within the audio rings' reach.
constexpr double kRecordDrainSeconds = 1.0;
// A live recording's wait when it has written everything there is, as an open clip's.
constexpr DWORD kRecordPollMs = 50;
// A stopped recording waits this long past the stop for frames captured before it that the encoder
// hasn't handed the ring yet (capture to ring is a few frames); then it ends with what it has.
constexpr double kRecordStopGraceSeconds = 0.5;

int64_t SecondsToQpc(double seconds, int64_t freq) {
    return static_cast<int64_t>(std::llround(seconds * static_cast<double>(freq)));
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".mp4") == 0) name.resize(name.size() - 4);
    return name;
}

}  // namespace

void ApplyReplayReach(const std::vector<SaveContext*>& contexts, ReplayReachState& state, bool replay_on,
                      bool recording, int64_t now) {
    if (!replay_on && recording) state.off_footage = true;
    for (SaveContext* ctx : contexts) {
        if (replay_on) {
            // The floor before the ceiling goes: no save sees the switch's footage end lifted
            // with the recording's still in reach.
            if (state.off_footage) ctx->replay_floor_qpc.store(now, std::memory_order_release);
            ctx->replay_off_qpc.store(0, std::memory_order_release);
        } else {
            int64_t none = 0;
            ctx->replay_off_qpc.compare_exchange_strong(none, now, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(ctx->clip_mutex);
            for (ClipSession* s : ctx->open_clips) s->Stop();
        }
        if (ctx->ring)
            ctx->ring->SetMaxAge(RingMaxAgeQpc(!replay_on && recording ? kRecordOnlyRingSeconds : ctx->buffer_seconds));
    }
    if (replay_on) state.off_footage = false;
}

const char* RecordEndName(RecordEnd end) {
    switch (end) {
        case RecordEnd::kStopped:     return "stopped";
        case RecordEnd::kDiskLow:     return "disk_low";
        case RecordEnd::kWriteFailed: return "write_failed";
        case RecordEnd::kNoFootage:   return "no_footage";
    }
    return "stopped";
}

// One file of the recording: the footage between two cuts.
struct RecordingSession::Part {
    RingPin                                 pin;
    std::shared_ptr<const SegmentParams>    segment;
    uint32_t                                fps = 60;
    std::vector<AuDescriptor>               queue;  // handed out by the pin, not yet written
    size_t                                  next = 0;
    bool                                    open = false;  // the file exists
    Mp4FragmentedWriter                     writer;
    std::optional<AudioTrackHub::ClipAudio> audio;
    std::optional<FileClock>                clock;
    MarkerTimeline::Hold                    marker_hold;
    int64_t                                 first_qpc   = 0;
    uint64_t                                first_index = 0;
    std::optional<AuDescriptor>             written;  // the newest AU written
    int64_t                                 last_drain = 0;
    bool                                    audio_failed = false;
    OpenRecording                           record;             // its crash record
    uint64_t                                durable_ticks = 0;  // what the record's markers cover
};

std::shared_ptr<RecordingSession> RecordingSession::Open(SaveContext* ctx, const RecordRequest& request,
                                                         const std::wstring* group_folder_name, SaveOutcome* why) {
    const auto refuse = [why](SaveOutcome outcome) {
        if (why) *why = outcome;
        return std::shared_ptr<RecordingSession>{};
    };
    std::shared_ptr<RecordingSession> s(new RecordingSession());
    s->ctx_       = ctx;
    s->qpc_freq_  = QpcFrequency();
    s->start_qpc_ = request.start_qpc;
    s->grouped_   = group_folder_name != nullptr;
    {
        // Listed first, as a clip is: a reallocation either sees this recording and refuses, or has
        // already blocked it here.
        std::lock_guard<std::mutex> lock(ctx->clip_mutex);
        if (ctx->clips_blocked) return refuse(SaveOutcome::kBusy);
        ctx->open_recordings.push_back(s.get());
        s->listed_ = true;
    }
    const double before = std::clamp(request.before_seconds, 0.0, ctx->buffer_seconds);
    s->first_floor_qpc_ = request.start_qpc - SecondsToQpc(before, s->qpc_freq_);
    if (request.floor_qpc != 0 && request.floor_qpc >= s->first_floor_qpc_) {
        s->first_floor_qpc_ = request.floor_qpc;
        s->floored_         = true;
    }

    s->app_   = request.app;
    s->marker_app_ = request.marker_app;
    s->title_ = NarrowUtf8(request.title);
    const double rate = ctx->bitrate_kbps.load() * 1000.0 / 8.0;
    s->dest_ = PickClipPath(ctx, request.start_qpc, group_folder_name, request.title,
                            ClipRoomNeeded(static_cast<uint64_t>(rate * kRecordDiskReserveSeconds)), "recording");
    if (s->dest_.path.empty()) {
        crash_upload::ReportError(crash_upload::ErrorKind::Save, "recording_no_room", "",
                                  "recording folder can't take a recording");
        return refuse(SaveOutcome::kFailed);
    }
    return s;
}

RecordingSession::~RecordingSession() { Close(); }

void RecordingSession::Close() {
    if (!listed_) return;
    std::lock_guard<std::mutex> lock(ctx_->clip_mutex);
    auto& open = ctx_->open_recordings;
    open.erase(std::remove(open.begin(), open.end(), this), open.end());
    listed_ = false;
}

bool RecordingSession::DiskLow(const Part& part) const {
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW((FolderOf(paths_.back()) + L"\\").c_str(), &free_bytes, nullptr, nullptr))
        return false;  // a failed look isn't a full disk; a failed write ends the recording
    // The rate this part has written at, the configured one as a floor: a quiet start mustn't
    // promise room for a busy scene.
    double rate = ctx_->bitrate_kbps.load() * 1000.0 / 8.0;
    if (part.written) {
        const double seconds = static_cast<double>(part.written->index - part.first_index + 1) / part.fps;
        if (seconds >= 1.0) rate = (std::max)(rate, static_cast<double>(part.writer.FileBytes()) / seconds);
    }
    return free_bytes.QuadPart < ClipRoomNeeded(static_cast<uint64_t>(rate * kRecordDiskReserveSeconds));
}

std::wstring RecordingSession::NextPartPath() {
    if (parts_stem_.empty()) {
        // The first cut: part 1 moves into a folder named after the recording (or, in a
        // multi-monitor group folder, stays there), renamed "... part 1".
        const std::wstring first = paths_.front();
        parts_stem_ = StemOf(first);
        parts_dir_  = FolderOf(first);
        if (!grouped_) {
            const std::wstring base = parts_dir_ + L"\\" + parts_stem_;
            for (int i = 0; i < 1000; ++i) {
                wchar_t suffix[16] = L"";
                if (i) swprintf_s(suffix, L"_%03d", i);
                const std::wstring folder = base + suffix;
                if (CreateDirectoryW(folder.c_str(), nullptr)) {
                    parts_dir_ = folder;
                    break;
                }
                if (GetLastError() != ERROR_ALREADY_EXISTS) {
                    Log::Warn("recording: can't make the parts' folder \"%ls\" (error %lu); parts go beside part 1",
                              folder.c_str(), GetLastError());
                    break;
                }
            }
        }
        std::wstring moved = parts_dir_ + L"\\" + parts_stem_ + L" part 1.mp4";
        reservations_.push_back(ReserveClipName(moved, L".mp4"));
        if (MoveFileExW(first.c_str(), moved.c_str(), 0)) {
            paths_.front() = moved;
            // Its id finds it too, but not on a volume without ids (FAT32).
            for (OpenRecording& r : kept_records_)
                if (r.Path() == first) r.Moved(moved);
        } else {
            Log::Warn("recording: can't move \"%ls\" to \"%ls\" (error %lu); it keeps its name", first.c_str(),
                      moved.c_str(), GetLastError());
        }
    }
    std::wstring path = parts_dir_ + L"\\" + parts_stem_ + L" part " + std::to_wstring(paths_.size() + 1) + L".mp4";
    reservations_.push_back(ReserveClipName(path, L".mp4"));
    return path;
}

bool RecordingSession::OpenPartFile(Part& part, const AuDescriptor& first) {
    const std::wstring path = paths_.empty() ? dest_.path : NextPartPath();
    if (!EnsureParentDir(path)) {
        Log::Error("recording: can't make the folder for \"%ls\"", path.c_str());
        return false;
    }
    std::vector<MuxAudioTrack> formats;
    if (ctx_->audio) {
        part.audio = ctx_->audio->BeginClip(first.qpc);
        for (const auto& p : part.audio->parts) {
            MuxAudioTrack format = p.track;
            format.frames.clear();
            formats.push_back(std::move(format));
        }
    }
    const SegmentParams& seg = *part.segment;
    part.record.Begin(ctx_->open_records_dir, path, app_, title_);
    if (!part.writer.Open(path, seg.codec_header, seg.width, seg.height, part.fps, seg.codec,
                          {seg.picture_width, seg.picture_height}, formats)) {
        Log::Error("recording: can't create \"%ls\"", path.c_str());
        part.audio.reset();
        part.record.End();
        return false;
    }
    part.record.Opened();
    part.open        = true;
    part.first_qpc   = first.qpc;
    part.first_index = first.index;
    part.clock.emplace(qpc_freq_, part.fps);
    // Markers from the first frame on: the timeline keeps them though the ring lets that footage go.
    if (ctx_->markers) part.marker_hold = ctx_->markers->HoldFrom(first.qpc);
    paths_.push_back(path);
    Log::Info("recording: part %zu \"%ls\"", paths_.size(), path.c_str());
    return true;
}

void RecordingSession::KeepRecordMarkers(Part& part) {
    if (!ctx_->markers || !part.record.Active()) return;
    const uint64_t durable = part.writer.DurableTicks();
    if (durable == part.durable_ticks) return;  // no fragment synced since the last look
    part.durable_ticks = durable;
    const int64_t end_qpc = part.first_qpc + static_cast<int64_t>(durable) * qpc_freq_ / part.fps;
    part.record.SetMarkers(MarkersForFile(ctx_->markers->ForFile(part.first_qpc, end_qpc, marker_app_), *part.clock));
}

void RecordingSession::TakeAudio(Part& part) {
    if (!part.audio) return;
    for (size_t i = 0; i < part.audio->parts.size(); ++i) {
        auto& p = part.audio->parts[i];
        for (const MuxAudioFrame& frame : p.track.frames)
            if (!part.writer.AddAudio(i, frame)) part.audio_failed = true;
        p.taken += p.track.frames.size();
        p.track.frames.clear();
    }
}

bool RecordingSession::FinishPart(Part& part) {
    if (!part.open) return true;
    if (!part.written) {
        // Not a frame went in (its first couldn't be read, or it was stopped at once): no part.
        part.writer.Discard();
        part.record.End();
        part.audio.reset();
        part.pin.Unpin();
        part.marker_hold.Reset();
        part.open = false;
        paths_.pop_back();
        return true;
    }
    const int64_t end_qpc = part.written->qpc + qpc_freq_ / part.fps;
    if (part.audio) {
        ctx_->audio->SettleClip(*part.audio, end_qpc);
        TakeAudio(part);
        part.audio.reset();
    }
    part.pin.Unpin();  // the last AU is written: the index mustn't hold back eviction
    MuxMarkers markers;
    if (ctx_->markers) markers = MarkersForFile(ctx_->markers->ForFile(part.first_qpc, end_qpc, marker_app_), *part.clock);
    part.marker_hold.Reset();
    const bool finished = part.writer.Finish(1, markers);
    // Plain: no record needed. Not: the next engine start makes it plain from its fragments.
    if (finished) {
        part.record.End();
    } else {
        part.record.SetMarkers(markers);
        if (part.record.Active()) kept_records_.push_back(std::move(part.record));  // to follow part 1's move
    }
    const bool ok = finished && !part.audio_failed;
    const uint64_t ticks = part.written->index - part.first_index + 1;
    written_seconds_ += static_cast<double>(ticks) / part.fps;
    seconds_.store(written_seconds_, std::memory_order_relaxed);
    part.open = false;
    if (!ok) Log::Error("recording: \"%ls\" couldn't be finished; it plays up to its last whole second",
                        paths_.empty() ? dest_.path.c_str() : paths_.back().c_str());
    return ok;
}

RecordingSession::Result RecordingSession::Run(const std::function<int64_t()>& now, const std::function<void()>& wait,
                                               const std::function<void(bool)>& started) {
    ComInit com;  // the AAC encoder SettleClip may open
    const auto begun = std::chrono::steady_clock::now();
    bool told = false;
    const auto tell = [&](bool ok) {
        if (told) return;
        told = true;
        if (started) started(ok);
    };

    RecordEnd end = RecordEnd::kStopped;
    std::unique_ptr<Part> part;
    std::vector<uint8_t> bytes;
    // A part starts on the first keyframe past this: what the last part wrote, or the floor.
    // A floor is a hard one (nothing from while replay was off); `before` alone may start at the
    // keyframe before it, as a save does.
    int64_t after_qpc = floored_ ? first_floor_qpc_ - 1 : (std::numeric_limits<int64_t>::min)();
    int64_t last_disk = now();
    size_t  cuts = 0;
    bool    rebuilding = false;  // the last cut was kClosing: an Init waits for every pin to go
    const auto cut = [&](const char* why) {
        if (part->written) after_qpc = (std::max)(after_qpc, part->written->qpc);
        const bool ok = FinishPart(*part);
        if (part->written) Log::Info("recording: part %zu ends (%s)", paths_.size(), why);
        part.reset();
        ++cuts;
        return ok;
    };
    // Moves the audio encoded through `through_qpc` into the file once a drain period has passed:
    // also while footage is paused, so the audio rings can't let go of the part's ending first.
    const auto drain = [&](int64_t t, int64_t through_qpc) {
        if (!part->audio || t - part->last_drain < SecondsToQpc(kRecordDrainSeconds, qpc_freq_)) return;
        ctx_->audio->DrainClip(*part->audio, through_qpc);
        TakeAudio(*part);
        part->last_drain = t;
    };

    for (;;) {
        // Stopped: footage from before the stop is still written, waiting kRecordStopGraceSeconds
        // at most for what the encoder hasn't delivered yet.
        const int64_t stop_at  = stop_qpc_.load(std::memory_order_acquire);
        const bool    stopping = stop_at != (std::numeric_limits<int64_t>::max)();
        const int64_t t = now();
        const bool    grace_over = stopping && t - stop_at >= SecondsToQpc(kRecordStopGraceSeconds, qpc_freq_);
        if (!part) {
            if (rebuilding && ctx_->ring->Closing()) {
                if (stopping) break;
                wait();
                continue;
            }
            rebuilding = false;
            RingPin pin = ctx_->ring->PinCovering(paths_.empty() ? first_floor_qpc_ : after_qpc + 1,
                                                  (std::numeric_limits<int64_t>::max)(), TruncateFloor::kIgnore);
            if (pin.empty()) {
                if (grace_over) break;
                if (paths_.empty() && t - start_qpc_ > SecondsToQpc(kRecordStartSeconds, qpc_freq_)) {
                    end = RecordEnd::kNoFootage;
                    break;
                }
                wait();
                continue;
            }
            part = std::make_unique<Part>();
            part->pin     = std::move(pin);
            part->queue   = part->pin.Aus();
            part->segment = part->pin.Params();
            if (!part->segment && ctx_->supervisor) {
                auto live     = ctx_->supervisor->CurrentParams();
                auto fallback = std::make_shared<SegmentParams>();
                fallback->codec_header = std::move(live.codec_header);
                fallback->codec        = live.codec;
                fallback->width        = live.width;
                fallback->height       = live.height;
                part->segment = fallback;
            }
            if (!part->segment) {
                end = RecordEnd::kWriteFailed;
                break;
            }
            part->fps = part->segment->fps ? part->segment->fps : ctx_->fps.load();
            if (!part->fps) part->fps = 60;
        }

        if (part->open && t - last_disk >= SecondsToQpc(kRecordDiskCheckSeconds, qpc_freq_)) {
            last_disk = t;
            if (DiskLow(*part)) {
                end = RecordEnd::kDiskLow;
                break;
            }
        }
        if (part->next == part->queue.size()) {
            const TailState state = part->pin.Follow((std::numeric_limits<int64_t>::max)(), part->queue);
            part->next = 0;
            if (part->queue.empty()) {
                if (state == TailState::kOpen) {
                    // Stopped: done once the newest frame reaches the stop, or the encoder had its time.
                    if (grace_over || (stopping && part->written &&
                                       part->written->qpc + qpc_freq_ / part->fps >= stop_at))
                        break;
                    // Footage paused (or not yet here): a recording waits, it never ends on a stall.
                    if (part->written) drain(t, part->written->qpc + qpc_freq_ / part->fps);
                    if (part->audio_failed) {
                        end = RecordEnd::kWriteFailed;
                        break;
                    }
                    wait();
                    continue;
                }
                rebuilding = state == TailState::kClosing;
                if (!cut(state == TailState::kSegmentEnded ? "the stream was cut"
                         : state == TailState::kClosing    ? "the buffer is being rebuilt"
                                                           : "the disk fell behind the encoder")) {
                    end = RecordEnd::kWriteFailed;
                    break;
                }
                continue;
            }
        }

        const AuDescriptor d = part->queue[part->next++];
        if (d.qpc >= stop_at) break;
        if (part->open && d.is_keyframe && part->audio && part->audio->format_changed) {
            // The audio changed format (its bitrate crossed 128 kbps) with no cut in the video: a
            // track can't, so the next part starts at this keyframe, losing no frame.
            after_qpc = d.qpc - 1;
            if (!cut("the audio format changed")) {
                end = RecordEnd::kWriteFailed;
                break;
            }
            continue;
        }
        if (!part->open) {
            // A part starts on a keyframe after everything the last one wrote.
            if (!d.is_keyframe || d.qpc <= after_qpc) {
                part->pin.ReleaseThrough(d);
                continue;
            }
            if (!OpenPartFile(*part, d)) {
                end = RecordEnd::kWriteFailed;
                break;
            }
            part->last_drain = t;
        } else if (part->written) {
            part->pin.ReleaseThrough(*part->written);
        }
        if (!part->pin.ReadInto(d, bytes)) {
            after_qpc = (std::max)(after_qpc, d.qpc);  // gone: the next part starts past it
            if (!cut("the disk fell behind the encoder")) {
                end = RecordEnd::kWriteFailed;
                break;
            }
            continue;
        }
        if (!part->writer.AddVideo({d.is_keyframe, d.index}, bytes)) {
            end = RecordEnd::kWriteFailed;
            break;
        }
        part->clock->Add(d.qpc, d.index);
        part->written = d;
        KeepRecordMarkers(*part);
        tell(true);
        seconds_.store(written_seconds_ + static_cast<double>(d.index - part->first_index + 1) / part->fps,
                       std::memory_order_relaxed);
        drain(t, d.qpc);
        if (part->audio_failed) {
            end = RecordEnd::kWriteFailed;
            break;
        }
    }
    if (part && !FinishPart(*part)) end = RecordEnd::kWriteFailed;
    part.reset();
    tell(!paths_.empty());

    Result result;
    result.paths     = paths_;
    result.fell_back = dest_.fell_back;
    result.seconds   = written_seconds_;
    result.end       = end;
    result.outcome   = end == RecordEnd::kWriteFailed ? SaveOutcome::kFailed
                     : paths_.empty()                 ? SaveOutcome::kNothingNew
                                                      : SaveOutcome::kSaved;
    if (end == RecordEnd::kWriteFailed)
        crash_upload::ReportError(crash_upload::ErrorKind::Save, "recording_write_failed", "",
                                  "couldn't write the recording");
    Log::Info("recording: ended (%s), %zu file(s), %.1f s", RecordEndName(end), paths_.size(), written_seconds_);
    if (Telemetry::Enabled()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begun).count();
        Telemetry::Event("recording", "outcome=%s end=%s duration_ms=%lld recording_s=%.1f parts=%zu cuts=%zu "
                                      "monitor=%s fell_back=%d path=%s",
                         SaveOutcomeName(result.outcome), RecordEndName(end), static_cast<long long>(ms),
                         result.seconds, paths_.size(), cuts, NarrowUtf8(ctx_->monitor_label).c_str(),
                         result.fell_back ? 1 : 0, paths_.empty() ? "" : NarrowUtf8(paths_.front()).c_str());
    }
    return result;
}

double RecordingResults::Seconds() const {
    double seconds = 0;
    for (const RecordingSession::Result& m : monitors) seconds = (std::max)(seconds, m.seconds);
    return seconds;
}

void RecordingSession::StopAt(int64_t at_qpc) {
    int64_t current = stop_qpc_.load(std::memory_order_acquire);
    while (at_qpc < current && !stop_qpc_.compare_exchange_weak(current, at_qpc, std::memory_order_acq_rel)) {
    }
}

void RecordingGroup::Stop() {
    const int64_t now = QpcNow();
    for (const auto& s : sessions_) s->StopAt(now);
}

const wchar_t* RecordEndToast(const RecordingResults& result) {
    // How it ended first: a recording cut short says why, whatever it kept.
    if (result.end == RecordEnd::kDiskLow) return L"record_disk_low";
    if (result.end == RecordEnd::kWriteFailed) return L"record_write_failed";
    if (result.end == RecordEnd::kNoFootage || result.outcome == SaveOutcome::kNothingNew) return L"record_nothing";
    return result.outcome == SaveOutcome::kSaved ? L"record_saved" : L"record_failed";
}

double RecordingGroup::Seconds() const {
    double seconds = 0;
    for (const auto& s : sessions_) seconds = (std::max)(seconds, s->Seconds());
    return seconds;
}

std::shared_ptr<RecordingGroup> StartRecording(const std::vector<SaveContext*>& contexts, const RecordRequest& request,
                                               std::shared_ptr<void> hold, RecordFeedback feedback) {
    struct Operation {
        std::promise<bool>             started;
        std::promise<RecordingResults> done;
        RecordingResults               result;
        std::atomic<size_t>            remaining{0};
        std::atomic<size_t>            not_started{0};
        std::atomic<bool>              told{false};
        std::shared_ptr<void>          hold;  // the last thread drops it, before it reports and Closes
        RecordFeedback                 feedback;
    };
    auto op = std::make_shared<Operation>();
    op->hold     = std::move(hold);
    op->feedback = std::move(feedback);
    std::shared_ptr<RecordingGroup> group(new RecordingGroup());
    group->started_ = op->started.get_future().share();
    group->done_    = op->done.get_future().share();
    group->id_      = request.id;
    EventHub* events = contexts.empty() || !request.id ? nullptr : contexts.front()->events;

    // A monitor's first frame (true) or its end before one (false): Started is true at the first
    // true, false once every monitor said false.
    const auto tell = [op, events, app = request.app, id = request.id](bool ok) {
        if (ok) {
            if (op->told.exchange(true)) return;
            op->started.set_value(true);
            if (events)
                events->PublishIfActive("recording_started", [&](JsonValue& f, JsonValue&) {
                    f.Set("app", JsonValue::MakeStr(app));
                    f.Set("recording", JsonValue::MakeNum(static_cast<double>(id)));
                });
            if (op->feedback.started) op->feedback.started();
        } else if (op->not_started.fetch_sub(1) == 1 && !op->told.exchange(true)) {
            op->started.set_value(false);
        }
    };
    const auto finish = [op, events, app = request.app, id = request.id] {
        RecordingResults& r = op->result;
        bool any_saved = false, any_failed = false;
        for (const RecordingSession::Result& m : r.monitors) {
            any_saved |= m.outcome == SaveOutcome::kSaved;
            any_failed |= m.outcome == SaveOutcome::kFailed || m.outcome == SaveOutcome::kBusy;
            if (r.end == RecordEnd::kStopped) r.end = m.end;
        }
        r.outcome = any_failed ? SaveOutcome::kFailed : any_saved ? SaveOutcome::kSaved : SaveOutcome::kNothingNew;
        if (events)
            events->PublishIfActive("recording_stopped", [&](JsonValue& f, JsonValue& private_fields) {
                JsonValue paths = JsonValue::MakeArray();
                for (const RecordingSession::Result& m : r.monitors)
                    for (const std::wstring& p : m.paths) paths.arr.push_back(JsonValue::MakeStr(NarrowUtf8(p)));
                f.Set("app", JsonValue::MakeStr(app));
                f.Set("recording", JsonValue::MakeNum(static_cast<double>(id)));
                f.Set("reason", JsonValue::MakeStr(SaveOutcomeName(r.outcome)));
                f.Set("end", JsonValue::MakeStr(RecordEndName(r.end)));
                f.Set("seconds", JsonValue::MakeNum(std::round(r.Seconds() * 100) / 100));
                private_fields.Set("paths", std::move(paths));
            });
        if (op->feedback.ended) op->feedback.ended(r);
        op->done.set_value(r);
    };

    const std::wstring folder = GroupFolderName(contexts, request.start_qpc, request.title);
    op->result.monitors.resize(contexts.size());
    std::vector<size_t> opened;
    for (size_t i = 0; i < contexts.size(); ++i) {
        SaveOutcome why = SaveOutcome::kFailed;
        auto session = RecordingSession::Open(contexts[i], request, folder.empty() ? nullptr : &folder, &why);
        if (!session) {
            op->result.monitors[i].outcome = why;
            continue;
        }
        group->sessions_.push_back(std::move(session));
        opened.push_back(i);
    }
    if (opened.empty()) {
        op->hold.reset();
        op->told = true;
        op->started.set_value(false);
        finish();
        return group;
    }
    op->remaining   = opened.size();  // fixed before any thread can finish
    op->not_started = opened.size();
    for (size_t k = 0; k < opened.size(); ++k) {
        std::thread([op, tell, finish, session = group->sessions_[k], i = opened[k]] {
            Log::SetThreadName("recording");
            RecordingSession::Result r = session->Run(QpcNow, [] { Sleep(kRecordPollMs); }, tell);
            op->result.monitors[i] = std::move(r);
            if (op->remaining.fetch_sub(1) == 1) {
                op->hold.reset();  // capture may disarm: nothing reads the rings for this any more
                if (!op->told.exchange(true)) op->started.set_value(false);  // never left unset
                finish();
                // The callbacks hold engine objects that go once the exit wait below passes.
                op->feedback = {};
            }
            session->Close();  // last: the engine's exit waits for it before tearing the context down
        }).detach();
    }
    return group;
}

Recordings::Started Recordings::Start(const std::string& app, const std::vector<SaveContext*>& contexts,
                                      RecordRequest request,
                                      const std::function<std::shared_ptr<void>(RecordRequest&)>& arm,
                                      RecordFeedback feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return {Refusal::kClosed, 0, nullptr};
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.app == app; });
    if (it != entries_.end()) {
        if (it->group->Done().wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return {Refusal::kAlreadyRecording, it->id, it->group};
        entries_.erase(it);  // an ended one the app never stopped: this start replaces it
    }
    request.id = next_id_++;
    request.marker_app = app;  // its markers are this app's, not another app's
    std::shared_ptr<void> hold = arm ? arm(request) : nullptr;
    auto group = StartRecording(contexts, request, std::move(hold), std::move(feedback));
    entries_.push_back({app, request.id, group});
    return {Refusal::kNone, request.id, std::move(group)};
}

std::shared_ptr<RecordingGroup> Recordings::Stop(const std::string& app, uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.app == app && (id == 0 || e.id == id); });
    if (it == entries_.end()) return nullptr;
    auto group = std::move(it->group);
    entries_.erase(it);
    group->Stop();
    return group;
}

std::vector<Recordings::Running> Recordings::List(const std::string& app) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Running> out;
    for (const Entry& e : entries_)
        if ((app.empty() || e.app == app) && e.group->Done().wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            out.push_back({e.id, e.app, e.group->Seconds()});
    return out;
}

void Recordings::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    for (const Entry& e : entries_) e.group->Stop();
    entries_.clear();  // each recording's own threads finish it; nothing here outlives the contexts
}

}  // namespace ombra
