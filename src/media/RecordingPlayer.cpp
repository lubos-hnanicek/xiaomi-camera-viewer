#include "media/RecordingPlayer.h"

#include <algorithm>
#include <exception>
#include <format>
#include <limits>

#include "app/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

namespace xv {
namespace {

using namespace std::chrono_literals;

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string trackTitle(const AVStream* stream, const char* kind, size_t number) {
    if (const AVDictionaryEntry* title =
            av_dict_get(stream->metadata, "title", nullptr, AV_DICT_MATCH_CASE);
        title != nullptr && title->value != nullptr && *title->value != '\0') {
        return title->value;
    }
    return std::format("{} {}", kind, number + 1);
}

} // namespace

RecordingPlayer::~RecordingPlayer() {
    close();
}

bool RecordingPlayer::open(const std::filesystem::path& path, D3D11Context& gpu,
                           AudioPlayer& audio, std::string& error) {
    close();

    AVFormatContext* opened = nullptr;
    const std::string target = utf8Of(path);
    if (const int rc = avformat_open_input(&opened, target.c_str(), nullptr, nullptr); rc < 0) {
        error = "could not open the recording: " + avError(rc);
        return false;
    }
    format_ = opened;
    format_->flags |= AVFMT_FLAG_FAST_SEEK;

    if (const int rc = avformat_find_stream_info(format_, nullptr); rc < 0) {
        error = "could not read the recording's tracks: " + avError(rc);
        close();
        return false;
    }

    path_ = path;
    gpu_ = &gpu;
    audio_ = &audio;
    durationMs_ = format_->duration == AV_NOPTS_VALUE
                      ? 0
                      : std::max<int64_t>(0, av_rescale_q(format_->duration,
                                                         AVRational{1, AV_TIME_BASE},
                                                         AVRational{1, 1000}));

    size_t videoNumber = 0;
    size_t audioNumber = 0;
    int defaultAudio = -1;
    for (unsigned int i = 0; i < format_->nb_streams; ++i) {
        AVStream* stream = format_->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;

        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
            (parameters->codec_id == AV_CODEC_ID_H264 ||
             parameters->codec_id == AV_CODEC_ID_HEVC)) {
            auto state = std::make_unique<VideoState>();
            state->info = VideoInfo{
                .streamIndex = static_cast<int>(i),
                .title = trackTitle(stream, "Video", videoNumber),
                .width = parameters->width,
                .height = parameters->height,
            };
            if (!state->decoder.open(gpu, parameters, error)) {
                error = std::format("{}: {}", state->info.title, error);
                close();
                return false;
            }

            videoByStream_[static_cast<int>(i)] = video_.size();
            videoInfo_.push_back(state->info);
            video_.push_back(std::move(state));
            ++videoNumber;
            continue;
        }

        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(parameters->codec_id) != nullptr) {
            audioInfo_.push_back(AudioInfo{
                .streamIndex = static_cast<int>(i),
                .title = trackTitle(stream, "Audio", audioNumber),
            });
            if (defaultAudio < 0 && (stream->disposition & AV_DISPOSITION_DEFAULT) != 0) {
                defaultAudio = static_cast<int>(audioInfo_.size()) - 1;
            }
            ++audioNumber;
        }
    }

    if (video_.empty()) {
        error = "the recording has no supported H.264 or H.265 video tracks";
        close();
        return false;
    }

    if (!audioInfo_.empty()) {
        if (defaultAudio < 0) {
            defaultAudio = 0;
        }
        if (!audio.start(error)) {
            close();
            return false;
        }
    }

    {
        std::scoped_lock lock(controlMutex_);
        stopping_ = false;
        playing_ = true;
        eof_ = false;
        basePositionMs_ = 0;
        baseTime_ = std::chrono::steady_clock::now();
        seekRequest_.reset();
        requestedAudioOption_ = defaultAudio;
        appliedAudioOption_.store(-2, std::memory_order_release);
        prerolling_ = false;
        workerAlive_ = true;
        error_.clear();
    }

    try {
        thread_ = std::thread(&RecordingPlayer::run, this);
    } catch (const std::exception& exception) {
        {
            std::scoped_lock lock(controlMutex_);
            workerAlive_ = false;
        }
        error = std::string("could not start the playback thread: ") + exception.what();
        close();
        return false;
    }

    XV_INFO("playing {} with {} video and {} audio track(s)", target, video_.size(),
            audioInfo_.size());
    return true;
}

