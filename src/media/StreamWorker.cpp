#include "media/StreamWorker.h"

#include <algorithm>
#include <exception>
#include <format>
#include <string_view>

#include "app/Log.h"
#include "media/AudioFormat.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace xv {
namespace {

using namespace std::chrono_literals;

// Reconnect backoff. Cameras drop sessions routinely (another client connects,
// the radio hiccups), so the first retry is quick and only sustained failure
// slows things down.
constexpr auto kMinBackoff = 1s;
constexpr auto kMaxBackoff = 30s;

std::chrono::seconds backoffFor(int attempt) {
    auto delay = kMinBackoff * (1 << std::min(attempt, 5));
    return std::min(std::chrono::duration_cast<std::chrono::seconds>(delay), kMaxBackoff);
}

// How long a pan/tilt hold survives without being renewed. The pad renews it
// every frame it is drawn, so this only has to outlast a slow frame.
constexpr auto kPtzHoldTimeout = 500ms;

} // namespace

const char* streamStateName(StreamState state) {
    switch (state) {
    case StreamState::Idle: return "Idle";
    case StreamState::Connecting: return "Connecting";
    case StreamState::Streaming: return "Live";
    case StreamState::Reconnecting: return "Reconnecting";
    case StreamState::Failed: return "Failed";
    }
    return "?";
}

StreamWorker::~StreamWorker() {
    stop();
}

void StreamWorker::start(D3D11Context& gpu, CameraConfig camera, AccountConfig account) {
    stop();

    camera_ = std::move(camera);
    account_ = std::move(account);

    stopping_.store(false, std::memory_order_release);
    permanent_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    setStatus(StreamState::Connecting, "Connecting");

    thread_ = std::thread(&StreamWorker::run, this, &gpu);
    ptzThread_ = std::thread(&StreamWorker::ptzLoop, this);
}

void StreamWorker::stop() {
    stopping_.store(true, std::memory_order_release);
    ptzSignal_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
    if (ptzThread_.joinable()) {
        ptzThread_.join();
    }

    running_.store(false, std::memory_order_release);

    std::scoped_lock lock(frameMutex_);
    if (pendingFrame_ != nullptr) {
        av_frame_free(&pendingFrame_);
    }
}

void StreamWorker::run(D3D11Context* gpu) {
    int attempt = 0;

    while (!stopping_.load(std::memory_order_acquire)) {
        const bool connected = session(*gpu);

        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }

        // Some failures are settled facts about the camera rather than bad
        // luck, and retrying them only buries the reason under a reconnect
        // spinner.
        if (permanent_.load(std::memory_order_acquire)) {
            XV_WARN("{}: not retrying, the failure is permanent", camera_.label());
            running_.store(false, std::memory_order_release);
            return;
        }

        // A session that carried frames before dropping is treated as healthy,
        // so a long-lived stream that blips does not inherit a long backoff.
        attempt = connected ? 0 : attempt + 1;

        const auto delay = backoffFor(attempt);
        {
            std::scoped_lock lock(statusMutex_);
            status_.attempt = attempt;
        }
        setStatus(StreamState::Reconnecting,
                  std::format("Reconnecting in {}s", static_cast<int>(delay.count())));

        // Sleep in slices so stopping stays responsive.
        constexpr auto kSlice = 100ms;
        const auto slices = std::chrono::duration_cast<std::chrono::milliseconds>(delay) / kSlice;
        for (auto i = decltype(slices){0}; i < slices; ++i) {
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::sleep_for(kSlice);
        }
    }

    running_.store(false, std::memory_order_release);
    setStatus(StreamState::Idle, "Stopped");
}

