#include "media/SdPlayer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <format>
#include <iterator>
#include <string_view>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

#include "app/Log.h"

namespace xv {

namespace {

// A frame timestamp that runs backwards by more than this means the camera has
// moved to another recording rather than that frames arrived out of order.
// Clips are about a minute, so a second is far below any real boundary and far
// above any jitter.
constexpr int64_t kResetThresholdMs = 1000;

// Playback on a two-lens camera's main lens arrives as the primary picture.
// The second tile still opens that same session so RDT can fetch its files;
// live frames on that socket are the other lens and are not shown.
std::string playbackChannel(const CameraConfig& camera) {
    if (isDualLens(camera.model)) {
        return {};
    }
    return camera.channel;
}

uint32_t recordingFileChannel(const CameraConfig& camera) {
    if (camera.channel.empty()) {
        return 1;
    }
    unsigned int n = 1;
    const char* first = camera.channel.data();
    const char* last = first + camera.channel.size();
    if (std::from_chars(first, last, n).ec != std::errc{} || n == 0) {
        return 1;
    }
    return n;
}

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string safeFileStem(const std::string& label) {
    std::string name = label;
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 0x20 ||
            std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            c = '-';
        }
    }
    return name;
}

std::string clipStamp(int64_t epoch) {
    const auto instant = std::chrono::sys_seconds{std::chrono::seconds{epoch}};
    try {
        return std::format("{:%Y-%m-%d %H-%M-%S}",
                           std::chrono::zoned_time{std::chrono::current_zone(), instant});
    } catch (const std::exception&) {
        return std::format("{:%Y-%m-%d %H-%M-%S}", instant);
    }
}

std::filesystem::path uniqueDestination(std::filesystem::path path) {
    if (!std::filesystem::exists(path)) {
        return path;
    }
    const auto parent = path.parent_path();
    const auto stem = path.stem();
    const auto ext = path.extension();
    for (int n = 2; n < 1000; ++n) {
        auto candidate = parent / stem;
        candidate += std::format(" ({})", n);
        candidate += ext;
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return path;
}

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

} // namespace

SdPlayer::~SdPlayer() {
    close();
}

void SdPlayer::open(D3D11Context& gpu, CameraConfig camera, AccountConfig account) {
    close();

    camera_ = std::move(camera);
    account_ = std::move(account);
    gpu_ = &gpu;

    stopping_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    filePlaying_.store(false, std::memory_order_release);
    fileAbort_.store(false, std::memory_order_release);
    fileBusy_.store(false, std::memory_order_release);

    {
        std::scoped_lock lock(clipsMutex_);
        clips_.clear();
    }
    {
        std::scoped_lock lock(commandMutex_);
        catalogueWanted_ = true;
        playWanted_ = 0;
    }

    // A previous camera's error, position and picture size would otherwise
    // describe this one until it produced its own.
    {
        std::scoped_lock lock(statusMutex_);
        status_ = Status{};
        status_.filePlayback = usesFilePlayback();
    }
    currentClip_ = 0;
    lastPts_ = -1;
    awaitingFirstFrame_.store(false, std::memory_order_release);
    playingFrom_.store(0, std::memory_order_release);
    if (usesFilePlayback()) {
        invalidatePicture();
    }

    setState(SdState::Connecting, "Connecting to the camera");

    thread_ = std::thread(&SdPlayer::run, this, &gpu);
    commandThread_ = std::thread(&SdPlayer::commandLoop, this);
    saveThread_ = std::thread(&SdPlayer::saveLoop, this);
    if (usesFilePlayback()) {
        fileThread_ = std::thread(&SdPlayer::fileLoop, this);
    }
}