void RecordingPlayer::close() {
    {
        std::scoped_lock lock(controlMutex_);
        stopping_ = true;
    }
    controlSignal_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }

    clearPendingFrames();
    for (auto& state : video_) {
        state->texture.reset();
    }
    video_.clear();
    videoInfo_.clear();
    audioInfo_.clear();
    videoByStream_.clear();
    audioDecoder_.close();
    if (audio_ != nullptr) {
        audio_->reset();
    }

    if (format_ != nullptr) {
        avformat_close_input(&format_);
    }
    path_.clear();
    gpu_ = nullptr;
    audio_ = nullptr;

    std::scoped_lock lock(controlMutex_);
    stopping_ = false;
    playing_ = false;
    eof_ = false;
    basePositionMs_ = 0;
    durationMs_ = 0;
    seekRequest_.reset();
    requestedAudioOption_ = -1;
    appliedAudioOption_.store(-2, std::memory_order_release);
    prerolling_ = false;
    workerAlive_ = false;
    error_.clear();
    prerollLeft_ = 0;
    prerollEpoch_ = 0;
}

void RecordingPlayer::play() {
    std::scoped_lock lock(controlMutex_);
    if (format_ == nullptr || !workerAlive_ || playing_) {
        return;
    }
    if (eof_ || basePositionMs_ >= durationMs_) {
        basePositionMs_ = 0;
        seekRequest_ = 0;
        eof_ = false;
        prerolling_ = true;
    }
    baseTime_ = std::chrono::steady_clock::now();
    playing_ = true;
    controlSignal_.notify_all();
}

void RecordingPlayer::pause() {
    {
        std::scoped_lock lock(controlMutex_);
        if (!playing_) {
            return;
        }
        basePositionMs_ = positionLocked(std::chrono::steady_clock::now());
        playing_ = false;
    }
    if (audio_ != nullptr) {
        audio_->reset();
    }
    controlSignal_.notify_all();
}

void RecordingPlayer::toggle() {
    if (status().playing) {
        pause();
    } else {
        play();
    }
}

void RecordingPlayer::seek(int64_t positionMs) {
    {
        std::scoped_lock lock(controlMutex_);
        if (format_ == nullptr || !workerAlive_) {
            return;
        }
        positionMs = clampPlaybackPosition(positionMs, durationMs_);
        // Keep the last picture on screen. Tearing down the D3D11 video
        // processor made every seek flash black and rebuild the GPU pipeline.
        basePositionMs_ = positionMs;
        baseTime_ = std::chrono::steady_clock::now();
        seekRequest_ = positionMs;
        eof_ = false;
        prerolling_ = true;
    }
    if (audio_ != nullptr) {
        audio_->reset();
    }
    controlSignal_.notify_all();
}

void RecordingPlayer::setAudioTrack(int option) {
    {
        std::scoped_lock lock(controlMutex_);
        if (option < -1 || option >= static_cast<int>(audioInfo_.size())) {
            return;
        }
        requestedAudioOption_ = option;
    }
    if (audio_ != nullptr) {
        audio_->reset();
    }
    controlSignal_.notify_all();
}

RecordingPlayer::Status RecordingPlayer::status() const {
    std::scoped_lock lock(controlMutex_);
    return Status{
        .open = format_ != nullptr,
        .playing = playing_,
        .eof = eof_,
        .failed = format_ != nullptr && !workerAlive_,
        .positionMs = positionLocked(std::chrono::steady_clock::now()),
        .durationMs = durationMs_,
        .selectedAudio = requestedAudioOption_,
        .fileName = path_.filename().string(),
        .error = error_,
    };
}

bool RecordingPlayer::present(size_t track, D3D11Context& gpu) {
    if (track >= video_.size()) {
        return false;
    }

    VideoState& state = *video_[track];
    AVFrame* frame = nullptr;
    {
        std::scoped_lock lock(state.frameMutex);
        frame = state.pendingFrame;
        state.pendingFrame = nullptr;
    }
    if (frame == nullptr) {
        return false;
    }

    const bool updated = state.texture.update(gpu, frame);
    av_frame_free(&frame);
    return updated;
}

const VideoFrameTexture* RecordingPlayer::texture(size_t track) const {
    return track < video_.size() ? &video_[track]->texture : nullptr;
}

