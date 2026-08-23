#pragma once

#include <windows.h>

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/Frameless.h"
#include "cloud/Miot.h"
#include "config/Config.h"
#include "media/AudioPlayer.h"
#include "media/GlobalRecorder.h"
#include "media/StreamWorker.h"
#include "render/D3D11Context.h"
#include "render/VideoFrameTexture.h"

namespace xv {

// AsyncTask keeps a blocking bridge call off the UI thread.
//
// Every cloud round trip can take seconds, and a login behind a captcha can take
// much longer, so nothing that touches the network may run inside a frame.
template <typename T>
class AsyncTask {
public:
    void start(std::function<T()> work) {
        future_ = std::async(std::launch::async, std::move(work));
        running_ = true;
    }

    // Returns the result exactly once, on the frame it becomes available.
    std::optional<T> poll() {
        if (!running_ || !future_.valid()) {
            return std::nullopt;
        }
        if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return std::nullopt;
        }
        running_ = false;
        return future_.get();
    }

    [[nodiscard]] bool busy() const { return running_; }

private:
    std::future<T> future_;
    bool running_ = false;
};

// One camera tile: its configuration, the worker feeding it, the texture it
// draws from, and its settings client.
struct CameraStream {
    CameraConfig config;
    std::unique_ptr<StreamWorker> worker;
    VideoFrameTexture texture;
    std::unique_ptr<MiotClient> miot;

    // Every MIoT operation is a cloud round trip, so they run off the UI thread
    // and one at a time per camera. The task yields an error message, empty on
    // success. While it is busy the settings controls are disabled, which also
    // guarantees the UI is not reading values the task is writing.
    AsyncTask<std::string> miotTask;
    bool miotLoaded = false;
    bool miotRefreshQueued = false;
    std::string miotError;
    std::string miotBusyLabel;
};

enum class Screen {
    Login,
    Cameras,
    Grid,
    Settings,
};

// A device discovered on the account, before it becomes a configured camera.
struct DiscoveredDevice {
    std::string did;
    std::string name;
    std::string model;
    std::string ip;
    std::string mac;
};

class App {
public:
    int run(HINSTANCE instance, int showCommand);

    // --- State shared with the views ---
    AppConfig& config() { return config_; }
    D3D11Context& gpu() { return gpu_; }
    std::vector<std::unique_ptr<CameraStream>>& streams() { return streams_; }
    std::vector<DiscoveredDevice>& devices() { return devices_; }

    Screen screen() const { return screen_; }
    void setScreen(Screen screen) { screen_ = screen; }

    [[nodiscard]] bool signedIn() const { return signedIn_; }

    // --- Actions the views trigger ---
    void beginLogin(const std::string& username, const std::string& password, const std::string& region);
    void submitCaptcha(const std::string& code);
    void submitVerify(const std::string& ticket);
    void signOut();

    void refreshDevices();
    [[nodiscard]] bool devicesBusy() const { return deviceTask_.busy(); }
    [[nodiscard]] const std::string& deviceError() const { return deviceError_; }

    // Adds a camera (or one lens of a dual-lens camera) to the grid.
    void addCamera(const DiscoveredDevice& device, const std::string& channel);
    void removeCamera(size_t index);

    void startStreams();
    void stopStreams(bool preserveGlobalRecording = false);
    void restartStream(CameraStream& stream);

    // Records the camera's own stream to a Matroska file, or stops doing so.
    // Nothing is re-encoded, so this costs almost nothing and the file is exactly
    // what the camera sent.
    void toggleRecording(CameraStream& stream);
    [[nodiscard]] bool recording(const CameraStream& stream) const;
    void toggleGlobalRecording();
    void stopGlobalRecording();
    [[nodiscard]] GlobalRecorder::Status globalRecordingStatus() const {
        return globalRecorder_.status();
    }
    [[nodiscard]] bool globalRecordingActive() const { return globalRecorder_.active(); }
    [[nodiscard]] bool globalRecordingAvailable() const;
    [[nodiscard]] bool globallyRecording(const CameraStream& stream) const;
    void openRecordingsFolder() const;

