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

#include "app/Frameless.h"
#include "app/Log.h"
#include "app/SingleInstance.h"
#include "app/Theme.h"
#include "bridge/Bridge.h"
#include "ui/Views.h"
#include "util/Encoding.h"

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
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);

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

    // rcNormalPosition is the restore rectangle, so this reads the same whether
    // the window is maximized, minimized or neither.
    const RECT& rect = placement.rcNormalPosition;
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
    if (screen_ == Screen::Grid) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

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

    ImGui::Render();

    constexpr float kClear[4] = {0.06f, 0.07f, 0.08f, 1.0f};
    gpu_.beginFrame(kClear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    gpu_.present(true);
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
                screen_ = Screen::Grid;
            }
            if (ImGui::MenuItem("Cameras", nullptr, screen_ == Screen::Cameras)) {
                screen_ = Screen::Cameras;
            }
            if (ImGui::MenuItem("Settings", nullptr, screen_ == Screen::Settings)) {
                screen_ = Screen::Settings;
            }
            ImGui::Separator();
            ImGui::MenuItem("Log", nullptr, &showLogWindow_);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Streams")) {
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
        const std::string summary = std::format("{} of {} live", live, streams_.size());
        const float width = ImGui::CalcTextSize(summary.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - captionWidth - width - 16.0f);
        ImGui::TextColored(live > 0 ? theme::kLive : theme::kMuted, "%s", summary.c_str());
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
            screen_ = config_.cameras.empty() ? Screen::Cameras : Screen::Grid;
            XV_INFO("restored the saved session for {}", config_.account.userId);
            if (config_.cameras.empty()) {
                refreshDevices();
            } else {
                startStreams();
            }
        } else {
            XV_WARN("saved session could not be restored: {}", responseError(*result));
            login_.error = "Your saved sign-in has expired. Please sign in again.";
            screen_ = Screen::Login;
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

    screen_ = config_.cameras.empty() ? Screen::Cameras : Screen::Grid;
    if (config_.cameras.empty()) {
        refreshDevices();
    } else {
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
    stream->miot = std::make_unique<MiotClient>(config_.account, camera.did);
    if (camera.enabled) {
        stream->worker->start(gpu_, camera, config_.account);
    }
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
    stopStreams();
    streams_.clear();

    for (const auto& camera : config_.cameras) {
        auto stream = std::make_unique<CameraStream>();
        stream->config = camera;
        stream->worker = std::make_unique<StreamWorker>();
        stream->miot = std::make_unique<MiotClient>(config_.account, camera.did);
        if (camera.enabled) {
            stream->worker->start(gpu_, camera, config_.account);
        }
        streams_.push_back(std::move(stream));
    }

    if (selected_ < 0 && !streams_.empty()) {
        selected_ = 0;
    }
}

void App::stopStreams() {
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

void App::loadSettingsFor(CameraStream& stream) {
    if (stream.miotTask.busy()) {
        stream.miotRefreshQueued = true;
        return;
    }

    if (!stream.miot) {
        stream.miot = std::make_unique<MiotClient>(config_.account, stream.config.did);
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
