#include "ui/Views.h"

#include <windows.h>

#include <shellapi.h>

#include <imgui.h>

#include <array>
#include <cfloat>
#include <cstring>
#include <span>

#include "app/App.h"
#include "app/Theme.h"
#include "app/Version.h"
#include "bridge/Bridge.h"
#include "config/Config.h"

namespace xv {
namespace {

// One row of a controls table: what to press, and what happens.
struct Binding {
    const char* input;
    const char* effect;
};

void bindings(const char* id, std::span<const Binding> rows) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("##effect", ImGuiTableColumnFlags_WidthStretch, 0.66f);

    for (const Binding& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(theme::kAccent, "%s", row.input);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", row.effect);
    }

    ImGui::EndTable();
    ImGui::Dummy(ImVec2(0, 10));
}

void paragraph(const char* text) {
    ImGui::TextWrapped("%s", text);
    ImGui::Dummy(ImVec2(0, 8));
}

void bullet(const char* text) {
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text);
}

// A note in the muted colour, for the exceptions that would otherwise read as
// part of the instruction above them.
void aside(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 8));
}

void drawGettingStarted() {
    paragraph("Sign in with your Mi account and pick the region your cameras are in. It has to be "
              "the region the Mi Home app uses: the wrong one gives an empty device list rather "
              "than an error, because Xiaomi keeps accounts in per-region shards.");
    paragraph("Then open Cameras, press Refresh from account, and add what you want to watch. A "
              "dual-lens CW500 offers each of its lenses separately. The grid starts streaming as "
              "soon as a camera is added, and picks it up again by itself every time the app "
              "starts.");

    ImGui::SeparatorText("Where the picture comes from");

    paragraph("Video and sound travel straight from the camera to this PC over the local network. "
              "Xiaomi's cloud is only used to sign in, list the devices, exchange the per-session "
              "keys and read or write camera settings.");
    paragraph("That also means the PC has to be on the same subnet as the cameras: they refuse "
              "peer-to-peer connections from anywhere else, and the discovery broadcast does not "
              "cross routers. A VPN or a routed VLAN will not do.");

    ImGui::SeparatorText("Around the window");

    paragraph("There is no Windows title bar. The menu bar is the title bar, with minimize, "
              "maximize and close at its right end, and dragging the empty part of it moves the "
              "window. Everything Windows does with a normal frame still works.");
    paragraph("The window opens where it was closed. Only one copy runs at a time: starting it "
              "again brings the running one to the front rather than opening a second that would "
              "compete for the cameras.");
    paragraph("This window and the log are not trapped inside it. Drag either one by its title "
              "bar past the edge of the application and it becomes a window of its own, which can "
              "be put beside the picture or on another monitor entirely. Dragging it back inside "
              "makes it part of the application again.");
    paragraph("One that has been let out stays above the application rather than disappearing "
              "behind it when the picture is clicked, and it minimizes and comes back with it. "
              "The application underneath goes on working normally while it is open.");
}

