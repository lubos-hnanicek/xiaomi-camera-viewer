#include "ui/Views.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>

#include "app/App.h"
#include "app/Theme.h"

namespace xv {
namespace {

struct PlaybackZoomState {
    int tile = -1;
    ImVec2 center = ImVec2(0.5f, 0.5f);
};

PlaybackZoomState& playbackZoomState() {
    static PlaybackZoomState state;
    return state;
}

struct PlaybackSeekState {
    bool dragging = false;
    int64_t previewMs = 0;
};

PlaybackSeekState& playbackSeekState() {
    static PlaybackSeekState state;
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

void gridDimensions(size_t count, int& columns, int& rows) {
    if (count <= 1) {
        columns = rows = 1;
        return;
    }
    columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    rows = static_cast<int>(std::ceil(static_cast<double>(count) / columns));
}

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

std::string playbackTime(int64_t milliseconds) {
    const int64_t seconds = std::max<int64_t>(0, milliseconds) / 1000;
    if (seconds >= 3600) {
        return std::format("{}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60,
                           seconds % 60);
    }
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

std::string localRecordingTime(int64_t utcMilliseconds) {
    const auto time = std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds{utcMilliseconds})};
    try {
        return std::format(
            "{:%Y-%m-%d %H:%M:%S %Z}",
            std::chrono::zoned_time{std::chrono::current_zone(), time});
    } catch (const std::exception&) {
        return std::format("{:%Y-%m-%d %H:%M:%S} UTC",
                           std::chrono::floor<std::chrono::seconds>(time));
    }
}

void handleKeys(App& app, size_t trackCount) {
    if (trackCount == 0 || ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen(nullptr,
                           ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }

    RecordingPlayer& player = app.playback();
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        player.toggle();
    }
    if (app.playbackFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        app.setPlaybackFocused(false);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        app.setPlaybackFocused(!app.playbackFocused());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && !playbackSeekState().dragging) {
        const auto status = player.status();
        player.seek(status.positionMs - 5000);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && !playbackSeekState().dragging) {
        const auto status = player.status();
        player.seek(status.positionMs + 5000);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && trackCount > 1) {
        const int count = static_cast<int>(trackCount);
        int next = app.playbackSelected() + (ImGui::GetIO().KeyShift ? -1 : 1);
        if (next < 0) {
            next = count - 1;
        } else if (next >= count) {
            next = 0;
        }
        app.setPlaybackSelected(next);
    }
}

bool drawToolbar(App& app) {
    RecordingPlayer& player = app.playback();
    const RecordingPlayer::Status status = player.status();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    bool leave = false;
    if (ImGui::BeginChild("##playback-toolbar", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
        if (ImGui::Button("Open...")) {
            app.openRecordingDialog();
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(!status.open || status.failed);
        if (ImGui::Button(status.playing ? "Pause" : "Play")) {
            player.toggle();
        }
        ImGui::SameLine();

        PlaybackSeekState& seeking = playbackSeekState();
        if (!status.open) {
            seeking = {};
        }
        const int64_t shownMs = seeking.dragging ? seeking.previewMs : status.positionMs;
        ImGui::Text("%s / %s", playbackTime(shownMs).c_str(),
                    playbackTime(status.durationMs).c_str());
        ImGui::SameLine();

        int64_t position = shownMs;
        constexpr int64_t zero = 0;
        const int64_t duration = std::max<int64_t>(status.durationMs, 1);
        ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x - 130.0f));
        if (ImGui::SliderScalar("##playback-position", ImGuiDataType_S64, &position, &zero,
                                &duration, "", ImGuiSliderFlags_NoInput)) {
            seeking.dragging = true;
            seeking.previewMs = position;
        }
        if (seeking.dragging && ImGui::IsItemDeactivatedAfterEdit()) {
            seeking.dragging = false;
            player.seek(position);
        }
        ImGui::SetItemTooltip("Seek to the previous keyframe on release (Left/Right: 5 seconds)");
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Back to live")) {
            leave = true;
        }

        bool focused = app.playbackFocused();
        if (ImGui::Checkbox("Focus one track", &focused)) {
            app.setPlaybackFocused(focused);
        }
        ImGui::SetItemTooltip("Double-clicking a video does the same (F)");

        ImGui::SameLine();
        ImGui::TextColored(theme::kMuted, "Audio");
        ImGui::SameLine();

        const auto& audios = player.audios();
        const char* preview = "Mute";
        if (status.selectedAudio >= 0 &&
            status.selectedAudio < static_cast<int>(audios.size())) {
            preview = audios[static_cast<size_t>(status.selectedAudio)].title.c_str();
        }
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::BeginCombo("##playback-audio", preview)) {
            if (ImGui::Selectable("Mute", status.selectedAudio < 0)) {
                player.setAudioTrack(-1);
            }
            for (size_t i = 0; i < audios.size(); ++i) {
                const bool selected = status.selectedAudio == static_cast<int>(i);
                if (ImGui::Selectable(audios[i].title.c_str(), selected)) {
                    player.setAudioTrack(static_cast<int>(i));
                }
            }
            ImGui::EndCombo();
        }

