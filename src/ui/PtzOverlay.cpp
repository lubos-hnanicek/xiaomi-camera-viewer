#include "ui/Views.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

#include "app/App.h"
#include "app/Theme.h"
#include "cloud/Miot.h"

namespace xv {
namespace {

// Which tile currently has a direction held down. Only the focused tile draws
// the pad, so a single slot is enough and it makes releasing outside the button
// still send a stop.
struct HeldMove {
    const CameraStream* stream = nullptr;
    std::string direction;
};

HeldMove& held() {
    static HeldMove state;
    return state;
}

// The settings worth reaching without leaving the picture. Everything else
// stays in the settings panel, where there is room to explain it.
constexpr const char* kAlarmKey = "hl-audible-alarm";
// Not the status indicator on the case: the button says LED but switches the
// fill light, which is the one that changes the picture.
constexpr const char* kLightKey = "hl-filllight-switch";
constexpr const char* kNightKey = "night-shot";

constexpr const char* kAlarmLabel = "Alarm";
constexpr const char* kLightLabel = "LED";
constexpr const char* kRecordLabel = "Record";
constexpr const char* kStopLabel = "Stop";
constexpr const char* kListenLabel = "Listen";
constexpr const char* kMuteLabel = "Mute";

// Draws an arrow button that reports being held rather than clicked. The camera
// moves one step per command, so the worker repeats it for as long as this stays
// held down.
bool holdButton(const char* label, ImVec2 size) {
    ImGui::Button(label, size);
    return ImGui::IsItemActive();
}

// One setting the pad offers, paired with what the camera last said about it.
struct Quick {
    const MiotProperty* property = nullptr;
    std::optional<MiotValue> value;
    bool shown = false;

    [[nodiscard]] bool on() const { return value.has_value() && value->asBool(); }
    [[nodiscard]] int current() const { return value.has_value() ? value->asInt() : -1; }
};

struct Controls {
    Quick alarm;
    Quick light;
    Quick night;
    bool unavailable = false; // the camera's settings could not be read at all
    bool locked = false;      // read in progress, or a write still going out

    // Not a camera setting: recording happens here, on this machine. It shares
    // the pad because it belongs to the same question as the rest of it -- what
    // to do about what is on screen right now.
    bool arrows = false;   // this lens can be pointed
    bool recording = false;
    bool recordingLive = false; // a file is actually open, not just requested
    std::string recordingLabel;
    std::string recordingError;
    std::string recordingFile;

