#include "media/MatroskaMuxer.h"

#include <algorithm>
#include <format>
#include <utility>

#include "app/Log.h"
#include "media/AudioFormat.h"
#include "xmbridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

namespace xv {
namespace {

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

AVCodecID videoCodecId(int codec) {
    return codec == XMB_CODEC_H265 ? AV_CODEC_ID_HEVC
                                   : codec == XMB_CODEC_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_NONE;
}

bool setExtradata(AVCodecParameters* parameters, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return true;
    }

    parameters->extradata =
        static_cast<uint8_t*>(av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters->extradata == nullptr) {
        return false;
    }

    std::copy(bytes.begin(), bytes.end(), parameters->extradata);
    parameters->extradata_size = static_cast<int>(bytes.size());
    return true;
}

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string utcTimestamp(std::chrono::system_clock::time_point time) {
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(time);
    const auto seconds = std::chrono::floor<std::chrono::seconds>(milliseconds);
    const auto fraction =
        std::chrono::duration_cast<std::chrono::milliseconds>(milliseconds - seconds).count();
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", seconds, fraction);
}

void setTrackMetadata(AVStream* stream, const std::string& title, bool defaultTrack) {
    if (!title.empty()) {
        av_dict_set(&stream->metadata, "title", title.c_str(), 0);
    }
    stream->disposition = defaultTrack ? AV_DISPOSITION_DEFAULT : 0;
}

} // namespace

std::vector<uint8_t> videoParameterSets(const uint8_t* data, size_t size, int codec) {
    std::vector<uint8_t> out;

    // Locate every Annex-B NAL unit so its exact extent is known.
    std::vector<std::pair<size_t, size_t>> units;
    size_t previous = 0;
    bool havePrevious = false;

    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }

        size_t payload = 0;
        if (data[i + 2] == 1) {
            payload = i + 3;
        } else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
            payload = i + 4;
        } else {
            continue;
        }

        if (havePrevious) {
            units.emplace_back(previous, i);
        }
        previous = payload;
        havePrevious = true;
        i = payload - 1;
    }
    if (havePrevious) {
        units.emplace_back(previous, size);
    }

    for (const auto& [begin, end] : units) {
        if (begin >= end) {
            continue;
        }

        bool wanted = false;
        if (codec == XMB_CODEC_H265) {
            const int type = (data[begin] >> 1) & 0x3F;
            wanted = type == 32 || type == 33 || type == 34; // VPS, SPS, PPS
        } else if (codec == XMB_CODEC_H264) {
            const int type = data[begin] & 0x1F;
            wanted = type == 7 || type == 8; // SPS, PPS
        }

        if (wanted) {
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), data + begin, data + end);
        }
    }

    return out;
}

MatroskaMuxer::~MatroskaMuxer() {
    close();
}

bool MatroskaMuxer::open(const std::filesystem::path& path,
                         const std::vector<MatroskaVideoTrack>& video,
                         const std::vector<MatroskaAudioTrack>& audio,
                         std::chrono::system_clock::time_point recordingStartUtc,
                         std::string& error) {
    close();

    if (video.empty()) {
        error = "a recording needs at least one video track";
        return false;
    }

    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    const std::string target = utf8Of(path);

    if (const int rc = avformat_alloc_output_context2(&format_, nullptr, "matroska", target.c_str());
        rc < 0) {
        error = "could not set up the Matroska muxer: " + avError(rc);
        format_ = nullptr;
        return false;
    }

    const std::string creationTime = utcTimestamp(recordingStartUtc);
    const std::string startUtcMs = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            recordingStartUtc.time_since_epoch())
            .count());
    if (av_dict_set(&format_->metadata, "creation_time", creationTime.c_str(), 0) < 0 ||
        av_dict_set(&format_->metadata, "xv_recording_start_utc_ms", startUtcMs.c_str(), 0) < 0) {
        error = "could not store the recording start time";
        close();
        return false;
    }

    for (const MatroskaVideoTrack& spec : video) {
        if (!addVideoTrack(spec, error)) {
            close();
            return false;
        }
    }
    for (const MatroskaAudioTrack& spec : audio) {
        if (!addAudioTrack(spec, error)) {
            close();
            return false;
        }
    }

    packet_ = av_packet_alloc();
    if (packet_ == nullptr) {
        error = "out of memory";
        close();
        return false;
    }

    if (const int rc = avio_open(&format_->pb, target.c_str(), AVIO_FLAG_WRITE); rc < 0) {
        error = "could not create the file: " + avError(rc);
        close();
        return false;
    }
    if (const int rc = avformat_write_header(format_, nullptr); rc < 0) {
        error = "the muxer rejected the streams: " + avError(rc);
        close();
        return false;
    }

    path_ = path;
    bytes_ = 0;
    durationMs_ = 0;
    wrotePacket_ = false;
    lastVideoPts_.assign(video_.size(), -1);
    lastAudioPts_.assign(audio_.size(), -1);

    XV_INFO("recording {} video and {} audio track(s) to {}", video_.size(), audio_.size(), target);
    return true;
}

