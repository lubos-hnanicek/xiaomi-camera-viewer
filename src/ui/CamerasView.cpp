#include "ui/Views.h"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <string>

#include "app/App.h"
#include "app/Theme.h"

namespace xv {
namespace {

// The cloud reports 0.0.0.0 for a camera it has no address for, which is not an
// address anyone can dial. The bridge finds those by MAC instead, so this only
// decides what to show, not whether the camera can be used.
bool hasAddress(const std::string& ip) {
    return !ip.empty() && ip != "0.0.0.0";
}

// Returns by value: a numeric profile has no static label to point at, and
// returning c_str() of a local would dangle.
std::string qualityLabel(const std::string& quality) {
    if (quality == "sd") return "Standard";
    if (quality == "auto") return "Automatic";
    if (quality.empty() || quality == "hd") return "High";
    return "Profile " + quality;
}

// What "High" resolves to depends on the model, and the table cannot cover
// every camera, so the numeric profiles are offered as a manual override.
constexpr std::array<const char*, 3> kNamedQualities{"hd", "sd", "auto"};
constexpr std::array<const char*, 6> kNumberedQualities{"0", "1", "2", "3", "4", "5"};

void drawConfiguredCameras(App& app) {
    ImGui::SeparatorText("On the grid");

    auto& streams = app.streams();
    if (streams.empty()) {
        ImGui::TextColored(theme::kMuted, "Nothing added yet.");
        return;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##configured", 6, kFlags)) {
        return;
    }

    ImGui::TableSetupColumn("Camera", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.16f);
    ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthStretch, 0.16f);
    ImGui::TableSetupColumn("Transport", ImGuiTableColumnFlags_WidthStretch, 0.14f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.14f);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch, 0.14f);
    ImGui::TableHeadersRow();

    std::optional<size_t> removing;

