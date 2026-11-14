#include "ui/Views.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "app/App.h"
#include "app/Theme.h"
#include "util/FileDialog.h"

namespace xv {
namespace {

std::filesystem::path pathFromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

void copyUtf8Path(const std::filesystem::path& path, ConfigurationDialogState& state) {
    const std::u8string encoded = path.u8string();
    const std::string value(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    if (value.size() >= state.recordingsDir.size()) {
        state.error = "The selected folder path is too long to store.";
        return;
    }
    std::snprintf(state.recordingsDir.data(), state.recordingsDir.size(), "%s", value.c_str());
    state.error.clear();
}

bool matchesFontFilter(std::string_view name, std::string_view filter) {
    return filter.empty() ||
           std::search(name.begin(), name.end(), filter.begin(), filter.end(),
                       [](unsigned char left, unsigned char right) {
                           return std::tolower(left) == std::tolower(right);
                       }) != name.end();
}

} // namespace

void drawConfigurationDialog(App& app) {
    ConfigurationDialogState& state = app.configurationDialog();
    if (state.openRequested) {
        ImGui::OpenPopup("Configuration");
        state.openRequested = false;
    }

    if (!ImGui::BeginPopupModal("Configuration", &state.open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("These preferences are saved in config.json.");
    ImGui::Separator();

    ImGui::SliderFloat("UI scale", &state.uiScale, 0.5f, 2.0f, "%.2fx");
    ImGui::SetItemTooltip("Scales the interface relative to the monitor's Windows display scale.");

    constexpr const char* kFontChoices[] = {"ProggyVector", "ProggyClean", "Windows font"};
    const char* fontLabel = state.fontChoice == 2
                                ? kFontChoices[2]
                                : state.fontChoice == 1 ? kFontChoices[1] : kFontChoices[0];
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("UI font", fontLabel)) {
        for (int i = 0; i < 3; ++i) {
            ImGui::BeginDisabled(i == 2 && state.windowsFonts.empty());
            const bool selected = ImGui::Selectable(kFontChoices[i], state.fontChoice == i);
            ImGui::EndDisabled();
            if (selected) {
                if (i == 2 && state.fontChoice != 2 &&
                    (std::string_view(state.uiFontName.data()) == "ProggyVector" ||
                     std::string_view(state.uiFontName.data()) == "ProggyClean")) {
                    const auto preferred = std::find_if(
                        state.windowsFonts.begin(), state.windowsFonts.end(),
                        [](const WindowsFontOption& font) {
                            return _stricmp(font.fileName.c_str(), "segoeui") == 0;
                        });
                    if (preferred != state.windowsFonts.end()) {
                        std::snprintf(state.uiFontName.data(), state.uiFontName.size(), "%s",
                                      preferred->fileName.c_str());
                    } else if (!state.windowsFonts.empty()) {
                        std::snprintf(state.uiFontName.data(), state.uiFontName.size(), "%s",
                                      state.windowsFonts.front().fileName.c_str());
                    }
                }
                state.fontChoice = i;
            }
        }
        ImGui::EndCombo();
    }
    if (state.fontChoice == 2) {
        const auto selectedFont = std::find_if(
            state.windowsFonts.begin(), state.windowsFonts.end(),
            [&](const WindowsFontOption& font) {
                return _stricmp(font.fileName.c_str(), state.uiFontName.data()) == 0;
            });
        const char* selectedFontLabel = selectedFont == state.windowsFonts.end()
                                            ? state.uiFontName.data()
                                            : selectedFont->displayName.c_str();
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::BeginCombo("##windows-font", selectedFontLabel)) {
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##font-search", "Search fonts",
                                     state.fontSearch.data(), state.fontSearch.size());
            const std::string_view filter(state.fontSearch.data());
            for (const WindowsFontOption& font : state.windowsFonts) {
                if (!matchesFontFilter(font.displayName, filter)) {
                    continue;
                }
                if (ImGui::Selectable(font.displayName.c_str(),
                                      _stricmp(font.fileName.c_str(), state.uiFontName.data()) ==
                                          0)) {
                    std::snprintf(state.uiFontName.data(), state.uiFontName.size(), "%s",
                                  font.fileName.c_str());
                }
            }
            ImGui::EndCombo();
        }
        if (!state.fontCatalogError.empty()) {
            ImGui::TextColored(theme::kFailed, "%s", state.fontCatalogError.c_str());
        } else if (state.windowsFonts.empty()) {
            ImGui::TextColored(theme::kMuted, "No Windows TrueType fonts were found.");
        }
    }

    ImGui::BeginDisabled(state.fontChoice == 1);
    ImGui::SliderFloat("Font size", &state.uiFontSize, 6.0f, 48.0f, "%.0f px");
    ImGui::EndDisabled();
    if (state.fontChoice == 1) {
        ImGui::TextColored(theme::kMuted, "ProggyClean uses its built-in fixed size.");
    }

    ImGui::InputFloat("Live view zoom", &state.liveViewZoom, 0.1f, 1.0f, "%.1fx");
    ImGui::SetItemTooltip("Magnification while holding the right mouse button; 1.0 disables it.");

    ImGui::SeparatorText("Recording folder");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##recordings-directory", state.recordingsDir.data(),
                     state.recordingsDir.size());
    if (ImGui::Button("Browse...")) {
        std::string error;
        const std::string draft(state.recordingsDir.data());
        const auto initialDirectory = draft.empty() ? app.config().recordingsDirectory()
                                                     : pathFromUtf8(draft);
        const auto selected =
            chooseRecordingDirectory(app.windowHandle(), initialDirectory, error);
        if (selected.has_value()) {
            copyUtf8Path(*selected, state);
        } else if (!error.empty()) {
            state.error = std::move(error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use default")) {
        state.recordingsDir[0] = '\0';
        state.error.clear();
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::kMuted, "Empty uses Videos\\XiaomiViewer");

    ImGui::TextColored(theme::kMuted, "Changes are saved and applied immediately.");
    if (!state.error.empty()) {
        ImGui::TextColored(theme::kFailed, "%s", state.error.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Apply", ImVec2(120.0f, 0.0f))) {
        if (app.applyConfigurationDialog()) {
            state.open = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        state.open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace xv
