#include "support/media_probe.h"

#include "support/process.h"
#include "ombra/json.h"
#include "ombra/util.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ombra::test {

namespace {

std::wstring FindTool(const wchar_t* exe) {
    wchar_t dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"OMBRA_FFMPEG_DIR", dir, MAX_PATH) > 0) {
        std::wstring path = std::wstring(dir) + L"\\" + exe;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
        return L"";
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, exe, nullptr, MAX_PATH, found, nullptr) == 0) return L"";
    return found;
}

const std::wstring& RequireTool(const wchar_t* exe) {
    static std::wstring probe = FindTool(L"ffprobe.exe");
    static std::wstring mpeg  = FindTool(L"ffmpeg.exe");
    const std::wstring& path = (std::wstring(exe) == L"ffprobe.exe") ? probe : mpeg;
    if (path.empty()) {
        FAIL("ombra-tests needs " << NarrowUtf8(exe)
             << " on PATH or in %OMBRA_FFMPEG_DIR% to check saved files");
    }
    return path;
}

std::string Str(const JsonValue* v) {
    return (v && v->type == JsonValue::Type::String) ? v->str : std::string();
}

uint32_t UintFrom(const JsonValue* v) {
    if (!v) return 0;
    if (v->type == JsonValue::Type::Number) return static_cast<uint32_t>(v->num);
    if (v->type == JsonValue::Type::String) return static_cast<uint32_t>(std::strtoul(v->str.c_str(), nullptr, 10));
    return 0;
}

}  // namespace

const std::wstring& FfprobePath() { return RequireTool(L"ffprobe.exe"); }
const std::wstring& FfmpegPath() { return RequireTool(L"ffmpeg.exe"); }

std::vector<StreamInfo> ProbeInfo::Of(const std::string& codec_type) const {
    std::vector<StreamInfo> out;
    for (const auto& s : streams)
        if (s.codec_type == codec_type) out.push_back(s);
    return out;
}

ProbeInfo Probe(const std::wstring& file) {
    ProbeInfo info;
    const ProcessResult r = RunProcess(Quote(FfprobePath()) +
                                       L" -v error -show_streams -show_format -of json " + Quote(file));
    if (!r.launched || r.exit_code != 0) {
        info.error = "ffprobe failed: " + r.err;
        return info;
    }
    JsonValue root;
    try {
        root = ParseJson(r.out);
    } catch (const std::string& e) {
        info.error = "ffprobe output is not JSON: " + e;
        return info;
    }
    if (const JsonValue* format = root.Find("format")) {
        info.format_name = Str(format->Find("format_name"));
        info.duration_s  = std::strtod(Str(format->Find("duration")).c_str(), nullptr);
    }
    if (const JsonValue* streams = root.Find("streams"); streams && streams->type == JsonValue::Type::Array) {
        for (const JsonValue& s : streams->arr) {
            StreamInfo si;
            if (const JsonValue* idx = s.Find("index")) si.index = static_cast<int>(idx->num);
            si.codec_type  = Str(s.Find("codec_type"));
            si.codec_name  = Str(s.Find("codec_name"));
            si.width       = UintFrom(s.Find("width"));
            si.height      = UintFrom(s.Find("height"));
            si.sample_rate = UintFrom(s.Find("sample_rate"));
            si.channels    = UintFrom(s.Find("channels"));
            si.color_range     = Str(s.Find("color_range"));
            si.color_space     = Str(s.Find("color_space"));
            si.color_transfer  = Str(s.Find("color_transfer"));
            si.color_primaries = Str(s.Find("color_primaries"));
            if (const JsonValue* tags = s.Find("tags")) {
                si.title = Str(tags->Find("title"));
                if (si.title.empty()) si.title = Str(tags->Find("handler_name"));
            }
            info.streams.push_back(std::move(si));
        }
    }
    info.ok = true;
    return info;
}

std::string DecodeErrors(const std::wstring& file) {
    const ProcessResult r = RunProcess(Quote(FfmpegPath()) + L" -hide_banner -nostdin -v error -i " +
                                       Quote(file) + L" -map 0 -f null -");
    std::string errors = r.err;
    if (!r.launched) errors += "ffmpeg did not launch";
    else if (r.exit_code != 0) errors += "ffmpeg exited with " + std::to_string(r.exit_code);
    return errors;
}

VideoFrames ProbeVideoFrames(const std::wstring& file) {
    VideoFrames frames;
    const ProcessResult r = RunProcess(Quote(FfprobePath()) +
                                       L" -v error -select_streams v:0 -show_frames"
                                       L" -show_entries frame=key_frame,pts_time -of csv=p=0 " + Quote(file));
    std::istringstream lines(r.out);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty() || line == "\r") continue;
        const size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        const bool   key = line.substr(0, comma) == "1";
        const double pts = std::strtod(line.c_str() + comma + 1, nullptr);
        if (frames.count == 0) {
            frames.first_is_key = key;
            frames.first_pts_s  = pts;
        }
        frames.last_pts_s = pts;
        ++frames.count;
    }
    return frames;
}

Pcm DecodeAudio(const std::wstring& file, int ordinal) {
    Pcm pcm;
    const ProcessResult r = RunProcess(Quote(FfmpegPath()) + L" -hide_banner -nostdin -v error -i " +
                                       Quote(file) + L" -map 0:a:" + std::to_wstring(ordinal) +
                                       L" -f s16le -acodec pcm_s16le -ac 2 -ar 48000 pipe:1");
    if (!r.launched || r.exit_code != 0) return pcm;
    pcm.samples.resize(r.out.size() / sizeof(int16_t));
    std::memcpy(pcm.samples.data(), r.out.data(), pcm.samples.size() * sizeof(int16_t));
    return pcm;
}

VideoBgra DecodeVideoBgra(const std::wstring& file, int max_frames) {
    VideoBgra     video;
    const ProbeInfo probe   = Probe(file);
    const auto      streams = probe.Of("video");
    if (!probe.ok || streams.empty()) return video;
    const ProcessResult r = RunProcess(Quote(FfmpegPath()) + L" -hide_banner -nostdin -v error -i " +
                                       Quote(file) +
                                       L" -map 0:v:0" +
                                       (max_frames > 0 ? L" -frames:v " + std::to_wstring(max_frames) : std::wstring()) +
                                       L" -vf scale=in_color_matrix=bt709:in_range=tv,format=bgra"
                                       L" -f rawvideo pipe:1");
    if (!r.launched || r.exit_code != 0) return video;
    video.width  = streams[0].width;
    video.height = streams[0].height;
    // ffmpeg decodes the container's crop (clap), which the stream's size doesn't include.
    const ProcessResult crop = RunProcess(Quote(FfprobePath()) +
                                          L" -v error -select_streams v:0 -show_entries"
                                          L" stream_side_data=crop_top,crop_bottom,crop_left,crop_right -of csv=p=0 " +
                                          Quote(file));
    unsigned top = 0, bottom = 0, left = 0, right = 0;
    if (crop.exit_code == 0 && sscanf_s(crop.out.c_str(), "%u,%u,%u,%u", &top, &bottom, &left, &right) == 4) {
        video.width -= left + right;
        video.height -= top + bottom;
    }
    video.bytes.assign(r.out.begin(), r.out.end());
    return video;
}

}  // namespace ombra::test
