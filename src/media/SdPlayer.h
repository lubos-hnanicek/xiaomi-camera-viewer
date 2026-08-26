#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge/Bridge.h"
#include "config/Config.h"
#include "media/AudioDecoder.h"
#include "media/AudioPlayer.h"
#include "media/VideoDecoder.h"
#include "render/VideoFrameTexture.h"

struct AVFrame;

namespace xv {

// One recording on the camera's card.
//
// The start is the whole of a clip's identity. Clips begin at whatever second
// the one before them ended rather than on the minute, so a start cannot be
// computed from a wall clock -- only read from the camera's catalogue -- and
// asking to play any other instant is refused.
struct SdClip {
    int64_t start = 0; // Unix seconds
    int32_t duration = 0;
    // True when the camera marked this minute as containing a detection. The
    // mark is packed into the index's length word, not a longer file.
    bool event = false;

    [[nodiscard]] int64_t end() const { return start + duration; }
};

enum class SdState {
    Idle,
    Connecting,
    Loading, // connected, fetching the catalogue
    Ready,   // catalogue in hand, showing the live picture
    Playing,
    Failed,
};

// SdPlayer plays the footage on a camera's SD card.
//
// The camera streams the recording over the same socket as the live picture,
// in real time, and the only handle on it is the catalogue of clip start
// times. That is what the CW400 and the CW500's main lens do. A CW500 also
// writes a second file per timestamp, but that picture has no local open path:
// channel 1's index is empty, so this player is not offered for that tile.
//
// What it adds is knowing where in time the picture is. Streamed playback
// restarts its timestamps at zero for every clip, and the moment on screen is
// the catalogue start plus that offset.
class SdPlayer {
public:
    struct Status {
        SdState state = SdState::Idle;
        std::string message;
        // Set when something went wrong that the user has to know about. Kept
        // apart from message, which is ordinary progress.
        std::string error;

        size_t clips = 0;
        // What the card covers, as Unix seconds, both zero when unknown.
        int64_t oldest = 0;
        int64_t newest = 0;

        // The moment the picture on screen was recorded, or zero while the live
        // picture is showing.
        int64_t position = 0;
        // The moment that was asked for, which is where the clip containing it
        // began rather than the instant itself.
        int64_t requested = 0;

        int width = 0;
        int height = 0;
        uint64_t framesDecoded = 0;
        bool audible = false;
        // True while a clip is being played from a downloaded file rather than
        // streamed. Unused: the CW500's second picture cannot be opened locally.
        bool filePlayback = false;
    };

    SdPlayer() = default;
    ~SdPlayer();

    SdPlayer(const SdPlayer&) = delete;
    SdPlayer& operator=(const SdPlayer&) = delete;

    // Opens a session with the camera and fetches its catalogue. Returns at
    // once; watch status() for progress.
    void open(D3D11Context& gpu, CameraConfig camera, AccountConfig account);
    void close();

    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] const CameraConfig& camera() const { return camera_; }

    // The catalogue, oldest first. Empty until the state reaches Ready. Copied
    // rather than borrowed: it is read every frame by the UI thread and rebuilt
    // by another, and a fortnight of clips is a few hundred kilobytes.
    [[nodiscard]] std::vector<SdClip> clips() const;
    [[nodiscard]] size_t clipCount() const;

    // Counts catalogues, so a view can tell whether the one it arranged into
    // days and hours is still the one in hand. A count of clips would not do:
    // two cameras can hold the same number of them.
    [[nodiscard]] uint64_t catalogueVersion() const {
        return catalogueVersion_.load(std::memory_order_acquire);
    }

    // Plays from the clip covering this instant, and on through the clips that
    // follow it. Nothing happens if no clip covers it.
    void play(int64_t instant);
    void stop();

    // Render-thread side, exactly as StreamWorker's.
    bool present(D3D11Context& gpu, VideoFrameTexture& texture);
    [[nodiscard]] Status status() const;

