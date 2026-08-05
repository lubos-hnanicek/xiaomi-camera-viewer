#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace xv {

// Recorder writes the camera's own access units into a Matroska file.
//
// Nothing is re-encoded. The bytes the camera sent are the bytes on disk, so a
// recording is exactly as good as the live picture and costs almost no CPU, and
// the file inherits whatever the camera chose to send: its codec, its
// resolution, and its timing, uneven frame intervals included.
//
// Everything here belongs to the stream worker's thread. The camera hands out
// access units on that thread and this writes them straight through, so there is
// no queue and no second thread to keep in step.
class Recorder {
public:
    Recorder() = default;
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Starts a file from `keyframe`, which has to be one: a recording that
    // begins mid-GOP opens on a picture the decoder cannot build.
    //
    // The parameter sets the camera sends in band become the track's extradata,
    // which is what lets a player know the resolution and profile without
    // decoding anything first.
    bool open(const std::filesystem::path& path, int codec, int width, int height,
              const uint8_t* keyframe, size_t size, std::string& error);

    // Appends one access unit. Fails only on a write error, which ends the
    // recording: a file that silently loses its middle is worse than a short one.
    bool write(const uint8_t* data, size_t size, int64_t ptsMs, bool keyframe,
               std::string& error);

    // Finishes the file. Safe to call when nothing is open, which is what makes
    // it usable from every teardown path.
    void close();

    [[nodiscard]] bool recording() const { return format_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] uint64_t bytesWritten() const { return bytes_; }

    // How much footage is in the file, from the camera's own timestamps rather
    // than the clock, so it measures what was recorded and not how long the
    // button was down.
    [[nodiscard]] int64_t durationMs() const { return lastPts_ < 0 ? 0 : lastPts_; }

private:
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* packet_ = nullptr;

    std::filesystem::path path_;
    uint64_t bytes_ = 0;

    // Timestamps arrive as the camera's own clock, which starts wherever it
    // happens to be, so the file is rebased on its first frame.
    int64_t firstPts_ = -1;
    int64_t lastPts_ = -1;
};

} // namespace xv
