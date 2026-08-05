#include "ui/Views.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "app/App.h"
#include "app/Theme.h"

namespace xv {
namespace {

// Xiaomi shards accounts by region and the IoT API only answers on the right
// one, so this has to be an explicit choice rather than a guess.
struct RegionOption {
    const char* code;
    const char* label;
};

constexpr std::array<RegionOption, 6> kRegions{{
    {"", "China (default)"},
    {"de", "Europe"},
    {"us", "United States"},
    {"sg", "Singapore"},
    {"ru", "Russia"},
    {"i2", "India"},
}};

int regionIndex(const std::string& code) {
    for (size_t i = 0; i < kRegions.size(); ++i) {
        if (code == kRegions[i].code) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// ImGui's text fields want a mutable char buffer, so each field keeps one and
// syncs it with the owning string.
template <size_t N>
bool textField(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
    std::array<char, N> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());

    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::InputText(label, buffer.data(), buffer.size(), flags);
    if (changed) {
        value = buffer.data();
    }
    return changed;
}

} // namespace

void drawLoginView(App& app) {
    App::LoginState& state = app.login();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    constexpr float kCardWidth = 460.0f;

    ImGui::SetCursorPos(ImVec2((available.x - kCardWidth) * 0.5f,
                               std::max(40.0f, available.y * 0.14f)));

    ImGui::BeginGroup();
    ImGui::PushItemWidth(kCardWidth);

    if (ImGui::BeginChild("##login", ImVec2(kCardWidth, 0), ImGuiChildFlags_AutoResizeY |
                                                                ImGuiChildFlags_Borders)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
        ImGui::TextUnformatted("Sign in to Mi Home");
        ImGui::PopFont();

        ImGui::TextColored(theme::kMuted,
                           "Your cameras are reachable on the local network, but Xiaomi\n"
                           "only hands out the stream keys to a signed-in account.");
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        const bool busy = state.busy;
        ImGui::BeginDisabled(busy);

        if (!state.needCaptcha && !state.needVerify) {
            ImGui::TextUnformatted("Mi account");
            textField<256>("##username", state.username);
            ImGui::TextColored(theme::kMuted, "Email, phone number, or numeric Mi account ID");

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextUnformatted("Password");
            textField<256>("##password", state.password, ImGuiInputTextFlags_Password);

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextUnformatted("Server region");
            int region = regionIndex(app.config().account.region);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##region", kRegions[static_cast<size_t>(region)].label)) {
                for (size_t i = 0; i < kRegions.size(); ++i) {
                    const bool selected = region == static_cast<int>(i);
                    if (ImGui::Selectable(kRegions[i].label, selected)) {
                        region = static_cast<int>(i);
                        app.config().account.region = kRegions[i].code;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextColored(theme::kMuted,
                               "Must match the region the Mi Home app uses for these cameras.");

            ImGui::Dummy(ImVec2(0, 12));

            const bool canSubmit = !state.username.empty() && !state.password.empty();
            ImGui::BeginDisabled(!canSubmit);
            if (ImGui::Button("Sign in", ImVec2(-1, 0))) {
                app.beginLogin(state.username, state.password, app.config().account.region);
            }
            ImGui::EndDisabled();

        } else if (state.needCaptcha) {
            ImGui::TextUnformatted("Xiaomi wants a captcha for this sign-in.");
            ImGui::Dummy(ImVec2(0, 8));

            if (ID3D11ShaderResourceView* captcha = app.captchaTexture()) {
                ImGui::Image(reinterpret_cast<ImTextureID>(captcha), ImVec2(200, 70));
            } else {
                ImGui::TextColored(theme::kFailed, "The captcha image could not be displayed.");
            }

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextUnformatted("Characters shown above");
            textField<64>("##captcha", state.captchaCode,
                          ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_AutoSelectAll);

            ImGui::Dummy(ImVec2(0, 12));
            ImGui::BeginDisabled(state.captchaCode.empty());
            if (ImGui::Button("Continue", ImVec2(-1, 0))) {
                app.submitCaptcha(state.captchaCode);
            }
            ImGui::EndDisabled();

        } else {
            ImGui::TextWrapped("%s", state.status.c_str());
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextUnformatted("Verification code");
            textField<64>("##verify", state.verifyTicket,
                          ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);

            ImGui::Dummy(ImVec2(0, 12));
            ImGui::BeginDisabled(state.verifyTicket.empty());
            if (ImGui::Button("Verify", ImVec2(-1, 0))) {
                app.submitVerify(state.verifyTicket);
            }
            ImGui::EndDisabled();
        }

        ImGui::EndDisabled();

        if (state.needCaptcha || state.needVerify) {
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::SmallButton("Start over")) {
                state.needCaptcha = false;
                state.needVerify = false;
                state.error.clear();
                state.status.clear();
            }
        }

        if (busy) {
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::TextColored(theme::kPending, "%s...", state.status.c_str());
        } else if (!state.needCaptcha && !state.needVerify && !state.status.empty()) {
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::TextColored(theme::kMuted, "%s", state.status.c_str());
        }

        if (!state.error.empty()) {
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kFailed);
            ImGui::TextWrapped("%s", state.error.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

} // namespace xv