void SdPlayer::close() {
    if (!running_.load(std::memory_order_acquire) && !thread_.joinable()) {
        return;
    }

    stopping_.store(true, std::memory_order_release);
    fileAbort_.store(true, std::memory_order_release);
    commandSignal_.notify_all();
    fileSignal_.notify_all();
    saveSignal_.notify_all();

    // The reader is blocked in readFrame. Closing the session is what unblocks
    // it; joining first waits for a camera that has gone silent, which is
    // exactly the state playback-stop can leave behind. FetchRecording waits
    // on the same session, so this unblocks a download in flight too.
    closeSession();

    if (thread_.joinable()) {
        thread_.join();
    }
    if (commandThread_.joinable()) {
        commandThread_.join();
    }
    if (fileThread_.joinable()) {
        fileThread_.join();
    }
    if (saveThread_.joinable()) {
        saveThread_.join();
    }

    clearFileQueue();
    fileAudioDecoder_.close();
    gpu_ = nullptr;

    running_.store(false, std::memory_order_release);

    std::scoped_lock lock(frameMutex_);
    if (pendingFrame_ != nullptr) {
        av_frame_free(&pendingFrame_);
    }
}

void SdPlayer::run(D3D11Context* gpu) {
    if (!openSession()) {
        running_.store(false, std::memory_order_release);
        commandSignal_.notify_all();
        return;
    }

    setState(SdState::Loading, "Reading the card's catalogue");
    commandSignal_.notify_all();

    VideoDecoder decoder;
    std::vector<uint8_t> buffer;
    XmbFrame meta{};

    while (!stopping_.load(std::memory_order_acquire)) {
        if (!Bridge::instance().readFrame(stream(), buffer, meta)) {
            break;
        }
        if (meta.size == 0) {
            continue; // the buffer was grown; this frame itself was dropped
        }

        // If this tile were the second CW500 lens, the live frames on this
        // socket would be the primary picture. Skip them: that is not playback.
        if (usesFilePlayback()) {
            continue;
        }

        if (meta.kind == XMB_KIND_AUDIO) {
            serviceAudio(buffer.data(), meta);
            continue;
        }
        if (meta.kind != XMB_KIND_VIDEO) {
            continue;
        }

        onFrame(buffer.data(), meta, decoder, *gpu);
    }

    if (!stopping_.load(std::memory_order_acquire)) {
        setError("The camera ended the session");
    }

    // close() may already have taken the handle to unblock this loop.
    closeSession();
    running_.store(false, std::memory_order_release);
    commandSignal_.notify_all();
}

bool SdPlayer::openSession() {
    Json request{
        {"user_id", account_.userId},
        {"did", camera_.did},
        {"model", camera_.model},
        {"ip", camera_.ip},
        {"channel", playbackChannel(camera_)},
        {"quality", camera_.quality},
        // TCP, not because it is faster but because the catalogue is rebuilt
        // from a byte stream spanning hundreds of transport messages and UDP
        // reorders them. The result of getting this wrong is not a slower read
        // but a meaningless one, which reads as a camera with an empty card.
        {"transport", "tcp"},
        {"audio", true},
    };

    XV_INFO("{}: opening for SD playback", camera_.label());

    Json info;
    Bridge::Stream stream = Bridge::instance().openStream(request, info);
    if (stream == nullptr) {
        const std::string reason = responseError(info);
        XV_WARN("{}: could not open for playback: {}", camera_.label(), reason);
        setError(reason);
        return false;
    }

    XV_INFO("{}: playback session over {} to {}", camera_.label(),
            info.value("protocol", std::string("?")), info.value("remote_addr", std::string("?")));

    std::scoped_lock lock(streamMutex_);
    stream_ = stream;
    return true;
}

void SdPlayer::closeSession() {
    Bridge::Stream stream = nullptr;
    {
        std::scoped_lock lock(streamMutex_);
        stream = stream_;
        stream_ = nullptr;
    }
    if (stream != nullptr) {
        Bridge::instance().closeStream(stream);
    }
}

Bridge::Stream SdPlayer::stream() const {
    std::scoped_lock lock(streamMutex_);
    return stream_;
}