    // Listening, which is the same kind of thing: local, immediate, and about
    // the camera on screen rather than a setting the camera keeps.
    bool listening = false;
    std::string audioFormat; // what the camera sends, empty until it sends any
    std::string audioError;  // why there is no sound, about the PC's speakers
};

Quick quickControl(const CameraStream& stream, const char* key) {
    Quick quick;
    quick.property = findCameraProperty(key);
    if (quick.property == nullptr) {
        return quick;
    }
    if (stream.miot) {
        quick.value = stream.miot->find(quick.property->siid, quick.property->piid);
    }
    // Dropped only once the camera has actually turned it down. Until the first
    // read lands everything is drawn, disabled, so the pad does not change size
    // the moment the answer arrives.
    quick.shown = !stream.miotLoaded || (quick.value.has_value() && quick.value->present);
    return quick;
}

Controls readControls(const App& app, const CameraStream& stream) {
    Controls controls;
    controls.arrows = stream.config.motorised();

    if (stream.worker) {
        const StreamWorker::Status status = stream.worker->status();
        controls.recording = stream.worker->recordingRequested();
        controls.recordingLive = status.recording;

        controls.listening = status.audible;
        controls.audioFormat = status.audio;
        if (controls.listening) {
            controls.audioError = app.audioError();
        }

        controls.recordingError = status.recordingError;
        controls.recordingFile = status.recordingPath;

        if (status.recording) {
            const auto seconds = status.recordedMs / 1000;
            const double megabytes = static_cast<double>(status.recordedBytes) / (1024.0 * 1024.0);
            controls.recordingLabel =
                std::format("{}:{:02} - {:.0f} MB", seconds / 60, seconds % 60, megabytes);
        } else if (controls.recording) {
            // Kept short because it has to fit beside the button. What it is
            // waiting for is a keyframe, which the tooltip says.
            controls.recordingLabel = "starting";
        }
    }

    controls.unavailable = !stream.miotError.empty();
    controls.locked = stream.miotTask.busy() || !stream.miotLoaded;
    if (controls.unavailable) {
        return controls;
    }

    controls.alarm = quickControl(stream, kAlarmKey);
    controls.light = quickControl(stream, kLightKey);
    controls.night = quickControl(stream, kNightKey);
    return controls;
}

// How big everything in the pad is.
//
// Measured rather than hardcoded, so the box cannot end up smaller than the
// contents it has to hold, and so it still fits when the UI scale grows the font
// and the frame padding along with it, or when a camera offers only some of the
// controls.
struct PadMetrics {
    ImVec2 padding{10.0f, 8.0f};
    ImVec2 spacing{4.0f, 4.0f};
    ImVec2 arrow;
    ImVec2 toggle;
    ImVec2 segment;
    ImVec2 record;
    ImVec2 listen;
    ImVec2 size;
};

float buttonWidthFor(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float nightButtonWidthFor(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 1.5f;
}

PadMetrics measurePad(const Controls& controls) {
    PadMetrics m;

    const float arrow = std::round(ImGui::GetFontSize() * 2.4f);
    m.arrow = ImVec2(arrow, arrow);

    const float frame = ImGui::GetFrameHeight();
    const float text = ImGui::GetTextLineHeight();

    const bool toggles = controls.alarm.shown || controls.light.shown;
    const auto segments =
        controls.night.shown ? controls.night.property->options.size() : size_t{0};

    // The two buttons keep their widths whichever labels they show. The recording
    // status below them is measured from a specimen so the pad does not move as
    // its elapsed time and file size change.
    m.record = ImVec2(std::max(buttonWidthFor(kRecordLabel), buttonWidthFor(kStopLabel)), frame);
    m.listen = ImVec2(std::max(buttonWidthFor(kListenLabel), buttonWidthFor(kMuteLabel)), frame);

    // Wide enough for the widest row, so nothing has to be cut short.
    float width = m.record.x + m.spacing.x + m.listen.x;
    if (!controls.recordingLabel.empty()) {
        const float dotAndGap = text * 0.52f + 6.0f;
        width = std::max(width, dotAndGap + ImGui::CalcTextSize("000:00 - 0000 MB").x);
    }
    if (controls.arrows) {
        width = std::max(width, arrow * 3.0f + m.spacing.x * 2.0f);
    }
    if (toggles) {
        width = std::max(width, (std::max(buttonWidthFor(kAlarmLabel),
                                          buttonWidthFor(kLightLabel))) *
                                        2.0f +
                                    m.spacing.x);
    }
    if (segments > 0) {
        float segment = 0.0f;
        for (const MiotEnumOption& option : controls.night.property->options) {
            segment = std::max(segment, nightButtonWidthFor(option.label));
        }
        width = std::max(width, segment * static_cast<float>(segments) +
                                   m.spacing.x * static_cast<float>(segments - 1));
    }

    // Stacked down the pad, with one spacing between each neighbouring pair.
    float content = 0.0f;
    int rows = 0;
    if (controls.arrows) {
        content += arrow * 3.0f;
        rows += 3;
    }
    if (controls.unavailable) {
        content += text;
        rows += 1;
    }
    if (toggles) {
        content += frame;
        rows += 1;
    }
    if (segments > 0) {
        content += text + frame; // the "Night vision" label and its buttons
        rows += 2;
    }
    content += frame; // the record and listen buttons, which every camera has
    rows += 1;
    if (!controls.recordingLabel.empty()) {
        content += text + 2.0f;
        rows += 1;
    }
    content += m.spacing.y * static_cast<float>(std::max(rows - 1, 0));

    m.toggle = ImVec2((width - m.spacing.x) * 0.5f, frame);
    if (segments > 0) {
        m.segment = ImVec2(
            (width - m.spacing.x * static_cast<float>(segments - 1)) / static_cast<float>(segments),
            frame);
    }
    m.size = ImVec2(width + m.padding.x * 2.0f, content + m.padding.y * 2.0f);
    return m;
}

// A button that stays lit while the setting is on, so the state is readable at
// a glance without a label spelling it out.
bool litButton(const char* label, bool lit, ImVec2 size, ImVec4 tint) {
    if (lit) {
        ImGui::PushStyleColor(ImGuiCol_Button, tint);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(tint.x * 1.15f, tint.y * 1.15f, tint.z * 1.15f, tint.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, tint);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
    }
    const bool pressed = ImGui::Button(label, size);
    if (lit) {
        ImGui::PopStyleColor(4);
    }
    return pressed;
}

void drawQuickControls(App& app, CameraStream& stream, const PadMetrics& pad,
                       const Controls& controls) {
    if (controls.unavailable) {
        ImGui::TextColored(theme::kFailed, "Controls unavailable");
        ImGui::SetItemTooltip("%s", stream.miotError.c_str());
        return;
    }

    ImGui::BeginDisabled(controls.locked);

    const bool both = controls.alarm.shown && controls.light.shown;
    const ImVec2 toggleSize = both ? pad.toggle : ImVec2(-1.0f, pad.toggle.y);

    if (controls.alarm.shown) {
        // Red, because this one is a siren and setting it off by accident is
        // worth making obvious.
        if (litButton(kAlarmLabel, controls.alarm.on(), toggleSize, theme::kFailed)) {
            app.writeSetting(stream, *controls.alarm.property, Json(!controls.alarm.on()));
        }
        ImGui::SetItemTooltip("The camera's audible alarm");
    }

    if (controls.light.shown) {
        if (both) {
            ImGui::SameLine();
        }
        if (litButton(kLightLabel, controls.light.on(), toggleSize, theme::kAccent)) {
            app.writeSetting(stream, *controls.light.property, Json(!controls.light.on()));
        }
        ImGui::SetItemTooltip("The camera's fill light");
    }

    if (controls.night.shown) {
        ImGui::TextColored(theme::kMuted, "Night vision");

        const auto& options = controls.night.property->options;
        for (size_t i = 0; i < options.size(); ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            const bool active = options[i].value == controls.night.current();
            if (litButton(options[i].label, active, pad.segment, theme::kAccent) && !active) {
                app.writeSetting(stream, *controls.night.property, Json(options[i].value));
            }
        }
    }

    ImGui::EndDisabled();
}

void drawListenControl(App& app, CameraStream& stream, const PadMetrics& pad,
                       const Controls& controls) {
    // A camera that is not sending audio has nothing to listen to, and a button
    // that would silently do nothing is worse than one that says why.
    const bool silent = controls.audioFormat.empty();

    ImGui::BeginDisabled(silent);
    if (litButton(controls.listening ? kMuteLabel : kListenLabel, controls.listening, pad.listen,
                  theme::kAccent)) {
        app.toggleListening(stream);
    }
    ImGui::EndDisabled();

    if (silent) {
        ImGui::SetItemTooltip("This camera is not sending any audio");
    } else if (!controls.audioError.empty()) {
        ImGui::SetItemTooltip("No sound: %s", controls.audioError.c_str());
    } else if (controls.listening) {
        ImGui::SetItemTooltip("Listening to %s. Use the Windows volume mixer to set the level.",
                              controls.audioFormat.c_str());
    } else {
        ImGui::SetItemTooltip("Hear this camera (%s). Only one camera plays at a time.",
                              controls.audioFormat.c_str());
    }
}

void drawRecordControl(App& app, CameraStream& stream, const PadMetrics& pad,
                       const Controls& controls) {
    const float rowStart = ImGui::GetCursorPosX();
    const float rowWidth = ImGui::GetContentRegionAvail().x;

    if (litButton(controls.recording ? kStopLabel : kRecordLabel, controls.recording, pad.record,
                  theme::kFailed)) {
        app.toggleRecording(stream);
    }

    if (!controls.recordingError.empty()) {
        ImGui::SetItemTooltip("Recording stopped: %s", controls.recordingError.c_str());
    } else if (controls.recordingLive) {
        ImGui::SetItemTooltip("Writing %s", controls.recordingFile.c_str());
    } else if (controls.recording) {
        ImGui::SetItemTooltip("Waiting for a keyframe, which is where the file has to start");
    } else {
        ImGui::SetItemTooltip("Save this stream to a Matroska file, exactly as the camera "
                              "sends it, with its audio");
    }

    ImGui::SameLine(rowStart + rowWidth - pad.listen.x);
    drawListenControl(app, stream, pad, controls);

    if (controls.recordingLabel.empty()) {
        return;
    }

    ImGui::AlignTextToFramePadding();

    if (!controls.recordingLive) {
        ImGui::TextColored(theme::kPending, "%s", controls.recordingLabel.c_str());
        return;
    }

    // The dot is what makes a recording pad recognisable at a glance; the rest is
    // for deciding whether to stop.
    const float line = ImGui::GetTextLineHeight();
    const float radius = line * 0.26f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cursor.x + radius, cursor.y + line * 0.5f),
                                                radius, ImGui::GetColorU32(theme::kFailed));
    ImGui::Dummy(ImVec2(radius * 2.0f, line));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(theme::kMuted, "%s", controls.recordingLabel.c_str());
}

} // namespace

