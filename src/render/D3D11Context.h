#pragma once

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>

namespace xv {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// D3D11Context owns the single device shared by ImGui, the video decoders and
// the colour-conversion blits.
//
// One device for everything matters: FFmpeg's D3D11VA decoder writes into
// textures owned by whichever device it was handed, and sharing textures across
// devices would mean an extra copy per frame. The cost is that the device is
// touched from decoder threads as well as the render thread, so multithread
// protection has to be switched on explicitly.
class D3D11Context {
public:
    bool initialize(HWND window, std::string& error);
    void shutdown();

    // Recreates the backbuffer views. Call on WM_SIZE.
    void resize(UINT width, UINT height);

    void beginFrame(const float clearColor[4]);
    void present(bool vsync);

    [[nodiscard]] ID3D11Device* device() const { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const { return context_.Get(); }
    [[nodiscard]] ID3D11VideoDevice* videoDevice() const { return videoDevice_.Get(); }
    [[nodiscard]] ID3D11VideoContext* videoContext() const { return videoContext_.Get(); }
    [[nodiscard]] ID3D11SamplerState* mipSampler() const { return mipSampler_.Get(); }
    [[nodiscard]] bool valid() const { return device_ != nullptr; }

    // Serialises access to the immediate context, which is not free-threaded
    // even with multithread protection enabled for the device.
    class ContextLock {
    public:
        explicit ContextLock(D3D11Context& owner);
        ~ContextLock();
        ContextLock(const ContextLock&) = delete;
        ContextLock& operator=(const ContextLock&) = delete;

    private:
        ID3D10Multithread* guard_;
    };

private:
    void releaseRenderTarget();
    bool createRenderTarget();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D10Multithread> multithread_;

    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11SamplerState> mipSampler_;

    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace xv