void drawControls() {
    ImGui::SeparatorText("Keyboard: the live grid");

    static constexpr std::array kGridKeys{
        Binding{"Tab / Shift+Tab", "Select the next or the previous camera, wrapping around"},
        Binding{"Arrow keys", "Pan and tilt the selected camera, moving for as long as the key is "
                              "held. A lens with no motor behind it ignores them."},
        Binding{"F", "Show only the selected camera, and go back"},
        Binding{"Esc", "Leave the single-camera view"},
        Binding{"R", "Start or stop recording the selected camera"},
        Binding{"A", "Listen to the selected camera, or mute it"},
    };
    bindings("##gridkeys", kGridKeys);

    aside("These keys belong to the cameras while the grid is on screen, which is why Tab walks "
          "the tiles here rather than the toolbar. They stand down while a text field has the "
          "keyboard or a dropdown is open, and they do nothing on the other screens.");

    ImGui::SeparatorText("Keyboard: anywhere");

    static constexpr std::array kGlobalKeys{
        Binding{"F1", "Open or close this help"},
        Binding{"Ctrl+S", "Save the configuration"},
        Binding{"Alt+F4", "Exit"},
        Binding{"Alt+Space", "The window menu Windows draws"},
        Binding{"Win + arrows", "Snap the window, as with any other window"},
    };
    bindings("##globalkeys", kGlobalKeys);

    aside("On the login, Cameras and Settings screens, Tab and Shift+Tab move between the fields "
          "and Space or Enter presses whatever is focused.");

    ImGui::SeparatorText("Mouse: the live grid");

    static constexpr std::array kGridMouse{
        Binding{"Left click a tile", "Select that camera"},
        Binding{"Double-click a tile", "Show only that camera, and go back"},
        Binding{"Hold the right button", "Magnify the live picture around the pointer"},
        Binding{"Move while holding it", "Pan around the magnified picture"},
        Binding{"Release the right button", "Back to the whole picture"},
    };
    bindings("##gridmouse", kGridMouse);

    aside("Magnification is 2x by default. live_view_zoom in config.json changes the factor, and "
          "1.0 turns it off; the right button then only selects the tile.");

    ImGui::SeparatorText("Mouse: the control pad");

    static constexpr std::array kPadMouse{
        Binding{"Hold an arrow", "Repeat a pan or tilt step for as long as it is held"},
        Binding{"Alarm", "Sound the camera's own siren, or stop it"},
        Binding{"LED", "Switch the fill light on or off"},
        Binding{"Night vision", "Pick a mode: the buttons are the ones the camera offers"},
        Binding{"Record / Listen", "The same as R and A"},
    };
    bindings("##padmouse", kPadMouse);

    aside("The pad belongs to the selected tile, and is left out of a tile too small to hold it "
          "without covering the picture. A camera that does not offer one of these settings does "
          "not show its button.");

    ImGui::SeparatorText("Mouse: the window");

    static constexpr std::array kWindowMouse{
        Binding{"Drag the empty menu bar", "Move the window"},
        Binding{"Double-click it", "Maximize, or restore"},
        Binding{"Drag an edge or corner", "Resize"},
        Binding{"Rest on maximize", "The Windows 11 snap layouts flyout"},
    };
    bindings("##windowmouse", kWindowMouse);
}

void drawRecordingAndSound() {
    ImGui::SeparatorText("Recording");

    paragraph("R, or Record on the pad, writes the selected camera's stream to a Matroska file in "
              "Videos\\XiaomiViewer. Streams -> Open recordings folder goes there, and "
              "recordings_dir in the configuration file moves it somewhere else.");
    paragraph("Nothing is re-encoded: the camera's own video and audio packets go into the "
              "container as they arrive. A recording is therefore exactly what the camera sent, at "
              "its own quality, and it costs a few percent of one core rather than a GPU encoder.");

    paragraph("Three things follow from recording the stream rather than a re-encode of it:");
    bullet("A file can only begin on a keyframe, so recording starts a second or two after being "
           "asked. The pad says starting until it does.");
    bullet("A session that drops ends its file, and the reconnection opens a new one, because two "
           "sessions do not share a timeline.");
    bullet("The audio track has to be declared when the file is created, so a recording started "
           "before the camera's first audio packet arrives is video only.");
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::SeparatorText("Listening");

    paragraph("A, or Listen on the pad, plays the selected camera's microphone. One camera is "
              "audible at a time: pressing A on another moves the sound rather than adding to it, "
              "because several cameras at once is noise nobody can pick a sound out of.");
    paragraph("The audible tile is marked AUDIO in its footer, so it can be found without going "
              "tile to tile. The volume belongs to the Windows volume mixer, where this app "
              "appears as its own session.");
    aside("Sound is recorded whether or not anyone is listening, since it is the same packets "
          "either way. Talking back is not implemented.");
}

