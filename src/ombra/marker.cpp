#include "ombra/marker.h"

#include "ombra/json.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace ombra {

namespace {

struct TypeInfo {
    MarkerType  type;
    const char* name;
    const char* title;
};
constexpr TypeInfo kTypes[] = {
    {MarkerType::kGeneric, "generic", "Marker"},       {MarkerType::kKill, "kill", "Kill"},
    {MarkerType::kDeath, "death", "Death"},            {MarkerType::kAssist, "assist", "Assist"},
    {MarkerType::kWin, "win", "Win"},                  {MarkerType::kLoss, "loss", "Loss"},
    {MarkerType::kObjective, "objective", "Objective"}, {MarkerType::kHighlight, "highlight", "Highlight"},
    {MarkerType::kRoundStart, "round_start", "Round start"}, {MarkerType::kRoundEnd, "round_end", "Round end"},
};

const TypeInfo& InfoOf(MarkerType type) {
    for (const TypeInfo& t : kTypes)
        if (t.type == type) return t;
    return kTypes[0];
}

}  // namespace

const char* MarkerTypeName(MarkerType type) { return InfoOf(type).name; }

std::optional<MarkerType> ParseMarkerType(std::string_view name) {
    for (const TypeInfo& t : kTypes)
        if (name == t.name) return t.type;
    return std::nullopt;
}

std::optional<std::string> CleanMarkerLabel(std::string_view label) {
    std::string out;
    for (size_t i = 0; i < label.size();) {
        const auto b = static_cast<uint8_t>(label[i]);
        const size_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (len == 0 || i + len > label.size()) return std::nullopt;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<uint8_t>(label[i + k]) & 0xC0) != 0x80) return std::nullopt;
        if (len == 1 && (b < 0x20 || b == 0x7F)) {  // control characters: a line break in a chapter
            i += 1;
            continue;
        }
        out.append(label.substr(i, len));
        i += len;
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    out = out.substr(first, out.find_last_not_of(' ') - first + 1);
    size_t chars = 0;  // code points: every byte that isn't a continuation byte
    for (char c : out)
        if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) ++chars;
    if (chars > kMarkerLabelMaxChars) return std::nullopt;
    return out;
}

std::string MarkerChapterTitle(const Marker& marker) {
    std::string title = marker.label.empty() ? InfoOf(marker.type).title : marker.label;
    if (!marker.owner.empty()) title += " \xC2\xB7 " + marker.owner;  // " · "
    return title;
}

// --- FileClock -----------------------------------------------------------------------------------

int64_t FileClock::TickQpc(const Anchor& a, uint64_t tick) const {
    return a.qpc + static_cast<int64_t>((tick - a.tick) * static_cast<uint64_t>(qpc_freq_) / fps_);
}

void FileClock::Add(int64_t qpc, uint64_t tick) {
    // A new anchor where this frame is more than half a frame off where the last anchor's rate
    // puts it: pacer jitter stays inside, a pause or a gap in the timeline doesn't.
    if (anchors_.empty() || tick < anchors_.back().tick ||
        std::llabs(qpc - TickQpc(anchors_.back(), tick)) > qpc_freq_ / fps_ / 2)
        anchors_.push_back({qpc, tick});
    last_qpc_  = qpc;
    last_tick_ = tick;
}

std::optional<uint64_t> FileClock::MsAt(int64_t qpc) const {
    if (anchors_.empty() || qpc < anchors_.front().qpc || qpc > last_qpc_ + qpc_freq_ / fps_) return std::nullopt;
    auto next = std::upper_bound(anchors_.begin(), anchors_.end(), qpc,
                                 [](int64_t q, const Anchor& a) { return q < a.qpc; });
    const Anchor& a = *(next - 1);
    // A millisecond of slack: frames are a whole number of qpc ticks apart, so the Nth frame sits
    // a little before N / fps and would otherwise round down to the frame before it.
    uint64_t tick = a.tick + static_cast<uint64_t>((qpc - a.qpc + qpc_freq_ / 1000) * fps_ / qpc_freq_);
    // Never into the next anchor's span (that frame is after `qpc`) or past the last frame.
    if (next != anchors_.end()) tick = (std::min)(tick, next->tick - 1);
    tick = (std::min)(tick, last_tick_);
    return (tick - anchors_.front().tick) * 1000 / fps_;
}