        if (!status.fileName.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(theme::kMuted, "%s", status.fileName.c_str());
        }

        if (status.recordingStartUtcMs >= 0) {
            const std::string localTime =
                localRecordingTime(status.recordingStartUtcMs + shownMs);
            ImGui::TextColored(theme::kMuted, "Recorded time: %s", localTime.c_str());
        }

        const std::string& openError = app.playbackError();
        const std::string& runtimeError = status.error;
        const std::string& error = openError.empty() ? runtimeError : openError;
        if (!error.empty()) {
            ImGui::TextColored(theme::kFailed, "%s", error.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (leave) {
        app.closePlayback();
    }
    return leave;
}

void drawTile(App& app, size_t index, ImVec2 size, bool selected) {
    RecordingPlayer& player = app.playback();
    const auto& info = player.videos()[index];
    player.present(index, app.gpu());
    const VideoFrameTexture* texture = player.texture(index);

    ImGui::PushID(static_cast<int>(index));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kTileBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, selected ? 2.0f : 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? theme::kAccent
                                                   : ImGui::GetStyleColorVec4(ImGuiCol_Border));

    if (ImGui::BeginChild("##playback-tile", size, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        constexpr float footerHeight = 26.0f;
        const float videoHeight = std::max(available.y - footerHeight, 1.0f);
        bool zoomActive = false;

        if (texture != nullptr && texture->ready()) {
            ImVec2 drawSize;
            ImVec2 offset;
            letterbox(available.x, videoHeight, static_cast<float>(texture->width()),
                      static_cast<float>(texture->height()), drawSize, offset);
            const ImVec2 imageOrigin(origin.x + offset.x, origin.y + offset.y);
            const ImVec2 imageEnd(imageOrigin.x + drawSize.x, imageOrigin.y + drawSize.y);
            const bool imageHovered = ImGui::IsMouseHoveringRect(imageOrigin, imageEnd);
            ImGuiIO& io = ImGui::GetIO();
            PlaybackZoomState& zoom = playbackZoomState();

            const float factor = app.config().liveViewZoom;
            const float visibleSpan = 1.0f / factor;
            const float halfSpan = visibleSpan * 0.5f;

            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                app.setPlaybackSelected(static_cast<int>(index));
                if (factor > 1.0f) {
                    zoom.tile = static_cast<int>(index);
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
            ImGui::Image(reinterpret_cast<ImTextureID>(texture->view()), drawSize, uv0, uv1);
            drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        } else {
            ImGui::Dummy(ImVec2(available.x, 0));
            ui::centeredNote("Waiting for video...", available.x, videoHeight);
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##playback-focus", ImVec2(available.x, videoHeight),
                               ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(zoomActive ? ImGuiMouseCursor_ResizeAll
                                             : ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked()) {
            app.setPlaybackSelected(static_cast<int>(index));
        }
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            app.setPlaybackSelected(static_cast<int>(index));
            app.setPlaybackFocused(!app.playbackFocused());
        }

        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + 8.0f, origin.y + videoHeight + 4.0f));
        ImGui::TextUnformatted(info.title.c_str());

        const std::string resolution = std::format("{}x{}", info.width, info.height);
        const float resolutionWidth = ImGui::CalcTextSize(resolution.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + available.x - resolutionWidth - 8.0f,
                                        origin.y + videoHeight + 4.0f));
        ImGui::TextColored(theme::kMuted, "%s", resolution.c_str());
    }
    ImGui::EndChild();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::PopID();
}

} // namespace

void drawPlaybackView(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##playback-root", ImVec2(0, 0), ImGuiChildFlags_None);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        playbackZoomState().tile = -1;
    }

    if (drawToolbar(app)) {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::Dummy(ImVec2(0, 6));

    RecordingPlayer& player = app.playback();
    const auto& tracks = player.videos();
    if (tracks.empty()) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ui::centeredNote("Open a Xiaomi Viewer MKV recording to play it.", available.x,
                         available.y);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    if (app.playbackSelected() < 0 ||
        app.playbackSelected() >= static_cast<int>(tracks.size())) {
        app.setPlaybackSelected(0);
    }
    handleKeys(app, tracks.size());

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const int selected = app.playbackSelected();
    if (app.playbackFocused()) {
        drawTile(app, static_cast<size_t>(selected), available, true);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    int columns = 1;
    int rows = 1;
    gridDimensions(tracks.size(), columns, rows);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float tileWidth = (available.x - spacing * (columns - 1)) / columns;
    const float tileHeight = (available.y - spacing * (rows - 1)) / rows;

    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i % static_cast<size_t>(columns) != 0) {
            ImGui::SameLine();
        }
        drawTile(app, i, ImVec2(tileWidth, tileHeight), static_cast<int>(i) == selected);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace xv
