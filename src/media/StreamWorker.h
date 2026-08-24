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
#include "media/AudioDecoder.h"
#include "media/AudioPlayer.h"
#include "media/GlobalRecorder.h"
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

        // What the camera is sending on the audio track, named for a tooltip,
        // and empty until the first audio frame of the session arrives.
        std::string audio;
        // Whether this camera is the one being listened to. Only one is.
        bool audible = false;

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

    // Attaches this logical live view to an app-level multi-track recording.
    // The recorder outlives every worker; detach only stops future submissions.
    void attachGlobalRecorder(GlobalRecorder* recorder, std::string videoId, bool audioOwner);
    void detachGlobalRecorder();

    // Sends this camera's audio to `speaker` until muted. The player is shared
    // between all cameras and only one may hold it, which the app enforces by
    // muting the previous camera before handing it over.
    //
    // Decoding happens on the reader thread and only while listening, so a
    // muted camera costs nothing beyond receiving the packets it was already
    // receiving.
    void listen(AudioPlayer* speaker);
    void mute();
    [[nodiscard]] bool listening() const {
        return speaker_.load(std::memory_order_acquire) != nullptr;
    }

private:
    void run(D3D11Context* gpu);
    bool session(D3D11Context& gpu);
    void onDecodedFrame(const AVFrame* frame);

    void setStatus(StreamState state, const std::string& message);

    void ptzLoop();
    void sendPtzStep(const std::string& direction);
    void setCurrentStream(Bridge::Stream stream);

    // Everything one audio access unit is used for: remembering the format,
    // feeding the file if one is open, and feeding the speaker if this camera
    // is the one being listened to.
    void serviceAudio(const uint8_t* data, const XmbFrame& meta);

    // Keeps the recorder in step with what the UI asked for, and writes this
    // access unit if a file is open. Called for every video frame.
    void serviceRecording(const uint8_t* data, const XmbFrame& meta);
    void serviceGlobalRecording(const uint8_t* data, const XmbFrame& meta);
    void notifyGlobalSessionEnded();
    // Ends a recording that cannot be continued, and says why on the tile.
    void abandonRecording(const std::string& error);
    void finishRecording();
    void publishRecordingStatus();
    [[nodiscard]] std::string recordingFileName(
        std::chrono::system_clock::time_point recordingStartUtc) const;

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

    // The audio format seen this session. The recorder needs it before it can
    // write a header, so it is remembered from the first audio frame. Atomic
    // because attaching a global recorder reads it from the UI thread.
    std::atomic<int> audioCodec_{0};
    std::atomic<int> audioRate_{0};

    // Where decoded audio goes, or null when this camera is not the audible
    // one. Set from the UI thread and read by the reader thread, which is the
    // only one that touches the decoder behind it.
    std::atomic<AudioPlayer*> speaker_{nullptr};
    AudioDecoder audioDecoder_;

    // Recording lives entirely on the reader thread, which is where the access
    // units are. The UI only sets the wish and reads the published status.
    std::atomic<bool> recordRequested_{false};
    std::mutex recordMutex_;
    std::filesystem::path recordDirectory_;
    Recorder recorder_;

    // Unlike the per-camera recorder, the global recorder is fed by several
    // reader threads. These fields are only an attachment; the recorder copies
    // packets into its own single-writer queue.
    std::mutex globalRecordMutex_;
    GlobalRecorder* globalRecorder_ = nullptr;
    std::string globalVideoId_;
    bool globalAudioOwner_ = false;
};

} // namespace xv