MuxMarkers MarkersForFile(const std::vector<Marker>& markers, const FileClock& clock) {
    MuxMarkers out;
    JsonValue list = JsonValue::MakeArray();
    for (const Marker& m : markers) {
        const std::optional<uint64_t> ms = clock.MsAt(m.qpc);
        if (!ms) continue;
        out.chapters.push_back({*ms, MarkerChapterTitle(m)});
        JsonValue item = JsonValue::MakeObject();
        item.Set("ms", JsonValue::MakeNum(static_cast<double>(*ms)));
        item.Set("kind", JsonValue::MakeStr(MarkerTypeName(m.type)));
        if (!m.label.empty()) item.Set("label", JsonValue::MakeStr(m.label));
        if (!m.owner.empty()) item.Set("app", JsonValue::MakeStr(m.owner));
        if (m.important) item.Set("important", JsonValue::MakeBool(true));
        list.arr.push_back(std::move(item));
    }
    if (list.arr.empty()) return out;
    JsonValue root = JsonValue::MakeObject();
    root.Set("v", JsonValue::MakeNum(1));
    root.Set("markers", std::move(list));
    out.json = WriteJsonCompact(root);
    return out;
}

// --- MarkerTimeline ------------------------------------------------------------------------------

MarkerTimeline::Hold& MarkerTimeline::Hold::operator=(Hold&& other) noexcept {
    if (this != &other) {
        Reset();
        timeline_ = std::exchange(other.timeline_, nullptr);
        id_       = std::exchange(other.id_, 0);
    }
    return *this;
}

void MarkerTimeline::Hold::Reset() {
    if (timeline_) timeline_->Release(id_);
    timeline_ = nullptr;
    id_       = 0;
}

MarkerTimeline::AddResult MarkerTimeline::Add(Marker& marker, const std::string& app_key, int64_t oldest_qpc) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t keep_from = oldest_qpc;
    for (const HoldRec& h : holds_) keep_from = (std::min)(keep_from, h.from_qpc);
    std::erase_if(entries_, [&](const Entry& e) { return e.marker.qpc < keep_from; });

    const auto mine = std::count_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& e) { return e.app_key == app_key; });
    if (static_cast<size_t>(mine) >= kMarkersPerApp) return AddResult::kAppFull;
    if (entries_.size() >= kMarkersTotal) return AddResult::kFull;
    marker.id = next_id_++;
    // `at` puts a marker in the past, so keep the list in qpc order.
    auto at = std::upper_bound(entries_.begin(), entries_.end(), marker.qpc,
                               [](int64_t q, const Entry& e) { return q < e.marker.qpc; });
    entries_.insert(at, Entry{marker, app_key});
    return AddResult::kAdded;
}

MarkerTimeline::Hold MarkerTimeline::HoldFrom(int64_t from_qpc) {
    std::lock_guard<std::mutex> lock(mutex_);
    Hold hold;
    hold.timeline_ = this;
    hold.id_       = next_hold_++;
    holds_.push_back({hold.id_, from_qpc});
    return hold;
}

void MarkerTimeline::Release(uint64_t hold_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(holds_, [&](const HoldRec& h) { return h.id == hold_id; });
}

std::vector<Marker> MarkerTimeline::Between(int64_t from_qpc, int64_t to_qpc) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Marker> out;
    for (const Entry& e : entries_)
        if (e.marker.qpc >= from_qpc && e.marker.qpc <= to_qpc) out.push_back(e.marker);
    return out;
}

size_t MarkerTimeline::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace ombra
