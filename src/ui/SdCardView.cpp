#include "ui/Views.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <vector>

#include "app/App.h"
#include "app/Theme.h"

namespace xv {
namespace {

// LocalClock turns the camera's Unix seconds into local wall-clock seconds.
//
// The offset is cached rather than looked up per clip: a fortnight of footage is
// twenty thousand clips, and a zone lookup each would be felt on every frame
// that rebuilds the browser. The cache is bounded by the range the zone itself
// reports, so a summer-time change inside the catalogue is still honoured.
class LocalClock {
public:
    [[nodiscard]] int64_t toLocal(int64_t epoch) {
        return epoch + offsetFor(epoch);
    }

private:
    [[nodiscard]] int64_t offsetFor(int64_t epoch) {
        const auto instant = std::chrono::sys_seconds{std::chrono::seconds{epoch}};
        if (valid_ && instant >= begin_ && instant < end_) {
            return offset_;
        }

        try {
            const auto* zone = std::chrono::current_zone();
            const auto info = zone->get_info(instant);
            begin_ = info.begin;
            end_ = info.end;
            offset_ = info.offset.count();
            valid_ = true;
        } catch (const std::exception&) {
            // Without a time zone database the only honest thing to show is
            // UTC, which is at least consistent with itself.
            begin_ = std::chrono::sys_seconds::min();
            end_ = std::chrono::sys_seconds::max();
            offset_ = 0;
            valid_ = true;
        }
        return offset_;
    }

    std::chrono::sys_seconds begin_{};
    std::chrono::sys_seconds end_{};
    int64_t offset_ = 0;
    bool valid_ = false;
};

// Formats local wall-clock seconds. The offset is already applied, so this
// deliberately formats them as if they were UTC.
std::string formatLocal(int64_t localSeconds, const char* pattern) {
    const auto point = std::chrono::sys_seconds{std::chrono::seconds{localSeconds}};
    if (std::string_view(pattern) == "date") {
        return std::format("{:%a %d %b %Y}", point);
    }
    if (std::string_view(pattern) == "hour") {
        return std::format("{:%H:00}", point);
    }
    if (std::string_view(pattern) == "minute") {
        return std::format("{:%H:%M:%S}", point);
    }
    return std::format("{:%Y-%m-%d %H:%M:%S}", point);
}

constexpr int64_t kDay = 24 * 60 * 60;
constexpr int64_t kHour = 60 * 60;

int64_t floorTo(int64_t value, int64_t unit) {
    return (value >= 0 ? value / unit : (value - unit + 1) / unit) * unit;
}

// The catalogue, arranged the way it is chosen from: a day, then an hour within
// it, then the clips in that hour. Mi Home presents the same three steps, and
// twenty thousand clips cannot be presented as one list.
struct Hour {
    int64_t localStart = 0;
    std::vector<size_t> clips;
};

struct Day {
    int64_t localStart = 0;
    std::string label;
    std::vector<Hour> hours;
    size_t clips = 0;
};

struct Browser {
    // Which catalogue this was built from. Counting clips would not do: two
    // cameras can hold the same number, and the browser would then still be
    // showing the first camera's days after the second one was opened.
    uint64_t version = 0;

    std::vector<SdClip> clips;
    std::vector<Day> days;

