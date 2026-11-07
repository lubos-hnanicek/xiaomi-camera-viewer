#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace xv {

// The audio the camera is sending, as a file has to describe it before it can
// carry any. Default-constructed means a recording with no sound.
struct AudioTrack {
    int codec = 0; // XMB_CODEC_*, zero for none
    int sampleRate = 0;
    int channels = 1;

    [[nodiscard]] bool valid() const { return codec != 0 && sampleRate > 0; }
};

struct MatroskaVideoTrack {
    int codec = 0; // XMB_CODEC_H264 or XMB_CODEC_H265
    int width = 0;
    int height = 0;
    std::vector<uint8_t> extradata;
    std::string title;
    bool defaultTrack = false;
};

struct MatroskaAudioTrack {
    AudioTrack format;
    std::string title;
    bool defaultTrack = false;
};

// Pulls the Annex-B VPS/SPS/PPS carried by camera keyframes into the form the
// Matroska muxer expects as track extradata.
[[nodiscard]] std::vector<uint8_t> videoParameterSets(const uint8_t* data, size_t size, int codec);

[[nodiscard]] constexpr int64_t monotonicRecordingPts(int64_t candidate, int64_t previous) {
    return candidate <= previous ? previous + 1 : candidate;
}

[[nodiscard]] inline int64_t recordingElapsedMs(
    std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at - start).count();
}

class RecordingTimeline {
public:
    void reset() {
        anchored_ = false;
        sourceClockUsable_ = true;
        sourceBaseMs_ = 0;
        elapsedBaseMs_ = 0;
        lastSourcePtsMs_ = 0;
    }

    [[nodiscard]] bool anchored() const { return anchored_; }

    int64_t mapVideo(int64_t sourcePtsMs, int64_t elapsedMs) {
        if (!anchored_) {
            anchored_ = true;
            sourceBaseMs_ = sourcePtsMs;
            elapsedBaseMs_ = elapsedMs;
            lastSourcePtsMs_ = sourcePtsMs;
            return elapsedMs;
        }

        const int64_t sourceDeltaMs = sourcePtsMs - sourceBaseMs_;
        const int64_t elapsedDeltaMs = elapsedMs - elapsedBaseMs_;
        if (sourceClockUsable_ &&
            (sourcePtsMs <= lastSourcePtsMs_ ||
             (elapsedDeltaMs >= 50 && sourceDeltaMs >= 0 && sourceDeltaMs < elapsedDeltaMs / 4))) {
            sourceClockUsable_ = false;
        }
        lastSourcePtsMs_ = sourcePtsMs;

        return sourceClockUsable_ ? elapsedBaseMs_ + sourceDeltaMs : elapsedMs;
    }

    [[nodiscard]] std::optional<int64_t> mapAudio(int64_t sourcePtsMs, int64_t elapsedMs) const {
        if (!anchored_) {
            return std::nullopt;
        }

        const int64_t ptsMs = sourceClockUsable_ ? elapsedBaseMs_ + (sourcePtsMs - sourceBaseMs_)
                                                 : elapsedMs;
        return ptsMs < 0 ? std::nullopt : std::optional<int64_t>{ptsMs};
    }

private:
    bool anchored_ = false;
    bool sourceClockUsable_ = true;
    int64_t sourceBaseMs_ = 0;
    int64_t elapsedBaseMs_ = 0;
    int64_t lastSourcePtsMs_ = 0;
};

// A small, single-threaded Matroska writer. Callers own timing and threading;
// this class owns only FFmpeg's format context, tracks and packet.
class MatroskaMuxer {
public:
    MatroskaMuxer() = default;
    ~MatroskaMuxer();

    MatroskaMuxer(const MatroskaMuxer&) = delete;
    MatroskaMuxer& operator=(const MatroskaMuxer&) = delete;

    bool open(const std::filesystem::path& path, const std::vector<MatroskaVideoTrack>& video,
              const std::vector<MatroskaAudioTrack>& audio,
              std::chrono::system_clock::time_point recordingStartUtc, std::string& error);

    bool writeVideo(size_t track, const uint8_t* data, size_t size, int64_t ptsMs, bool keyframe,
                    std::string& error);
    bool writeAudio(size_t track, const uint8_t* data, size_t size, int64_t ptsMs,
                    std::string& error);

    void close();

    [[nodiscard]] bool open() const { return format_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] uint64_t bytesWritten() const { return bytes_; }
    [[nodiscard]] int64_t durationMs() const { return durationMs_; }

private:
    bool addVideoTrack(const MatroskaVideoTrack& spec, std::string& error);
    bool addAudioTrack(const MatroskaAudioTrack& spec, std::string& error);
    bool writePacket(AVStream* stream, int64_t& lastPts, const uint8_t* data, size_t size,
                     int64_t ptsMs, bool keyframe, std::string& error);

    AVFormatContext* format_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::vector<AVStream*> video_;
    std::vector<AVStream*> audio_;
    std::vector<int64_t> lastVideoPts_;
    std::vector<int64_t> lastAudioPts_;

    std::filesystem::path path_;
    uint64_t bytes_ = 0;
    int64_t durationMs_ = 0;
    bool wrotePacket_ = false;
};

} // namespace xv
