#include "ui/Views.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "app/App.h"
#include "app/Theme.h"

namespace xv {

namespace ui {

void centeredNote(const char* text, float regionWidth, float regionHeight) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 origin = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(origin.x + (regionWidth - size.x) * 0.5f,
                               origin.y + (regionHeight - size.y) * 0.5f));
    ImGui::TextColored(theme::kMuted, "%s", text);
}

void statusBadge(const char* label, int state) {
    ImVec4 color = theme::kMuted;
    switch (static_cast<StreamState>(state)) {
    case StreamState::Streaming: color = theme::kLive; break;
    case StreamState::Connecting:
    case StreamState::Reconnecting: color = theme::kPending; break;
    case StreamState::Failed: color = theme::kFailed; break;
    case StreamState::Idle: break;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float radius = ImGui::GetTextLineHeight() * 0.28f;
    const float centerY = position.y + ImGui::GetTextLineHeight() * 0.5f;

    draw->AddCircleFilled(ImVec2(position.x + radius, centerY), radius, ImGui::ColorConvertFloat4ToU32(color));

    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(color, "%s", label);
}

std::string humanBytes(unsigned long long bytes) {
    constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} {}", bytes, kUnits[unit])
                     : std::format("{:.1f} {}", value, kUnits[unit]);
}

} // namespace ui

namespace {

struct LiveViewZoomState {
    int tile = -1;
    ImVec2 center = ImVec2(0.5f, 0.5f);
};

LiveViewZoomState& liveViewZoomState() {
    static LiveViewZoomState state;
    return state;
}

void useMipSampler(const ImDrawList*, const ImDrawCmd* command) {
    auto* renderState =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    auto* sampler = static_cast<ID3D11SamplerState*>(command->UserCallbackData);
    if (renderState != nullptr && sampler != nullptr) {
        renderState->DeviceContext->PSSetSamplers(0, 1, &sampler);
    }
}

// Chooses a tile grid. Auto keeps tiles as close to square as it can, which is
// what suits 16:9 video in a landscape window.
void gridDimensions(GridLayout layout, size_t count, int& columns, int& rows) {
    switch (layout) {
    case GridLayout::One:
        columns = rows = 1;
        return;
    case GridLayout::TwoByTwo:
        columns = rows = 2;
        return;
    case GridLayout::ThreeByThree:
        columns = rows = 3;
        return;
    case GridLayout::Auto:
        break;
    }

    if (count <= 1) {
        columns = rows = 1;
        return;
    }
    columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    rows = static_cast<int>(std::ceil(static_cast<double>(count) / columns));
}

// Fits `contentWidth` x `contentHeight` inside the box while preserving aspect,
// returning the drawn size and the offset that centres it.
void letterbox(float boxWidth, float boxHeight, float contentWidth, float contentHeight,
               ImVec2& size, ImVec2& offset) {
    if (contentWidth <= 0.0f || contentHeight <= 0.0f) {
        size = ImVec2(boxWidth, boxHeight);
        offset = ImVec2(0, 0);
        return;
    }

    const float scale = std::min(boxWidth / contentWidth, boxHeight / contentHeight);
    size = ImVec2(contentWidth * scale, contentHeight * scale);
    offset = ImVec2((boxWidth - size.x) * 0.5f, (boxHeight - size.y) * 0.5f);
}

void drawTile(App& app, size_t index, CameraStream& stream, ImVec2 size, bool focused) {
    ImGui::PushID(static_cast<int>(index));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kTileBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, focused ? 2.0f : 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, focused ? theme::kAccent
                                                   : ImGui::GetStyleColorVec4(ImGuiCol_Border));

