// Crash finalize: every recording file being written has a record in data_dir\recordings\open, so
// the engine start after a crash or a power cut can make a file left fragmented plain
// (Mp4FragmentedWriter::Recover), with the markers it had on the disk. A record is written before
// its file is created and deleted once the file is plain.
#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "ombra/mux.h"

namespace ombra {

// What a record holds.
struct OpenRecordingInfo {
    std::wstring path;
    std::string  file_id;  // FileIdAt's: finds the file when it was moved; empty before it exists
    std::string  app;      // who asked for the recording, as its toasts name it
    std::string  title;
    int          tries = 0;  // recoveries started, synced before each: a file that crashes Recover can't loop
    MuxMarkers   markers;    // those inside footage already on the disk
};
inline constexpr int kMaxRecoverTries = 3;  // engine starts that try a file before it's left fragmented

std::wstring OpenRecordingsDir();  // data_dir\recordings\open; empty when data_dir is unresolvable

// A file's identity on its volume (volume serial, 128-bit file id), stable across a rename or a
// move within the volume. Empty when the file isn't there or the volume has no ids.
std::string FileIdAt(const std::wstring& path);
// Where the file with `file_id` is now, looked up on the volume `on_volume_of` is on. Empty when it's gone,
// or deleted (the Recycle Bin, or \$Extend\$Deleted while a handle keeps it).
std::wstring PathOfFileId(const std::wstring& on_volume_of, const std::string& file_id);

// One file's record. Its methods log and carry on when the record can't be written: the recording
// matters more than its crash insurance. Not thread-safe; the destructor leaves the record.
class OpenRecording {
public:
    // Writes the record for `path`, which doesn't exist yet, into `dir`.
    bool Begin(const std::wstring& dir, const std::wstring& path, const std::string& app, const std::string& title);
    void Opened();                           // the file exists: its id goes in
    void Moved(const std::wstring& path);    // renamed (part 1 into its parts' folder)
    void SetMarkers(const MuxMarkers& markers);  // rewrites the record only when they changed
    void End();                              // the file is plain, or gone: the record goes
    bool Active() const { return !record_.empty(); }
    const std::wstring& Path() const { return info_.path; }

private:
    bool Write();
    std::wstring      record_;
    OpenRecordingInfo info_;
};

// A record an engine left behind, read at start before this engine opens a recording of its own.
struct LeftRecording {
    std::wstring      record;
    OpenRecordingInfo info;
};
std::vector<LeftRecording> ListLeftRecordings(const std::wstring& dir);

struct RecoveredRecording {
    std::wstring path;
    double       seconds = 0;
    std::string  app;
};
// Makes each left file plain where it can (see Mp4FragmentedWriter::Recover) and drops its record.
// A file another program holds, or one that failed (a full disk), keeps its record for the next
// start, a failed one up to kMaxRecoverTries. Returns what was made plain. Slow (reads each file's
// moofs and last fragment): off the engine's main thread; `stop` leaves the rest for the next start.
std::vector<RecoveredRecording> RecoverLeftRecordings(const std::vector<LeftRecording>& left,
                                                      const std::stop_token& stop = {});

// The record's JSON, exposed for tests.
std::string       WriteOpenRecordingJson(const OpenRecordingInfo& info);
bool              ParseOpenRecordingJson(const std::string& text, OpenRecordingInfo& out);

}  // namespace ombra