void SdPlayer::commandLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        bool wantCatalogue = false;
        int64_t wantPlay = 0;

        {
            std::unique_lock lock(commandMutex_);
            commandSignal_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) ||
                       !running_.load(std::memory_order_acquire) ||
                       ((catalogueWanted_ || playWanted_ != 0) && stream() != nullptr);
            });

            if (stopping_.load(std::memory_order_acquire) ||
                !running_.load(std::memory_order_acquire)) {
                return;
            }

            wantCatalogue = catalogueWanted_;
            catalogueWanted_ = false;
            wantPlay = playWanted_;
            playWanted_ = 0;
        }

        if (wantCatalogue) {
            loadCatalogue();
        }
        if (wantPlay == 0) {
            continue;
        }

        // Stopping is asked for with a negative instant, since zero already
        // means "nothing pending".
        if (wantPlay < 0) {
            if (usesFilePlayback()) {
                abortFilePlayback();
                filePlaying_.store(false, std::memory_order_release);
                invalidatePicture();
            } else {
                const Json answer =
                    Bridge::instance().streamCommand(stream(), {{"method", "playback.stop"}});
                if (!responseOk(answer)) {
                    XV_WARN("{}: could not stop playback: {}", camera_.label(),
                            responseError(answer));
                }
            }
            playingFrom_.store(0, std::memory_order_release);
            {
                std::scoped_lock lock(statusMutex_);
                status_.position = 0;
                status_.requested = 0;
            }
            setState(SdState::Ready, usesFilePlayback() ? "Pick a clip" : "Live");
            continue;
        }

        if (usesFilePlayback()) {
            runFilePlayback(wantPlay);
            continue;
        }

        // A clip's own start is the only thing the camera accepts, so the
        // instant asked for is translated into the clip covering it here rather
        // than in the UI, which should not have to know that.
        SdClip clip;
        size_t index = 0;
        {
            std::scoped_lock lock(clipsMutex_);
            const auto after = std::upper_bound(
                clips_.begin(), clips_.end(), wantPlay,
                [](int64_t instant, const SdClip& c) { return instant < c.start; });
            if (after == clips_.begin()) {
                setError("The card holds nothing that far back");
                continue;
            }
            const auto found = std::prev(after);
            clip = *found;
            index = static_cast<size_t>(std::distance(clips_.begin(), found));
        }

        setState(SdState::Playing, "Asking the camera for the recording");

        // Far enough ahead that the camera runs on into the clips that follow
        // rather than stopping at the end of this one. It restarts its
        // timestamps at each boundary, which is what trackPosition counts.
        constexpr int64_t kRunOnSeconds = 6 * 60 * 60;

        const Json answer = Bridge::instance().streamCommand(
            stream(), {
                          {"method", "playback.start"},
                          {"start", clip.start},
                          {"end", clip.start + kRunOnSeconds},
                      });

        if (!responseOk(answer)) {
            setError(responseError(answer));
            continue;
        }
        if (!answer.value("found", false)) {
            // The camera listed this clip and now cannot open it, which happens
            // when the card has wrapped since the catalogue was read.
            setError("The camera no longer has that recording");
            continue;
        }

        currentClip_ = index;
        playingFrom_.store(clip.start, std::memory_order_release);
        awaitingFirstFrame_.store(true, std::memory_order_release);

        {
            std::scoped_lock lock(statusMutex_);
            status_.requested = clip.start;
        }
        setState(SdState::Playing, "Playing");

        XV_INFO("{}: playing from {} ({}s)", camera_.label(), clip.start, clip.duration);
    }
}

void SdPlayer::loadCatalogue() {
    const auto started = std::chrono::steady_clock::now();

    const Json answer =
        Bridge::instance().streamCommand(stream(), {{"method", "recordings.list"}, {"channel", 0}});

    if (!responseOk(answer)) {
        setError(responseError(answer));
        return;
    }

    std::vector<SdClip> clips;
    for (const auto& entry : answer.value("clips", Json::array())) {
        clips.push_back(SdClip{
            entry.value("start", int64_t{0}),
            entry.value("duration", int32_t{0}),
            entry.value("event", false),
        });
    }

    if (clips.empty()) {
        setError("The camera has no recordings, or no card");
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    XV_INFO("{}: {} clips on the card, read in {}ms", camera_.label(), clips.size(),
            elapsed.count());

    const int64_t oldest = clips.front().start;
    const int64_t newest = clips.back().end();

    size_t count = 0;
    {
        std::scoped_lock lock(clipsMutex_);
        clips_ = std::move(clips);
        count = clips_.size();
    }
    catalogueVersion_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::scoped_lock lock(statusMutex_);
        status_.clips = count;
        status_.oldest = oldest;
        status_.newest = newest;
    }

    setState(SdState::Ready, usesFilePlayback() ? "Pick a clip" : "Live");
}