void RecordingPlayer::run() {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        setError("could not allocate a playback packet");
        return;
    }

    bool havePacket = false;
    bool snapClockAfterSeek = false;
    int64_t prerollUntilMs = -1;

    while (true) {
        std::optional<int64_t> seek;
        int requestedAudio = -1;
        bool shouldPlay = false;
        bool preroll = false;
        int64_t position = 0;
        {
            std::unique_lock lock(controlMutex_);
            controlSignal_.wait(lock, [this] {
                return stopping_ || seekRequest_.has_value() ||
                       requestedAudioOption_ !=
                           appliedAudioOption_.load(std::memory_order_acquire) ||
                       playing_ || prerolling_;
            });
            if (stopping_) {
                break;
            }
            if (seekRequest_) {
                seek = seekRequest_;
                seekRequest_.reset();
            }
            requestedAudio = requestedAudioOption_;
            shouldPlay = playing_;
            preroll = prerolling_;
            position = positionLocked(std::chrono::steady_clock::now());
        }

        if (seek) {
            if (havePacket) {
                av_packet_unref(packet);
                havePacket = false;
            }
            std::string error;
            if (!applySeek(*seek, error)) {
                setError(error);
                break;
            }
            beginPreroll();
            snapClockAfterSeek = true;
            prerollUntilMs = -1;
            continue;
        }

        if (requestedAudio != appliedAudioOption_.load(std::memory_order_acquire)) {
            std::string error;
            if (!applyAudioSelection(requestedAudio, error)) {
                std::scoped_lock lock(controlMutex_);
                error_ = std::move(error);
                requestedAudioOption_ = -1;
                appliedAudioOption_.store(-1, std::memory_order_release);
            } else {
                std::scoped_lock lock(controlMutex_);
                error_.clear();
            }
        }

        if (!shouldPlay && !preroll) {
            continue;
        }

        if (!havePacket) {
            const int rc = av_read_frame(format_, packet);
            if (rc == AVERROR_EOF) {
                std::scoped_lock lock(controlMutex_);
                basePositionMs_ = durationMs_;
                playing_ = false;
                eof_ = true;
                prerolling_ = false;
                continue;
            }
            if (rc < 0) {
                setError("could not read the recording: " + avError(rc));
                break;
            }
            havePacket = true;
        }

        const int64_t rawPts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        const int64_t ptsMs = packetPtsMs(packet->stream_index, rawPts);
        const int appliedAudio = appliedAudioOption_.load(std::memory_order_acquire);
        const bool selectedAudio =
            appliedAudio >= 0 &&
            audioInfo_[static_cast<size_t>(appliedAudio)].streamIndex == packet->stream_index;
        const bool isVideo = videoByStream_.contains(packet->stream_index);
        if (snapClockAfterSeek && isVideo && ptsMs >= 0) {
            snapClockAfterSeek = false;
            prerollUntilMs = ptsMs + 500;
            std::scoped_lock lock(controlMutex_);
            basePositionMs_ = clampPlaybackPosition(ptsMs, durationMs_);
            baseTime_ = std::chrono::steady_clock::now();
            position = basePositionMs_;
        }
        if (preroll && isVideo && prerollUntilMs >= 0 && ptsMs >= prerollUntilMs) {
            prerollLeft_ = 0;
            endPreroll();
            preroll = false;
        }

        // Skip the paced wait while catching the keyframe after a seek, otherwise
        // a paused scrub sits on a 5 ms poll and a playing seek burns the GOP.
        const int64_t leadMs = selectedAudio ? 120 : 30;
        if (!preroll && (isVideo || selectedAudio) && ptsMs >= 0 && ptsMs > position + leadMs) {
            const int64_t waitMs = std::min<int64_t>(20, ptsMs - position - leadMs);
            std::unique_lock lock(controlMutex_);
            controlSignal_.wait_for(lock, std::chrono::milliseconds(waitMs));
            continue;
        }

        if (const auto video = videoByStream_.find(packet->stream_index);
            video != videoByStream_.end()) {
            const size_t track = video->second;
            if (!video_[track]->decoder.decode(packet, [this, track](const AVFrame* frame) {
                    storeFrame(track, frame);
                    completePrerollTrack(track);
                })) {
                setError(std::format("{} could not be decoded", video_[track]->info.title));
                break;
            }
        } else if (selectedAudio && shouldPlay) {
            if (!audioDecoder_.decode(packet, [this](const AVFrame* frame) {
                    if (audio_ != nullptr) {
                        audio_->submit(frame);
                    }
                })) {
                setError("the selected audio track could not be decoded");
                break;
            }
        }

        av_packet_unref(packet);
        havePacket = false;
    }

    if (havePacket) {
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    {
        std::scoped_lock lock(controlMutex_);
        workerAlive_ = false;
        prerolling_ = false;
    }
}

void RecordingPlayer::storeFrame(size_t track, const AVFrame* frame) {
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr || av_frame_ref(copy, frame) < 0) {
        av_frame_free(&copy);
        return;
    }

    VideoState& state = *video_[track];
    std::scoped_lock lock(state.frameMutex);
    if (state.pendingFrame != nullptr) {
        av_frame_free(&state.pendingFrame);
    }
    state.pendingFrame = copy;
}