bool StreamWorker::session(D3D11Context& gpu) {
    setStatus(StreamState::Connecting, "Negotiating with the camera");

    Json request{
        {"user_id", account_.userId},
        {"did", camera_.did},
        {"model", camera_.model},
        {"ip", camera_.ip},
        {"channel", camera_.channel},
        {"quality", camera_.quality},
        {"transport", camera_.transport},
        {"audio", camera_.audio},
    };

    // Logged in full because this is the line that matters when a camera will
    // not connect: model, address and channel are what distinguish a wrong
    // region from an unsupported model from an unreachable host.
    XV_INFO("{}: opening did={} model={} ip={} channel={} quality={} region={}", camera_.label(),
            camera_.did, camera_.model, camera_.ip,
            camera_.channel.empty() ? "0" : camera_.channel,
            camera_.quality.empty() ? "hd" : camera_.quality,
            account_.region.empty() ? "cn" : account_.region);

    Json info;
    Bridge::Stream stream = Bridge::instance().openStream(request, info);
    if (stream == nullptr) {
        const std::string reason = responseError(info);
        XV_WARN("{}: could not open stream: {}", camera_.label(), reason);
        setStatus(StreamState::Failed, reason);
        if (info.value("permanent", false)) {
            permanent_.store(true, std::memory_order_release);
        }
        return false;
    }

    XV_INFO("{}: connected over {} to {} (vendor {})", camera_.label(),
            info.value("protocol", std::string("?")),
            info.value("remote_addr", std::string("?")),
            info.value("vendor", std::string("?")));

    {
        std::scoped_lock lock(statusMutex_);
        status_.host = info.value("host", std::string());
    }
    setStatus(StreamState::Streaming, "Waiting for the first keyframe");

    setCurrentStream(stream);

    VideoDecoder decoder;
    std::vector<uint8_t> buffer;
    XmbFrame meta{};

    // A new session may negotiate a different format, and a recording started
    // in it must describe what this session sends rather than the last one.
    audioCodec_.store(0, std::memory_order_release);
    audioRate_.store(0, std::memory_order_release);
    {
        std::scoped_lock lock(statusMutex_);
        status_.audio.clear();
    }

    bool sawVideo = false;
    bool sawKeyframe = false;

    while (!stopping_.load(std::memory_order_acquire)) {
        if (!Bridge::instance().readFrame(stream, buffer, meta)) {
            break;
        }
        if (meta.size == 0) {
            continue; // buffer was grown; the frame itself was dropped
        }

        {
            std::scoped_lock lock(statusMutex_);
            status_.framesReceived++;
            status_.bytesReceived += meta.size;
        }

        if (meta.kind == XMB_KIND_AUDIO) {
            serviceAudio(buffer.data(), meta);
            continue;
        }
        if (meta.kind != XMB_KIND_VIDEO) {
            continue;
        }

        sawVideo = true;

        if (!decoder.isOpen()) {
            std::string error;
            if (!decoder.open(gpu, meta.codec, error)) {
                XV_ERROR("{}: {}", camera_.label(), error);
                setStatus(StreamState::Failed, error);
                break;
            }
        }

        // Feeding inter frames before the first keyframe just produces decoder
        // noise, so wait for a clean entry point.
        if (!sawKeyframe) {
            if (meta.keyframe == 0) {
                continue;
            }
            sawKeyframe = true;
            setStatus(StreamState::Streaming, "Live");
            XV_INFO("{}: first keyframe, codec={}", camera_.label(),
                    meta.codec == XMB_CODEC_H265 ? "H.265" : "H.264");
        }

        // Recording takes the camera's bytes, not the decoder's picture, so it
        // happens before the decode and is unaffected by it.
        serviceGlobalRecording(buffer.data(), meta);
        serviceRecording(buffer.data(), meta);

        decoder.decode(buffer.data(), meta.size, meta.pts_ms,
                       [this](const AVFrame* frame) { onDecodedFrame(frame); });

        {
            std::scoped_lock lock(statusMutex_);
            status_.framesDecoded = decoder.framesDecoded();
        }
    }

    // Worth saying out loud: a camera that carried video but no audio after
    // being asked for both is the case where listening silently does nothing.
    if (camera_.audio && audioCodec_.load(std::memory_order_acquire) == 0 && sawVideo) {
        XV_WARN("{}: no audio arrived, although the session asked for it", camera_.label());
    }

    // A file is finished with the session that filled it, so a reconnect starts
    // a new one rather than splicing two different sessions into one timeline.
    notifyGlobalSessionEnded();
    finishRecording();

    // Retire the handle before closing so a PTZ command in flight cannot follow
    // a pointer that is about to be freed.
    setCurrentStream(nullptr);
    Bridge::instance().closeStream(stream);

    if (!stopping_.load(std::memory_order_acquire)) {
        XV_INFO("{}: session ended", camera_.label());
    }

    return sawVideo;
}

