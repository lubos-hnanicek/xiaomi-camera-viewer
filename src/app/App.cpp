#include "app/App.h"

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "app/Frameless.h"
#include "app/Log.h"
#include "app/Resources.h"
#include "app/SingleInstance.h"
#include "app/Theme.h"
#include "app/Version.h"
#include "bridge/Bridge.h"
#include "ui/Views.h"
#include "util/Encoding.h"
#include "util/FileDialog.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace xv {
namespace {

constexpr wchar_t kWindowClass[] = L"XiaomiViewerWindow";
constexpr wchar_t kWindowTitle[] = L"Xiaomi Camera Viewer";

// Where the window opens when there is nothing saved, or when what was saved
// would put it somewhere it cannot be reached.
constexpr int kDefaultWidth = 1480;
constexpr int kDefaultHeight = 900;

// How much of a restored window has to land on a monitor for the position to be
// used. Monitors get unplugged, resolutions change and laptops come home from
// the docking station, so a position that was fine last time can leave the
// window somewhere with no way to drag it back.
constexpr double kMinVisibleFraction = 0.5;

// One size out of the icon group in our own resources. The size is asked for
// explicitly because LoadIcon would always take the 32-pixel image and let
// Windows shrink it, and a 16-pixel icon scaled from that is the smeared one
// seen in the taskbar and the Alt+Tab list.
HICON loadAppIcon(HINSTANCE instance, int width, int height) {
    return static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                           width, height, LR_DEFAULTCOLOR));
}

long long rectArea(const RECT& rect) {
    const long long width = rect.right - rect.left;
    const long long height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return width * height;
}

struct VisibleArea {
    RECT window;
    long long area;
};

BOOL CALLBACK addMonitorOverlap(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == 0) {
        return TRUE;
    }

    auto* visible = reinterpret_cast<VisibleArea*>(param);
    RECT overlap{};
    if (::IntersectRect(&overlap, &visible->window, &info.rcWork) != 0) {
        visible->area += rectArea(overlap);
    }
    return TRUE;
}

// Whether enough of the window would be on screen to use it. Measured against
// the work areas rather than the whole desktop, so a window restored under the
// taskbar counts as hidden, which is how it would feel.
bool mostlyOnScreen(const RECT& rect) {
    const long long total = rectArea(rect);
    if (total == 0) {
        return false;
    }

    // Work areas do not overlap, so the parts on each monitor simply add up.
    VisibleArea visible{rect, 0};
    ::EnumDisplayMonitors(nullptr, nullptr, &addMonitorOverlap, reinterpret_cast<LPARAM>(&visible));

    return static_cast<double>(visible.area) >= static_cast<double>(total) * kMinVisibleFraction;
}

// GetWindowRect reports screen coordinates, while WINDOWPLACEMENT uses workspace
// coordinates for an ordinary top-level window. Account for a taskbar docked to
// the top or left of the window's monitor before putting that rectangle into a
// WINDOWPLACEMENT structure.
bool screenToWorkspace(RECT& rect) {
    const HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || ::GetMonitorInfoW(monitor, &info) == 0) {
        return false;
    }

    const int offsetX = info.rcWork.left - info.rcMonitor.left;
    const int offsetY = info.rcWork.top - info.rcMonitor.top;
    ::OffsetRect(&rect, -offsetX, -offsetY);
    return true;
}

// Names the claim on being the only running copy. Local\ scopes it to the
// signed-in user's session, which is the granularity the app's own state has:
// the config and the log live in that user's %APPDATA%.
constexpr wchar_t kSingleInstanceName[] = L"Local\\XiaomiCameraViewer.SingleInstance";

// Decodes a PNG into a fresh RGBA texture using WIC, which ships with Windows
// and saves pulling in an image library for the one captcha image.
bool decodePngToTexture(ID3D11Device* device, const std::vector<uint8_t>& png,
                        ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),
                                            static_cast<DWORD>(png.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                                &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()),
                                     pixels.data()))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * 4;

    ComPtr<ID3D11Texture2D> created;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &created))) {
        return false;
    }

    ComPtr<ID3D11ShaderResourceView> createdView;
    if (FAILED(device->CreateShaderResourceView(created.Get(), nullptr, &createdView))) {
        return false;
    }

    texture = created;
    view = createdView;
    return true;
}

// The caption glyphs are drawn rather than typed: the default font has no dash,
// box or cross worth using, and shapes this simple are cheaper to draw than to
// carry a font for.
enum class CaptionGlyph { Minimize, Maximize, Restore, Close };

void drawCaptionGlyph(ImDrawList* canvas, ImVec2 centre, float size, CaptionGlyph glyph,
                      ImU32 colour) {
    const float half = std::round(size * 0.5f);
    const float thickness = std::max(1.0f, std::round(size / 10.0f));

    switch (glyph) {
    case CaptionGlyph::Minimize:
        canvas->AddLine(ImVec2(centre.x - half, centre.y), ImVec2(centre.x + half, centre.y), colour,
                        thickness);
        break;

    case CaptionGlyph::Maximize:
        canvas->AddRect(ImVec2(centre.x - half, centre.y - half),
                        ImVec2(centre.x + half, centre.y + half), colour, 0.0f, thickness);
        break;

    case CaptionGlyph::Restore: {
        // Two overlapping squares, the back one peeking out at the top right.
        // The front one is filled first so the back one does not show through.
        const float offset = std::round(size * 0.25f);
        canvas->AddRect(ImVec2(centre.x - half + offset, centre.y - half),
                        ImVec2(centre.x + half, centre.y + half - offset), colour, 0.0f, thickness);
        canvas->AddRectFilled(ImVec2(centre.x - half, centre.y - half + offset),
                              ImVec2(centre.x + half - offset, centre.y + half),
                              ImGui::GetColorU32(ImGuiCol_MenuBarBg));
        canvas->AddRect(ImVec2(centre.x - half, centre.y - half + offset),
                        ImVec2(centre.x + half - offset, centre.y + half), colour, 0.0f, thickness);
        break;
    }

    case CaptionGlyph::Close:
        canvas->AddLine(ImVec2(centre.x - half, centre.y - half),
                        ImVec2(centre.x + half, centre.y + half), colour, thickness);
        canvas->AddLine(ImVec2(centre.x + half, centre.y - half),
                        ImVec2(centre.x - half, centre.y + half), colour, thickness);
        break;
    }
}

