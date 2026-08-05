#include "render/D3D11Context.h"

#include <format>

#include "app/Log.h"

namespace xv {

D3D11Context::ContextLock::ContextLock(D3D11Context& owner) : guard_(owner.multithread_.Get()) {
    if (guard_ != nullptr) {
        guard_->Enter();
    }
}

D3D11Context::ContextLock::~ContextLock() {
    if (guard_ != nullptr) {
        guard_->Leave();
    }
}

bool D3D11Context::initialize(HWND window, std::string& error) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifndef NDEBUG
    // Only ask for the debug layer if it is actually installed, otherwise
    // device creation fails outright on machines without the SDK.
    if (SUCCEEDED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                      nullptr, 0, D3D11_SDK_VERSION, nullptr, nullptr, nullptr))) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
#endif

    constexpr D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL achieved{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels,
                                     static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION,
                                     device_.GetAddressOf(), &achieved, context_.GetAddressOf());
    if (FAILED(hr)) {
        error = std::format("Direct3D 11 device creation failed (0x{:08X}). A GPU supporting "
                            "feature level 11.0 with video acceleration is required.",
                            static_cast<unsigned>(hr));
        return false;
    }

    // Decoder threads submit work through the same device as the render thread.
    if (SUCCEEDED(context_.As(&multithread_))) {
        multithread_->SetMultithreadProtected(TRUE);
    } else {
        error = "Direct3D 11 device does not support multithread protection.";
        return false;
    }

    // Video interfaces drive hardware HEVC decode and the YUV to RGB blit.
    // Without them there is no point continuing: the alternative is a software
    // decode of 4K HEVC, which will not keep up.
    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        error = "This GPU or driver does not expose the Direct3D 11 video interfaces.";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        error = "Could not reach the DXGI factory for this device.";
        return false;
    }

    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
        char name[sizeof(adapterDesc.Description)]{};
        ::WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name), nullptr, nullptr);
        XV_INFO("graphics adapter: {}", name);
    }

    RECT client{};
    ::GetClientRect(window, &client);
    width_ = static_cast<UINT>(client.right - client.left);
    height_ = static_cast<UINT>(client.bottom - client.top);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    // Flip presentation is what lets a windowed app hit the compositor's fast
    // path, which matters when several video tiles are updating at once.
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr,
                                         swapChain_.GetAddressOf());
    if (FAILED(hr)) {
        error = std::format("Swap chain creation failed (0x{:08X}).", static_cast<unsigned>(hr));
        return false;
    }

    // Alt+Enter fullscreen is handled by the app, not DXGI.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    if (!createRenderTarget()) {
        error = "Could not create the swap chain render target.";
        return false;
    }

    return true;
}

void D3D11Context::shutdown() {
    releaseRenderTarget();
    swapChain_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    multithread_.Reset();
    context_.Reset();
    device_.Reset();
}

bool D3D11Context::createRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                                     renderTarget_.GetAddressOf()));
}

void D3D11Context::releaseRenderTarget() {
    renderTarget_.Reset();
}

void D3D11Context::resize(UINT width, UINT height) {
    if (swapChain_ == nullptr || width == 0 || height == 0) {
        return;
    }
    if (width == width_ && height == height_) {
        return;
    }

    ContextLock lock(*this);

    releaseRenderTarget();
    if (FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
        XV_ERROR("swap chain resize to {}x{} failed", width, height);
    }
    createRenderTarget();

    width_ = width;
    height_ = height;
}

void D3D11Context::beginFrame(const float clearColor[4]) {
    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(target, clearColor);
}

void D3D11Context::present(bool vsync) {
    if (swapChain_ == nullptr) {
        return;
    }
    const HRESULT hr = swapChain_->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        XV_ERROR("the graphics device was lost (0x{:08X}); a restart is needed",
                 static_cast<unsigned>(hr));
    }
}

} // namespace xv
