#pragma once

#include "render/D3D11Context.h"

#include <cstdint>

struct AVFrame;

namespace xv {

// VideoFrameTexture converts one camera's decoded output into something ImGui
// can draw.
//
// The decoder hands back NV12 (or P010 for 10-bit) surfaces living in a texture
// array owned by D3D11VA. ImGui needs a plain RGBA shader resource view, and the
// conversion also has to apply the right colour space. The D3D11 video processor
// does both in one pass on the GPU, so the frame never touches system memory.
class VideoFrameTexture {
public:
    ~VideoFrameTexture();

    // Blits the decoded frame into the internal RGBA texture. Safe to call with
    // frames of changing dimensions; the pipeline is rebuilt as needed.
    bool update(D3D11Context& gpu, const AVFrame* frame);

    void reset();

    [[nodiscard]] ID3D11ShaderResourceView* view() const { return outputView_.Get(); }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }
    [[nodiscard]] bool ready() const { return outputView_ != nullptr; }

private:
    bool ensurePipeline(D3D11Context& gpu, uint32_t width, uint32_t height, DXGI_FORMAT inputFormat,
                        uint32_t surfaceWidth, uint32_t surfaceHeight);

    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11Texture2D> outputTexture_;
    ComPtr<ID3D11VideoProcessorOutputView> processorOutput_;
    ComPtr<ID3D11ShaderResourceView> outputView_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT inputFormat_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace xv