void SdPlayer::onFrame(const uint8_t* data, const XmbFrame& meta, VideoDecoder& decoder,
                       D3D11Context& gpu) {
    if (!decoder.isOpen()) {
        std::string error;
        if (!decoder.open(gpu, meta.codec, error)) {
            XV_ERROR("{}: {}", camera_.label(), error);
            setError(error);
            return;
        }
    }

    if (trackPosition(meta)) {
        // Live and recorded footage are different streams on the same socket.
        // Continuing a GOP across that jump produces a frozen or corrupt
        // picture, which is what "back to live" looked like on the CW400.
        decoder.flush();
        if (meta.keyframe == 0) {
            return;
        }
    }

    decoder.decode(data, meta.size, meta.pts_ms,
                   [this](const AVFrame* frame) { onDecodedFrame(frame); });

    std::scoped_lock lock(statusMutex_);
    status_.framesDecoded = decoder.framesDecoded();
}

bool SdPlayer::trackPosition(const XmbFrame& meta) {
    // Nothing is playing, so the picture is live and has no place on the card's
    // timeline. A jump here is the camera coming back from a recording.
    if (playingFrom_.load(std::memory_order_acquire) == 0) {
        const bool jumped = lastPts_ >= 0 && (meta.pts_ms > lastPts_ + kResetThresholdMs ||
                                              meta.pts_ms < lastPts_ - kResetThresholdMs);
        lastPts_ = meta.pts_ms;
        return jumped;
    }

    const bool wentBackwards = lastPts_ >= 0 && meta.pts_ms < lastPts_ - kResetThresholdMs;
    lastPts_ = meta.pts_ms;

    if (wentBackwards) {
        if (awaitingFirstFrame_.exchange(false, std::memory_order_acq_rel)) {
            // The first jump back is the switch away from the live picture, not
            // the end of a clip: the camera was counting from its own uptime
            // and starts again at zero for the recording.
        } else {
            // Every later one is a clip boundary. The camera plays straight on
            // into the next recording and says so only by starting its
            // timestamps again.
            std::scoped_lock lock(clipsMutex_);
            if (currentClip_ + 1 < clips_.size()) {
                currentClip_++;
            }
        }
        return true;
    }
    if (awaitingFirstFrame_.load(std::memory_order_acquire)) {
        // Frames from before the switch, still in flight. They belong to the
        // live picture and would otherwise be dated as recorded footage.
        return false;
    }

    int64_t start = 0;
    {
        std::scoped_lock lock(clipsMutex_);
        if (currentClip_ < clips_.size()) {
            start = clips_[currentClip_].start;
        }
    }
    if (start == 0) {
        return false;
    }

    std::scoped_lock lock(statusMutex_);
    status_.position = start + meta.pts_ms / 1000;
    return false;
}

void SdPlayer::onDecodedFrame(const AVFrame* frame) {
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr) {
        return;
    }

    // A reference rather than a pixel copy: this bumps the refcount on the
    // decoder's D3D11 surface so it outlives the render thread's use of it.
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
        av_frame_free(&pendingFrame_);
    }
    pendingFrame_ = copy;
}

void SdPlayer::serviceAudio(const uint8_t* data, const XmbFrame& meta) {
    AudioPlayer* speaker = speaker_.load(std::memory_order_acquire);
    if (speaker == nullptr) {
        // Closed here rather than in mute() so that the decoder is only ever
        // touched by the thread that feeds it.
        audioDecoder_.close();
        return;
    }

    if (!audioDecoder_.isOpen()) {
        std::string error;
        if (!audioDecoder_.open(meta.codec, meta.sample_rate, error)) {
            XV_WARN("{}: cannot play the recording's sound: {}", camera_.label(), error);
            mute();
            return;
        }
    }

    audioDecoder_.decode(data, meta.size,
                         [speaker](const AVFrame* frame) { speaker->submit(frame); });
}

