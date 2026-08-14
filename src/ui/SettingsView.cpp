#include "ui/Views.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "app/App.h"
#include "app/Theme.h"
#include "cloud/Miot.h"

namespace xv {
namespace {

int enumIndex(const MiotProperty& property, int value) {
    for (size_t i = 0; i < property.options.size(); ++i) {
        if (property.options[i].value == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void drawProperty(App& app, CameraStream& stream, const MiotProperty& property,
                  const MiotValue& current) {
    ImGui::PushID(property.key);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(property.label);
    if (property.help != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        ImGui::SetItemTooltip("%s", property.help);
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);

    const bool locked = !property.writable || stream.miotTask.busy();
    ImGui::BeginDisabled(locked);

    switch (property.type) {
    case MiotType::Bool: {
        bool value = current.asBool();
        if (ImGui::Checkbox("##value", &value)) {
            app.writeSetting(stream, property, Json(value));
        }
        break;
    }

    case MiotType::Enum: {
        const int index = enumIndex(property, current.asInt());
        const char* preview = index >= 0 ? property.options[static_cast<size_t>(index)].label
                                         : "Unknown";
        if (ImGui::BeginCombo("##value", preview)) {
            for (const auto& option : property.options) {
                const bool selected = option.value == current.asInt();
                if (ImGui::Selectable(option.label, selected)) {
                    app.writeSetting(stream, property, Json(option.value));
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        break;
    }

    case MiotType::Int: {
        int value = current.asInt();
        if (property.maximum > property.minimum) {
            // Committing on release rather than on drag keeps a slider from
            // firing a cloud call for every pixel of movement.
            if (ImGui::SliderInt("##value", &value, property.minimum, property.maximum) &&
                ImGui::IsItemDeactivatedAfterEdit()) {
                app.writeSetting(stream, property, Json(value));
            }
        } else if (property.writable) {
            if (ImGui::InputInt("##value", &value) && ImGui::IsItemDeactivatedAfterEdit()) {
                app.writeSetting(stream, property, Json(value));
            }
        } else {
            ImGui::Text("%d", value);
        }
        break;
    }

    case MiotType::String: {
        const std::string text = current.asString();
        if (property.writable) {
            std::array<char, 256> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
            if (ImGui::InputText("##value", buffer.data(), buffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                app.writeSetting(stream, property, Json(std::string(buffer.data())));
            }
        } else {
            ImGui::TextColored(theme::kMuted, "%s", text.empty() ? "-" : text.c_str());
        }
        break;
    }
    }

    ImGui::EndDisabled();
    ImGui::PopID();
}

void drawStorageSummary(const CameraStream& stream) {
    const MiotProperty* totalProperty = findCameraProperty(stream.config.model, "sd-total");
    const MiotProperty* freeProperty = findCameraProperty(stream.config.model, "sd-free");
    if (totalProperty == nullptr || freeProperty == nullptr || !stream.miot) {
        return;
    }

    const std::optional<MiotValue> total =
        stream.miot->find(totalProperty->siid, totalProperty->piid);
    const std::optional<MiotValue> freeSpace =
        stream.miot->find(freeProperty->siid, freeProperty->piid);
    if (!total.has_value() || !total->present || total->asInt() <= 0) {
        return;
    }

    const auto totalBytes = static_cast<unsigned long long>(total->asInt()) * 1024ull;
    const auto freeBytes = freeSpace.has_value() && freeSpace->present
                               ? static_cast<unsigned long long>(freeSpace->asInt()) * 1024ull
                               : 0ull;
    const float used = totalBytes > 0
                           ? 1.0f - static_cast<float>(freeBytes) / static_cast<float>(totalBytes)
                           : 0.0f;

    ImGui::TextColored(theme::kMuted, "SD card");
    ImGui::ProgressBar(std::clamp(used, 0.0f, 1.0f), ImVec2(-1, 0),
                       std::format("{} of {} used", ui::humanBytes(totalBytes - freeBytes),
                                   ui::humanBytes(totalBytes))
                           .c_str());
    ImGui::Dummy(ImVec2(0, 6));
}

void drawActions(App& app, CameraStream& stream) {
    ImGui::SeparatorText("Actions");

    const bool busy = stream.miotTask.busy();

    for (const auto& action : cameraActions(stream.config.model)) {
        ImGui::PushID(action.key);

        ImGui::BeginDisabled(busy);
        if (action.destructive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.16f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
        }

        if (ImGui::Button(action.label, ImVec2(230, 0))) {
            if (action.destructive) {
                ImGui::OpenPopup("##confirm");
            } else {
                app.invokeAction(stream, action);
            }
        }

        if (action.destructive) {
            ImGui::PopStyleColor(2);
        }
        ImGui::EndDisabled();

        if (action.help != nullptr) {
            ImGui::SameLine();
            ImGui::TextColored(theme::kMuted, "%s", action.help);
        }

        if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", action.label);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextColored(theme::kPending, "%s",
                               action.help != nullptr ? action.help : "This cannot be undone.");
            ImGui::Dummy(ImVec2(0, 10));

            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button("Confirm", ImVec2(120, 0))) {
                app.invokeAction(stream, action);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }
}

} // namespace

void drawSettingsView(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##settingsroot", ImVec2(0, 0), ImGuiChildFlags_None);

    auto& streams = app.streams();

    if (streams.empty()) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ui::centeredNote("Add a camera first.", available.x, available.y);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    int selected = app.selected();
    if (selected < 0 || selected >= static_cast<int>(streams.size())) {
        selected = 0;
        app.setSelected(0);
    }

    CameraStream& stream = *streams[static_cast<size_t>(selected)];

    // --- Header ---
    ImGui::TextColored(theme::kMuted, "Camera");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::BeginCombo("##camera", stream.config.label().c_str())) {
        for (size_t i = 0; i < streams.size(); ++i) {
            const bool isSelected = static_cast<int>(i) == selected;
            if (ImGui::Selectable(streams[i]->config.label().c_str(), isSelected)) {
                app.setSelected(static_cast<int>(i));
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(stream.miotTask.busy());
    if (ImGui::Button("Refresh")) {
        app.loadSettingsFor(stream);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Back to grid")) {
        app.setScreen(Screen::Grid);
    }

    if (stream.miotTask.busy()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::kPending, "%s...", stream.miotBusyLabel.c_str());
    }

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(theme::kMuted, "%s  -  %s", stream.config.model.c_str(),
                       stream.config.ip.c_str());
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    // Settings are only read on demand: each refresh is a cloud round trip, so
    // opening the panel is what triggers the first one.
    if (!stream.miotLoaded && !stream.miotTask.busy() && stream.miotError.empty()) {
        app.loadSettingsFor(stream);
    }

    if (!stream.miotError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kFailed);
        ImGui::TextWrapped("Could not read settings: %s", stream.miotError.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("Try again")) {
            stream.miotError.clear();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    if (!stream.miotLoaded) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ui::centeredNote("Reading settings from the camera...", available.x, available.y);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    if (ImGui::BeginChild("##settingsscroll", ImVec2(0, 0))) {
        drawStorageSummary(stream);

        // Group properties as the spec does, and skip whole groups the camera
        // does not implement rather than showing empty headings.
        const char* currentGroup = nullptr;
        bool tableOpen = false;

        const auto closeTable = [&] {
            if (tableOpen) {
                ImGui::EndTable();
                ImGui::Dummy(ImVec2(0, 10));
                tableOpen = false;
            }
        };

        for (const auto& property : cameraProperties(stream.config.model)) {
            if (!stream.miot->supported(property)) {
                continue;
            }
            const std::optional<MiotValue> value = stream.miot->find(property.siid, property.piid);
            if (!value.has_value()) {
                continue;
            }

            if (currentGroup == nullptr || std::string(currentGroup) != property.group) {
                closeTable();
                currentGroup = property.group;
                ImGui::SeparatorText(currentGroup);
                tableOpen = ImGui::BeginTable("##props", 2,
                                              ImGuiTableFlags_SizingStretchProp |
                                                  ImGuiTableFlags_RowBg);
                if (tableOpen) {
                    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                }
            }

            if (tableOpen) {
                drawProperty(app, stream, property, *value);
            }
        }

        closeTable();

        if (currentGroup == nullptr) {
            ImGui::TextColored(theme::kMuted,
                               "This camera did not report any of the settings this app knows about.");
        }

        drawActions(app, stream);
    }
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace xv
