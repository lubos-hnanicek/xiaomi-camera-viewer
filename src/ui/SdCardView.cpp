#include "ui/Views.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <unordered_set>
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

    // Clip starts the user has marked for Save. Kept as starts rather than
    // list indexes because the same clip can appear after a catalogue refresh
    // with a different index.
    std::unordered_set<int64_t> selected;
    int64_t selectAnchor = 0;
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
    state.selected.clear();
    state.selectAnchor = 0;

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

        if (!status.saveMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.saveMessage.c_str());
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
            bool openMenu = false;
            for (const size_t index : hour.clips) {
                const SdClip& clip = state.clips[index];
                // The clip that is playing is marked, which is how the list
                // stays meaningful once the camera has run on past the one that
                // was actually asked for.
                const bool playing = status.position >= clip.start && status.position < clip.end();
                const bool marked = state.selected.contains(clip.start);
                const char* mark = clip.event ? "  event" : "";
                const std::string label =
                    std::format("{}  {}s{}##{}", formatLocal(clock.toLocal(clip.start), "minute"),
                                clip.duration, mark, clip.start);

                if (playing) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccent);
                }
                if (ImGui::Selectable(label.c_str(), marked)) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyShift && state.selectAnchor != 0) {
                        int from = -1;
                        int to = -1;
                        for (int i = 0; i < static_cast<int>(hour.clips.size()); ++i) {
                            const int64_t start = state.clips[hour.clips[static_cast<size_t>(i)]].start;
                            if (start == state.selectAnchor) {
                                from = i;
                            }
                            if (start == clip.start) {
                                to = i;
                            }
                        }
                        if (from >= 0 && to >= 0) {
                            if (from > to) {
                                std::swap(from, to);
                            }
                            if (!io.KeyCtrl) {
                                state.selected.clear();
                            }
                            for (int i = from; i <= to; ++i) {
                                state.selected.insert(
                                    state.clips[hour.clips[static_cast<size_t>(i)]].start);
                            }
                        } else {
                            if (!io.KeyCtrl) {
                                state.selected.clear();
                            }
                            state.selected.insert(clip.start);
                        }
                    } else if (io.KeyCtrl) {
                        if (marked) {
                            state.selected.erase(clip.start);
                        } else {
                            state.selected.insert(clip.start);
                        }
                        state.selectAnchor = clip.start;
                    } else {
                        state.selected.clear();
                        state.selected.insert(clip.start);
                        state.selectAnchor = clip.start;
                        chosen = clip.start;
                    }
                }
                if (playing) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    if (!marked) {
                        state.selected.clear();
                        state.selected.insert(clip.start);
                        state.selectAnchor = clip.start;
                    }
                    openMenu = true;
                }
            }
            ImGui::EndListBox();

            if (openMenu) {
                ImGui::OpenPopup("##sd-clip-menu");
            }
            if (ImGui::BeginPopup("##sd-clip-menu")) {
                const size_t count = state.selected.size();
                const std::string item =
                    count <= 1 ? std::string("Save clip")
                               : std::format("Save {} clips", count);
                if (ImGui::MenuItem(item.c_str(), nullptr, false, count > 0)) {
                    std::vector<SdClip> clips;
                    clips.reserve(count);
                    for (const SdClip& clip : state.clips) {
                        if (state.selected.contains(clip.start)) {
                            clips.push_back(clip);
                        }
                    }
                    app.sdPlayer().saveClips(std::move(clips),
                                            app.config().recordingsDirectory());
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();

    return chosen;
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

void useMipSampler(const ImDrawList*, const ImDrawCmd* command) {
    auto* renderState =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    auto* sampler = static_cast<ID3D11SamplerState*>(command->UserCallbackData);
    if (renderState != nullptr && sampler != nullptr) {
        renderState->DeviceContext->PSSetSamplers(0, 1, &sampler);
    }
}

struct SdZoomState {
    bool active = false;
    ImVec2 center = ImVec2(0.5f, 0.5f);
};

SdZoomState& sdZoomState() {
    static SdZoomState state;
    return state;
}

struct SdSeekState {
    bool dragging = false;
    int64_t preview = 0;
};

SdSeekState& sdSeekState() {
    static SdSeekState state;
    return state;
}

bool dayRange(const Browser& state, int64_t& begin, int64_t& end) {
    if (state.day < 0 || state.day >= static_cast<int>(state.days.size())) {
        return false;
    }
    const Day& day = state.days[static_cast<size_t>(state.day)];
    begin = 0;
    end = 0;
    for (const Hour& hour : day.hours) {
        for (const size_t index : hour.clips) {
            if (index >= state.clips.size()) {
                continue;
            }
            const SdClip& clip = state.clips[index];
            if (begin == 0 || clip.start < begin) {
                begin = clip.start;
            }
            if (clip.end() > end) {
                end = clip.end();
            }
        }
    }
    return end > begin;
}

void revealInstant(Browser& state, int64_t instant) {
    LocalClock clock;
    const int64_t local = clock.toLocal(instant);
    const int64_t dayStart = floorTo(local, kDay);
    const int64_t hourStart = floorTo(local, kHour);
    for (int i = 0; i < static_cast<int>(state.days.size()); ++i) {
        if (state.days[static_cast<size_t>(i)].localStart != dayStart) {
            continue;
        }
        state.day = i;
        const Day& day = state.days[static_cast<size_t>(i)];
        for (int j = 0; j < static_cast<int>(day.hours.size()); ++j) {
            if (day.hours[static_cast<size_t>(j)].localStart == hourStart) {
                state.hour = j;
                return;
            }
        }
        state.hour = day.hours.empty() ? -1 : 0;
        return;
    }
}

void handleSeekKeys(App& app) {
    if (ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen(nullptr,
                           ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        sdSeekState().dragging) {
        return;
    }

    int delta = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
        delta = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        delta = 1;
    }
    if (delta == 0) {
        return;
    }

    Browser& state = browser();
    const SdPlayer::Status status = app.sdPlayer().status();
    int64_t start = 0;
    if (status.position == 0 && status.requested == 0) {
        if (state.day < 0 || state.hour < 0 ||
            state.day >= static_cast<int>(state.days.size())) {
            return;
        }
        const Day& day = state.days[static_cast<size_t>(state.day)];
        if (state.hour >= static_cast<int>(day.hours.size())) {
            return;
        }
        const Hour& hour = day.hours[static_cast<size_t>(state.hour)];
        if (hour.clips.empty()) {
            return;
        }
        const size_t index = delta > 0 ? hour.clips.front() : hour.clips.back();
        if (index < state.clips.size()) {
            start = state.clips[index].start;
            app.sdPlayer().play(start);
        }
    } else {
        start = app.sdPlayer().skipClip(delta);
    }
    if (start > 0) {
        revealInstant(state, start);
    }
}

void drawSeekBar(App& app) {
    Browser& state = browser();
    const SdPlayer::Status status = app.sdPlayer().status();
    SdSeekState& seeking = sdSeekState();

    int64_t begin = 0;
    int64_t end = 0;
    const bool haveRange = dayRange(state, begin, end);
    if (!haveRange) {
        seeking = {};
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    if (ImGui::BeginChild("##sd-seek", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
        ImGui::BeginDisabled(!haveRange);
        LocalClock clock;
        const int64_t shown = seeking.dragging ? seeking.preview
                            : (status.position > 0 ? status.position
                               : (status.requested > 0 ? status.requested : begin));
        if (haveRange) {
            ImGui::TextUnformatted(formatLocal(clock.toLocal(shown), "full").c_str());
            ImGui::SameLine();
        }

        int64_t position = haveRange ? std::clamp(shown, begin, end) : 0;
        ImGui::SetNextItemWidth(-1.0f);
        if (haveRange &&
            ImGui::SliderScalar("##sd-position", ImGuiDataType_S64, &position, &begin, &end, "",
                                ImGuiSliderFlags_NoInput)) {
            seeking.dragging = true;
            seeking.preview = position;
        }
        if (seeking.dragging && ImGui::IsItemDeactivatedAfterEdit()) {
            seeking.dragging = false;
            app.sdPlayer().play(position);
            revealInstant(state, position);
        }
        ImGui::SetItemTooltip(
            "The camera plays from the start of the clip covering this moment. "
            "Left and Right skip a clip.");
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void drawPicture(App& app, float height) {
    SdPlayer& player = app.sdPlayer();
    const SdPlayer::Status status = player.status();

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        sdZoomState().active = false;
    }

    if (ImGui::BeginChild("##sd-picture", ImVec2(0, height), ImGuiChildFlags_Borders)) {
        const ImVec2 tileOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 area = ImGui::GetContentRegionAvail();

        player.present(app.gpu(), app.sdTexture());
        ID3D11ShaderResourceView* view = app.sdTexture().view();
        bool zoomActive = false;

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
            ImVec2 drawSize;
            ImVec2 offset;
            letterbox(area.x, area.y, contentWidth, contentHeight, drawSize, offset);

            const ImVec2 imageOrigin(tileOrigin.x + offset.x, tileOrigin.y + offset.y);
            const ImVec2 imageEnd(imageOrigin.x + drawSize.x, imageOrigin.y + drawSize.y);
            const bool imageHovered = ImGui::IsMouseHoveringRect(imageOrigin, imageEnd);
            ImGuiIO& io = ImGui::GetIO();
            SdZoomState& zoom = sdZoomState();

            const float factor = app.config().liveViewZoom;
            const float visibleSpan = 1.0f / factor;
            const float halfSpan = visibleSpan * 0.5f;

            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && factor > 1.0f) {
                zoom.active = true;
                const float pointerX =
                    std::clamp((io.MousePos.x - imageOrigin.x) / drawSize.x, 0.0f, 1.0f);
                const float pointerY =
                    std::clamp((io.MousePos.y - imageOrigin.y) / drawSize.y, 0.0f, 1.0f);
                zoom.center.x = pointerX + (0.5f - pointerX) * visibleSpan;
                zoom.center.y = pointerY + (0.5f - pointerY) * visibleSpan;
            }

            zoomActive = zoom.active && ImGui::IsMouseDown(ImGuiMouseButton_Right);
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
            ImGui::Image(reinterpret_cast<ImTextureID>(view), drawSize, uv0, uv1);
            drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);

            if (imageHovered) {
                ImGui::SetMouseCursor(zoomActive ? ImGuiMouseCursor_ResizeAll
                                                 : ImGuiMouseCursor_Hand);
            }
        }
    }
    ImGui::EndChild();
}

} // namespace

void drawSdCardView(App& app) {
    syncBrowser(app);
    handleSeekKeys(app);

    drawToolbar(app);
    ImGui::Spacing();

    const float seekReserve = 48.0f;
    const float height = std::max(80.0f, ImGui::GetContentRegionAvail().y - seekReserve);

    const int64_t chosen = drawBrowser(app, height);
    ImGui::SameLine();
    drawPicture(app, height);

    if (chosen > 0) {
        app.sdPlayer().play(chosen);
    }

    drawSeekBar(app);
}

} // namespace xv
