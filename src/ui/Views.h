#pragma once

#include <string>

namespace xv {

class App;
struct CameraStream;

// Sign-in, including the captcha and two-step verification detours.
void drawLoginView(App& app);

// Device discovery: what the account can see, and what is already on the grid.
void drawCamerasView(App& app);

// The live video grid.
void drawGridView(App& app);

// Pan/tilt pad and presets, drawn over the focused tile.
void drawPtzOverlay(App& app, CameraStream& stream, float x, float y, float width, float height);

// Camera settings, driven by the MIoT property table.
void drawSettingsView(App& app);

// The pages of the help window, named so the Help menu can ask for one of them
// by name rather than by an index that would drift as pages are added.
inline constexpr const char* kHelpTabStart = "Getting started";
inline constexpr const char* kHelpTabControls = "Controls";
inline constexpr const char* kHelpTabRecording = "Recording and sound";
inline constexpr const char* kHelpTabCameras = "Cameras";
inline constexpr const char* kHelpTabTrouble = "Troubleshooting";
inline constexpr const char* kHelpTabAbout = "About";

// Instructions, including what every key and mouse button does. `open` is the
// window's own state, cleared when it is closed. `tab` names the page to bring
// to the front, and is cleared once that has happened; null leaves the window on
// whichever page was last read.
void drawHelpWindow(App& app, bool& open, const char*& tab);

// Shared helpers.
namespace ui {

// Draws centred, dimmed text inside the current region, for placeholder states.
void centeredNote(const char* text, float regionWidth, float regionHeight);

// Renders a coloured status pill such as "Live" or "Reconnecting".
void statusBadge(const char* label, int state);

// Formats a byte count as B/KB/MB/GB.
std::string humanBytes(unsigned long long bytes);

} // namespace ui

} // namespace xv
