#include "cloud/Miot.h"

#include "app/Log.h"

namespace xv {
namespace {

// Property table transcribed from the isa.camera.hlmax (CW500 dual) MIoT
// specification. Service 6 is Xiaomi's grab-bag of vendor extensions, which is
// where the fill light and siren live.
const std::vector<MiotProperty> kProperties = {
    // --- Image ---
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On"}, {2, "Auto"}}},
    {"hdr-mode", "HDR", "Image", 2, 9, MiotType::Bool, true},
    {"image-rollover", "Flip image", "Image", 2, 2, MiotType::Enum, true, 0, 0, 1,
     {{0, "Normal"}, {180, "Upside down"}}},
    {"image-distortion-correction", "Distortion correction", "Image", 2, 12, MiotType::Bool, true},
    {"time-watermark", "Timestamp overlay", "Image", 2, 4, MiotType::Bool, true},

    // --- Detection ---
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true, 0, 0, 1,
     {{1, "Low"}, {2, "High"}, {3, "Highest"}}},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30, 1, {},
     "Minutes to wait before raising the same alarm again."},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String, true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String, true},
    {"human-detection", "Human detection", "Detection", 13, 4, MiotType::Bool, true},
    {"move-detection", "Frame change detection", "Detection", 13, 5, MiotType::Bool, true},

    // --- Tracking ---
    {"motion-tracking", "Track movement", "Tracking", 2, 8, MiotType::Bool, true},
    {"human-tracking", "Track people", "Tracking", 2, 10, MiotType::Bool, true},
    {"ai-frame", "Highlight detections", "Tracking", 2, 11, MiotType::Bool, true},

    // --- Lights and sound ---
    {"indicator-light", "Status LED", "Lights and sound", 3, 1, MiotType::Bool, true},
    {"hl-filllight-switch", "Fill light", "Lights and sound", 6, 16, MiotType::Bool, true},
    {"hl-filllight-lum", "Fill light brightness", "Lights and sound", 6, 11, MiotType::Int, true, 0, 100, 5},
    {"hl-audible-alarm", "Siren", "Lights and sound", 6, 10, MiotType::Bool, true},
    {"hl-nightvision-state", "Night vision active", "Lights and sound", 6, 17, MiotType::Bool, false},

    // --- Recording and storage ---
    {"recording-mode", "Recording mode", "Recording", 2, 7, MiotType::Enum, true, 0, 0, 1,
     {{0, "Continuous"}, {1, "On motion"}, {2, "Off"}}},
    {"sd-status", "SD card", "Recording", 4, 1, MiotType::Enum, false, 0, 0, 1,
     {{0, "Ready"},
      {1, "Not inserted"},
      {2, "Low space"},
      {3, "Device error"},
      {4, "Formatting"},
      {5, "Ejected"},
      {6, "Not initialised"},
      {7, "Too small"},
      {8, "Incompatible"},
      {9, "File error"}}},
    {"sd-total", "SD total (KB)", "Recording", 4, 2, MiotType::Int, false},
    {"sd-free", "SD free (KB)", "Recording", 4, 3, MiotType::Int, false},
    {"sd-used", "SD used (KB)", "Recording", 4, 4, MiotType::Int, false},

    // --- Pan and tilt ---
    {"hl-get-location", "Current position", "Pan and tilt", 6, 12, MiotType::String, false},
    {"active-fav-area", "Active preset", "Pan and tilt", 9, 2, MiotType::Int, true, 0, 64, 1, {},
     "Writing a preset number moves the lens to that saved position."},
    {"fav-area", "Saved positions", "Pan and tilt", 9, 1, MiotType::String, false},

    // --- Dual-lens ---
    {"dome-switch", "Motorised lens on", "Dual lens", 6, 25, MiotType::Bool, true},
    {"box-switch", "Fixed lens on", "Dual lens", 6, 26, MiotType::Bool, true},
    {"box-dome-linkage", "Lens linkage", "Dual lens", 6, 27, MiotType::Enum, true, 0, 0, 1,
     {{-1, "Default"}, {0, "Off"}, {1, "On"}}},
};

