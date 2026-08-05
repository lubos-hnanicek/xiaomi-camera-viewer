#include "render/VideoFrameTexture.h"

#include "app/Log.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace xv {
namespace {

// The decoder's output surface and the array slice within it are passed through
// the AVFrame's data pointers by the D3D11VA hwaccel.
ID3D11Texture2D* frameTexture(const AVFrame* frame) {
    return reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
}

UINT frameArraySlice(const AVFrame* frame) {
    return static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
}

// Cameras tag almost nothing, so unspecified is treated as BT.709 limited
// range, which is what these sensors actually produce.
DXGI_COLOR_SPACE_TYPE inputColorSpace(const AVFrame* frame) {
    const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;

    if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL) {
        return fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    }
    if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M) {
        return fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    }
    return fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                     : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

} // namespace

VideoFrameTexture::~VideoFrameTexture() {
    reset();
}

void VideoFrameTexture::reset() {
    outputView_.Reset();
    processorOutput_.Reset();
    outputTexture_.Reset();
    processor_.Reset();
    enumerator_.Reset();
    width_ = 0;
    height_ = 0;
    inputFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool VideoFrameTexture::ensurePipeline(D3D11Context& gpu, uint32_t width, uint32_t height,
                                       DXGI_FORMAT inputFormat, uint32_t surfaceWidth,
                                       uint32_t surfaceHeight) {
    if (processor_ && width == width_ && height == height_ && inputFormat == inputFormat_) {
        return true;
    }

    reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = width;
    desc.InputHeight = height;
    desc.OutputWidth = width;
    desc.OutputHeight = height;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(gpu.videoDevice()->CreateVideoProcessorEnumerator(&desc, enumerator_.GetAddressOf()))) {
        XV_ERROR("could not create a video processor enumerator for {}x{}", width, height);
        return false;
    }

    if (FAILED(gpu.videoDevice()->CreateVideoProcessor(enumerator_.Get(), 0, processor_.GetAddressOf()))) {
        XV_ERROR("could not create a video processor for {}x{}", width, height);
        reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = width;
    texture.Height = height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(gpu.device()->CreateTexture2D(&texture, nullptr, outputTexture_.GetAddressOf()))) {
        XV_ERROR("could not allocate a {}x{} RGBA target", width, height);
        reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    if (FAILED(gpu.videoDevice()->CreateVideoProcessorOutputView(
            outputTexture_.Get(), enumerator_.Get(), &outputDesc, processorOutput_.GetAddressOf()))) {
        XV_ERROR("could not create the video processor output view");
        reset();
        return false;
    }

    if (FAILED(gpu.device()->CreateShaderResourceView(outputTexture_.Get(), nullptr,
                                                      outputView_.GetAddressOf()))) {
        XV_ERROR("could not create the shader resource view for the video texture");
        reset();
        return false;
    }

    width_ = width;
    height_ = height;
    inputFormat_ = inputFormat;

    XV_INFO("video output pipeline ready at {}x{} from a {}x{} decoder surface", width, height,
            surfaceWidth, surfaceHeight);
    return true;
}

bool VideoFrameTexture::update(D3D11Context& gpu, const AVFrame* frame) {
    if (frame == nullptr || frame->format != AV_PIX_FMT_D3D11 || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    ID3D11Texture2D* source = frameTexture(frame);
    if (source == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);

    D3D11Context::ContextLock lock(gpu);

    if (!ensurePipeline(gpu, static_cast<uint32_t>(frame->width),
                        static_cast<uint32_t>(frame->height), sourceDesc.Format, sourceDesc.Width,
                        sourceDesc.Height)) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.FourCC = 0; // inherit from the texture
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = frameArraySlice(frame);

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(gpu.videoDevice()->CreateVideoProcessorInputView(source, enumerator_.Get(), &inputDesc,
                                                                inputView.GetAddressOf()))) {
        return false;
    }

    ID3D11VideoContext* video = gpu.videoContext();

    // Colour space has to be set per blit: input views are transient, and the
    // driver defaults are not reliably BT.709.
    ComPtr<ID3D11VideoContext1> video1;
    if (SUCCEEDED(video->QueryInterface(IID_PPV_ARGS(&video1)))) {
        video1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, inputColorSpace(frame));
        video1->VideoProcessorSetOutputColorSpace1(processor_.Get(),
                                                   DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE legacy{};
        legacy.Usage = 0;          // playback
        legacy.RGB_Range = 0;      // full range RGB out
        legacy.YCbCr_Matrix = 1;   // BT.709
        legacy.Nominal_Range = frame->color_range == AVCOL_RANGE_JPEG
                                   ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                   : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &legacy);
        video->VideoProcessorSetOutputColorSpace(processor_.Get(), &legacy);
    }

    // The decoder allocates its surfaces padded up to the codec's alignment: a
    // 1440-line stream arrives on a 1536-line surface. Left to itself the
    // processor scales the whole surface into the output, which squeezes the
    // picture and leaves the padding as a black band along the bottom. The
    // source rectangle is the crop the stream already asked for.
    const RECT sourceRect{0, 0, frame->width, frame->height};
    const RECT destRect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    video->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &sourceRect);
    video->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &destRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = inputView.Get();

    const HRESULT hr = video->VideoProcessorBlt(processor_.Get(), processorOutput_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        XV_WARN("video processor blit failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

} // namespace xv