void drawCameras() {
    ImGui::SeparatorText("The Cameras screen");

    paragraph("Refresh from account lists what the account can see. Adding a camera puts a tile on "
              "the grid and starts streaming it; removing one takes the tile away and leaves the "
              "camera alone.");
    paragraph("Quality is per camera. High picks the profile that means the full resolution on "
              "that model, which is not the same number on every model. If a camera gives no "
              "picture, or a small one, on High, pick a numbered profile under Override instead.");
    aside("Transport can be forced to UDP or TCP for a network that mishandles one of them. Auto "
          "is right almost always.");

    ImGui::SeparatorText("The Settings screen");

    paragraph("What is shown here is the camera's own MIoT specification: night vision, HDR, image "
              "flip, motion detection and its sensitivity, tracking, the fill light, the siren, "
              "the indicator LED, recording mode and SD card status. A camera that does not report "
              "a setting does not show it, and the (?) beside a name explains what it does.");
    paragraph("Every read and write is a round trip to Xiaomi's cloud, so the panel reads once "
              "when it is opened rather than polling, the controls are disabled while a write is "
              "in flight, and a write is followed by a read so what you see is what the camera "
              "accepted rather than what it was asked for.");
    aside("Refresh reads it all again. Actions that cannot be undone, such as formatting the SD "
          "card, ask first.");

    ImGui::SeparatorText("The grid");

    paragraph("Layout picks 1, 4 or 9 tiles, or Auto, which fits the cameras that are configured. "
              "In a fixed layout too small for the list, the cameras that do not fit are not drawn "
              "and cannot be selected; a larger layout brings them back.");
    paragraph("Each tile's footer carries its name and state, its resolution and how much has been "
              "received, REC with a timer while it is recording, and AUDIO when it is the camera "
              "being heard. A camera that drops is reconnected automatically, backing off between "
              "attempts.");
}

void drawTroubleshooting(App& app) {
    ImGui::SeparatorText("Nothing connects at all");

    paragraph("Windows Firewall has to let this app receive UDP. Discovery and the handshake both "
              "send from an ephemeral port and get their answer from a different port on the "
              "camera, which Windows has no outbound conversation to match and therefore drops as "
              "unsolicited.");
    paragraph("Windows normally asks on the first run, and allowing it there is enough -- provided "
              "the profile allowed is the one the camera network uses. A Wi-Fi network set to "
              "Public is not covered by a rule that allows only Private. If that prompt was "
              "dismissed, Windows recorded the refusal as a block rule, and a block rule beats an "
              "allow rule added afterwards, so that one has to be deleted first.");

    ImGui::SeparatorText("One camera will not connect");

    bullet("The log tells a camera that is switched off from replies being dropped: nothing "
           "answering at all reads as nothing on the subnet answering, while a camera that has "
           "moved or is off reads as other devices answering and that one not.");
    bullet("A camera on another subnet cannot be reached, whatever the firewall says.");
    bullet("Cameras that negotiate an older transport than CS2 are rejected with a message saying "
           "so, rather than failing silently.");
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::SeparatorText("The camera will not move");

    paragraph("With another session open to the same camera, the Mi Home app for instance, motor "
              "commands are accepted and then ignored. The picture keeps arriving, so this looks "
              "like broken pan and tilt rather than a camera that is busy. Closing the other "
              "viewer fixes it.");
    aside("Movement is stepwise because the camera only accepts move one step, and how far a step "
          "goes is the camera's decision: roughly 16 degrees on a CW400 against under 2 on a "
          "CW500. On a dual-lens camera only the primary lens has a motor.");

    ImGui::SeparatorText("The device list is empty");

    paragraph("Almost always the wrong region. Xiaomi keeps accounts in per-region shards and "
              "answers a query to the wrong one with an empty list rather than an error, so pick "
              "the region the Mi Home app uses and refresh again.");

    ImGui::SeparatorText("Where to look");

    paragraph("View -> Log shows what the app is doing as it happens, and the same lines are "
              "written to xiaomi-viewer.log in %APPDATA%\\XiaomiViewer, beside config.json. That "
              "log is the first thing to read when a camera will not connect, and the first thing "
              "to attach to a bug report.");

    if (ImGui::Button("Open configuration folder", ImVec2(230, 0))) {
        ::ShellExecuteW(nullptr, L"open", AppConfig::directory().c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open recordings folder", ImVec2(230, 0))) {
        app.openRecordingsFolder();
    }
}

void drawAbout() {
    ImGui::SeparatorText("Xiaomi Camera Viewer");

    ImGui::Text("Version %s", XV_VERSION);
    ImGui::TextColored(theme::kMuted, "Bridge %s", Bridge::instance().version().c_str());
    ImGui::TextColored(theme::kMuted, "Dear ImGui %s", ImGui::GetVersion());
    ImGui::Dummy(ImVec2(0, 10));

    paragraph("A native Windows viewer for Xiaomi CW400 and CW500 cameras, with provisional "
              "support for the CW300 and CW700S. One executable, no Docker, no WSL, no "
              "browser.");

    ImGui::SeparatorText("Privacy");

    bullet("Signing in, listing cameras and reading or writing settings go to Xiaomi's servers in "
           "the region you pick, and are handled by Xiaomi under their privacy policy.");
    bullet("Video and audio travel directly between the cameras and this PC.");
    bullet("Nothing goes to the maintainers of this project or anyone else. There is no telemetry, "
           "no crash reporting and no update check.");
    bullet("The account token is stored on this PC only, encrypted with your Windows account key "
           "so another account cannot read it.");
    ImGui::Dummy(ImVec2(0, 10));

    ImGui::SeparatorText("Credits");

    paragraph("The Xiaomi protocol implementation is derived from go2rtc by Alexey Khit, under the "
              "MIT license. This project would not exist without that reverse-engineering work. "
              "THIRD-PARTY-NOTICES.md beside the executable lists every dependency and its "
              "license.");

    ImGui::TextLinkOpenURL("go2rtc", "https://github.com/AlexxIT/go2rtc");
    ImGui::SameLine();
    ImGui::TextColored(theme::kMuted, "|");
    ImGui::SameLine();
    ImGui::TextLinkOpenURL("Xiaomi's privacy policy", "https://privacy.mi.com/all/en_US");
    ImGui::Dummy(ImVec2(0, 10));

    aside("Not affiliated with, endorsed by, or supported by Xiaomi.");
}

} // namespace

