#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace xv {

// One Mi account. The pass token is the sensitive part: it is a bearer
// credential for the whole account, so it is never written in the clear.
struct AccountConfig {
    std::string userId;
    std::string region; // "", "de", "i2", "ru", "sg" or "us"
    std::string token;

    [[nodiscard]] bool valid() const { return !userId.empty() && !token.empty(); }
};

// One camera tile. A dual-lens camera contributes two of these, sharing an IP
// but differing in did and channel.
struct CameraConfig {
    std::string did;
    std::string model;
    std::string name;
    std::string ip;
    std::string channel;   // "" or "0" for the primary lens
    std::string quality;   // "", "auto", "sd", "hd" or "0".."5"
    std::string transport; // "", "udp" or "tcp"
    bool enabled = true;
    // Whether to ask the camera for audio at all. On by default: an audio
    // stream is a few kilobytes a second next to the video's megabits, and
    // asking for it up front is what lets listening and recording be turned on
    // without renegotiating the session. Turning it off in config.json is the
    // escape hatch for a model that dislikes being asked.
    bool audio = true;

    // Display name that stays useful when a camera has no name set.
    [[nodiscard]] std::string label() const;

    // Whether this lens can be pointed. The motor belongs to one lens, so on a
    // dual-lens model only the primary one moves.
    [[nodiscard]] bool motorised() const;
};

// Models known to carry two independent lenses. Each shows up once in the device
// list but streams two channels, so the picker offers both.
[[nodiscard]] bool isDualLens(const std::string& model);

// Models with a pan and tilt motor. Anything not listed is assumed fixed, so a
// new motorised model shows no pad rather than a pad that does nothing.
[[nodiscard]] bool hasMotor(const std::string& model);

// Where the window was when the app last closed, so it opens back in the same
// place. The rectangle is the restore position, which a maximized window has as
// well: it is where the window goes when it is un-maximized.
//
// These are handed back to SetWindowPlacement in workspace coordinates. They
// normally come from GetWindowPlacement; a visible, non-maximized window uses
// GetWindowRect instead so Windows Snap's current bounds are kept, then converts
// those screen coordinates to the same workspace coordinate system.
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;

    // Nothing has been saved yet if there is no size to restore.
    [[nodiscard]] bool valid() const { return width > 0 && height > 0; }
};

enum class GridLayout {
    One = 0,
    TwoByTwo,
    ThreeByThree,
    Auto,
};

struct AppConfig {
    AccountConfig account;
    std::vector<CameraConfig> cameras;
    GridLayout layout = GridLayout::Auto;
    WindowPlacement window;
    float uiScale = 1.0f;
    // Magnification used while the right mouse button is held over live video.
    // It is intentionally config-file-only for now; 1.0 disables magnification.
    float liveViewZoom = 2.0f;

    // Where recordings are written. Empty means the default, which is a folder
    // in the user's Videos library: recordings are large and belong where the
    // rest of the machine's video lives, not in an application data directory.
    std::string recordingsDir;

    [[nodiscard]] std::filesystem::path recordingsDirectory() const;

    // Loads from %APPDATA%\XiaomiViewer\config.json, returning defaults if it
    // is absent or unreadable.
    static AppConfig load();
    bool save() const;

    static std::filesystem::path directory();
    static std::filesystem::path configPath();
    static std::filesystem::path logPath();

    [[nodiscard]] CameraConfig* findCamera(const std::string& did, const std::string& channel);
};

} // namespace xv