bool SdPlayer::present(D3D11Context& gpu, VideoFrameTexture& texture) {
    if (dropPicture_.exchange(false, std::memory_order_acq_rel)) {
        texture.reset();
        std::scoped_lock lock(frameMutex_);
        if (pendingFrame_ != nullptr) {
            av_frame_free(&pendingFrame_);
        }
        return false;
    }

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

std::vector<SdClip> SdPlayer::clips() const {
    std::scoped_lock lock(clipsMutex_);
    return clips_;
}

size_t SdPlayer::clipCount() const {
    std::scoped_lock lock(clipsMutex_);
    return clips_.size();
}

void SdPlayer::play(int64_t instant) {
    {
        std::scoped_lock lock(commandMutex_);
        playWanted_ = instant;
    }
    commandSignal_.notify_all();
}

void SdPlayer::stop() {
    {
        std::scoped_lock lock(commandMutex_);
        playWanted_ = -1;
    }
    commandSignal_.notify_all();
}

void SdPlayer::saveClips(std::vector<SdClip> clips, std::filesystem::path directory) {
    if (clips.empty() || directory.empty() ||
        !running_.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::scoped_lock lock(saveMutex_);
        const bool idle = saveQueue_.empty();
        if (idle) {
            std::scoped_lock status(statusMutex_);
            status_.saveDone = 0;
            status_.saveTotal = clips.size();
            status_.saveMessage = std::format("Saving 0 of {}", clips.size());
        } else {
            std::scoped_lock status(statusMutex_);
            status_.saveTotal += clips.size();
            status_.saveMessage =
                std::format("Saving {} of {}", status_.saveDone, status_.saveTotal);
        }
        saveDirectory_ = std::move(directory);
        for (SdClip& clip : clips) {
            saveQueue_.push_back(std::move(clip));
        }
    }
    saveSignal_.notify_all();
}

void SdPlayer::saveLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        SdClip clip;
        std::filesystem::path directory;
        {
            std::unique_lock lock(saveMutex_);
            saveSignal_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) || !saveQueue_.empty();
            });
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            clip = saveQueue_.front();
            saveQueue_.pop_front();
            directory = saveDirectory_;
        }

        saveOne(clip, directory);
    }
}

void SdPlayer::saveOne(const SdClip& clip, const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    const std::string name =
        std::format("{} {}.mp4", safeFileStem(camera_.label()), clipStamp(clip.start));
    const std::filesystem::path dest = uniqueDestination(directory / pathFromUtf8(name));

    std::filesystem::path temp;
    std::string error;
    if (!fetchRecordingFile(clip, 0, temp, error)) {
        if (!temp.empty()) {
            std::filesystem::remove(temp, ec);
        }
        std::scoped_lock lock(statusMutex_);
        status_.saveDone++;
        status_.saveMessage = error.empty() ? "Could not save that clip" : error;
        XV_WARN("{}: could not save clip {}: {}", camera_.label(), clip.start, error);
        return;
    }

    std::filesystem::rename(temp, dest, ec);
    if (ec) {
        std::filesystem::copy_file(temp, dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temp, ec);
    }
    if (ec) {
        std::filesystem::remove(temp, ec);
        std::scoped_lock lock(statusMutex_);
        status_.saveDone++;
        status_.saveMessage = "Could not write " + utf8Of(dest);
        XV_WARN("{}: could not keep {}: {}", camera_.label(), utf8Of(dest), ec.message());
        return;
    }

    XV_INFO("{}: saved clip {} to {}", camera_.label(), clip.start, utf8Of(dest));

    bool more = false;
    {
        std::scoped_lock lock(saveMutex_);
        more = !saveQueue_.empty();
    }
    std::scoped_lock lock(statusMutex_);
    status_.saveDone++;
    if (more) {
        status_.saveMessage =
            std::format("Saving {} of {}", status_.saveDone, status_.saveTotal);
    } else if (status_.saveDone == status_.saveTotal) {
        status_.saveMessage = status_.saveTotal == 1
                                  ? "Saved 1 clip"
                                  : std::format("Saved {} clips", status_.saveTotal);
    } else {
        status_.saveMessage =
            std::format("Saved {} of {}", status_.saveDone, status_.saveTotal);
    }
}