void drawHelpWindow(App& app, bool& open, const char*& tab) {
    // Sized in text rather than in pixels, so the page holds the same number of
    // words per line on a scaled display as it does on an unscaled one. Nothing
    // about the window layout is persisted between runs, so the help also opens
    // centred over the picture rather than wherever ImGui would cascade it.
    const float unit = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(unit * 64.0f, unit * 48.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(unit * 34.0f, unit * 24.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin("Help", &open)) {
        ImGui::End();
        return;
    }

    // A page asked for by the menu is brought to the front once; after that the
    // window remembers whichever page was last read, which is what someone
    // pressing F1 twice in a row expects.
    const auto flagsFor = [&tab](const char* name) -> ImGuiTabItemFlags {
        return tab != nullptr && std::strcmp(tab, name) == 0 ? ImGuiTabItemFlags_SetSelected : 0;
    };

    // Each page scrolls on its own, so moving between them does not carry one
    // page's scroll position onto another.
    const auto page = [](const char* id) {
        return ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_None);
    };

    if (ImGui::BeginTabBar("##helptabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(kHelpTabStart, nullptr, flagsFor(kHelpTabStart))) {
            if (page("##start")) {
                drawGettingStarted();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(kHelpTabControls, nullptr, flagsFor(kHelpTabControls))) {
            if (page("##controls")) {
                drawControls();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(kHelpTabRecording, nullptr, flagsFor(kHelpTabRecording))) {
            if (page("##recording")) {
                drawRecordingAndSound();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(kHelpTabCameras, nullptr, flagsFor(kHelpTabCameras))) {
            if (page("##cameras")) {
                drawCameras();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(kHelpTabTrouble, nullptr, flagsFor(kHelpTabTrouble))) {
            if (page("##trouble")) {
                drawTroubleshooting(app);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(kHelpTabAbout, nullptr, flagsFor(kHelpTabAbout))) {
            if (page("##about")) {
                drawAbout();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    tab = nullptr;
    ImGui::End();
}

} // namespace xv