bool MatroskaMuxer::addVideoTrack(const MatroskaVideoTrack& spec, std::string& error) {
    if (spec.width <= 0 || spec.height <= 0) {
        error = "the picture size is not known yet";
        return false;
    }
    const AVCodecID codec = videoCodecId(spec.codec);
    if (codec == AV_CODEC_ID_NONE) {
        error = "the video codec cannot be stored in Matroska";
        return false;
    }
    if (spec.extradata.empty()) {
        error = "this keyframe carries no parameter sets";
        return false;
    }

    AVStream* stream = avformat_new_stream(format_, nullptr);
    if (stream == nullptr) {
        error = "could not add a video track";
        return false;
    }

    stream->time_base = AVRational{1, 1000};
    AVCodecParameters* parameters = stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = codec;
    parameters->width = spec.width;
    parameters->height = spec.height;
    if (!setExtradata(parameters, spec.extradata)) {
        error = "out of memory";
        return false;
    }

    setTrackMetadata(stream, spec.title, spec.defaultTrack);
    video_.push_back(stream);
    return true;
}

bool MatroskaMuxer::addAudioTrack(const MatroskaAudioTrack& spec, std::string& error) {
    const auto codec = static_cast<AVCodecID>(audio::avCodecId(spec.format.codec));
    if (!spec.format.valid() || codec == AV_CODEC_ID_NONE) {
        error = std::format("the camera's audio codec ({}) cannot be stored in Matroska",
                            audio::codecName(spec.format.codec));
        return false;
    }

    AVStream* stream = avformat_new_stream(format_, nullptr);
    if (stream == nullptr) {
        error = "could not add an audio track";
        return false;
    }

    stream->time_base = AVRational{1, 1000};
    AVCodecParameters* parameters = stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = codec;
    parameters->sample_rate =
        audio::outputSampleRate(spec.format.codec, spec.format.sampleRate);
    av_channel_layout_default(&parameters->ch_layout, spec.format.channels);

    switch (codec) {
    case AV_CODEC_ID_PCM_ALAW:
    case AV_CODEC_ID_PCM_MULAW: parameters->bits_per_coded_sample = 8; break;
    case AV_CODEC_ID_PCM_S16LE: parameters->bits_per_coded_sample = 16; break;
    case AV_CODEC_ID_OPUS:
        if (!setExtradata(
                parameters, audio::opusHead(spec.format.channels, spec.format.sampleRate))) {
            error = "out of memory";
            return false;
        }
        break;
    default: break;
    }

    setTrackMetadata(stream, spec.title, spec.defaultTrack);
    audio_.push_back(stream);
    return true;
}

bool MatroskaMuxer::writeVideo(size_t track, const uint8_t* data, size_t size, int64_t ptsMs,
                               bool keyframe, std::string& error) {
    if (track >= video_.size()) {
        error = "invalid video track";
        return false;
    }
    return writePacket(video_[track], lastVideoPts_[track], data, size, ptsMs, keyframe, error);
}

bool MatroskaMuxer::writeAudio(size_t track, const uint8_t* data, size_t size, int64_t ptsMs,
                               std::string& error) {
    if (track >= audio_.size()) {
        error = "invalid audio track";
        return false;
    }
    return writePacket(audio_[track], lastAudioPts_[track], data, size, ptsMs, true, error);
}

bool MatroskaMuxer::writePacket(AVStream* stream, int64_t& lastPts, const uint8_t* data,
                                size_t size, int64_t ptsMs, bool keyframe, std::string& error) {
    if (format_ == nullptr || packet_ == nullptr) {
        error = "the recording is not open";
        return false;
    }

    ptsMs = monotonicRecordingPts(ptsMs, lastPts);

    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->stream_index = stream->index;
    packet_->pts = av_rescale_q(ptsMs, AVRational{1, 1000}, stream->time_base);
    packet_->dts = packet_->pts;
    packet_->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

    const int rc = av_interleaved_write_frame(format_, packet_);
    packet_->data = nullptr;
    packet_->size = 0;
    if (rc < 0) {
        error = "could not write to the file: " + avError(rc);
        return false;
    }

    lastPts = ptsMs;
    durationMs_ = std::max(durationMs_, ptsMs);
    bytes_ += size;
    wrotePacket_ = true;
    return true;
}

void MatroskaMuxer::close() {
    if (format_ != nullptr && format_->pb != nullptr) {
        if (wrotePacket_) {
            if (const int rc = av_write_trailer(format_); rc < 0) {
                XV_WARN("recording {} did not finish cleanly: {}", utf8Of(path_), avError(rc));
            }
        }
        avio_closep(&format_->pb);
    }

    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (format_ != nullptr) {
        avformat_free_context(format_);
        format_ = nullptr;
    }

    video_.clear();
    audio_.clear();
    lastVideoPts_.clear();
    lastAudioPts_.clear();
    wrotePacket_ = false;
}

} // namespace xv