    int day = -1;
    int hour = -1;
};

Browser& browser() {
    static Browser state;
    return state;
}

void rebuild(Browser& state, SdPlayer& player) {
    state.clips = player.clips();
    state.days.clear();
    state.day = -1;
    state.hour = -1;

    LocalClock clock;
    for (size_t i = 0; i < state.clips.size(); ++i) {
        const int64_t local = clock.toLocal(state.clips[i].start);
        const int64_t day = floorTo(local, kDay);
        const int64_t hour = floorTo(local, kHour);

        if (state.days.empty() || state.days.back().localStart != day) {
            state.days.push_back(Day{day, formatLocal(day, "date"), {}, 0});
        }
        Day& current = state.days.back();
        if (current.hours.empty() || current.hours.back().localStart != hour) {
            current.hours.push_back(Hour{hour, {}});
        }
        current.hours.back().clips.push_back(i);
        current.clips++;
    }

    // Newest first. Someone opening a camera's card almost always wants what
    // happened recently, and scrolling a fortnight to reach it is a chore.
    std::reverse(state.days.begin(), state.days.end());
    for (Day& day : state.days) {
        std::reverse(day.hours.begin(), day.hours.end());
    }

    if (!state.days.empty()) {
        state.day = 0;
        if (!state.days[0].hours.empty()) {
            state.hour = 0;
        }
    }
}

void syncBrowser(App& app) {
    Browser& state = browser();
    SdPlayer& player = app.sdPlayer();

    const uint64_t version = player.catalogueVersion();
    if (state.version == version) {
        return;
    }

    state.version = version;
    rebuild(state, player);
}

const char* stateLabel(const SdPlayer::Status& status) {
    switch (status.state) {
    case SdState::Idle: return "Idle";
    case SdState::Connecting: return "Connecting";
    case SdState::Loading: return "Reading card";
    case SdState::Ready: return status.filePlayback ? "Ready" : "Live";
    case SdState::Playing: return "Playing";
    case SdState::Failed: return "Failed";
    }
    return "";
}

ImVec4 stateColor(SdState state) {
    switch (state) {
    case SdState::Playing: return theme::kAccent;
    case SdState::Ready: return theme::kLive;
    case SdState::Failed: return theme::kFailed;
    default: return theme::kPending;
    }
}

void drawToolbar(App& app) {
    SdPlayer& player = app.sdPlayer();
    const SdPlayer::Status status = player.status();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    if (ImGui::BeginChild("##sd-toolbar", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
        ImGui::TextUnformatted(player.camera().label().c_str());
        ImGui::SameLine();

        ImGui::TextColored(stateColor(status.state), "%s", stateLabel(status));
        ImGui::SameLine();

        ImGui::BeginDisabled(status.state == SdState::Connecting);
        if (ImGui::Button("Back to live")) {
            app.closeSdPlayback();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Closes the card and returns to the live grid.");
        ImGui::SameLine();

        bool audible = status.audible;
        if (ImGui::Checkbox("Sound", &audible)) {
            app.toggleSdListening();
        }
        ImGui::SameLine();

        // The moment on screen, which is the whole point of the screen: a
        // recording without a time on it is just video.
        if (status.position > 0) {
            LocalClock clock;
            ImGui::TextColored(theme::kAccent, "%s",
                               formatLocal(clock.toLocal(status.position), "full").c_str());
        } else if (status.state == SdState::Playing && !status.message.empty()) {
            ImGui::TextDisabled("%s", status.message.c_str());
        } else if (status.clips > 0) {
            LocalClock clock;
            ImGui::TextDisabled("%zu clips, %s to %s", status.clips,
                                formatLocal(clock.toLocal(status.oldest), "full").c_str(),
                                formatLocal(clock.toLocal(status.newest), "full").c_str());
        } else {
            ImGui::TextDisabled("%s", status.message.c_str());
        }

        if (!status.error.empty()) {
            ImGui::TextColored(theme::kFailed, "%s", status.error.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// The three-step chooser. Returns the instant to play, or zero.
int64_t drawBrowser(App& app, float height) {
    Browser& state = browser();
    const SdPlayer::Status status = app.sdPlayer().status();

    int64_t chosen = 0;

    if (ImGui::BeginChild("##sd-browser", ImVec2(280, height), ImGuiChildFlags_Borders)) {
        if (state.days.empty()) {
            ui::centeredNote(status.state == SdState::Loading ? "Reading the card..."
                                                             : "Nothing on the card",
                             ImGui::GetContentRegionAvail().x, 80.0f);
            ImGui::EndChild();
            return 0;
        }

        ImGui::TextDisabled("Day");
        if (ImGui::BeginListBox("##sd-days", ImVec2(-FLT_MIN, 140))) {
            for (int i = 0; i < static_cast<int>(state.days.size()); ++i) {
                const Day& day = state.days[static_cast<size_t>(i)];
                const std::string label = std::format("{}  ({})", day.label, day.clips);
                if (ImGui::Selectable(label.c_str(), state.day == i)) {
                    state.day = i;
                    state.hour = day.hours.empty() ? -1 : 0;
                }
            }
            ImGui::EndListBox();
        }

        if (state.day < 0 || state.day >= static_cast<int>(state.days.size())) {
            ImGui::EndChild();
            return 0;
        }
        const Day& day = state.days[static_cast<size_t>(state.day)];

        ImGui::TextDisabled("Hour");
        if (ImGui::BeginListBox("##sd-hours", ImVec2(-FLT_MIN, 140))) {
            for (int i = 0; i < static_cast<int>(day.hours.size()); ++i) {
                const Hour& hour = day.hours[static_cast<size_t>(i)];
                const std::string label = std::format("{}  ({})",
                                                      formatLocal(hour.localStart, "hour"),
                                                      hour.clips.size());
                if (ImGui::Selectable(label.c_str(), state.hour == i)) {
                    state.hour = i;
                }
            }
            ImGui::EndListBox();
        }

        if (state.hour < 0 || state.hour >= static_cast<int>(day.hours.size())) {
            ImGui::EndChild();
            return 0;
        }
        const Hour& hour = day.hours[static_cast<size_t>(state.hour)];

        ImGui::TextDisabled("Clip");
        if (ImGui::BeginListBox("##sd-clips", ImVec2(-FLT_MIN, -FLT_MIN))) {
            LocalClock clock;
            for (const size_t index : hour.clips) {
                const SdClip& clip = state.clips[index];
                // The clip that is playing is marked, which is how the list
                // stays meaningful once the camera has run on past the one that
                // was actually asked for.
                const bool playing = status.position >= clip.start && status.position < clip.end();
                const char* mark = clip.event ? "  event" : "";
                const std::string label =
                    std::format("{}  {}s{}##{}", formatLocal(clock.toLocal(clip.start), "minute"),
                                clip.duration, mark, clip.start);

                if (playing) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccent);
                }
                if (ImGui::Selectable(label.c_str(), playing)) {
                    chosen = clip.start;
                }
                if (playing) {
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndListBox();
        }
    }
    ImGui::EndChild();

    return chosen;
}

void drawPicture(App& app, float height) {
    SdPlayer& player = app.sdPlayer();
    const SdPlayer::Status status = player.status();

    if (ImGui::BeginChild("##sd-picture", ImVec2(0, height), ImGuiChildFlags_Borders)) {
        const ImVec2 area = ImGui::GetContentRegionAvail();

        player.present(app.gpu(), app.sdTexture());
        ID3D11ShaderResourceView* view = app.sdTexture().view();

        if (view == nullptr) {
            const char* note = "Waiting for the camera";
            if (!status.error.empty()) {
                note = status.error.c_str();
            } else if (status.filePlayback && status.state == SdState::Playing) {
                note = status.message.empty() ? "Fetching the recording" : status.message.c_str();
            } else if (status.filePlayback) {
                note = "Pick a clip to play this lens";
            }
            ui::centeredNote(note, area.x, area.y);
        } else {
            const float contentWidth = static_cast<float>(std::max(status.width, 1));
            const float contentHeight = static_cast<float>(std::max(status.height, 1));
            const float scale = std::min(area.x / contentWidth, area.y / contentHeight);
            const ImVec2 size(contentWidth * scale, contentHeight * scale);

            const ImVec2 origin = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(origin.x + (area.x - size.x) * 0.5f,
                                       origin.y + (area.y - size.y) * 0.5f));
            ImGui::Image(reinterpret_cast<ImTextureID>(view), size);
        }
    }
    ImGui::EndChild();
}

} // namespace

void drawSdCardView(App& app) {
    syncBrowser(app);

    drawToolbar(app);
    ImGui::Spacing();

    const float height = ImGui::GetContentRegionAvail().y;

    const int64_t chosen = drawBrowser(app, height);
    ImGui::SameLine();
    drawPicture(app, height);

    if (chosen > 0) {
        app.sdPlayer().play(chosen);
    }
}

} // namespace xv