void StreamWorker::onDecodedFrame(const AVFrame* frame) {
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr) {
        return;
    }

    // A reference, not a pixel copy: this just bumps the refcount on the
    // decoder's D3D11 surface so it survives until the render thread is done.
    if (av_frame_ref(copy, frame) < 0) {
        av_frame_free(&copy);
        return;
    }

    {
        std::scoped_lock lock(statusMutex_);
        status_.width = frame->width;
        status_.height = frame->height;
    }

    std::scoped_lock lock(frameMutex_);
    if (pendingFrame_ != nullptr) {
        // The render thread never saw the previous frame; discard it.
        av_frame_free(&pendingFrame_);
        std::scoped_lock statusLock(statusMutex_);
        status_.dropped++;
    }
    pendingFrame_ = copy;
}

bool StreamWorker::present(D3D11Context& gpu, VideoFrameTexture& texture) {
    AVFrame* frame = nullptr;

    {
        std::scoped_lock lock(frameMutex_);
        frame = pendingFrame_;
        pendingFrame_ = nullptr;
    }

    if (frame == nullptr) {
        return false;
    }

    const bool updated = texture.update(gpu, frame);
    av_frame_free(&frame);
    return updated;
}

void StreamWorker::setStatus(StreamState state, const std::string& message) {
    std::scoped_lock lock(statusMutex_);
    status_.state = state;
    status_.message = message;
}

StreamWorker::Status StreamWorker::status() const {
    std::scoped_lock lock(statusMutex_);
    return status_;
}

void StreamWorker::holdPtz(const std::string& direction) {
    {
        std::scoped_lock lock(ptzMutex_);
        ptzHeld_ = direction;
        // Renewing the deadline is what keeps the stepping alive. A caller that
        // stops calling stops the movement, whether or not it says so.
        ptzHeldUntil_ = std::chrono::steady_clock::now() + kPtzHoldTimeout;
    }
    ptzSignal_.notify_one();
}

void StreamWorker::releasePtz() {
    {
        std::scoped_lock lock(ptzMutex_);
        if (ptzHeld_.empty()) {
            return;
        }
        ptzHeld_.clear();
    }
    // Nothing is sent on release: the last step has already finished or will
    // finish on its own. The signal only wakes the loop out of its wait.
    ptzSignal_.notify_one();
}

void StreamWorker::setCurrentStream(Bridge::Stream stream) {
    // Taking the lock here also waits out any command already being sent, so a
    // handle is never closed while the PTZ thread is still inside the bridge.
    std::scoped_lock lock(streamMutex_);
    currentStream_ = stream;
}

void StreamWorker::ptzLoop() {
    // How often a held direction is repeated. One step is a few degrees, so this
    // sets the panning speed: short enough to feel continuous, long enough that a
    // brief tap nudges the lens instead of throwing it across the room.
    constexpr auto kStepInterval = std::chrono::milliseconds(300);

    while (!stopping_.load(std::memory_order_acquire)) {
        std::string direction;

        {
            std::unique_lock lock(ptzMutex_);
            ptzSignal_.wait(lock, [this] {
                return !ptzHeld_.empty() || stopping_.load(std::memory_order_acquire);
            });

            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }

            // A hold nobody has renewed lately belongs to a pad that is no longer
            // on screen to release it.
            if (std::chrono::steady_clock::now() > ptzHeldUntil_) {
                ptzHeld_.clear();
                continue;
            }
            direction = ptzHeld_;
        }

        sendPtzStep(direction);

        // Sit out the interval, but give up the moment the button is released or
        // moves to another direction, so the pad stays responsive.
        std::unique_lock lock(ptzMutex_);
        ptzSignal_.wait_for(lock, kStepInterval, [this, &direction] {
            return ptzHeld_ != direction || stopping_.load(std::memory_order_acquire);
        });
    }
}

void StreamWorker::startRecording(std::filesystem::path directory) {
    {
        std::scoped_lock lock(recordMutex_);
        recordDirectory_ = std::move(directory);
    }
    {
        std::scoped_lock lock(statusMutex_);
        status_.recordingError.clear();
    }
    recordRequested_.store(true, std::memory_order_release);
}

void StreamWorker::stopRecording() {
    // The reader thread owns the file and closes it when it next looks, which is
    // on the next frame. Doing it here would mean two threads in one muxer.
    recordRequested_.store(false, std::memory_order_release);
}

