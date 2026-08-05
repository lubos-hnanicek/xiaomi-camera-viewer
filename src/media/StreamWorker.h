#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <filesystem>

#include "bridge/Bridge.h"
#include "config/Config.h"
#include "media/Recorder.h"
#include "media/VideoDecoder.h"
#include "render/VideoFrameTexture.h"

struct AVFrame;

namespace xv {

enum class StreamState {
    Idle,
    Connecting,
    Streaming,
    Reconnecting,
    Failed,
};

const char* streamStateName(StreamState state);

// StreamWorker runs one camera end to end on its own thread: it opens the
// bridge session, decodes what arrives, and parks the newest decoded frame for
// the render thread to pick up.
//
// Only the newest frame is kept. If the UI falls behind there is no value in
// showing it a backlog of stale video, and dropping is strictly better than
// growing latency on a live view.
class StreamWorker {
public:
    struct Status {
        StreamState state = StreamState::Idle;
        std::string message;
        // Where the camera actually answered, which is not the configured
        // address when it had to be found on the network.
        std::string host;
        uint64_t framesDecoded = 0;
        uint64_t framesReceived = 0;
        uint64_t bytesReceived = 0;
        uint64_t dropped = 0;
        int width = 0;
        int height = 0;
        int attempt = 0;

        // Recording, as the UI needs to describe it: whether a file is open,
        // which one, how much has gone into it, and how much footage that is.
        bool recording = false;
        std::string recordingPath;
        std::string recordingError;
        uint64_t recordedBytes = 0;
        int64_t recordedMs = 0;
    };

    StreamWorker() = default;
    ~StreamWorker();

    StreamWorker(const StreamWorker&) = delete;
    StreamWorker& operator=(const StreamWorker&) = delete;

    void start(D3D11Context& gpu, CameraConfig camera, AccountConfig account);
    void stop();

    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }

    // Render-thread side: blits the pending frame, if any, into `texture`.
    // Returns true when the texture changed.
    bool present(D3D11Context& gpu, VideoFrameTexture& texture);

    [[nodiscard]] Status status() const;
    [[nodiscard]] const CameraConfig& camera() const { return camera_; }

    // Holds a pan/tilt direction down. The camera moves a fixed step per command
    // and stops by itself, so a held direction is repeated until released.
    // Sending happens on a worker thread, so the UI never blocks on the network.
    void holdPtz(const std::string& direction);
    void releasePtz();

    // Asks for the stream to be written to a file under `directory`.
    //
    // Recording starts at the next keyframe, so the request is a wish rather
    // than an act: nothing happens until the camera offers an entry point, and
    // the file is named when it does. The wish outlives a dropped session, which
    // means a reconnect continues into a new file rather than quietly stopping.
    void startRecording(std::filesystem::path directory);
    void stopRecording();
    [[nodiscard]] bool recordingRequested() const {
        return recordRequested_.load(std::memory_order_acquire);
    }

private:
    void run(D3D11Context* gpu);
    bool session(D3D11Context& gpu);
    void onDecodedFrame(const AVFrame* frame);

    void setStatus(StreamState state, const std::string& message);

    void ptzLoop();
    void sendPtzStep(const std::string& direction);
    void setCurrentStream(Bridge::Stream stream);

    // Keeps the recorder in step with what the UI asked for, and writes this
    // access unit if a file is open. Called for every video frame.
    void serviceRecording(const uint8_t* data, const XmbFrame& meta);
    void finishRecording();
    void publishRecordingStatus();
    [[nodiscard]] std::string recordingFileName() const;

    CameraConfig camera_;
    AccountConfig account_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> permanent_{false};

    mutable std::mutex statusMutex_;
    Status status_;

    // Handoff slot for the newest decoded frame.
    std::mutex frameMutex_;
    AVFrame* pendingFrame_ = nullptr;

    // The live session handle, republished each time a session is established.
    std::mutex streamMutex_;
    Bridge::Stream currentStream_ = nullptr;

    // PTZ runs on its own thread. Over UDP the bridge retries an unacknowledged
    // command for several seconds, which must block neither the UI thread nor
    // the frame reader.
    std::thread ptzThread_;
    std::mutex ptzMutex_;
    std::condition_variable ptzSignal_;
    // The direction currently held down, empty when nothing is, and when that
    // hold expires unless it is renewed.
    std::string ptzHeld_;
    std::chrono::steady_clock::time_point ptzHeldUntil_{};

    // Recording lives entirely on the reader thread, which is where the access
    // units are. The UI only sets the wish and reads the published status.
    std::atomic<bool> recordRequested_{false};
    std::mutex recordMutex_;
    std::filesystem::path recordDirectory_;
    Recorder recorder_;
};

} // namespace xv