void drawPtzOverlay(App& app, CameraStream& stream, float x, float y, float width, float height) {
    // The pad shows live camera state, so looking at it is what triggers the
    // first read. Same on-demand rule the settings panel follows, for the same
    // reason: every refresh is a round trip to Xiaomi's cloud.
    if (!stream.miotLoaded && !stream.miotTask.busy() && stream.miotError.empty()) {
        app.loadSettingsFor(stream);
    }

    const Controls controls = readControls(app, stream);
    const PadMetrics pad = measurePad(controls);

    // On a small tile the pad would cover the picture it is meant to aim, so it
    // is simply not drawn.
    if (width < pad.size.x + 40.0f || height < pad.size.y + 40.0f) {
        return;
    }

    ImGui::PushID(&stream);

    const ImVec2 origin(x + 14.0f, y + height - pad.size.y - 14.0f);
    ImGui::SetCursorScreenPos(origin);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.07f, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad.padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, pad.spacing);

    if (ImGui::BeginChild("##ptz", pad.size,
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (controls.arrows) {
            const ImVec2 buttonSize = pad.arrow;

            std::string direction;

            // Up
            ImGui::Dummy(ImVec2(pad.arrow.x, 0));
            ImGui::SameLine();
            if (holdButton("^##up", buttonSize)) {
                direction = "up";
            }

            // Left, centre, right
            if (holdButton("<##left", buttonSize)) {
                direction = "left";
            }
            ImGui::SameLine();
            ImGui::Dummy(buttonSize);
            ImGui::SameLine();
            if (holdButton(">##right", buttonSize)) {
                direction = "right";
            }

            // Down
            ImGui::Dummy(ImVec2(pad.arrow.x, 0));
            ImGui::SameLine();
            if (holdButton("v##down", buttonSize)) {
                direction = "down";
            }

            // The hold is renewed every frame rather than only on the first one.
            // The worker keeps stepping while it is renewed and gives up shortly
            // after the renewals stop, so a pad that disappears mid-press -- the
            // stream drops, the tile loses focus -- cannot leave the camera
            // turning.
            HeldMove& state = held();
            const bool isThisTile = state.stream == &stream;

            if (!direction.empty()) {
                state.stream = &stream;
                state.direction = direction;
                if (stream.worker) {
                    stream.worker->holdPtz(direction);
                }
            } else if (isThisTile && !state.direction.empty()) {
                state.direction.clear();
                state.stream = nullptr;
                if (stream.worker) {
                    stream.worker->releasePtz();
                }
            }
        }

        drawQuickControls(app, stream, pad, controls);
        drawRecordControl(app, stream, pad, controls);
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

} // namespace xv