void paintCaptionButton(ImVec2 origin, ImVec2 size, CaptionGlyph glyph, bool hovered, bool held,
                        bool danger) {
    ImDrawList* canvas = ImGui::GetWindowDrawList();

    if (hovered) {
        // Red for close, the way every other window on the desktop does it.
        const ImVec4 tint = danger ? ImVec4(0.77f, 0.20f, 0.20f, 1.0f)
                                   : ImVec4(1.0f, 1.0f, 1.0f, held ? 0.16f : 0.09f);
        canvas->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                              ImGui::GetColorU32(held && danger ? ImVec4(0.62f, 0.16f, 0.16f, 1.0f)
                                                                : tint));
    }

    const ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
    const ImU32 colour = ImGui::GetColorU32(hovered ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                                    : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    drawCaptionGlyph(canvas, centre, std::round(size.y * 0.32f), glyph, colour);
}

} // namespace

// --- Window -----------------------------------------------------------------

LRESULT CALLBACK App::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* app = reinterpret_cast<App*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app != nullptr) {
        return app->handleMessage(window, message, wParam, lParam);
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return 1;
    }

    switch (message) {
    // --- The frame the app draws instead of Windows ---

    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            params->rgrc[0] = frameless::clientRectFor(window, params->rgrc[0]);
            return 0;
        }
        break;

    case WM_NCHITTEST:
        return frameless::hitTest(window, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)},
                                  caption_);

    // Windows owns the mouse over the maximize button, because that is what
    // makes the snap layouts flyout appear. The press, the release and the
    // hover highlight all have to be answered here instead of in ImGui.
    case WM_NCMOUSEMOVE:
        maximizeHot_ = wParam == HTMAXBUTTON;
        if (maximizeHot_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0};
            ::TrackMouseEvent(&track);
        }
        break;

    case WM_NCMOUSELEAVE:
        maximizeHot_ = false;
        maximizePressed_ = false;
        break;

    case WM_NCLBUTTONDOWN:
        if (wParam == HTMAXBUTTON) {
            maximizePressed_ = true;
            return 0;
        }
        break;

    case WM_NCLBUTTONUP:
        if (wParam == HTMAXBUTTON) {
            if (maximizePressed_) {
                maximizePressed_ = false;
                ::ShowWindow(window, ::IsZoomed(window) != 0 ? SW_RESTORE : SW_MAXIMIZE);
            }
            return 0;
        }
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            occluded_ = true;
        } else {
            occluded_ = false;
            // Resizing during the message is legal but resizing once per frame
            // keeps the swap chain and ImGui in step.
            pendingWidth_ = LOWORD(lParam);
            pendingHeight_ = HIWORD(lParam);
        }
        return 0;

    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = POINT{900, 600};
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) {
            return 0; // Alt on its own should not open the system menu
        }
        break;

    case WM_CLOSE:
        running_ = false;
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(window, message, wParam, lParam);
}

bool App::createWindow(HINSTANCE instance, int showCommand, std::string& error) {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &App::windowProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
    wc.lpszClassName = kWindowClass;

    // Both sizes are loaded from our own resources: the large one is what
    // Alt+Tab and the taskbar show, the small one is the window's own. Leaving
    // hIconSm null would have Windows derive it from the large image rather
    // than use the one drawn for that size. The handles live as long as the
    // class, which is as long as the process, and go with it.
    wc.hIcon = loadAppIcon(instance, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON));
    wc.hIconSm =
        loadAppIcon(instance, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON));

    if (::RegisterClassExW(&wc) == 0) {
        error = "Could not register the window class.";
        return false;
    }

    window_ = ::CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, kDefaultWidth, kDefaultHeight, nullptr, nullptr,
                                instance, this);
    if (window_ == nullptr) {
        error = "Could not create the application window.";
        return false;
    }

    // The caption is drawn by the app, so the only frame left to Windows is the
    // resize border. Dark mode still matters for the fraction of a second before
    // the first frame, and for the shadow's tint.
    const BOOL darkMode = TRUE;
    ::DwmSetWindowAttribute(window_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode,
                            sizeof(darkMode));

    // Recompute the frame now that WM_NCCALCSIZE has something to say about it.
    ::SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    ::ShowWindow(window_, restorePlacement(showCommand));
    ::UpdateWindow(window_);
    return true;
}

int App::restorePlacement(int showCommand) {
    const WindowPlacement& saved = config_.window;
    if (!saved.valid()) {
        return showCommand;
    }

    const RECT rect{saved.x, saved.y, saved.x + saved.width, saved.y + saved.height};
    if (!mostlyOnScreen(rect)) {
        XV_INFO("the saved window position ({},{} {}x{}) is mostly off screen; opening where "
                "Windows puts it instead",
                saved.x, saved.y, saved.width, saved.height);
        return showCommand;
    }

    // SW_HIDE moves the window without showing it, since ShowWindow below is
    // what decides whether it comes up normal or maximized.
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    placement.showCmd = SW_HIDE;
    placement.rcNormalPosition = rect;

    if (::SetWindowPlacement(window_, &placement) == 0) {
        XV_WARN("could not restore the saved window position (error {})", ::GetLastError());
        return showCommand;
    }

    // The command from the shell is dropped here on purpose: it says how to open
    // a window with no history, and this one has history.
    return saved.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
}

