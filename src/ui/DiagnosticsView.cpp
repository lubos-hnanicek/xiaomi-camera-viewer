#include "ui/Views.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>

#include "app/App.h"
#include "app/Theme.h"

namespace xv {

void drawCameraDiagnosticsWindow(App& app) {
    auto& view = app.diagnosticsView();
    auto& collector = app.cameraDiagnostics();
    ImGui::SetNextWindowSize(ImVec2(700, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(460, 380), ImVec2(1000, 900));
    if (!ImGui::Begin("Camera diagnostics", &view.open)) {
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(!app.signedIn() || app.devicesBusy() || collector.busy());
    if (ImGui::Button(app.devicesBusy() ? "Refreshing..." : "Refresh from account")) {
        app.refreshDevices();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu account camera(s)", app.devices().size());
    if (!app.deviceError().empty()) {
        ImGui::TextColored(theme::kFailed, "%s", app.deviceError().c_str());
    }

    ImGui::BeginDisabled(collector.busy() || app.devicesBusy() || !app.signedIn());
    if (ImGui::BeginTable("##diagnosticdevices", 2,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                         ImVec2(0, 210))) {
        ImGui::TableSetupColumn("Camera", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableHeadersRow();
        for (const auto& device : app.devices()) {
            ImGui::PushID(device.did.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string& label = device.name.empty() ? device.model : device.name;
            if (ImGui::Selectable(label.c_str(), view.selectedDid == device.did,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                view.selectedDid = device.did;
                view.error.clear();
            }
            ImGui::SetItemTooltip("%s\n%s", label.c_str(), device.model.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(device.model.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (app.devices().empty() && !app.devicesBusy()) {
        ImGui::TextDisabled(app.signedIn() ? "No account cameras found." : "Sign in to choose an account camera.");
    }
    ImGui::Checkbox("Include SD catalogue", &view.includeStorage);
    ImGui::SetItemTooltip("Reads storage channels 0 and 10 without downloading footage. May take 45 seconds per channel.");
    ImGui::Checkbox("Include firmware information", &view.includeFirmwareInfo);
    ImGui::SetItemTooltip("Read-only firmware version/URL queries. No download or upgrade.");
    ImGui::Checkbox("Include application log", &view.includeAppLog);
    ImGui::SetItemTooltip("The redacted log can describe other cameras. Review it before sharing.");
    ImGui::EndDisabled();

    const auto selected = std::find_if(app.devices().begin(), app.devices().end(),
        [&](const DiscoveredDevice& device) { return device.did == view.selectedDid; });
    const std::string blocked = app.diagnosticsBlockedReason(view.selectedDid);
    ImGui::BeginDisabled(selected == app.devices().end() || !blocked.empty() || app.devicesBusy());
    if (ImGui::Button("Collect diagnostics")) {
        app.collectCameraDiagnostics(*selected);
    }
    ImGui::SetItemTooltip("Close Mi Home live view first. This camera's live tiles will pause during collection. No footage or audio is saved, and nothing is uploaded.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (collector.busy()) {
        if (ImGui::Button("Cancel collection")) {
            collector.cancel();
        }
    } else if (ImGui::Button("Privacy and next steps")) {
        app.showDiagnosticsHelp();
    }

    const auto& status = collector.status();
    if (collector.busy()) {
        ImGui::TextColored(theme::kPending, "Collecting...");
        ImGui::TextWrapped("%s", status.step.c_str());
    } else {
        switch (status.state) {
        case CameraDiagnostics::State::Complete:
            ImGui::TextColored(theme::kLive, "Diagnostics collected. Review before sharing.");
            break;
        case CameraDiagnostics::State::Partial:
            ImGui::TextColored(theme::kPending, "Partial diagnostics collected. Review before sharing.");
            break;
        case CameraDiagnostics::State::Cancelled:
            ImGui::TextColored(theme::kPending, "Collection cancelled.");
            break;
        case CameraDiagnostics::State::Failed:
            ImGui::TextColored(theme::kFailed, "Collection failed.");
            break;
        default: break;
        }
        if (!blocked.empty()) {
            ImGui::TextWrapped("%s", blocked.c_str());
        }
    }
    if (!view.error.empty()) { ImGui::TextWrapped("%s", view.error.c_str()); }
    if (!status.error.empty()) { ImGui::TextWrapped("%s", status.error.c_str()); }

    std::error_code error;
    if (!collector.busy() && !status.directory.empty() && std::filesystem::is_directory(status.directory, error)) {
        if (ImGui::Button("Open bundle folder")) { app.openDiagnosticsFolder(); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::filesystem::is_regular_file(status.directory / L"owner-report.txt", error));
        if (ImGui::Button("Open owner report")) { app.openDiagnosticsReport(); }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace xv