    // Plays this camera through the speakers, and stops any other camera that
    // was playing. One at a time is the whole design: several cameras at once
    // is noise nobody can pick a sound out of.
    void toggleListening(CameraStream& stream);
    [[nodiscard]] bool listening(const CameraStream& stream) const;
    void muteAll();
    // Why nothing can be heard, empty when there is nothing wrong. Only ever
    // about the output device; a camera that sends no audio is not an error.
    [[nodiscard]] std::string audioError() const { return audio_.error(); }

    // MIoT operations, all asynchronous. A write is followed by a refresh so
    // the panel shows what the camera actually accepted rather than what was
    // asked for.
    void loadSettingsFor(CameraStream& stream);
    void writeSetting(CameraStream& stream, const MiotProperty& property, const Json& value);
    void invokeAction(CameraStream& stream, const MiotAction& action);

    // Login conversation state, owned here and rendered by LoginView.
    struct LoginState {
        std::string username;
        std::string password;
        std::string region;
        std::string status;
        std::string error;
        std::string verifyTarget;
        std::string captchaCode;
        std::string verifyTicket;
        bool needCaptcha = false;
        bool needVerify = false;
        bool busy = false;
    };
    LoginState& login() { return login_; }
    [[nodiscard]] ID3D11ShaderResourceView* captchaTexture() const { return captchaView_.Get(); }

    // Tile the user is currently focused on, or -1 for none. Drives which
    // camera the PTZ pad and settings panel act on.
    int selected() const { return selected_; }
    void setSelected(int index) { selected_ = index; }

    bool fullscreenTile() const { return fullscreenTile_; }
    void setFullscreenTile(bool value) { fullscreenTile_ = value; }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool createWindow(HINSTANCE instance, int showCommand, std::string& error);

    // Puts the window back where it was left, and reports the ShowWindow command
    // to open it with. Falls back to the one the shell asked for if there is
    // nothing saved or the saved position is no longer on a monitor.
    int restorePlacement(int showCommand);
    void rememberPlacement();

    bool setupImGui();
    void teardownImGui();

    void frame();

    // The shortcuts that belong to the whole app rather than to one screen.
    // ImGui draws a menu item's shortcut as a label and does nothing else with
    // it, so the keys the menus advertise are answered here.
    void handleGlobalKeys();

    void drawMenuBar();

    // The caption the app draws for itself: the three buttons, and the strip
    // beside them that behaves like a title bar. Laid out while the menu bar is
    // drawn and read back when Windows asks where a point landed.
    void drawCaptionButtons(float dragFrom);
    void pumpTasks();

    void applyLoginResult(const Json& response);
    void setCaptcha(const std::string& base64Png);
    void restoreSession();
    void attachGlobalRecorder(CameraStream& stream);
    void detachGlobalRecorders();
    [[nodiscard]] static std::string globalVideoId(const CameraConfig& camera);

    HWND window_ = nullptr;
    D3D11Context gpu_;
    AppConfig config_;

    Screen screen_ = Screen::Login;
    bool running_ = true;
    bool occluded_ = false;
    UINT pendingWidth_ = 0;
    UINT pendingHeight_ = 0;

    frameless::CaptionLayout caption_;
    bool maximizeHot_ = false;
    bool maximizePressed_ = false;

    bool signedIn_ = false;
    LoginState login_;
    ComPtr<ID3D11ShaderResourceView> captchaView_;
    ComPtr<ID3D11Texture2D> captchaTexture2D_;

    AsyncTask<Json> loginTask_;
    AsyncTask<Json> deviceTask_;
    AsyncTask<Json> restoreTask_;
    std::string deviceError_;

    // Declared before the streams so it outlives them: a worker feeding it must
    // be gone before the player it feeds is.
    AudioPlayer audio_;
    GlobalRecorder globalRecorder_;

    std::vector<std::unique_ptr<CameraStream>> streams_;
    std::vector<DiscoveredDevice> devices_;

    int selected_ = -1;
    bool fullscreenTile_ = false;
    bool showLogWindow_ = false;
    bool showHelpWindow_ = false;
    // The help page the menu asked for, or null to leave the window where it was.
    const char* helpTab_ = nullptr;
};

} // namespace xv
