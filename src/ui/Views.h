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
