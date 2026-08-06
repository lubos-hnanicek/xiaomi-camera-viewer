#include "media/AudioDecoder.h"

#include <algorithm>
#include <format>
#include <vector>

#include "app/Log.h"
#include "media/AudioFormat.h"
#include "xmbridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace xv {
namespace {

std::string averror(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

AudioDecoder::~AudioDecoder() {
    close();
}

bool AudioDecoder::open(int codec, int sampleRate, std::string& error) {
    close();

    const auto id = static_cast<AVCodecID>(audio::avCodecId(codec));
    if (id == AV_CODEC_ID_NONE) {
        error = std::format("unsupported audio codec id {}", codec);
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(id);
    if (decoder == nullptr) {
        error = std::format("this build has no {} decoder", audio::codecName(codec));
        return false;
    }

    codec_ = avcodec_alloc_context3(decoder);
    if (codec_ == nullptr) {
        error = "out of memory";
        return false;
    }

    // The cameras send one microphone. Saying so matters for Opus, which reads
    // its channel count from the header below and would otherwise fall back to
    // libavcodec's stereo default and halve the playback rate.
    av_channel_layout_default(&codec_->ch_layout, 1);
    codec_->sample_rate = audio::outputSampleRate(codec, sampleRate);

    if (id == AV_CODEC_ID_OPUS) {
        const std::vector<uint8_t> head = audio::opusHead(1, sampleRate);
        codec_->extradata =
            static_cast<uint8_t*>(av_mallocz(head.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (codec_->extradata == nullptr) {
            error = "out of memory";
            close();
            return false;
        }
        std::copy(head.begin(), head.end(), codec_->extradata);
        codec_->extradata_size = static_cast<int>(head.size());
    }

    if (const int rc = avcodec_open2(codec_, decoder, nullptr); rc < 0) {
        error = std::format("could not start the {} decoder: {}", audio::codecName(codec),
                            averror(rc));
        close();
        return false;
    }

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr) {
        error = "could not allocate decoder working buffers";
        close();
        return false;
    }

    codecId_ = codec;
    sampleRate_ = codec_->sample_rate;

    XV_INFO("audio decoder ready for {} at {} Hz", audio::codecName(codec), sampleRate_);
    return true;
}

void AudioDecoder::close() {
    if (frame_ != nullptr) {
        av_frame_free(&frame_);
    }
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (codec_ != nullptr) {
        avcodec_free_context(&codec_);
    }
    codecId_ = 0;
    sampleRate_ = 0;
}

bool AudioDecoder::decode(const uint8_t* data, size_t size, const FrameCallback& onFrame) {
    if (codec_ == nullptr || data == nullptr || size == 0) {
        return codec_ != nullptr;
    }

    // Points at the caller's read buffer rather than taking it over, the same
    // way the video path does: the buffer is reused for the next packet and
    // libavcodec copies what it needs during send.
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);

    const int rc = avcodec_send_packet(codec_, packet_);

    packet_->data = nullptr;
    packet_->size = 0;

    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        // A packet lost in transit leaves the next one looking corrupt. That is
        // a click in the audio, not a reason to stop listening.
        if (rc == AVERROR_INVALIDDATA) {
            return true;
        }
        XV_WARN("audio decoder rejected a packet: {}", averror(rc));
        return rc != AVERROR(EINVAL);
    }

    for (;;) {
        const int received = avcodec_receive_frame(codec_, frame_);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return true;
        }
        if (received < 0) {
            XV_WARN("audio decoder failed to produce samples: {}", averror(received));
            return false;
        }

        if (onFrame) {
            onFrame(frame_);
        }
        av_frame_unref(frame_);
    }
}

} // namespace xv
