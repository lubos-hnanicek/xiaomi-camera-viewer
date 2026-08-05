#include "media/VideoDecoder.h"

#include <format>

#include "app/Log.h"
#include "xmbridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

namespace xv {
namespace {

std::string averror(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Insists on the D3D11 surface format. Falling back to software here would
// silently turn a 4K stream into a slideshow, so a hard failure is preferable.
AVPixelFormat selectFormat(AVCodecContext* /*context*/, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            return AV_PIX_FMT_D3D11;
        }
    }
    XV_ERROR("the decoder did not offer a D3D11 surface format");
    return AV_PIX_FMT_NONE;
}

// FFmpeg serialises its own access to the shared device through these.
void lockDevice(void* context) {
    static_cast<ID3D10Multithread*>(context)->Enter();
}

void unlockDevice(void* context) {
    static_cast<ID3D10Multithread*>(context)->Leave();
}

} // namespace

VideoDecoder::~VideoDecoder() {
    close();
}

bool VideoDecoder::open(D3D11Context& gpu, int codecId, std::string& error) {
    close();

    AVCodecID ffmpegCodec = AV_CODEC_ID_NONE;
    switch (codecId) {
    case XMB_CODEC_H265: ffmpegCodec = AV_CODEC_ID_HEVC; break;
    case XMB_CODEC_H264: ffmpegCodec = AV_CODEC_ID_H264; break;
    default:
        error = std::format("unsupported video codec id {}", codecId);
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(ffmpegCodec);
    if (codec == nullptr) {
        error = "libavcodec has no decoder for this stream";
        return false;
    }

    // Wrap the application's existing D3D11 device rather than letting FFmpeg
    // create its own, so decoded surfaces can be blitted without a cross-device
    // copy.
    hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hwDevice_ == nullptr) {
        error = "could not allocate a D3D11VA device context";
        return false;
    }

    auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
    auto* d3d11 = static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);

    d3d11->device = gpu.device();
    d3d11->device->AddRef();
    d3d11->device_context = gpu.context();
    d3d11->device_context->AddRef();

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(gpu.context()->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        d3d11->video_device = gpu.videoDevice();
        d3d11->video_device->AddRef();
        d3d11->video_context = gpu.videoContext();
        d3d11->video_context->AddRef();

        d3d11->lock = lockDevice;
        d3d11->unlock = unlockDevice;
        d3d11->lock_ctx = multithread.Detach(); // released by the context's free callback
    }

    if (const int rc = av_hwdevice_ctx_init(hwDevice_); rc < 0) {
        error = std::format("D3D11VA initialisation failed: {}", averror(rc));
        close();
        return false;
    }

    codec_ = avcodec_alloc_context3(codec);
    if (codec_ == nullptr) {
        error = "could not allocate a decoder context";
        close();
        return false;
    }

    codec_->hw_device_ctx = av_buffer_ref(hwDevice_);
    codec_->get_format = selectFormat;
    codec_->thread_count = 1; // the GPU does the work; extra threads only add latency
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_->flags2 |= AV_CODEC_FLAG2_FAST;

    if (const int rc = avcodec_open2(codec_, codec, nullptr); rc < 0) {
        error = std::format("could not open the decoder: {}", averror(rc));
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

    codecId_ = codecId;
    XV_INFO("hardware decoder ready for {}", codecId == XMB_CODEC_H265 ? "H.265" : "H.264");
    return true;
}

void VideoDecoder::close() {
    if (frame_ != nullptr) {
        av_frame_free(&frame_);
    }
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (codec_ != nullptr) {
        avcodec_free_context(&codec_);
    }
    if (hwDevice_ != nullptr) {
        av_buffer_unref(&hwDevice_);
    }
    codecId_ = 0;
    framesDecoded_ = 0;
}

void VideoDecoder::flush() {
    if (codec_ != nullptr) {
        avcodec_flush_buffers(codec_);
    }
}

bool VideoDecoder::decode(const uint8_t* data, size_t size, int64_t ptsMs, const FrameCallback& onFrame) {
    if (codec_ == nullptr || data == nullptr || size == 0) {
        return codec_ != nullptr;
    }

    // av_packet_from_data would take ownership of the buffer; the caller reuses
    // its read buffer every frame, so point at it instead and let libavcodec
    // copy what it needs during send.
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->pts = ptsMs;
    packet_->dts = ptsMs;

    const int rc = avcodec_send_packet(codec_, packet_);

    packet_->data = nullptr;
    packet_->size = 0;

    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        // Corrupt or partial access units are common right after connecting,
        // before the first keyframe arrives, so this is not fatal on its own.
        if (rc == AVERROR_INVALIDDATA) {
            return true;
        }
        XV_WARN("decoder rejected a packet: {}", averror(rc));
        return rc != AVERROR(EINVAL);
    }

    return drain(onFrame);
}

bool VideoDecoder::drain(const FrameCallback& onFrame) {
    for (;;) {
        const int rc = avcodec_receive_frame(codec_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return true;
        }
        if (rc < 0) {
            XV_WARN("decoder failed to produce a frame: {}", averror(rc));
            return false;
        }

        ++framesDecoded_;
        if (onFrame) {
            onFrame(frame_);
        }
        av_frame_unref(frame_);
    }
}

} // namespace xv