    for (size_t i = 0; i < streams.size(); ++i) {
        CameraStream& stream = *streams[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();

        const StreamWorker::Status status =
            stream.worker ? stream.worker->status() : StreamWorker::Status{};

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        bool enabled = stream.config.enabled;
        if (ImGui::Checkbox("##enabled", &enabled)) {
            stream.config.enabled = enabled;
            if (CameraConfig* stored =
                    app.config().findCamera(stream.config.did, stream.config.channel)) {
                stored->enabled = enabled;
            }
            app.config().save();
            app.restartStream(stream);
        }
        ImGui::SetItemTooltip("Stream this camera");
        ImGui::SameLine();
        ImGui::TextUnformatted(stream.config.label().c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        if (hasAddress(status.host)) {
            // Found on the network, so this is where it really is.
            ImGui::TextColored(theme::kMuted, "%s", status.host.c_str());
        } else if (hasAddress(stream.config.ip)) {
            ImGui::TextColored(theme::kMuted, "%s", stream.config.ip.c_str());
        } else {
            ImGui::TextColored(theme::kPending, "searching");
            ImGui::SetItemTooltip(
                "Xiaomi does not know this camera's address, so it is found by its "
                "hardware address on the local network.");
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##quality", qualityLabel(stream.config.quality).c_str())) {
            const auto choose = [&app, &stream](const char* code) {
                if (stream.config.quality == code) {
                    return;
                }
                stream.config.quality = code;
                if (CameraConfig* stored =
                        app.config().findCamera(stream.config.did, stream.config.channel)) {
                    stored->quality = code;
                }
                app.config().save();
                app.restartStream(stream);
            };

            for (const char* code : kNamedQualities) {
                if (ImGui::Selectable(qualityLabel(code).c_str(), stream.config.quality == code)) {
                    choose(code);
                }
            }

            ImGui::SeparatorText("Override");
            ImGui::SetItemTooltip(
                "Use if High gives no picture or the wrong lens on this model");

            for (const char* code : kNumberedQualities) {
                if (ImGui::Selectable(qualityLabel(code).c_str(), stream.config.quality == code)) {
                    choose(code);
                }
            }

            ImGui::EndCombo();
        }

        ImGui::TableSetColumnIndex(3);
        ImGui::SetNextItemWidth(-1.0f);
        constexpr std::array<const char*, 3> kTransports{"", "udp", "tcp"};
        const char* transportLabel =
            stream.config.transport.empty() ? "Automatic" : stream.config.transport.c_str();
        if (ImGui::BeginCombo("##transport", transportLabel)) {
            for (const char* code : kTransports) {
                const bool selected = stream.config.transport == code;
                if (ImGui::Selectable(*code == '\0' ? "Automatic" : code, selected)) {
                    stream.config.transport = code;
                    if (CameraConfig* stored =
                            app.config().findCamera(stream.config.did, stream.config.channel)) {
                        stored->transport = code;
                    }
                    app.config().save();
                    app.restartStream(stream);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The camera normally picks this. Force TCP if UDP is blocked.");

        ImGui::TableSetColumnIndex(4);
        ImGui::AlignTextToFramePadding();
        ui::statusBadge(streamStateName(status.state), static_cast<int>(status.state));
        if (!status.message.empty()) {
            ImGui::SetItemTooltip("%s", status.message.c_str());
        }

        ImGui::TableSetColumnIndex(5);
        if (ImGui::SmallButton("Reconnect")) {
            app.restartStream(stream);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            removing = i;
        }

        ImGui::PopID();
    }

    ImGui::EndTable();

    if (removing.has_value()) {
        app.removeCamera(*removing);
    }
}

void drawDiscoveredDevices(App& app) {
    ImGui::SeparatorText("Found on your account");

    if (app.devicesBusy()) {
        ImGui::TextColored(theme::kPending, "Asking Xiaomi for your devices...");
        return;
    }

    if (!app.deviceError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kFailed);
        ImGui::TextWrapped("%s", app.deviceError().c_str());
        ImGui::PopStyleColor();
        return;
    }

    auto& devices = app.devices();
    if (devices.empty()) {
        ImGui::TextColored(theme::kMuted,
                           "No cameras found. Press Refresh, and check that the region matches "
                           "the one your Mi Home app uses.");
        return;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##discovered", 4, kFlags)) {
        return;
    }

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.18f);
    ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < devices.size(); ++i) {
        const DiscoveredDevice& device = devices[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(device.name.empty() ? device.did.c_str() : device.name.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kMuted, "%s", device.model.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        if (hasAddress(device.ip)) {
            ImGui::TextColored(theme::kMuted, "%s", device.ip.c_str());
        } else {
            ImGui::TextColored(theme::kPending, "not reported");
            ImGui::SetItemTooltip(
                "Xiaomi does not know where this camera is. It will be looked up by its "
                "hardware address on the local network when it is opened.");
        }

        ImGui::TableSetColumnIndex(3);

        const bool dual = isDualLens(device.model);
        const bool primaryAdded = app.config().findCamera(device.did, "") != nullptr;

        if (dual) {
            ImGui::BeginDisabled(primaryAdded);
            if (ImGui::SmallButton("Add lens 1")) {
                app.addCamera(device, "");
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            const bool secondaryAdded = app.config().findCamera(device.did, "1") != nullptr;
            ImGui::BeginDisabled(secondaryAdded);
            if (ImGui::SmallButton("Add lens 2")) {
                app.addCamera(device, "1");
            }
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(primaryAdded);
            if (ImGui::SmallButton(primaryAdded ? "Added" : "Add to grid")) {
                app.addCamera(device, "");
            }
            ImGui::EndDisabled();
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}

} // namespace

void drawCamerasView(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##camerasroot", ImVec2(0, 0), ImGuiChildFlags_None);

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextUnformatted("Cameras");
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(16, 0));
    ImGui::SameLine();

    ImGui::BeginDisabled(app.devicesBusy());
    if (ImGui::Button("Refresh from account")) {
        app.refreshDevices();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Back to grid")) {
        app.setScreen(Screen::Grid);
    }

    ImGui::Dummy(ImVec2(0, 10));

    drawConfiguredCameras(app);
    ImGui::Dummy(ImVec2(0, 16));
    drawDiscoveredDevices(app);

    ImGui::Dummy(ImVec2(0, 16));
    ImGui::TextColored(theme::kMuted,
                       "Video travels directly from the camera to this PC over the local network. "
                       "Xiaomi's cloud is only used to sign in, list devices and exchange the "
                       "per-session keys.");

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace xv