    void listen(AudioPlayer* speaker);
    void mute();
    [[nodiscard]] bool listening() const {
        return speaker_.load(std::memory_order_acquire) != nullptr;
    }

private:
    struct FileJob {
        std::filesystem::path path;
        SdClip clip;
        size_t index = 0;
    };

    void run(D3D11Context* gpu);
    void commandLoop();
    void fileLoop();

    bool openSession();
    void closeSession();
    void loadCatalogue();

    void onFrame(const uint8_t* data, const XmbFrame& meta, VideoDecoder& decoder,
                 D3D11Context& gpu);
    void onDecodedFrame(const AVFrame* frame);
    void serviceAudio(const uint8_t* data, const XmbFrame& meta);

    // Works out where in time a frame belongs, from the timestamp the camera
    // gave it and the clip boundaries crossed so far. True when the camera has
    // jumped to a different recording (or back to live), which is when the
    // decoder has to be flushed: it cannot continue a GOP across that.
    bool trackPosition(const XmbFrame& meta);

    void setState(SdState state, const std::string& message);
    void setError(const std::string& error);
    void invalidatePicture();

    [[nodiscard]] bool usesFilePlayback() const;
    [[nodiscard]] uint32_t fileChannel() const;
    [[nodiscard]] bool commandInterrupted();
    bool fetchRecordingFile(const SdClip& clip, std::filesystem::path& path, std::string& error);
    bool enqueueClip(size_t index, bool reportError);
    void runFilePlayback(int64_t instant);
    void abortFilePlayback();
    void clearFileQueue();
    void playFile(const FileJob& job);

    [[nodiscard]] Bridge::Stream stream() const;

    CameraConfig camera_;
    AccountConfig account_;
    D3D11Context* gpu_ = nullptr;

    std::thread thread_;
    std::thread commandThread_;
    std::thread fileThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex statusMutex_;
    Status status_;

    mutable std::mutex clipsMutex_;
    std::vector<SdClip> clips_;

    std::mutex frameMutex_;
    AVFrame* pendingFrame_ = nullptr;

    mutable std::mutex streamMutex_;
    Bridge::Stream stream_ = nullptr;

    // Requests for the command thread. Sending playback commands takes a round
    // trip to the camera and the reader thread is blocked waiting for frames,
    // so neither of them can be the thread that asks.
    std::mutex commandMutex_;
    std::condition_variable commandSignal_;
    bool catalogueWanted_ = true;
    // Zero means nothing pending, negative means stop.
    int64_t playWanted_ = 0;

    // Downloaded clips, played on fileThread_ so the reader can stay blocked
    // in the live socket. Kept for the path that would fetch a second CW500
    // picture; that fetch is refused by the camera, so the thread is not
    // started for any tile this build offers.
    std::mutex fileMutex_;
    std::condition_variable fileSignal_;
    std::deque<FileJob> fileQueue_;
    std::atomic<bool> filePlaying_{false};
    std::atomic<bool> fileAbort_{false};
    std::atomic<bool> fileBusy_{false};
    std::atomic<bool> dropPicture_{false};
    AudioDecoder fileAudioDecoder_;

    // Which clip is on screen, as an index into clips_, and the timestamp of the
    // last frame. Touched only by the reader thread.
    size_t currentClip_ = 0;
    int64_t lastPts_ = -1;
    // Set when a playback request has been accepted but its first frame has not
    // arrived. The camera marks the switch by restarting its timestamps, and
    // that first restart means "playback began" rather than "a clip ended".
    std::atomic<bool> awaitingFirstFrame_{false};
    std::atomic<int64_t> playingFrom_{0};

    std::atomic<uint64_t> catalogueVersion_{0};

    std::atomic<AudioPlayer*> speaker_{nullptr};
    AudioDecoder audioDecoder_;
};

} // namespace xv