    if (ImGui::BeginChild("##tile", size, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 tileOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();

        // Pull whatever the decoder produced since the last frame.
        if (stream.worker) {
            stream.worker->present(app.gpu(), stream.texture);
        }

        const StreamWorker::Status status =
            stream.worker ? stream.worker->status() : StreamWorker::Status{};

        constexpr float kFooterHeight = 26.0f;
        const float videoHeight = std::max(available.y - kFooterHeight, 1.0f);
        bool zoomActive = false;

        if (stream.texture.ready()) {
            ImVec2 drawSize;
            ImVec2 offset;
            letterbox(available.x, videoHeight, static_cast<float>(stream.texture.width()),
                      static_cast<float>(stream.texture.height()), drawSize, offset);

            const ImVec2 imageOrigin(tileOrigin.x + offset.x, tileOrigin.y + offset.y);
            const ImVec2 imageEnd(imageOrigin.x + drawSize.x, imageOrigin.y + drawSize.y);
            const bool imageHovered = ImGui::IsMouseHoveringRect(imageOrigin, imageEnd);
            ImGuiIO& io = ImGui::GetIO();
            LiveViewZoomState& zoom = liveViewZoomState();

            const float factor = app.config().liveViewZoom;
            const float visibleSpan = 1.0f / factor;
            const float halfSpan = visibleSpan * 0.5f;

            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                app.setSelected(static_cast<int>(index));

                if (factor > 1.0f) {
                    zoom.tile = static_cast<int>(index);

                    // Keep the point under the cursor in the same place when the
                    // magnification starts, except where an image edge prevents it.
                    const float pointerX =
                        std::clamp((io.MousePos.x - imageOrigin.x) / drawSize.x, 0.0f, 1.0f);
                    const float pointerY =
                        std::clamp((io.MousePos.y - imageOrigin.y) / drawSize.y, 0.0f, 1.0f);
                    zoom.center.x = pointerX + (0.5f - pointerX) * visibleSpan;
                    zoom.center.y = pointerY + (0.5f - pointerY) * visibleSpan;
                }
            }

            zoomActive = zoom.tile == static_cast<int>(index) &&
                         ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (zoomActive) {
                // Dragging moves the picture with the mouse. Scaling by the UV
                // span makes the movement feel the same at every zoom factor.
                if (!ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    zoom.center.x -= io.MouseDelta.x * visibleSpan / drawSize.x;
                    zoom.center.y -= io.MouseDelta.y * visibleSpan / drawSize.y;
                }
                zoom.center.x = std::clamp(zoom.center.x, halfSpan, 1.0f - halfSpan);
                zoom.center.y = std::clamp(zoom.center.y, halfSpan, 1.0f - halfSpan);
            }

            const ImVec2 uv0 = zoomActive
                                   ? ImVec2(zoom.center.x - halfSpan, zoom.center.y - halfSpan)
                                   : ImVec2(0.0f, 0.0f);
            const ImVec2 uv1 = zoomActive
                                   ? ImVec2(zoom.center.x + halfSpan, zoom.center.y + halfSpan)
                                   : ImVec2(1.0f, 1.0f);

            ImGui::SetCursorScreenPos(imageOrigin);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddCallback(useMipSampler, app.gpu().mipSampler());
            ImGui::Image(reinterpret_cast<ImTextureID>(stream.texture.view()), drawSize, uv0, uv1);
            drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        } else {
            const char* note = !stream.config.enabled ? "Disabled"
                               : status.state == StreamState::Failed
                                   ? "Not connected"
                                   : "Connecting...";
            ImGui::Dummy(ImVec2(available.x, 0));
            ui::centeredNote(note, available.x, videoHeight);
        }

        // The whole video area is a click target for focusing this tile, and a
        // double click toggles fullscreen the way a video player would.
        ImGui::SetCursorScreenPos(tileOrigin);
        ImGui::InvisibleButton("##focus", ImVec2(available.x, videoHeight),
                               ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(zoomActive ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked()) {
            app.setSelected(static_cast<int>(index));
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            app.setSelected(static_cast<int>(index));
            app.setFullscreenTile(!app.fullscreenTile());
        }

        // Footer: name on the left, live state and resolution on the right.
        ImGui::SetCursorScreenPos(ImVec2(tileOrigin.x + 8.0f, tileOrigin.y + videoHeight + 4.0f));
        ImGui::BeginGroup();
        ui::statusBadge(stream.config.label().c_str(), static_cast<int>(status.state));
        ImGui::EndGroup();

        std::string detail;
        if (status.state == StreamState::Streaming && status.width > 0) {
            detail = std::format("{}x{}  {}", status.width, status.height,
                                 ui::humanBytes(status.bytesReceived));
        } else {
            detail = status.message;
        }

        // A recording has to be visible on a tile nobody is looking at, because
        // that is exactly when it is forgotten about. The same goes for which
        // camera the sound is coming from, which is otherwise a guess.
        std::string recording;
        if (status.recording) {
            const auto seconds = status.recordedMs / 1000;
            recording = std::format("REC {}:{:02}", seconds / 60, seconds % 60);
        }

        const char* audible = status.audible ? "AUDIO" : "";

        if (!detail.empty() || !recording.empty() || status.audible) {
            constexpr float kGap = 10.0f;
            const float detailWidth = detail.empty() ? 0.0f : ImGui::CalcTextSize(detail.c_str()).x;
            const float recordingWidth =
                recording.empty() ? 0.0f : ImGui::CalcTextSize(recording.c_str()).x + kGap;
            const float audibleWidth =
                status.audible ? ImGui::CalcTextSize(audible).x + kGap : 0.0f;

            ImGui::SetCursorScreenPos(ImVec2(tileOrigin.x + available.x - detailWidth -
                                                 recordingWidth - audibleWidth - 8.0f,
                                             tileOrigin.y + videoHeight + 4.0f));

            if (status.audible) {
                ImGui::TextColored(theme::kAccent, "%s", audible);
                ImGui::SetItemTooltip("This is the camera you are hearing");
                if (!recording.empty() || !detail.empty()) {
                    ImGui::SameLine(0.0f, kGap);
                }
            }
            if (!recording.empty()) {
                ImGui::TextColored(theme::kFailed, "%s", recording.c_str());
                if (!detail.empty()) {
                    ImGui::SameLine(0.0f, kGap);
                }
            }
            if (!detail.empty()) {
                ImGui::TextColored(theme::kMuted, "%s", detail.c_str());
            }
        }

        // The pad belongs to the focused tile only, so it does not clutter the
        // grid or leave ambiguity about which camera it would act on. Every
        // camera gets one: a lens with no motor behind it still has an alarm, a
        // light and a recording to start, and the pad leaves out the arrows.
        if (focused && stream.texture.ready()) {
            drawPtzOverlay(app, stream, tileOrigin.x, tileOrigin.y, available.x, videoHeight);
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// Keys the grid answers to.
//
// `reachable` is how many tiles Tab can land on, which is what is on screen: in a
// fixed layout too small for the camera list the ones that do not fit are not
// drawn, and selecting a camera nobody can see would leave the pad pointing at
// nothing. In the focused view every camera is reachable, because the selected
// one is the one being shown.
void handleKeys(App& app, size_t reachable) {
    ImGuiIO& io = ImGui::GetIO();

    // A field collecting keystrokes, or an open dropdown, is a better claim on
    // these keys than the camera is.
    if (io.WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }

    auto& streams = app.streams();
    const int selected = app.selected();

    if (app.fullscreenTile() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        app.setFullscreenTile(false);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        app.setFullscreenTile(!app.fullscreenTile());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        app.toggleRecording(*streams[static_cast<size_t>(selected)]);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_A)) {
        app.toggleListening(*streams[static_cast<size_t>(selected)]);
    }

    // Tab moves to the next camera, shift-Tab to the previous one, both wrapping
    // around. Nothing to move to is not worth acting on.
    if (ImGui::IsKeyPressed(ImGuiKey_Tab) && reachable > 1) {
        const int count = static_cast<int>(reachable);
        int next = selected + (io.KeyShift ? -1 : 1);
        if (next < 0) {
            next = count - 1;
        } else if (next >= count) {
            next = 0;
        }
        app.setSelected(next);
    }

    // The arrow keys point the selected camera, the same hold-to-move way the
    // pad's arrows do: the worker repeats a step for as long as the hold is
    // renewed, which is every frame a key is down.
    //
    // Which camera is being turned is remembered by index rather than by pointer,
    // because reconnecting rebuilds the tiles and a pointer taken before that
    // would outlive its stream.
    static int turning = -1;

    const char* direction = nullptr;
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
        direction = "up";
    } else if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
        direction = "down";
    } else if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
        direction = "left";
    } else if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
        direction = "right";
    }

    const int wanted = app.selected(); // Tab may have just moved it
    const bool movable = wanted >= 0 && wanted < static_cast<int>(streams.size()) &&
                         streams[static_cast<size_t>(wanted)]->config.motorised();

    // Let go of the camera that was turning as soon as the keys stop asking for
    // it, or the moment Tab moves on to another one.
    if (turning >= 0 && (direction == nullptr || turning != wanted)) {
        if (turning < static_cast<int>(streams.size()) && streams[static_cast<size_t>(turning)]->worker) {
            streams[static_cast<size_t>(turning)]->worker->releasePtz();
        }
        turning = -1;
    }

    if (direction != nullptr && movable) {
        if (StreamWorker* worker = streams[static_cast<size_t>(wanted)]->worker.get()) {
            worker->holdPtz(direction);
            turning = wanted;
        }
    }
}