const std::vector<MiotAction> kActions = {
    {"ptz-calibrate", "Calibrate pan and tilt", "Pan and tilt", 2, 1, false,
     "Re-homes the motor. The lens will sweep its full range."},
    {"ptz-calibrate-x", "Calibrate horizontal only", "Pan and tilt", 2, 3},
    {"ptz-calibrate-y", "Calibrate vertical only", "Pan and tilt", 2, 4},
    {"restart-device", "Restart camera", "Maintenance", 2, 2, true,
     "The camera will be offline for around a minute."},
    {"sd-format", "Format SD card", "Maintenance", 4, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 4, 2, false},
};

} // namespace

const std::vector<MiotProperty>& cameraProperties() {
    return kProperties;
}

const std::vector<MiotAction>& cameraActions() {
    return kActions;
}

const MiotProperty* findCameraProperty(std::string_view key) {
    for (const auto& property : kProperties) {
        if (key == property.key) {
            return &property;
        }
    }
    return nullptr;
}

MiotClient::MiotClient(AccountConfig account, std::string did)
    : account_(std::move(account)), did_(std::move(did)) {}

bool MiotClient::refresh(std::string& error) {
    Json props = Json::array();
    for (const auto& property : kProperties) {
        props.push_back(Json{{"siid", property.siid}, {"piid", property.piid}});
    }

    const Json response = Bridge::instance().call("miot.get", Json{
                                                                  {"user_id", account_.userId},
                                                                  {"did", did_},
                                                                  {"props", props},
                                                              });

    if (!responseOk(response)) {
        error = responseError(response);
        return false;
    }

    const Json& result = response["result"];
    if (!result.is_array()) {
        error = "the camera returned an unexpected property list";
        return false;
    }

    std::vector<MiotValue> values;
    values.reserve(result.size());

    for (const auto& entry : result) {
        if (!entry.is_object()) {
            continue;
        }
        MiotValue value;
        value.siid = entry.value("siid", 0);
        value.piid = entry.value("piid", 0);
        value.code = entry.value("code", -1);
        // A non-zero code means this model does not implement the property, so
        // it is recorded but stays hidden in the UI.
        value.present = value.code == 0 && entry.contains("value");
        if (value.present) {
            value.value = entry["value"];
        }
        values.push_back(std::move(value));
    }

    {
        const std::scoped_lock lock(mutex_);
        values_ = std::move(values);
    }
    return true;
}

std::optional<MiotValue> MiotClient::find(int siid, int piid) const {
    const std::scoped_lock lock(mutex_);
    for (const auto& value : values_) {
        if (value.siid == siid && value.piid == piid) {
            return value;
        }
    }
    return std::nullopt;
}

bool MiotClient::supported(const MiotProperty& property) const {
    const std::optional<MiotValue> value = find(property.siid, property.piid);
    return value.has_value() && value->present;
}

bool MiotClient::write(const MiotProperty& property, const Json& value, std::string& error) {
    const Json response = Bridge::instance().call(
        "miot.set", Json{
                        {"user_id", account_.userId},
                        {"did", did_},
                        {"props", Json::array({Json{{"siid", property.siid},
                                                    {"piid", property.piid},
                                                    {"value", value}}})},
                    });

    if (!responseOk(response)) {
        error = responseError(response);
        XV_WARN("could not set {}: {}", property.key, error);
        return false;
    }

    // Reflect the change locally so the control does not snap back while the
    // next refresh is still in flight.
    for (auto& current : values_) {
        if (current.siid == property.siid && current.piid == property.piid) {
            current.value = value;
            current.present = true;
            break;
        }
    }

    XV_INFO("set {} on {}", property.key, did_);
    return true;
}

bool MiotClient::invoke(const MiotAction& action, std::string& error) {
    const Json response = Bridge::instance().call("miot.action", Json{
                                                                     {"user_id", account_.userId},
                                                                     {"did", did_},
                                                                     {"siid", action.siid},
                                                                     {"aiid", action.aiid},
                                                                     {"in", Json::array()},
                                                                 });

    if (!responseOk(response)) {
        error = responseError(response);
        XV_WARN("could not invoke {}: {}", action.key, error);
        return false;
    }

    XV_INFO("invoked {} on {}", action.key, did_);
    return true;
}

} // namespace xv
