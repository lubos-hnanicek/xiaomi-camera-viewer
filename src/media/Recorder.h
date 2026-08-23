#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "media/MatroskaMuxer.h"

namespace xv {

// Recorder writes the camera's own access units into a Matroska file.
//
// Nothing is re-encoded. The bytes the camera sent are the bytes on disk, so a
// recording is exactly as good as the live picture and costs almost no CPU, and
// the file inherits whatever the camera chose to send: its codecs, its
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
    //
    // `audio` describes the second track, and has to be known now rather than
    // when the first audio packet turns up: a Matroska header lists its tracks
    // and cannot grow one later.
    bool open(const std::filesystem::path& path, int codec, int width, int height,
              const uint8_t* keyframe, size_t size, const AudioTrack& audio, std::string& error);

    // Appends one access unit. Fails only on a write error, which ends the
    // recording: a file that silently loses its middle is worse than a short one.
    bool writeVideo(const uint8_t* data, size_t size, int64_t ptsMs, bool keyframe,
                    std::string& error);

    // Appends one audio packet. Silently does nothing when the file has no
    // audio track, or before the first video frame has fixed the timeline the
    // two tracks share.
    bool writeAudio(const uint8_t* data, size_t size, int64_t ptsMs, std::string& error);

    [[nodiscard]] bool hasAudio() const { return hasAudio_; }

    // Finishes the file. Safe to call when nothing is open, which is what makes
    // it usable from every teardown path.
    void close();

    [[nodiscard]] bool recording() const { return muxer_.open(); }
    [[nodiscard]] const std::filesystem::path& path() const { return muxer_.path(); }
    [[nodiscard]] uint64_t bytesWritten() const { return muxer_.bytesWritten(); }

    // How much footage is in the file, from the camera's own timestamps rather
    // than the clock, so it measures what was recorded and not how long the
    // button was down.
    [[nodiscard]] int64_t durationMs() const { return lastPts_ < 0 ? 0 : lastPts_; }

private:
    MatroskaMuxer muxer_;
    bool hasAudio_ = false;

    // Timestamps arrive as the camera's own clock, which starts wherever it
    // happens to be, so the file is rebased on its first frame. Both tracks are
    // rebased on the same one, which is what keeps them in sync with each other.
    int64_t firstPts_ = -1;
    int64_t lastPts_ = -1;
    int64_t lastAudioPts_ = -1;
};

} // namespace xv