void drawToolbar(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    if (ImGui::BeginChild("##toolbar", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
        ImGui::TextColored(theme::kMuted, "Layout");
        ImGui::SameLine();

        constexpr const char* kLayoutNames[] = {"Single", "2 x 2", "3 x 3", "Auto"};
        int layout = static_cast<int>(app.config().layout);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("##layout", &layout, kLayoutNames, IM_ARRAYSIZE(kLayoutNames))) {
            app.config().layout = static_cast<GridLayout>(layout);
            app.config().save();
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();

        bool fullscreen = app.fullscreenTile();
        if (ImGui::Checkbox("Focus one tile", &fullscreen)) {
            app.setFullscreenTile(fullscreen);
        }
        ImGui::SetItemTooltip("Show only the selected camera. Double-clicking a tile does the same.");

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();

        if (ImGui::Button("Reconnect all")) {
            app.startStreams();
        }

        ImGui::SameLine();
        if (ImGui::Button("Manage cameras")) {
            app.setScreen(Screen::Cameras);
        }

        ImGui::SameLine();
        if (ImGui::Button("Camera settings")) {
            app.setScreen(Screen::Settings);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace

void drawGridView(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##gridroot", ImVec2(0, 0), ImGuiChildFlags_None);

    // A fresh press starts with no owner; the tile under it may claim the zoom
    // below. This also clears stale state after leaving and returning to the grid.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        liveViewZoomState().tile = -1;
    }

    drawToolbar(app);
    ImGui::Dummy(ImVec2(0, 6));

    auto& streams = app.streams();

    if (streams.empty()) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ui::centeredNote("No cameras yet. Use Manage cameras to add one.", available.x, available.y);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    if (int selected = app.selected();
        selected < 0 || selected >= static_cast<int>(streams.size())) {
        app.setSelected(0);
    }

    int columns = 1;
    int rows = 1;
    gridDimensions(app.config().layout, streams.size(), columns, rows);

    // With a fixed layout the grid may not hold everything; paging would be
    // overkill, so extra cameras simply fall outside the chosen layout and the
    // user picks a larger one.
    const size_t visible = std::min(streams.size(), static_cast<size_t>(columns) * rows);

    // Before anything is drawn, so a camera picked with Tab is the one this frame
    // highlights rather than the next one.
    handleKeys(app, app.fullscreenTile() ? streams.size() : visible);

    const int selected = app.selected();
    const ImVec2 available = ImGui::GetContentRegionAvail();

    if (app.fullscreenTile()) {
        drawTile(app, static_cast<size_t>(selected), *streams[static_cast<size_t>(selected)],
                 available, true);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float tileWidth = (available.x - spacing * (columns - 1)) / columns;
    const float tileHeight = (available.y - spacing * (rows - 1)) / rows;

    for (size_t i = 0; i < visible; ++i) {
        if (i % static_cast<size_t>(columns) != 0) {
            ImGui::SameLine();
        }
        drawTile(app, i, *streams[i], ImVec2(tileWidth, tileHeight),
                 static_cast<int>(i) == selected);
    }

    if (visible < streams.size()) {
        ImGui::TextColored(theme::kPending, "%zu more camera(s) do not fit this layout.",
                           streams.size() - visible);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace xv
