#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "media/MatroskaMuxer.h"

namespace xv {

// Maps one camera clock epoch onto the application's elapsed recording clock.
// Resetting and anchoring a new epoch is how a reconnect becomes a real gap.
class RecordingTimeline {
public:
    void reset() {
        anchored_ = false;
        sourceBaseMs_ = 0;
        elapsedBaseMs_ = 0;
    }
    void anchor(int64_t sourcePtsMs, int64_t elapsedMs) {
        anchored_ = true;
        sourceBaseMs_ = sourcePtsMs;
        elapsedBaseMs_ = elapsedMs;
    }
    [[nodiscard]] bool anchored() const { return anchored_; }
    [[nodiscard]] std::optional<int64_t> map(int64_t sourcePtsMs) const {
        if (!anchored_) {
            return std::nullopt;
        }
        return elapsedBaseMs_ + (sourcePtsMs - sourceBaseMs_);
    }

private:
    bool anchored_ = false;
    int64_t sourceBaseMs_ = 0;
    int64_t elapsedBaseMs_ = 0;
};

class GlobalRecorder {
public:
    enum class State {
        Idle,
        Preparing,
        Recording,
        Error,
    };

    struct Participant {
        std::string videoId;   // did + channel: one per live view
        std::string sourceId;  // did: one physical clock and microphone
        std::string videoTitle;
        std::string audioTitle;
        bool audioOwner = false;
    };

    struct Status {
        State state = State::Idle;
        size_t participants = 0;
        size_t prepared = 0;
        std::string path;
        std::string error;
        uint64_t bytes = 0;
        int64_t durationMs = 0;
    };

    GlobalRecorder() = default;
    ~GlobalRecorder();

    GlobalRecorder(const GlobalRecorder&) = delete;
    GlobalRecorder& operator=(const GlobalRecorder&) = delete;

    bool start(const std::filesystem::path& directory, std::vector<Participant> participants,
               std::string& error);
    void stop();

    void submitVideo(const std::string& videoId, const uint8_t* data, size_t size, int codec,
                     int width, int height, int64_t ptsMs, bool keyframe);
    void submitAudio(const std::string& sourceId, const uint8_t* data, size_t size, int codec,
                     int sampleRate, int64_t ptsMs);
    void declareAudioFormat(const std::string& sourceId, int codec, int sampleRate);
    void sessionEnded(const std::string& videoId);

    [[nodiscard]] bool active() const { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] Status status() const;
    [[nodiscard]] bool participates(const std::string& videoId) const;
    [[nodiscard]] bool audioOwner(const std::string& videoId) const;

private:
    enum class EventType {
        Video,
        Audio,
        AudioFormat,
        SessionEnded,
    };

    struct Event {
        EventType type = EventType::Video;
        std::string id;
        std::vector<uint8_t> data;
        int codec = 0;
        int sampleRate = 0;
        int width = 0;
        int height = 0;
        int64_t ptsMs = 0;
        int64_t elapsedMs = 0;
        bool keyframe = false;
    };

    struct VideoRuntime {
        Participant participant;
        MatroskaVideoTrack spec;
        bool prepared = false;
        bool segmentStarted = false;
        size_t muxTrack = 0;
        int64_t lastOutputMs = -1;
        size_t prerollBytes = 0;
        std::deque<Event> preroll;
        RecordingTimeline timeline;
    };

    struct SourceRuntime {
        std::string title;
        std::string audioOwnerVideoId;
        AudioTrack audio;
        bool audioExpected = false;
        std::optional<size_t> muxTrack;
        int64_t lastOutputMs = -1;
        size_t prerollBytes = 0;
        std::deque<Event> preroll;
    };

    void enqueue(Event event);
    void run();
    bool process(Event event, std::string& error);
    bool prepareVideo(Event event, std::string& error);
    bool openFile(std::string& error);
    bool writeVideo(const Event& event, std::string& error);
    bool writeAudio(const Event& event, std::string& error);
    void endSession(const std::string& videoId);
    void fail(const std::string& error);
    void publishProgress();
    [[nodiscard]] std::string preparationTimeoutError() const;
    [[nodiscard]] int64_t elapsedNow() const;
    [[nodiscard]] std::string fileName(
        std::chrono::system_clock::time_point recordingStartUtc) const;

    static constexpr size_t kMaxQueueBytes = 64 * 1024 * 1024;
    static constexpr size_t kMaxPrerollBytes = 16 * 1024 * 1024;
    static constexpr size_t kMaxAudioPrerollBytes = 2 * 1024 * 1024;
    static constexpr int64_t kAudioFormatWaitMs = 1500;
    static constexpr int64_t kPreparationTimeoutMs = 60'000;

    mutable std::mutex membershipMutex_;
    std::unordered_set<std::string> participantIds_;
    std::unordered_set<std::string> audioOwnerIds_;

    mutable std::mutex statusMutex_;
    Status status_;

    std::atomic<bool> active_{false};
    std::atomic<bool> accepting_{false};
    std::chrono::steady_clock::time_point started_{};
    std::chrono::system_clock::time_point startedUtc_{};
    std::filesystem::path path_;

    std::mutex queueMutex_;
    std::condition_variable queueSignal_;
    std::deque<Event> queue_;
    size_t queuedBytes_ = 0;
    bool stopRequested_ = false;
    std::string pendingError_;
    std::thread thread_;

    // Writer-thread-only state.
    std::vector<std::string> videoOrder_;
    std::unordered_map<std::string, VideoRuntime> videos_;
    std::unordered_map<std::string, SourceRuntime> sources_;
    int64_t latestElapsedMs_ = 0;
    MatroskaMuxer muxer_;
};

[[nodiscard]] const char* globalRecordingStateName(GlobalRecorder::State state);

} // namespace xv