void StreamWorker::attachGlobalRecorder(GlobalRecorder* recorder, std::string videoId,
                                        bool audioOwner) {
    std::scoped_lock lock(globalRecordMutex_);
    globalRecorder_ = recorder;
    globalVideoId_ = std::move(videoId);
    globalAudioOwner_ = audioOwner;
    if (globalRecorder_ != nullptr && globalAudioOwner_) {
        globalRecorder_->declareAudioFormat(
            camera_.did, audioCodec_.load(std::memory_order_acquire),
            audioRate_.load(std::memory_order_acquire));
    }
}

void StreamWorker::detachGlobalRecorder() {
    std::scoped_lock lock(globalRecordMutex_);
    globalRecorder_ = nullptr;
    globalVideoId_.clear();
    globalAudioOwner_ = false;
}

void StreamWorker::serviceAudio(const uint8_t* data, const XmbFrame& meta) {
    if (meta.codec != audioCodec_.load(std::memory_order_acquire) ||
        meta.sample_rate != audioRate_.load(std::memory_order_acquire)) {
        audioCodec_.store(meta.codec, std::memory_order_release);
        audioRate_.store(meta.sample_rate, std::memory_order_release);

        // Logged because until this line existed nobody knew what these
        // cameras actually send, and a model that sends something else is
        // going to be found by reading this.
        XV_INFO("{}: audio is {} at {} Hz, first packet {} bytes", camera_.label(),
                audio::codecName(meta.codec), meta.sample_rate, meta.size);

        {
            std::scoped_lock lock(statusMutex_);
            status_.audio =
                std::format("{} at {} kHz", audio::codecName(meta.codec), meta.sample_rate / 1000);
        }

        // A format that changed under a decoder that is already running is not
        // something the decoder can be told about, so it is reopened.
        audioDecoder_.close();
    }

    {
        std::scoped_lock lock(globalRecordMutex_);
        if (globalRecorder_ != nullptr && globalAudioOwner_) {
            globalRecorder_->submitAudio(camera_.did, data, meta.size, meta.codec, meta.sample_rate,
                                         meta.pts_ms);
        }
    }

    if (recorder_.recording()) {
        std::string error;
        if (!recorder_.writeAudio(data, meta.size, meta.pts_ms, error)) {
            abandonRecording(error);
        }
    }

    AudioPlayer* speaker = speaker_.load(std::memory_order_acquire);
    if (speaker == nullptr) {
        // Nobody is listening. Closing here rather than in mute() keeps the
        // decoder on the one thread that ever touches it.
        audioDecoder_.close();
        return;
    }

    if (!audioDecoder_.isOpen()) {
        std::string error;
        if (!audioDecoder_.open(meta.codec, meta.sample_rate, error)) {
            XV_WARN("{}: cannot listen: {}", camera_.label(), error);
            // Muting rather than retrying every packet: the next one would fail
            // the same way and fill the log doing it.
            mute();
            return;
        }
    }

    audioDecoder_.decode(data, meta.size,
                         [speaker](const AVFrame* frame) { speaker->submit(frame); });
}

void StreamWorker::listen(AudioPlayer* speaker) {
    speaker_.store(speaker, std::memory_order_release);
    std::scoped_lock lock(statusMutex_);
    status_.audible = speaker != nullptr;
}

void StreamWorker::mute() {
    listen(nullptr);
}

void StreamWorker::serviceRecording(const uint8_t* data, const XmbFrame& meta) {
    if (!recordRequested_.load(std::memory_order_acquire)) {
        finishRecording();
        return;
    }

    if (!recorder_.recording()) {
        // A file has to start on a keyframe, so a request made mid-GOP waits for
        // the next one. That is a second or two on these cameras.
        if (meta.keyframe == 0) {
            return;
        }

        int width = 0;
        int height = 0;
        {
            std::scoped_lock lock(statusMutex_);
            width = status_.width;
            height = status_.height;
        }

        std::filesystem::path directory;
        {
            std::scoped_lock lock(recordMutex_);
            directory = recordDirectory_;
        }

        // Whatever this session has been carrying. A camera that has sent no
        // audio by now gets a file with no audio track, which is the honest
        // description of what there is to record.
        const AudioTrack audio{audioCodec_.load(std::memory_order_acquire),
                               audioRate_.load(std::memory_order_acquire), 1};

        const auto recordingStartUtc = std::chrono::system_clock::now();
        std::string error;
        if (!recorder_.open(directory / recordingFileName(recordingStartUtc), recordingStartUtc,
                            meta.codec, width, height, data, meta.size, audio, error)) {
            // Whatever went wrong would go wrong again on the next keyframe, so
            // the request is dropped rather than retried into a loop of errors.
            XV_ERROR("{}: cannot record: {}", camera_.label(), error);
            recordRequested_.store(false, std::memory_order_release);
            std::scoped_lock lock(statusMutex_);
            status_.recordingError = error;
            return;
        }
    }

    std::string error;
    if (!recorder_.writeVideo(data, meta.size, meta.pts_ms, meta.keyframe != 0, error)) {
        abandonRecording(error);
        return;
    }

    publishRecordingStatus();
}

