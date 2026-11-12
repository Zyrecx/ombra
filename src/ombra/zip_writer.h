// A minimal .zip writer: stored entries (no compression), CRC-32, no ZIP64, so every unzipper and
// Explorer opens it. For the diagnostics bundle (diagnostics.h), a few MB of text and dumps; not
// for clips. Written here rather than vendoring zlib/minizip (~100 lines against a dependency).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ombra {

uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0);

class ZipWriter {
public:
    // Builds the archive in memory; Write() puts it on disk at once.
    // `name` uses forward slashes ("logs/ombra-20261106.log"); ASCII or UTF-8.
    void Add(std::string_view name, std::string_view bytes);
    bool Write(const std::wstring& path) const;
    const std::string& Bytes() const { return out_; }  // valid after Finish()
    void               Finish();
    size_t             Count() const { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        uint32_t    crc = 0, size = 0, offset = 0;
    };
    std::vector<Entry> entries_;
    std::string        out_;
    bool               finished_ = false;
};

}  // namespace ombra