void SdPlayer::listen(AudioPlayer* speaker) {
    speaker_.store(speaker, std::memory_order_release);
    std::scoped_lock lock(statusMutex_);
    status_.audible = speaker != nullptr;
}

void SdPlayer::mute() {
    listen(nullptr);
}

SdPlayer::Status SdPlayer::status() const {
    std::scoped_lock lock(statusMutex_);
    return status_;
}

void SdPlayer::setState(SdState state, const std::string& message) {
    std::scoped_lock lock(statusMutex_);
    status_.state = state;
    status_.message = message;
    if (state != SdState::Failed) {
        status_.error.clear();
    }
}

void SdPlayer::setError(const std::string& error) {
    std::scoped_lock lock(statusMutex_);
    status_.state = SdState::Failed;
    status_.error = error;
    status_.message = error;
}

void SdPlayer::invalidatePicture() {
    dropPicture_.store(true, std::memory_order_release);
    std::scoped_lock lock(frameMutex_);
    if (pendingFrame_ != nullptr) {
        av_frame_free(&pendingFrame_);
    }
}

bool SdPlayer::usesFilePlayback() const {
    // A CW500 writes `%timestamp_1.mp4` for the second picture, but FileCommand
    // looks the time up in that channel's empty index and never sends the file.
    // Camera SD card no longer offers this tile; the branch is kept so an old
    // open cannot stream the other lens's live frames as if they were playback.
    return isDualLens(camera_.model) && !camera_.channel.empty() && camera_.channel != "0";
}

uint32_t SdPlayer::fileChannel() const {
    return recordingFileChannel(camera_);
}

bool SdPlayer::commandInterrupted() {
    std::scoped_lock lock(commandMutex_);
    return playWanted_ != 0;
}

bool SdPlayer::fetchRecordingFile(const SdClip& clip, uint32_t channel, std::filesystem::path& path,
                                 std::string& error) {
    const Json answer = Bridge::instance().streamCommand(
        stream(), {
                      {"method", "recordings.file"},
                      {"start", clip.start},
                      {"channel", channel},
                  });
    XV_INFO("{}: recordings.file start={} channel={} -> {}", camera_.label(), clip.start, channel,
            answer.dump());
    if (!responseOk(answer)) {
        error = responseError(answer);
        return false;
    }
    if (!answer.value("found", false)) {
        error = "The camera no longer has that recording";
        return false;
    }
    const std::string stored = answer.value("path", std::string{});
    if (stored.empty()) {
        error = "The camera sent a recording with no file";
        return false;
    }
    path = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(stored.data()), stored.size()));
    return true;
}

bool SdPlayer::enqueueClip(size_t index, bool reportError) {
    SdClip clip;
    {
        std::scoped_lock lock(clipsMutex_);
        if (index >= clips_.size()) {
            return false;
        }
        clip = clips_[index];
    }

    std::filesystem::path path;
    std::string error;
    if (!fetchRecordingFile(clip, fileChannel(), path, error)) {
        if (reportError && !stopping_.load(std::memory_order_acquire) && !commandInterrupted()) {
            setError(error);
        }
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        return false;
    }
    if (stopping_.load(std::memory_order_acquire) || commandInterrupted()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }

    {
        std::scoped_lock lock(fileMutex_);
        fileQueue_.push_back(FileJob{std::move(path), clip, index});
    }
    fileSignal_.notify_all();
    return true;
}

void SdPlayer::clearFileQueue() {
    std::deque<FileJob> leftover;
    {
        std::scoped_lock lock(fileMutex_);
        leftover.swap(fileQueue_);
    }
    std::error_code ec;
    for (const auto& job : leftover) {
        std::filesystem::remove(job.path, ec);
    }
}

void SdPlayer::abortFilePlayback() {
    fileAbort_.store(true, std::memory_order_release);
    clearFileQueue();
    fileSignal_.notify_all();

    std::unique_lock lock(fileMutex_);
    fileSignal_.wait(lock, [this] {
        return stopping_.load(std::memory_order_acquire) ||
               !fileBusy_.load(std::memory_order_acquire);
    });
    if (!stopping_.load(std::memory_order_acquire)) {
        fileAbort_.store(false, std::memory_order_release);
    }
}