void App::rememberPlacement() {
    if (window_ == nullptr || ::IsWindow(window_) == 0) {
        return; // gone already, so last time's position stays saved
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (::GetWindowPlacement(window_, &placement) == 0) {
        XV_WARN("could not read the window position (error {})", ::GetLastError());
        return;
    }

    // Windows keeps the pre-snap restore rectangle in rcNormalPosition. When the
    // window is neither minimized nor maximized, read its actual bounds instead
    // so a snapped position is remembered. Minimized and maximized windows still
    // need rcNormalPosition because their current bounds are not the bounds they
    // should return to.
    RECT rect = placement.rcNormalPosition;
    if (::IsIconic(window_) == 0 && ::IsZoomed(window_) == 0) {
        RECT current{};
        if (::GetWindowRect(window_, &current) != 0 && screenToWorkspace(current)) {
            rect = current;
        } else {
            XV_WARN("could not read the current window bounds (error {})", ::GetLastError());
        }
    }

    config_.window.x = rect.left;
    config_.window.y = rect.top;
    config_.window.width = rect.right - rect.left;
    config_.window.height = rect.bottom - rect.top;

    // A minimized window keeps the state it will go back to in its flags, so
    // closing from the taskbar still remembers that it was maximized.
    config_.window.maximized =
        placement.showCmd == SW_SHOWMAXIMIZED ||
        (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
}

// --- ImGui ------------------------------------------------------------------

bool App::setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // window layout is derived, not persisted

    // The help and the log are read against the picture, so being unable to put
    // one beside the window rather than on top of it is a real limitation. With
    // viewports on, dragging one past the edge of the app hands it an OS window
    // of its own, and it can go on another monitor entirely.
    //
    // Docking is deliberately left off: this app has fixed screens rather than a
    // layout to arrange, and dock targets over live video would be noise.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // A window that left the app is still part of it, so it is made an owned
    // window rather than a top-level one of its own. Windows then keeps it above
    // the app whatever is clicked, and brings it back up with it, which is the
    // whole point: a help page that disappears behind the window it is being
    // read against is no help at all. An owner is not a modal parent, so the app
    // underneath stays fully usable.
    //
    // ImGui defaults this the other way, and without it every dragged-out window
    // is a stranger to the app that Windows is free to bury behind it.
    io.ConfigViewportsNoDefaultParent = false;

    // No taskbar button for them either. They are panels of this app, not
    // windows to alt-tab to. Being owned windows, they minimize and restore with
    // the app rather than being left behind on an empty desktop.
    io.ConfigViewportsNoTaskBarIcon = true;

    const float dpiScale = static_cast<float>(::GetDpiForWindow(window_)) / 96.0f;
    const float scale = dpiScale * config_.uiScale;

    theme::apply(scale);
    ImGui::GetStyle().FontScaleMain = scale;

    if (!ImGui_ImplWin32_Init(window_)) {
        return false;
    }
    return ImGui_ImplDX11_Init(gpu_.device(), gpu_.context());
}

void App::teardownImGui() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// --- Lifecycle --------------------------------------------------------------

int App::run(HINSTANCE instance, int showCommand) {
    // Settled before anything else, because opening the log truncates it and a
    // second copy would wipe the running copy's log on its way to finding out it
    // is not wanted. Two copies would also fight over the cameras: a camera with
    // a second session open keeps sending video while quietly ignoring every
    // pan and tilt command.
    const SingleInstance onlyInstance(kSingleInstanceName);
    if (!onlyInstance.claimed()) {
        if (!activateRunningInstance(kWindowClass)) {
            ::MessageBoxW(nullptr, L"Xiaomi Camera Viewer is already running.", kWindowTitle,
                          MB_ICONINFORMATION | MB_OK);
        }
        return 0;
    }

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    config_ = AppConfig::load();
    log::init(AppConfig::logPath());
    XV_INFO("Xiaomi Camera Viewer {} starting", XV_VERSION);

    if (!onlyInstance.error().empty()) {
        XV_WARN("could not check whether another copy is already running: {}",
                onlyInstance.error());
    }

    std::string error;

    if (!createWindow(instance, showCommand, error)) {
        ::MessageBoxA(nullptr, error.c_str(), "Xiaomi Camera Viewer", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!gpu_.initialize(window_, error)) {
        XV_ERROR("{}", error);
        ::MessageBoxA(window_, error.c_str(), "Xiaomi Camera Viewer", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!setupImGui()) {
        ::MessageBoxA(window_, "Could not initialise the user interface.", "Xiaomi Camera Viewer",
                      MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!Bridge::instance().load(error)) {
        XV_ERROR("{}", error);
        login_.error = error;
    } else {
        restoreSession();
    }

    MSG message{};
    while (running_) {
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running_ = false;
            }
        }
        if (!running_) {
            break;
        }

        if (pendingWidth_ != 0 && pendingHeight_ != 0) {
            gpu_.resize(pendingWidth_, pendingHeight_);
            pendingWidth_ = 0;
            pendingHeight_ = 0;
        }

        if (occluded_) {
            // Nothing is visible, so yield instead of spinning on an empty
            // present that DXGI would reject anyway.
            ::Sleep(80);
            continue;
        }

        frame();
    }

    XV_INFO("shutting down");
    playback_.close();
    stopStreams();
    audio_.stop();
    // Read before the window goes anywhere, and saved along with everything else.
    rememberPlacement();
    config_.save();

    teardownImGui();
    gpu_.shutdown();
    log::shutdown();

    ::CoUninitialize();
    return 0;
}

void App::frame() {
    pumpTasks();

    // The live grid takes Tab and the arrow keys for itself: Tab picks the camera
    // and the arrows point it. ImGui's keyboard navigation wants the same keys for
    // moving a focus ring around the toolbar, and would answer a press meant for
    // the camera by activating a button, so it stands down while the grid is up.
    // Every other screen is a form, where tabbing between fields is the point.
    ImGuiIO& io = ImGui::GetIO();
    if (screen_ == Screen::Grid || screen_ == Screen::Playback || screen_ == Screen::SdCard) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    handleGlobalKeys();
    drawMenuBar();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr, kRootFlags);
    ImGui::PopStyleVar(2);

    switch (screen_) {
    case Screen::Login: drawLoginView(*this); break;
    case Screen::Cameras: drawCamerasView(*this); break;
    case Screen::Grid: drawGridView(*this); break;
    case Screen::Settings: drawSettingsView(*this); break;
    case Screen::Playback: drawPlaybackView(*this); break;
    case Screen::SdCard: drawSdCardView(*this); break;
    }

    ImGui::End();

    if (showLogWindow_) {
        ImGui::SetNextWindowSize(ImVec2(900, 460), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Log", &showLogWindow_)) {
            const auto entries = log::recent();
            if (ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const auto& entry : entries) {
                    ImVec4 color = theme::kMuted;
                    switch (entry.level) {
                    case log::Level::Warn: color = theme::kPending; break;
                    case log::Level::Error: color = theme::kFailed; break;
                    case log::Level::Info: color = ImGui::GetStyleColorVec4(ImGuiCol_Text); break;
                    default: break;
                    }
                    ImGui::TextColored(color, "%s  %-5s  %s", entry.timestamp.c_str(),
                                       log::levelName(entry.level), entry.message.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    if (showHelpWindow_) {
        drawHelpWindow(*this, showHelpWindow_, helpTab_);
    }

    ImGui::Render();

    constexpr float kClear[4] = {0.06f, 0.07f, 0.08f, 1.0f};
    gpu_.beginFrame(kClear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // A window that has been dragged out of the main one draws into a swap chain
    // of its own, which nothing else in this loop knows about. Both calls do
    // nothing while everything is still inside.
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();

    gpu_.present(true);
}

void App::handleGlobalKeys() {
    // Neither of these is a key a text field consumes, so unlike the grid's
    // camera keys they need no guard against one having the keyboard: Ctrl+S in
    // the middle of typing a password is still a request to save.
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        showHelpWindow_ = !showHelpWindow_;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        config_.save();
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        openRecordingDialog();
    }
    if (screen_ == Screen::Grid && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_R)) {
        toggleGlobalRecording();
    }
    if (screen_ == Screen::SdCard && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        closeSdPlayback();
    }
}

void App::drawMenuBar() {
    // The menu bar doubles as the title bar now, so it is given the height of
    // one rather than the height of a menu.
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x, std::round(ImGui::GetFontSize() * 0.6f)));

    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar();
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open recording...", "Ctrl+O")) {
            openRecordingDialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save configuration", "Ctrl+S")) {
            config_.save();
        }
        if (ImGui::MenuItem("Open configuration folder")) {
            ::ShellExecuteW(nullptr, L"open", AppConfig::directory().c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            running_ = false;
        }
        ImGui::EndMenu();
    }

    if (signedIn_) {
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Live grid", nullptr, screen_ == Screen::Grid)) {
                leavePlayerScreens();
                screen_ = Screen::Grid;
            }
            if (ImGui::MenuItem("Cameras", nullptr, screen_ == Screen::Cameras)) {
                leavePlayerScreens();
                screen_ = Screen::Cameras;
            }
            if (ImGui::MenuItem("Settings", nullptr, screen_ == Screen::Settings)) {
                leavePlayerScreens();
                screen_ = Screen::Settings;
            }
            if (ImGui::MenuItem("Playback", nullptr, screen_ == Screen::Playback,
                                playback_.status().open || !playbackError_.empty())) {
                leavePlayerScreens();
                screen_ = Screen::Playback;
            }

            // One camera's card at a time, chosen here because the card belongs
            // to a camera rather than to the app. A dual-lens CW500 is two live
            // tiles over one card, and each lens has its own catalogue on it,
            // so both are listed and each plays its own picture.
            if (ImGui::BeginMenu("Camera SD card", !config_.cameras.empty())) {
                for (size_t i = 0; i < config_.cameras.size(); ++i) {
                    const CameraConfig& camera = config_.cameras[i];
                    const bool supported = sdPlaybackSupported(camera);
                    if (ImGui::MenuItem(camera.label().c_str(), nullptr, false, supported)) {
                        openSdPlayback(i);
                    }
                    if (!supported) {
                        ImGui::SetItemTooltip(
                            "This model's card has not been worked out yet. Only the "
                            "CW400 and CW500 answer the requests this build sends.");
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::MenuItem("Log", nullptr, &showLogWindow_);
            ImGui::EndMenu();
        }

        // Neither player screen has live streams to act on: both stopped them to
        // get the camera to themselves.
        if (ImGui::BeginMenu("Streams",
                             screen_ != Screen::Playback && screen_ != Screen::SdCard)) {
            // "Restart", because it rebuilds the tiles from the camera list and
            // reconnects whatever is already running along with whatever is not.
            if (ImGui::MenuItem("Restart all")) {
                startStreams();
            }
            ImGui::SetItemTooltip("Reconnect every enabled camera and pick up any "
                                  "changes to the camera list.");
            if (ImGui::MenuItem("Stop all")) {
                stopStreams();
            }

            ImGui::Separator();

            const GlobalRecorder::Status global = globalRecorder_.status();
            const bool globalBusy = globalRecorder_.active();
            if (ImGui::MenuItem(globalBusy ? "Stop global recording" : "Record all live cameras",
                                "Shift+R", false,
                                globalBusy || globalRecordingAvailable())) {
                toggleGlobalRecording();
            }
            ImGui::SetItemTooltip("Writes one video track per currently live view and one audio "
                                  "track per physical camera into a single Matroska file.");
            if (global.state == GlobalRecorder::State::Error) {
                ImGui::TextColored(theme::kFailed, "Global recording: %s", global.error.c_str());
            }

            ImGui::Separator();

            CameraStream* focused = selected_ >= 0 && selected_ < static_cast<int>(streams_.size())
                                        ? streams_[static_cast<size_t>(selected_)].get()
                                        : nullptr;
            const bool busy = focused != nullptr && recording(*focused);

            if (ImGui::MenuItem(busy ? "Stop recording" : "Record selected camera", "R",
                                false, focused != nullptr)) {
                toggleRecording(*focused);
            }
            ImGui::SetItemTooltip("Writes the camera's own stream to a Matroska file without "
                                  "re-encoding it, audio included.");
            if (ImGui::MenuItem("Open recordings folder")) {
                openRecordingsFolder();
            }

            ImGui::Separator();

            const bool audible = focused != nullptr && listening(*focused);
            if (ImGui::MenuItem(audible ? "Mute" : "Listen to selected camera", "A", false,
                                focused != nullptr)) {
                toggleListening(*focused);
            }
            ImGui::SetItemTooltip("Only one camera is audible at a time.");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Account")) {
            ImGui::TextDisabled("Signed in as %s", config_.account.userId.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Sign out")) {
                signOut();
            }
            ImGui::EndMenu();
        }
    } else {
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Log", nullptr, &showLogWindow_);
            ImGui::EndMenu();
        }
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Contents", "F1")) {
            showHelpWindow_ = true;
            helpTab_ = nullptr;
        }
        if (ImGui::MenuItem("Keyboard and mouse")) {
            showHelpWindow_ = true;
            helpTab_ = kHelpTabControls;
        }
        if (ImGui::MenuItem("Troubleshooting")) {
            showHelpWindow_ = true;
            helpTab_ = kHelpTabTrouble;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About")) {
            showHelpWindow_ = true;
            helpTab_ = kHelpTabAbout;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Xiaomi Camera Viewer %s", XV_VERSION);
        ImGui::TextDisabled("Bridge %s", Bridge::instance().version().c_str());
        ImGui::EndMenu();
    }

    // Everything from here to the caption buttons is empty bar, and dragging it
    // moves the window.
    const float dragFrom = ImGui::GetCursorPosX();
    const float captionWidth = std::round(ImGui::GetWindowHeight() * 1.6f) * 3.0f;

    // Right-aligned status summary, clear of the buttons.
    if (signedIn_ && !streams_.empty()) {
        int live = 0;
        for (const auto& stream : streams_) {
            if (stream->worker && stream->worker->status().state == StreamState::Streaming) {
                ++live;
            }
        }
        std::string summary = std::format("{} of {} live", live, streams_.size());
        const GlobalRecorder::Status global = globalRecorder_.status();
        if (global.state == GlobalRecorder::State::Preparing) {
            summary += std::format("  |  preparing {}/{}", global.prepared, global.participants);
        } else if (global.state == GlobalRecorder::State::Recording) {
            const int64_t seconds = global.durationMs / 1000;
            summary += std::format("  |  ALL REC {}:{:02}", seconds / 60, seconds % 60);
        }
        const float width = ImGui::CalcTextSize(summary.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - captionWidth - width - 16.0f);
        ImGui::TextColored(global.state == GlobalRecorder::State::Recording
                               ? theme::kFailed
                               : live > 0 ? theme::kLive : theme::kMuted,
                           "%s", summary.c_str());
    }

    drawCaptionButtons(dragFrom);

    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();
}

void App::drawCaptionButtons(float dragFrom) {
    const float height = ImGui::GetWindowHeight();
    const ImVec2 size(std::round(height * 1.6f), height);
    const float first = ImGui::GetWindowWidth() - size.x * 3.0f;

    ImGui::SetCursorPos(ImVec2(first, 0.0f));
    if (ImGui::InvisibleButton("##minimize", size)) {
        ::ShowWindow(window_, SW_MINIMIZE);
    }
    paintCaptionButton(ImGui::GetItemRectMin(), size, CaptionGlyph::Minimize, ImGui::IsItemHovered(),
                       ImGui::IsItemActive(), false);

    // No button here, only paint: the hit test hands this rectangle to Windows
    // so hovering it opens the snap layouts flyout, which means the press and
    // the hover arrive as non-client messages instead of through ImGui.
    const bool maximized = ::IsZoomed(window_) != 0;
    ImGui::SetCursorPos(ImVec2(first + size.x, 0.0f));
    ImGui::Dummy(size);
    paintCaptionButton(ImGui::GetItemRectMin(), size,
                       maximized ? CaptionGlyph::Restore : CaptionGlyph::Maximize, maximizeHot_,
                       maximizePressed_, false);

    ImGui::SetCursorPos(ImVec2(first + size.x * 2.0f, 0.0f));
    if (ImGui::InvisibleButton("##close", size)) {
        running_ = false;
    }
    paintCaptionButton(ImGui::GetItemRectMin(), size, CaptionGlyph::Close, ImGui::IsItemHovered(),
                       ImGui::IsItemActive(), true);

    caption_.height = height;
    caption_.dragFrom = dragFrom;
    caption_.dragTo = first;
    caption_.maximizeFrom = first + size.x;
    caption_.maximizeTo = first + size.x * 2.0f;
}

// --- Tasks ------------------------------------------------------------------

void App::pumpTasks() {
    if (auto result = restoreTask_.poll()) {
        if (responseOk(*result)) {
            signedIn_ = true;
            const Json& account = (*result)["account"];
            config_.account.userId = account.value("user_id", config_.account.userId);
            config_.account.token = account.value("token", config_.account.token);
            config_.save();
            XV_INFO("restored the saved session for {}", config_.account.userId);
            const bool keepPlayback = playbackSuspendedLive_;
            if (!keepPlayback) {
                screen_ = config_.cameras.empty() ? Screen::Cameras : Screen::Grid;
            }
            if (config_.cameras.empty()) {
                refreshDevices();
            } else if (!keepPlayback) {
                startStreams();
            }
        } else {
            XV_WARN("saved session could not be restored: {}", responseError(*result));
            login_.error = "Your saved sign-in has expired. Please sign in again.";
            if (!playbackSuspendedLive_) {
                screen_ = Screen::Login;
            }
        }
        login_.busy = false;
    }

    if (auto result = loginTask_.poll()) {
        applyLoginResult(*result);
    }

    if (auto result = deviceTask_.poll()) {
        if (responseOk(*result)) {
            devices_.clear();
            for (const auto& entry : (*result)["devices"]) {
                devices_.push_back(DiscoveredDevice{
                    entry.value("did", std::string{}),
                    entry.value("name", std::string{}),
                    entry.value("model", std::string{}),
                    entry.value("ip", std::string{}),
                    entry.value("mac", std::string{}),
                });
            }
            deviceError_.clear();
            XV_INFO("found {} camera(s) on the account", devices_.size());
        } else {
            deviceError_ = responseError(*result);
        }
    }

    for (auto& stream : streams_) {
        auto result = stream->miotTask.poll();
        if (!result) {
            continue;
        }

        stream->miotBusyLabel.clear();
        stream->miotError = *result;
        if (result->empty()) {
            stream->miotLoaded = true;
        }

        // A write only tells us it was accepted; re-reading is what confirms the
        // camera's actual state, including values it silently clamped.
        if (stream->miotRefreshQueued) {
            stream->miotRefreshQueued = false;
            loadSettingsFor(*stream);
        }
    }
}

// --- Sign-in ----------------------------------------------------------------

void App::restoreSession() {
    if (!config_.account.valid()) {
        return;
    }

    login_.busy = true;
    login_.status = "Restoring your saved sign-in";

    const AccountConfig account = config_.account;
    restoreTask_.start([account] {
        return Bridge::instance().call("login.token", Json{
                                                          {"user_id", account.userId},
                                                          {"region", account.region},
                                                          {"token", account.token},
                                                      });
    });
}

void App::beginLogin(const std::string& username, const std::string& password,
                     const std::string& region) {
    login_.busy = true;
    login_.error.clear();
    login_.status = "Signing in";
    login_.needCaptcha = false;
    login_.needVerify = false;
    config_.account.region = region;

    loginTask_.start([username, password, region] {
        return Bridge::instance().call("login.begin", Json{
                                                          {"username", username},
                                                          {"password", password},
                                                          {"region", region},
                                                      });
    });
}

void App::submitCaptcha(const std::string& code) {
    login_.busy = true;
    login_.error.clear();
    login_.status = "Checking the captcha";

    loginTask_.start(
        [code] { return Bridge::instance().call("login.captcha", Json{{"code", code}}); });
}

void App::submitVerify(const std::string& ticket) {
    login_.busy = true;
    login_.error.clear();
    login_.status = "Checking the verification code";

    loginTask_.start(
        [ticket] { return Bridge::instance().call("login.verify", Json{{"ticket", ticket}}); });
}

void App::applyLoginResult(const Json& response) {
    login_.busy = false;

    if (!responseOk(response)) {
        login_.error = responseError(response);
        login_.status.clear();
        return;
    }

    const std::string status = response.value("status", std::string{});

    if (status == "captcha") {
        login_.needCaptcha = true;
        login_.needVerify = false;
        login_.captchaCode.clear();
        login_.status = "Enter the characters shown below";
        setCaptcha(response.value("captcha_png", std::string{}));
        return;
    }

    if (status == "verify") {
        login_.needVerify = true;
        login_.needCaptcha = false;
        login_.verifyTicket.clear();

        const std::string phone = response.value("verify_phone", std::string{});
        const std::string email = response.value("verify_email", std::string{});
        login_.verifyTarget = !phone.empty() ? phone : email;
        login_.status = login_.verifyTarget.empty()
                            ? "Enter the verification code Xiaomi sent you"
                            : std::format("Enter the code sent to {}", login_.verifyTarget);
        return;
    }

    if (status != "success") {
        login_.error = "Xiaomi returned an unexpected sign-in state.";
        return;
    }

    const Json& account = response["account"];
    config_.account.userId = account.value("user_id", std::string{});
    config_.account.token = account.value("token", std::string{});
    config_.account.region = account.value("region", config_.account.region);
    config_.save();

    signedIn_ = true;
    login_ = LoginState{};
    captchaView_.Reset();
    captchaTexture2D_.Reset();

    XV_INFO("signed in as {}", config_.account.userId);

    const bool keepPlayback = playbackSuspendedLive_;
    if (!keepPlayback) {
        screen_ = config_.cameras.empty() ? Screen::Cameras : Screen::Grid;
    }
    if (config_.cameras.empty()) {
        refreshDevices();
    } else if (!keepPlayback) {
        startStreams();
    }
}

void App::setCaptcha(const std::string& base64Png) {
    captchaView_.Reset();
    captchaTexture2D_.Reset();

    const auto png = encoding::base64Decode(base64Png);
    if (png.empty()) {
        return;
    }
    if (!decodePngToTexture(gpu_.device(), png, captchaTexture2D_, captchaView_)) {
        XV_WARN("could not decode the captcha image");
    }
}

void App::signOut() {
    closePlayback(false);
    stopStreams();

    const std::string userId = config_.account.userId;
    if (!userId.empty()) {
        Bridge::instance().call("account.forget", Json{{"user_id", userId}});
    }

    config_.account = AccountConfig{};
    config_.save();

    devices_.clear();
    signedIn_ = false;
    login_ = LoginState{};
    screen_ = Screen::Login;

    XV_INFO("signed out");
}

// --- Devices and streams ----------------------------------------------------

void App::refreshDevices() {
    if (deviceTask_.busy()) {
        return;
    }

    deviceError_.clear();

    const std::string userId = config_.account.userId;
    deviceTask_.start([userId] {
        return Bridge::instance().call("device.list", Json{{"user_id", userId}});
    });
}

void App::addCamera(const DiscoveredDevice& device, const std::string& channel) {
    if (config_.findCamera(device.did, channel) != nullptr) {
        return;
    }

    CameraConfig camera;
    camera.did = device.did;
    camera.model = device.model;
    camera.name = device.name;
    camera.ip = device.ip;
    camera.channel = channel;
    camera.quality = "hd";
    camera.enabled = true;

    config_.cameras.push_back(camera);
    config_.save();

    XV_INFO("added {} to the grid", camera.label());

    auto stream = std::make_unique<CameraStream>();
    stream->config = camera;
    stream->worker = std::make_unique<StreamWorker>();
    stream->miot = std::make_unique<MiotClient>(config_.account, camera.did, camera.model);
    if (camera.enabled) {
        stream->worker->start(gpu_, camera, config_.account);
    }
    attachGlobalRecorder(*stream);
    streams_.push_back(std::move(stream));
}

void App::removeCamera(size_t index) {
    if (index >= streams_.size()) {
        return;
    }

    const std::string label = streams_[index]->config.label();

    if (streams_[index]->worker) {
        streams_[index]->worker->mute();
        streams_[index]->worker->stop();
    }

    const auto& removed = streams_[index]->config;
    std::erase_if(config_.cameras, [&](const CameraConfig& camera) {
        return camera.did == removed.did && camera.channel == removed.channel;
    });

    streams_.erase(streams_.begin() + static_cast<ptrdiff_t>(index));
    config_.save();

    if (selected_ >= static_cast<int>(streams_.size())) {
        selected_ = streams_.empty() ? -1 : static_cast<int>(streams_.size()) - 1;
    }

    XV_INFO("removed {} from the grid", label);
}

void App::startStreams() {
    // Rebuild the worker list from configuration so this doubles as the
    // "apply changes" path.
    stopStreams(true);
    streams_.clear();

    for (const auto& camera : config_.cameras) {
        auto stream = std::make_unique<CameraStream>();
        stream->config = camera;
        stream->worker = std::make_unique<StreamWorker>();
        stream->miot = std::make_unique<MiotClient>(config_.account, camera.did, camera.model);
        if (camera.enabled) {
            stream->worker->start(gpu_, camera, config_.account);
        }
        attachGlobalRecorder(*stream);
        streams_.push_back(std::move(stream));
    }

    if (selected_ < 0 && !streams_.empty()) {
        selected_ = 0;
    }
}

void App::stopStreams(bool preserveGlobalRecording) {
    if (!preserveGlobalRecording) {
        stopGlobalRecording();
    }
    muteAll();
    for (auto& stream : streams_) {
        if (stream->worker) {
            stream->worker->stop();
        }
        stream->texture.reset();
    }
}

void App::restartStream(CameraStream& stream) {
    if (stream.worker) {
        stream.worker->stop();
    }
    stream.texture.reset();
    if (stream.config.enabled) {
        stream.worker->start(gpu_, stream.config, config_.account);
    }
}

void App::toggleRecording(CameraStream& stream) {
    if (!stream.worker) {
        return;
    }

    if (stream.worker->recordingRequested()) {
        stream.worker->stopRecording();
        return;
    }

    stream.worker->startRecording(config_.recordingsDirectory());
}

bool App::recording(const CameraStream& stream) const {
    return stream.worker && stream.worker->recordingRequested();
}

void App::toggleGlobalRecording() {
    if (globalRecorder_.active()) {
        stopGlobalRecording();
        return;
    }

    detachGlobalRecorders();

    std::vector<CameraStream*> live;
    live.reserve(streams_.size());
    for (const auto& stream : streams_) {
        if (stream->worker && stream->worker->status().state == StreamState::Streaming) {
            live.push_back(stream.get());
        }
    }
    if (live.empty()) {
        XV_WARN("global recording was requested with no live cameras");
        return;
    }

    // One logical lens owns each physical camera's microphone. Prefer the
    // primary lens when it is in the snapshot, otherwise use the live lens.
    std::unordered_map<std::string, CameraStream*> audioOwners;
    for (CameraStream* stream : live) {
        if (!stream->config.audio) {
            continue;
        }
        const bool primary = stream->config.channel.empty() || stream->config.channel == "0";
        auto [found, inserted] = audioOwners.emplace(stream->config.did, stream);
        if (!inserted && primary) {
            found->second = stream;
        }
    }

    std::vector<GlobalRecorder::Participant> participants;
    participants.reserve(live.size());
    for (CameraStream* stream : live) {
        std::string physicalTitle = stream->config.name;
        if (physicalTitle.empty()) {
            physicalTitle = stream->config.model;
        }
        if (physicalTitle.empty()) {
            physicalTitle = stream->config.did;
        }

        participants.push_back(GlobalRecorder::Participant{
            .videoId = globalVideoId(stream->config),
            .sourceId = stream->config.did,
            .videoTitle = stream->config.label(),
            .audioTitle = physicalTitle,
            .audioOwner = audioOwners.contains(stream->config.did) &&
                          audioOwners.at(stream->config.did) == stream,
        });
    }

    std::string error;
    if (!globalRecorder_.start(config_.recordingsDirectory(), std::move(participants), error)) {
        XV_ERROR("cannot start global recording: {}", error);
        return;
    }

    for (CameraStream* stream : live) {
        attachGlobalRecorder(*stream);
    }
}

void App::stopGlobalRecording() {
    detachGlobalRecorders();
    globalRecorder_.stop();
}

bool App::globalRecordingAvailable() const {
    return std::any_of(streams_.begin(), streams_.end(), [](const auto& stream) {
        return stream->worker && stream->worker->status().state == StreamState::Streaming;
    });
}

bool App::globallyRecording(const CameraStream& stream) const {
    return globalRecorder_.participates(globalVideoId(stream.config));
}

void App::attachGlobalRecorder(CameraStream& stream) {
    if (!stream.worker) {
        return;
    }
    const std::string videoId = globalVideoId(stream.config);
    if (!globalRecorder_.participates(videoId)) {
        stream.worker->detachGlobalRecorder();
        return;
    }
    stream.worker->attachGlobalRecorder(&globalRecorder_, videoId,
                                        globalRecorder_.audioOwner(videoId));
}

void App::detachGlobalRecorders() {
    for (auto& stream : streams_) {
        if (stream->worker) {
            stream->worker->detachGlobalRecorder();
        }
    }
}

std::string App::globalVideoId(const CameraConfig& camera) {
    return camera.did + '\x1f' + camera.channel;
}

void App::toggleListening(CameraStream& stream) {
    if (!stream.worker) {
        return;
    }

    if (stream.worker->listening()) {
        stream.worker->mute();
        XV_INFO("stopped listening to {}", stream.config.label());
        return;
    }

    muteAll();

    // Started on first use rather than at launch, so a machine with no sound
    // card, or a user who never listens, never opens an audio device at all.
    std::string error;
    if (!audio_.start(error)) {
        XV_ERROR("{}", error);
        return;
    }

    // Whatever the previous camera left buffered belongs to the previous
    // camera, and playing it first would be heard as a stutter on switching.
    audio_.reset();

    stream.worker->listen(&audio_);
    XV_INFO("listening to {}", stream.config.label());
}

bool App::listening(const CameraStream& stream) const {
    return stream.worker && stream.worker->listening();
}

void App::muteAll() {
    for (auto& stream : streams_) {
        if (stream->worker) {
            stream->worker->mute();
        }
    }
}

void App::openRecordingsFolder() const {
    const auto directory = config_.recordingsDirectory();

    // Created on the way, because the interesting case is opening it before
    // anything has been recorded and finding an empty folder rather than an
    // error about a path that does not exist.
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);

    ::ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::openRecordingDialog() {
    // Ctrl+O reaches here from the SD card screen too, and that screen holds a
    // session open. Left running it would go on pulling video from a camera
    // nothing is showing.
    if (screen_ == Screen::SdCard) {
        closeSdPlayback(false);
    }

    const auto directory = config_.recordingsDirectory();
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);

    std::string error;
    const auto selected = chooseRecordingFile(window_, directory, error);
    if (!selected) {
        if (!error.empty()) {
            XV_ERROR("recording picker failed: {}", error);
            ::MessageBoxA(window_, error.c_str(), "Could not open recording",
                          MB_OK | MB_ICONERROR);
        }
        return;
    }

    bool recordingActive = globalRecorder_.active();
    for (const auto& stream : streams_) {
        recordingActive = recordingActive || (stream->worker && stream->worker->recordingRequested());
    }
    if (recordingActive &&
        ::MessageBoxW(window_,
                      L"Opening a recording stops the current recording and disconnects the live "
                      L"streams. Continue?",
                      L"Open recording", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    playback_.close();
    if (!playbackSuspendedLive_) {
        stopStreams();
        playbackSuspendedLive_ = true;
    }
    playbackSelected_ = 0;
    playbackFocused_ = false;
    playbackError_.clear();

    if (!playback_.open(*selected, gpu_, audio_, error)) {
        playbackError_ = std::move(error);
    }
    screen_ = Screen::Playback;
}

void App::closePlayback(bool resumeLive) {
    playback_.close();
    playbackError_.clear();
    playbackSelected_ = 0;
    playbackFocused_ = false;

    const bool shouldResume = playbackSuspendedLive_;
    playbackSuspendedLive_ = false;
    if (resumeLive && signedIn_ && shouldResume) {
        startStreams();
        screen_ = config_.cameras.empty() ? Screen::Cameras : Screen::Grid;
    } else if (resumeLive) {
        screen_ = signedIn_ ? (config_.cameras.empty() ? Screen::Cameras : Screen::Grid)
                            : Screen::Login;
    }
}

// leavePlayerScreens gives up whatever a player screen was holding before the
// menu moves somewhere else. Both of them suspended the live grid to get here,
// and both have to hand it back; the screen itself is chosen by the caller,
// which is why neither is asked to resume.
void App::leavePlayerScreens() {
    if (screen_ == Screen::Playback) {
        closePlayback(false);
    }
    if (screen_ == Screen::SdCard) {
        closeSdPlayback(false);
    }

    // Whichever it was, the live grid was stopped to make room for it.
    if (signedIn_ && streams_.empty() && !config_.cameras.empty()) {
        startStreams();
    }
}

bool App::sdPlaybackSupported(const CameraConfig& camera) {
    // The catalogue and the playback command were recovered from the firmware
    // of these two boards, and the request shapes are not guesses that would
    // degrade gracefully elsewhere: a model that reads them differently answers
    // nothing at all. Better to say a camera is not supported than to offer a
    // screen that will sit empty.
    //
    // Both of a CW500's lenses qualify: each keeps its own catalogue on the
    // card, the second under storage channel 10. See sdRecordingChannel.
    return camera.model.find("hlc8a") != std::string::npos ||
           camera.model.find("500dh") != std::string::npos;
}

void App::openSdPlayback(size_t cameraIndex) {
    if (cameraIndex >= config_.cameras.size()) {
        return;
    }

    const CameraConfig camera = config_.cameras[cameraIndex];

    if (!sdPlaybackSupported(camera)) {
        return;
    }

    if (globalRecorder_.active()) {
        const int answer = ::MessageBoxA(
            window_,
            "A recording is in progress. Opening a camera's card stops every live "
            "stream, which ends it. Continue?",
            "Recording in progress", MB_OKCANCEL | MB_ICONWARNING);
        if (answer != IDOK) {
            return;
        }
    }

    sdPlayer_.close();

    // One session per camera. The grid is streaming this camera already, and a
    // second connection would be arguing with the first over the same
    // peer-to-peer link.
    if (!sdSuspendedLive_) {
        stopStreams();
        sdSuspendedLive_ = true;
    }

    XV_INFO("opening the card on {}", camera.label());
    sdTexture_.reset();
    sdPlayer_.open(gpu_, camera, config_.account);
    screen_ = Screen::SdCard;
}

void App::closeSdPlayback(bool resumeLive) {
    sdPlayer_.mute();
    sdPlayer_.close();
    sdTexture_.reset();

    const bool shouldResume = sdSuspendedLive_;
    sdSuspendedLive_ = false;

    if (resumeLive && signedIn_ && shouldResume) {
        startStreams();
        screen_ = config_.cameras.empty() ? Screen::Cameras : Screen::Grid;
    } else if (resumeLive) {
        screen_ = signedIn_ ? (config_.cameras.empty() ? Screen::Cameras : Screen::Grid)
                            : Screen::Login;
    }
}

void App::toggleSdListening() {
    if (sdPlayer_.listening()) {
        sdPlayer_.mute();
        return;
    }

    // Only one thing may hold the speaker, so whatever had it gives it up.
    muteAll();
    sdPlayer_.listen(&audio_);
}

void App::loadSettingsFor(CameraStream& stream) {
    if (stream.miotTask.busy()) {
        stream.miotRefreshQueued = true;
        return;
    }

    if (!stream.miot) {
        stream.miot =
            std::make_unique<MiotClient>(config_.account, stream.config.did, stream.config.model);
    }

    stream.miotBusyLabel = "Reading settings";
    MiotClient* client = stream.miot.get();
    stream.miotTask.start([client]() -> std::string {
        std::string error;
        return client->refresh(error) ? std::string{} : error;
    });
}

void App::writeSetting(CameraStream& stream, const MiotProperty& property, const Json& value) {
    if (stream.miotTask.busy() || !stream.miot) {
        return;
    }

    stream.miotBusyLabel = std::format("Applying {}", property.label);
    stream.miotRefreshQueued = true;

    MiotClient* client = stream.miot.get();
    const MiotProperty target = property;
    stream.miotTask.start([client, target, value]() -> std::string {
        std::string error;
        return client->write(target, value, error) ? std::string{} : error;
    });
}

void App::invokeAction(CameraStream& stream, const MiotAction& action) {
    if (stream.miotTask.busy() || !stream.miot) {
        return;
    }

    stream.miotBusyLabel = std::format("Running {}", action.label);
    stream.miotRefreshQueued = true;

    MiotClient* client = stream.miot.get();
    const MiotAction target = action;
    stream.miotTask.start([client, target]() -> std::string {
        std::string error;
        return client->invoke(target, error) ? std::string{} : error;
    });
}

} // namespace xv