void RecordingPlayer::completePrerollTrack(size_t track) {
    VideoState& state = *video_[track];
    if (state.prerollEpoch == prerollEpoch_) {
        return;
    }
    state.prerollEpoch = prerollEpoch_;
    if (prerollLeft_ == 0) {
        return;
    }
    --prerollLeft_;
    if (prerollLeft_ == 0) {
        endPreroll();
    }
}

void RecordingPlayer::beginPreroll() {
    ++prerollEpoch_;
    prerollLeft_ = video_.size();
    std::scoped_lock lock(controlMutex_);
    prerolling_ = prerollLeft_ > 0;
}

void RecordingPlayer::endPreroll() {
    std::scoped_lock lock(controlMutex_);
    prerolling_ = false;
    // Restart the clock after the keyframe is on screen so the time spent
    // seeking is not skipped as if it had already played.
    baseTime_ = std::chrono::steady_clock::now();
}

void RecordingPlayer::setStreamDiscard(int selectedAudioOption) {
    if (format_ == nullptr) {
        return;
    }
    for (unsigned int i = 0; i < format_->nb_streams; ++i) {
        AVStream* stream = format_->streams[i];
        const AVMediaType type = stream->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO) {
            stream->discard = AVDISCARD_DEFAULT;
            continue;
        }
        if (type == AVMEDIA_TYPE_AUDIO) {
            const bool selected =
                selectedAudioOption >= 0 &&
                static_cast<size_t>(selectedAudioOption) < audioInfo_.size() &&
                audioInfo_[static_cast<size_t>(selectedAudioOption)].streamIndex ==
                    static_cast<int>(i);
            stream->discard = selected ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
            continue;
        }
        stream->discard = AVDISCARD_ALL;
    }
}

void RecordingPlayer::clearPendingFrames() {
    for (auto& state : video_) {
        std::scoped_lock lock(state->frameMutex);
        if (state->pendingFrame != nullptr) {
            av_frame_free(&state->pendingFrame);
        }
    }
}

bool RecordingPlayer::applySeek(int64_t positionMs, std::string& error) {
    const int64_t target =
        av_rescale_q(positionMs, AVRational{1, 1000}, AVRational{1, AV_TIME_BASE});

    // Cap max_ts at the requested time so the demuxer looks for the previous
    // keyframe instead of scanning the rest of the file for a "closer" one.
    int rc = avformat_seek_file(format_, -1, std::numeric_limits<int64_t>::min(), target, target,
                                AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        rc = avformat_seek_file(format_, -1, std::numeric_limits<int64_t>::min(), target,
                                std::numeric_limits<int64_t>::max(), AVSEEK_FLAG_BACKWARD);
    }
    if (rc < 0) {
        error = "could not seek in the recording: " + avError(rc);
        return false;
    }

    for (auto& state : video_) {
        state->decoder.flush();
    }
    audioDecoder_.flush();
    clearPendingFrames();
    if (audio_ != nullptr) {
        audio_->reset();
    }
    return true;
}

bool RecordingPlayer::applyAudioSelection(int option, std::string& error) {
    audioDecoder_.close();
    if (audio_ != nullptr) {
        audio_->reset();
    }
    if (option < 0) {
        setStreamDiscard(-1);
        appliedAudioOption_.store(-1, std::memory_order_release);
        return true;
    }

    const int streamIndex = audioInfo_[static_cast<size_t>(option)].streamIndex;
    if (!audioDecoder_.open(format_->streams[streamIndex]->codecpar, error)) {
        return false;
    }
    setStreamDiscard(option);
    appliedAudioOption_.store(option, std::memory_order_release);
    return true;
}

void RecordingPlayer::setError(std::string error) {
    XV_ERROR("playback stopped: {}", error);
    std::scoped_lock lock(controlMutex_);
    error_ = std::move(error);
    basePositionMs_ = positionLocked(std::chrono::steady_clock::now());
    playing_ = false;
    prerolling_ = false;
    workerAlive_ = false;
}

int64_t RecordingPlayer::positionLocked(std::chrono::steady_clock::time_point now) const {
    if (!playing_ || prerolling_) {
        return clampPlaybackPosition(basePositionMs_, durationMs_);
    }
    const int64_t elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - baseTime_).count();
    return clampPlaybackPosition(basePositionMs_ + elapsed, durationMs_);
}

int64_t RecordingPlayer::packetPtsMs(int streamIndex, int64_t pts) const {
    if (pts == AV_NOPTS_VALUE || streamIndex < 0 ||
        streamIndex >= static_cast<int>(format_->nb_streams)) {
        return -1;
    }
    return av_rescale_q(pts, format_->streams[streamIndex]->time_base, AVRational{1, 1000});
}

} // namespace xv