void SdPlayer::runFilePlayback(int64_t instant) {
    abortFilePlayback();
    invalidatePicture();

    size_t index = 0;
    SdClip clip;
    {
        std::scoped_lock lock(clipsMutex_);
        const auto after = std::upper_bound(
            clips_.begin(), clips_.end(), instant,
            [](int64_t t, const SdClip& c) { return t < c.start; });
        if (after == clips_.begin()) {
            setError("The card holds nothing that far back");
            return;
        }
        const auto found = std::prev(after);
        clip = *found;
        index = static_cast<size_t>(std::distance(clips_.begin(), found));
    }

    setState(SdState::Playing, "Fetching the recording");

    if (!enqueueClip(index, true)) {
        filePlaying_.store(false, std::memory_order_release);
        return;
    }

    filePlaying_.store(true, std::memory_order_release);

    playingFrom_.store(clip.start, std::memory_order_release);
    {
        std::scoped_lock lock(statusMutex_);
        status_.requested = clip.start;
    }
    setState(SdState::Playing, "Playing");
    XV_INFO("{}: playing lens {} from {} ({}s) via file download", camera_.label(),
            fileChannel(), clip.start, clip.duration);

    size_t next = index + 1;
    size_t total = 0;
    {
        std::scoped_lock lock(clipsMutex_);
        total = clips_.size();
    }

    while (!stopping_.load(std::memory_order_acquire) && !commandInterrupted()) {
        size_t queued = 0;
        {
            std::scoped_lock lock(fileMutex_);
            queued = fileQueue_.size();
        }
        if (queued < 1 && next < total) {
            if (!enqueueClip(next, false)) {
                next = total;
                continue;
            }
            ++next;
            continue;
        }
        if (queued == 0 && !fileBusy_.load(std::memory_order_acquire)) {
            break;
        }

        std::unique_lock lock(commandMutex_);
        commandSignal_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return stopping_.load(std::memory_order_acquire) || playWanted_ != 0;
        });
    }

    if (stopping_.load(std::memory_order_acquire) || commandInterrupted()) {
        return;
    }

    abortFilePlayback();
    filePlaying_.store(false, std::memory_order_release);
    playingFrom_.store(0, std::memory_order_release);
    invalidatePicture();
    {
        std::scoped_lock lock(statusMutex_);
        status_.position = 0;
        status_.requested = 0;
    }
    setState(SdState::Ready, "Pick a clip");
}

void SdPlayer::fileLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        FileJob job;
        {
            std::unique_lock lock(fileMutex_);
            fileSignal_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) || !fileQueue_.empty();
            });
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            job = std::move(fileQueue_.front());
            fileQueue_.pop_front();
            fileBusy_.store(true, std::memory_order_release);
        }
        commandSignal_.notify_all();

        playFile(job);

        std::error_code ec;
        std::filesystem::remove(job.path, ec);

        fileBusy_.store(false, std::memory_order_release);
        fileSignal_.notify_all();
        commandSignal_.notify_all();
    }
}

