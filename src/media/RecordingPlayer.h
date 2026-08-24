#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "media/AudioDecoder.h"
#include "media/AudioPlayer.h"
#include "media/VideoDecoder.h"
#include "render/VideoFrameTexture.h"

struct AVFormatContext;
struct AVFrame;

namespace xv {

[[nodiscard]] constexpr int64_t clampPlaybackPosition(int64_t positionMs, int64_t durationMs) {
    return positionMs < 0 ? 0 : positionMs > durationMs ? durationMs : positionMs;
}

class RecordingPlayer {
public:
    struct VideoInfo {
        int streamIndex = -1;
        std::string title;
        int width = 0;
        int height = 0;
    };

    struct AudioInfo {
        int streamIndex = -1;
        std::string title;
    };

    struct Status {
        bool open = false;
        bool playing = false;
        bool eof = false;
        bool failed = false;
        int64_t positionMs = 0;
        int64_t durationMs = 0;
        int64_t recordingStartUtcMs = -1;
        int selectedAudio = -1; // option index, -1 for mute
        std::string fileName;
        std::string error;
    };

    RecordingPlayer() = default;
    ~RecordingPlayer();

    RecordingPlayer(const RecordingPlayer&) = delete;
    RecordingPlayer& operator=(const RecordingPlayer&) = delete;

    bool open(const std::filesystem::path& path, D3D11Context& gpu, AudioPlayer& audio,
              std::string& error);
    void close();

    void play();
    void pause();
    void toggle();
    void seek(int64_t positionMs);
    void setAudioTrack(int option);

    [[nodiscard]] Status status() const;
    [[nodiscard]] const std::vector<VideoInfo>& videos() const { return videoInfo_; }
    [[nodiscard]] const std::vector<AudioInfo>& audios() const { return audioInfo_; }

    // Render-thread side of the decoder handoff.
    bool present(size_t track, D3D11Context& gpu);
    [[nodiscard]] const VideoFrameTexture* texture(size_t track) const;

private:
    struct VideoState {
        VideoInfo info;
        VideoDecoder decoder;
        std::mutex frameMutex;
        AVFrame* pendingFrame = nullptr;
        VideoFrameTexture texture;
        uint32_t prerollEpoch = 0;
    };

    void run();
    void storeFrame(size_t track, const AVFrame* frame);
    void completePrerollTrack(size_t track);
    void beginPreroll();
    void endPreroll();
    void setStreamDiscard(int selectedAudioOption);
    void clearPendingFrames();
    bool applySeek(int64_t positionMs, std::string& error);
    bool applyAudioSelection(int option, std::string& error);
    void setError(std::string error);
    [[nodiscard]] int64_t positionLocked(std::chrono::steady_clock::time_point now) const;
    [[nodiscard]] int64_t packetPtsMs(int streamIndex, int64_t pts) const;

    AVFormatContext* format_ = nullptr;
    std::filesystem::path path_;
    D3D11Context* gpu_ = nullptr;
    AudioPlayer* audio_ = nullptr;

    std::vector<std::unique_ptr<VideoState>> video_;
    std::vector<VideoInfo> videoInfo_;
    std::vector<AudioInfo> audioInfo_;
    std::unordered_map<int, size_t> videoByStream_;

    AudioDecoder audioDecoder_;
    std::atomic<int> appliedAudioOption_{-2};

    mutable std::mutex controlMutex_;
    std::condition_variable controlSignal_;
    bool stopping_ = false;
    bool workerAlive_ = false;
    bool playing_ = false;
    bool eof_ = false;
    bool prerolling_ = false;
    int64_t basePositionMs_ = 0;
    int64_t durationMs_ = 0;
    int64_t recordingStartUtcMs_ = -1;
    size_t prerollLeft_ = 0;
    uint32_t prerollEpoch_ = 0;
    std::chrono::steady_clock::time_point baseTime_{};
    std::optional<int64_t> seekRequest_;
    int requestedAudioOption_ = -1;
    std::string error_;

    std::thread thread_;
};

} // namespace xv
