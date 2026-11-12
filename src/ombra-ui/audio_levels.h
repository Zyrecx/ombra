#pragma once
// Live levels for the Audio page's meters. The UI reads Windows' own peak meters
// (IAudioMeterInformation) on the endpoint or the app's audio sessions: no stream is opened, nothing
// is captured, and nothing crosses the engine pipe. A capture endpoint only reads while some client
// streams from it, so a mic shows a level while the engine records it.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ombra_ui {

struct LevelTarget {
    std::string id;      // audio.sources id; the reader keys its cache by it
    bool input = false;  // a capture endpoint (mic), else a render endpoint
    std::string device;  // endpoint id (UTF-8), "" or "default" = Windows' default
    std::string app;     // exe name: that app's sessions on the endpoint, not the whole endpoint
};

class LevelReader {
public:
    virtual ~LevelReader() = default;
    // Linear peak (0..1) per target, in order; 0 for a target with nothing to read.
    virtual void Read(const std::vector<LevelTarget>& targets, std::vector<float>* peaks) = 0;
};

// What Settings creates when the Audio page shows. Unset (tests, --drive) means no meters move.
using LevelReaderFactory = std::function<std::unique_ptr<LevelReader>()>;
void SetLevelReaderFactory(LevelReaderFactory factory);
std::unique_ptr<LevelReader> MakeLevelReader();
std::unique_ptr<LevelReader> MakeWasapiLevelReader();

// The same level every read, by source id (tests and --drive's `fixture meters`).
class FixedLevelReader final : public LevelReader {
public:
    explicit FixedLevelReader(std::map<std::string, float> peaks) : peaks_(std::move(peaks)) {}
    void Read(const std::vector<LevelTarget>& targets, std::vector<float>* peaks) override {
        peaks->clear();
        for (const auto& t : targets) {
            const auto it = peaks_.find(t.id);
            peaks->push_back(it != peaks_.end() ? it->second : 0.0f);
        }
    }

private:
    std::map<std::string, float> peaks_;
};

// A shown level falls 20 dB per 1.7 s, OBS's default decay; it rises at once.
constexpr float kMeterDecayDbPerSec = 20.0f / 1.7f;
float PeakToDb(float linear);

}  // namespace ombra_ui