void StreamWorker::serviceGlobalRecording(const uint8_t* data, const XmbFrame& meta) {
    int width = 0;
    int height = 0;
    {
        std::scoped_lock lock(statusMutex_);
        width = status_.width;
        height = status_.height;
    }

    std::scoped_lock lock(globalRecordMutex_);
    if (globalRecorder_ != nullptr) {
        globalRecorder_->submitVideo(globalVideoId_, data, meta.size, meta.codec, width, height,
                                     meta.pts_ms, meta.keyframe != 0);
    }
}

void StreamWorker::notifyGlobalSessionEnded() {
    std::scoped_lock lock(globalRecordMutex_);
    if (globalRecorder_ != nullptr) {
        globalRecorder_->sessionEnded(globalVideoId_);
    }
}

void StreamWorker::abandonRecording(const std::string& error) {
    XV_ERROR("{}: recording stopped: {}", camera_.label(), error);

    // Dropping the request rather than retrying: whatever stopped the write,
    // the next packet would meet it again, and a file with a hole in the middle
    // is worth less than a short one that ends cleanly.
    recordRequested_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(statusMutex_);
        status_.recordingError = error;
    }
    finishRecording();
}

void StreamWorker::finishRecording() {
    if (!recorder_.recording()) {
        return;
    }

    const std::filesystem::path path = recorder_.path();
    const double megabytes = static_cast<double>(recorder_.bytesWritten()) / (1024.0 * 1024.0);
    const double seconds = static_cast<double>(recorder_.durationMs()) / 1000.0;

    recorder_.close();
    XV_INFO("{}: recorded {:.1f} s, {:.1f} MB", camera_.label(), seconds, megabytes);

    publishRecordingStatus();
}

void StreamWorker::publishRecordingStatus() {
    std::scoped_lock lock(statusMutex_);
    status_.recording = recorder_.recording();
    status_.recordingPath = recorder_.recording() ? recorder_.path().filename().string() : "";
    status_.recordedBytes = recorder_.bytesWritten();
    status_.recordedMs = recorder_.durationMs();
}

std::string StreamWorker::recordingFileName(
    std::chrono::system_clock::time_point recordingStartUtc) const {
    // Local time, because a recording is looked for by when it was made and the
    // person looking is in their own timezone. UTC if the zone database is not
    // there to ask, which is better than no file at all.
    std::string stamp;
    const auto now = std::chrono::floor<std::chrono::seconds>(recordingStartUtc);
    try {
        stamp = std::format("{:%Y-%m-%d %H-%M-%S}",
                            std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (const std::exception&) {
        stamp = std::format("{:%Y-%m-%d %H-%M-%S}", now);
    }

    // Everything Windows will not have in a name, plus the separators, so a
    // camera called "Front / Back" cannot write into a directory of its own.
    std::string name = camera_.label();
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 0x20 ||
            std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            c = '-';
        }
    }

    return std::format("{} {}.mkv", name, stamp);
}

void StreamWorker::sendPtzStep(const std::string& direction) {
    // Holding the stream lock for the send also keeps the handle alive for its
    // duration, which is why the session retires the handle under the same lock.
    std::scoped_lock lock(streamMutex_);
    if (currentStream_ == nullptr) {
        return; // between sessions; the press has nowhere to go
    }

    const Json response = Bridge::instance().streamCommand(
        currentStream_, Json{{"method", "ptz.step"}, {"direction", direction}});

    if (!responseOk(response)) {
        XV_WARN("{}: pan/tilt step {} failed: {}", camera_.label(), direction,
                responseError(response));
        return;
    }

    XV_DEBUG("{}: pan/tilt step {}", camera_.label(), direction);
}

} // namespace xv
