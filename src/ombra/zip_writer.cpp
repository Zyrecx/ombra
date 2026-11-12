#include "ombra/zip_writer.h"

#include <windows.h>

#include <array>

namespace ombra {

namespace {

const std::array<uint32_t, 256>& CrcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void Put16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}
void Put32(std::string& s, uint32_t v) {
    Put16(s, static_cast<uint16_t>(v & 0xFFFF));
    Put16(s, static_cast<uint16_t>(v >> 16));
}

// 1980-01-01 00:00 in DOS date/time: the bundle's entries carry no meaningful mtime.
constexpr uint16_t kDosTime = 0;
constexpr uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;
constexpr uint16_t kUtf8Flag = 1 << 11;

}  // namespace

uint32_t Crc32(const void* data, size_t size, uint32_t crc) {
    const auto& t = CrcTable();
    const auto* p = static_cast<const uint8_t*>(data);
    crc           = ~crc;
    for (size_t i = 0; i < size; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void ZipWriter::Add(std::string_view name, std::string_view bytes) {
    Entry e;
    e.name   = std::string(name);
    e.crc    = Crc32(bytes.data(), bytes.size());
    e.size   = static_cast<uint32_t>(bytes.size());
    e.offset = static_cast<uint32_t>(out_.size());
    Put32(out_, 0x04034B50);  // local file header
    Put16(out_, 20);          // version needed
    Put16(out_, kUtf8Flag);
    Put16(out_, 0);           // stored
    Put16(out_, kDosTime);
    Put16(out_, kDosDate);
    Put32(out_, e.crc);
    Put32(out_, e.size);
    Put32(out_, e.size);
    Put16(out_, static_cast<uint16_t>(e.name.size()));
    Put16(out_, 0);
    out_ += e.name;
    out_.append(bytes.data(), bytes.size());
    entries_.push_back(std::move(e));
}

void ZipWriter::Finish() {
    if (finished_) return;
    finished_                 = true;
    const uint32_t cd_offset  = static_cast<uint32_t>(out_.size());
    for (const Entry& e : entries_) {
        Put32(out_, 0x02014B50);  // central directory header
        Put16(out_, 20);          // made by
        Put16(out_, 20);          // needed
        Put16(out_, kUtf8Flag);
        Put16(out_, 0);
        Put16(out_, kDosTime);
        Put16(out_, kDosDate);
        Put32(out_, e.crc);
        Put32(out_, e.size);
        Put32(out_, e.size);
        Put16(out_, static_cast<uint16_t>(e.name.size()));
        Put16(out_, 0);  // extra
        Put16(out_, 0);  // comment
        Put16(out_, 0);  // disk
        Put16(out_, 0);  // internal attrs
        Put32(out_, 0);  // external attrs
        Put32(out_, e.offset);
        out_ += e.name;
    }
    const uint32_t cd_size = static_cast<uint32_t>(out_.size()) - cd_offset;
    Put32(out_, 0x06054B50);  // end of central directory
    Put16(out_, 0);
    Put16(out_, 0);
    Put16(out_, static_cast<uint16_t>(entries_.size()));
    Put16(out_, static_cast<uint16_t>(entries_.size()));
    Put32(out_, cd_size);
    Put32(out_, cd_offset);
    Put16(out_, 0);
}

bool ZipWriter::Write(const std::wstring& path) const {
    if (!finished_) return false;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD      written = 0;
    const BOOL ok = WriteFile(f, out_.data(), static_cast<DWORD>(out_.size()), &written, nullptr);
    CloseHandle(f);
    return ok && written == out_.size();
}

}  // namespace ombra
