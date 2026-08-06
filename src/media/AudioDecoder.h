#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xv {

// AudioDecoder turns the camera's audio packets into PCM.
//
// The counterpart to VideoDecoder, and much smaller: audio is a few kilobytes a
// second, so there is no hardware path to arrange and nothing to keep on the
// GPU. Decoded frames are handed over as they come out of libavcodec, in
// whatever sample format the codec produces, because the player has to run
// everything through a resampler for the output device anyway and converting
// twice would only lose precision.
//
// Only opened while a camera is being listened to. Recording does not need it:
// a recording stores the camera's own packets, not decoded samples.
class AudioDecoder {
public:
    // Called on the decoder thread. The AVFrame is only valid for the call.
    using FrameCallback = std::function<void(const AVFrame*)>;

    AudioDecoder() = default;
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // `codec` is one of the XMB_CODEC_* audio constants and `sampleRate` is the
    // rate the camera declared, which the codec may override.
    bool open(int codec, int sampleRate, std::string& error);
    void close();

    [[nodiscard]] bool isOpen() const { return codec_ != nullptr; }
    [[nodiscard]] int codecId() const { return codecId_; }
    [[nodiscard]] int sampleRate() const { return sampleRate_; }

    // Feeds one packet. Returns false on a fatal decoder error; a packet that
    // produces no samples is not one.
    bool decode(const uint8_t* data, size_t size, const FrameCallback& onFrame);

private:
    AVCodecContext* codec_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;

    int codecId_ = 0;
    int sampleRate_ = 0;
};

} // namespace xv
