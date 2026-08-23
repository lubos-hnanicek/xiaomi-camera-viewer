#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "render/D3D11Context.h"

struct AVBufferRef;
struct AVCodecParameters;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xv {

// VideoDecoder wraps libavcodec configured for D3D11VA hardware decoding.
//
// It is deliberately not Media Foundation: the built-in HEVC decoder there is
// gated behind the HEVC Video Extensions package from the Store, which would
// mean telling users to buy a codec before the app works. Going through
// libavcodec's D3D11VA path talks to the same GPU decoder without that.
class VideoDecoder {
public:
    // Called on the decoder thread for each decoded frame. The AVFrame is only
    // valid for the duration of the call.
    using FrameCallback = std::function<void(const AVFrame*)>;

    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // `codecId` is one of the XMB_CODEC_* video constants.
    bool open(D3D11Context& gpu, int codecId, std::string& error);
    // Opens from a demuxed file track, including its codec-private data.
    bool open(D3D11Context& gpu, const AVCodecParameters* parameters, std::string& error);
    void close();

    [[nodiscard]] bool isOpen() const { return codec_ != nullptr; }
    [[nodiscard]] int codecId() const { return codecId_; }

    // Feeds one Annex-B access unit. Returns false only on a fatal decoder
    // error; a frame that merely fails to produce output is not an error.
    bool decode(const uint8_t* data, size_t size, int64_t ptsMs, const FrameCallback& onFrame);
    bool decode(const AVPacket* packet, const FrameCallback& onFrame);

    // Discards decoder state, for use after a reconnect.
    void flush();

    [[nodiscard]] uint64_t framesDecoded() const { return framesDecoded_; }

private:
    bool open(D3D11Context& gpu, int ffmpegCodecId, int publicCodecId,
              const AVCodecParameters* parameters, std::string& error);
    bool send(const AVPacket* packet, const FrameCallback& onFrame);
    bool drain(const FrameCallback& onFrame);

    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;

    int codecId_ = 0;
    uint64_t framesDecoded_ = 0;
};

} // namespace xv