void SdPlayer::playFile(const FileJob& job) {
    if (gpu_ == nullptr) {
        return;
    }

    AVFormatContext* format = nullptr;
    const std::string target = utf8Of(job.path);
    if (const int rc = avformat_open_input(&format, target.c_str(), nullptr, nullptr); rc < 0) {
        if (!fileAbort_.load(std::memory_order_acquire)) {
            setError("could not open the downloaded recording: " + avError(rc));
        }
        return;
    }

    if (const int rc = avformat_find_stream_info(format, nullptr); rc < 0) {
        avformat_close_input(&format);
        if (!fileAbort_.load(std::memory_order_acquire)) {
            setError("could not read the downloaded recording: " + avError(rc));
        }
        return;
    }

    int videoStream = -1;
    int audioStream = -1;
    for (unsigned int i = 0; i < format->nb_streams; ++i) {
        const AVCodecParameters* parameters = format->streams[i]->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0 &&
            (parameters->codec_id == AV_CODEC_ID_H264 || parameters->codec_id == AV_CODEC_ID_HEVC)) {
            videoStream = static_cast<int>(i);
        }
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0 &&
            avcodec_find_decoder(parameters->codec_id) != nullptr) {
            audioStream = static_cast<int>(i);
        }
    }
    if (videoStream < 0) {
        avformat_close_input(&format);
        setError("the downloaded recording has no supported video");
        return;
    }

    VideoDecoder decoder;
    std::string error;
    if (!decoder.open(*gpu_, format->streams[videoStream]->codecpar, error)) {
        avformat_close_input(&format);
        setError(error);
        return;
    }

    fileAudioDecoder_.close();
    bool audioOpen = false;

    currentClip_ = job.index;
    playingFrom_.store(job.clip.start, std::memory_order_release);
    {
        std::scoped_lock lock(statusMutex_);
        status_.requested = job.clip.start;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        avformat_close_input(&format);
        setError("out of memory");
        return;
    }

    auto clockBase = std::chrono::steady_clock::now();
    int64_t basePtsMs = -1;

    const auto streamPtsMs = [&](int streamIndex, int64_t pts) {
        return av_rescale_q(pts, format->streams[streamIndex]->time_base, AVRational{1, 1000});
    };

    while (!stopping_.load(std::memory_order_acquire) &&
           !fileAbort_.load(std::memory_order_acquire)) {
        const int rc = av_read_frame(format, packet);
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            if (!fileAbort_.load(std::memory_order_acquire)) {
                setError("could not read the downloaded recording: " + avError(rc));
            }
            break;
        }

        const bool isVideo = packet->stream_index == videoStream;
        const bool isAudio = audioStream >= 0 && packet->stream_index == audioStream;
        if (!isVideo && !isAudio) {
            av_packet_unref(packet);
            continue;
        }

        const int64_t rawPts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        const int64_t ptsMs = rawPts == AV_NOPTS_VALUE ? -1 : streamPtsMs(packet->stream_index, rawPts);

        if (isVideo && ptsMs >= 0) {
            if (basePtsMs < 0) {
                basePtsMs = ptsMs;
                clockBase = std::chrono::steady_clock::now();
            }
            for (;;) {
                if (stopping_.load(std::memory_order_acquire) ||
                    fileAbort_.load(std::memory_order_acquire)) {
                    break;
                }
                const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - clockBase)
                                            .count();
                const int64_t wall = basePtsMs + elapsed;
                if (ptsMs <= wall + 30) {
                    break;
                }
                std::unique_lock lock(fileMutex_);
                const int64_t waitMs = std::min<int64_t>(20, ptsMs - wall - 30);
                fileSignal_.wait_for(lock, std::chrono::milliseconds(waitMs), [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                           fileAbort_.load(std::memory_order_acquire);
                });
            }
            if (stopping_.load(std::memory_order_acquire) ||
                fileAbort_.load(std::memory_order_acquire)) {
                av_packet_unref(packet);
                break;
            }

            decoder.decode(packet, [this](const AVFrame* frame) { onDecodedFrame(frame); });
            {
                std::scoped_lock lock(statusMutex_);
                status_.position = job.clip.start + ptsMs / 1000;
                status_.framesDecoded = decoder.framesDecoded();
            }
        } else if (isAudio) {
            AudioPlayer* speaker = speaker_.load(std::memory_order_acquire);
            if (speaker != nullptr) {
                if (!audioOpen) {
                    if (fileAudioDecoder_.open(format->streams[audioStream]->codecpar, error)) {
                        audioOpen = true;
                    } else {
                        XV_WARN("{}: cannot play the recording's sound: {}", camera_.label(), error);
                    }
                }
                if (audioOpen) {
                    fileAudioDecoder_.decode(packet, [speaker](const AVFrame* frame) {
                        speaker->submit(frame);
                    });
                }
            } else {
                fileAudioDecoder_.close();
                audioOpen = false;
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    fileAudioDecoder_.close();
    avformat_close_input(&format);
}

} // namespace xv
