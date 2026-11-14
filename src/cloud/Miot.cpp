#include "cloud/Miot.h"

#include <chrono>
#include <thread>

#include "app/Log.h"

namespace xv {
namespace {

// Property table transcribed from the CW400/CW500 MIoT layouts. Service 6 is
// Xiaomi's grab-bag of vendor extensions, which is where the fill light and
// siren live.
const std::vector<MiotProperty> kDefaultProperties = {
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

const std::vector<MiotAction> kDefaultActions = {
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

const std::vector<MiotProperty> kC400Properties = {
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"image-rollover", "Flip image", "Image", 2, 2, MiotType::Enum, true, 0, 0, 1,
     {{0, "Normal"}, {180, "Upside down"}}},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "On"}, {1, "Off"}, {2, "Auto"}}},
    {"glimmer-full-color", "Low-light color", "Image", 2, 6, MiotType::Bool, true},
    {"recording-mode", "Recording mode", "Recording", 2, 7, MiotType::Enum, true, 0, 0, 1,
     {{0, "Continuous"}, {1, "On motion"}, {2, "Off"}}},
    {"motion-tracking", "Track movement", "Tracking", 2, 8, MiotType::Bool, true},
    {"local-storage", "Local storage", "Recording", 2, 9, MiotType::Bool, true},
    {"indicator-light", "Status LED", "Lights and sound", 3, 1, MiotType::Bool, true},
    {"sd-total", "SD total (bytes)", "Recording", 4, 2, MiotType::Int, false, 0, 0, 1, {}, nullptr, MiotUnit::Bytes},
    {"sd-free", "SD free (bytes)", "Recording", 4, 3, MiotType::Int, false, 0, 0, 1, {}, nullptr, MiotUnit::Bytes},
    {"sd-used", "SD used (bytes)", "Recording", 4, 4, MiotType::Int, false, 0, 0, 1, {}, nullptr, MiotUnit::Bytes},
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Low"}, {1, "High"}}},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String, true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String, true},
    {"baby-wake-switch", "Baby wake detection", "Detection", 7, 3, MiotType::Bool, true},
    {"babycry-switch", "Baby crying detection", "Detection", 7, 5, MiotType::Bool, true},
};

const std::vector<MiotAction> kC400Actions = {
    {"sd-format", "Format SD card", "Maintenance", 4, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 4, 2, false},
};

const std::vector<MiotProperty> kC500ProProperties = {
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"image-rollover", "Flip image", "Image", 2, 2, MiotType::Enum, true, 0, 180, 180,
     {{0, "Normal"}, {180, "Upside down"}}},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "On"}, {1, "Off"}, {2, "Auto"}}},
    {"time-watermark", "Timestamp overlay", "Image", 2, 4, MiotType::Bool, true},
    {"glimmer-full-color", "Low-light color", "Image", 2, 6, MiotType::Bool, true},
    {"recording-mode", "Recording mode", "Recording", 2, 7, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On motion"}, {2, "Continuous"}}},
    {"human-tracking", "Track people", "Tracking", 2, 11, MiotType::Bool, true},
    {"hdr-mode", "HDR", "Image", 2, 13, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On"}, {2, "Auto"}}},
    {"image-distortion-correction", "Distortion correction", "Image", 2, 14, MiotType::Bool, true},
    {"indicator-light", "Status LED", "Lights and sound", 3, 1, MiotType::Bool, true},
    {"sd-status", "SD card", "Recording", 4, 1, MiotType::Enum, false, 0, 0, 1,
     {{0, "Ready"}, {1, "Not inserted"}, {2, "Full"}, {3, "Damaged"},
      {4, "Formatting"}, {5, "Ejected"}, {6, "Repairing"}, {7, "Ejecting"},
      {8, "Not initialised"}, {9, "Not enough space"}, {10, "File error"},
      {11, "Incompatible"}}},
    {"sd-total", "SD total (KB)", "Recording", 4, 2, MiotType::Int, false},
    {"sd-free", "SD free (KB)", "Recording", 4, 3, MiotType::Int, false},
    {"sd-used", "SD used (KB)", "Recording", 4, 4, MiotType::Int, false},
    {"remaining-recording-duration", "Remaining recording time (minutes)", "Recording", 4, 6,
     MiotType::Int, false},
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Low"}, {2, "High"}}},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String, true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String, true},
    {"babycry-detection", "Baby crying detection", "Detection", 8, 10, MiotType::Bool, true},
    {"sound-detection", "Sound detection", "Detection", 8, 11, MiotType::Bool, true},
    {"pet-detection", "Pet detection", "Detection", 8, 12, MiotType::Bool, true},
    {"fav-area", "Saved positions", "Pan and tilt", 10, 1, MiotType::String, true},
    {"active-fav-area", "Active preset", "Pan and tilt", 10, 2, MiotType::Int, true, 0, 64, 1,
     {}, "Writing a preset number moves the lens to that saved position."},
    {"cruise-switch", "Cruise", "Pan and tilt", 11, 1, MiotType::Bool, true},
    {"cruise-cycle", "Cruise cycle (minutes)", "Pan and tilt", 11, 2, MiotType::Int, true, 0, 1440},
    {"cruise-start-time", "Cruise starts", "Pan and tilt", 11, 3, MiotType::String, true},
    {"cruise-end-time", "Cruise ends", "Pan and tilt", 11, 4, MiotType::String, true},
    {"cruise-repetition", "Cruise repetition", "Pan and tilt", 11, 5, MiotType::String, true},
    {"cruise-mode", "Cruise mode", "Pan and tilt", 11, 6, MiotType::Enum, true, 0, 0, 1,
     {{0, "Panoramic"}, {1, "Fixed point"}}},
    {"cruise-position", "Cruise positions", "Pan and tilt", 11, 7, MiotType::String, true},
    {"smart-care", "Smart care", "Detection", 13, 1, MiotType::Bool, true},
    {"smart-care-area", "Smart care area", "Detection", 13, 2, MiotType::String, true},
    {"smart-care-mode", "Smart care mode", "Detection", 13, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Key areas"}, {2, "No movement"}}},
    {"smart-care-time-period", "Smart care active period", "Detection", 13, 4,
     MiotType::String, true},
};

const std::vector<MiotAction> kC500ProActions = {
    {"sd-format", "Format SD card", "Maintenance", 4, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 4, 2, false},
};

// These catalogued C500 revisions share the same service and property ids.
const std::vector<MiotProperty> kMxiangC500Properties = {
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On"}, {2, "Auto"}}},
    {"image-rollover", "Flip image", "Image", 2, 4, MiotType::Enum, true, 0, 0, 1,
     {{0, "Normal"}, {90, "90 degrees"}, {180, "Upside down"}, {270, "270 degrees"}}},
    {"time-watermark", "Timestamp overlay", "Image", 2, 5, MiotType::Bool, true},
    {"glimmer-full-color", "Low-light color", "Image", 2, 7, MiotType::Bool, true},
    {"motion-tracking", "Track movement", "Tracking", 2, 8, MiotType::Bool, true},
    {"human-tracking", "Track people", "Tracking", 2, 9, MiotType::Bool, true},
    {"recording-mode", "Recording mode", "Recording", 2, 11, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On motion"}, {2, "Continuous"}}},
    {"local-storage", "Local storage", "Recording", 2, 12, MiotType::Bool, true},
    {"hdr-mode", "HDR", "Image", 2, 13, MiotType::Bool, true},
    {"image-distortion-correction", "Distortion correction", "Image", 2, 14, MiotType::Bool, true},
    {"auto-sound-light-warning", "Automatic sound and light warning", "Lights and sound", 2, 15, MiotType::Bool, true},
    {"hl-audible-alarm", "Siren and warning light", "Lights and sound", 2, 16, MiotType::Bool, true},
    {"sd-total", "SD total (KB)", "Recording", 3, 2, MiotType::Int, false},
    {"sd-free", "SD free (KB)", "Recording", 3, 3, MiotType::Int, false},
    {"sd-used", "SD used (KB)", "Recording", 3, 4, MiotType::Int, false},
    {"indicator-light", "Status LED", "Lights and sound", 4, 1, MiotType::Bool, true},
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Low"}, {1, "Medium"}, {2, "High"}}},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String, true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String, true},
    {"pet-detection", "Pet detection", "Detection", 8, 3, MiotType::Bool, true},
    {"human-detection", "Human detection", "Detection", 8, 6, MiotType::Bool, true},
    {"move-detection", "Frame change detection", "Detection", 8, 7, MiotType::Bool, true},
    {"babycry-detection", "Baby crying detection", "Detection", 8, 8, MiotType::Bool, true},
    {"pet-tracking", "Track pets", "Tracking", 8, 21, MiotType::Bool, true},
};

const std::vector<MiotProperty> kMxiangC500CnEnProperties = {
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"image-rollover", "Flip image", "Image", 2, 2, MiotType::Enum, true, 0, 0, 1,
     {{0, "Normal"}, {90, "90 degrees"}, {180, "Upside down"}, {270, "270 degrees"}}},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On"}, {2, "Auto"}}},
    {"time-watermark", "Timestamp overlay", "Image", 2, 4, MiotType::Bool, true},
    {"glimmer-full-color", "Low-light color", "Image", 2, 6, MiotType::Bool, true},
    {"recording-mode", "Recording mode", "Recording", 2, 7, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On motion"}, {2, "Continuous"}}},
    {"motion-tracking", "Track movement", "Tracking", 2, 8, MiotType::Bool, true},
    {"local-storage", "Local storage", "Recording", 2, 9, MiotType::Bool, true},
    {"human-tracking", "Track people", "Tracking", 2, 11, MiotType::Bool, true},
    {"image-distortion-correction", "Distortion correction", "Image", 2, 14, MiotType::Bool, true},
    {"auto-sound-light-warning", "Automatic sound and light warning", "Lights and sound", 2, 15, MiotType::Bool, true},
    {"hl-audible-alarm", "Siren and warning light", "Lights and sound", 2, 16, MiotType::Bool, true},
    {"sd-total", "SD total (KB)", "Recording", 4, 2, MiotType::Int, false},
    {"sd-free", "SD free (KB)", "Recording", 4, 3, MiotType::Int, false},
    {"sd-used", "SD used (KB)", "Recording", 4, 4, MiotType::Int, false},
    {"indicator-light", "Status LED", "Lights and sound", 3, 1, MiotType::Bool, true},
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Low"}, {1, "Medium"}, {2, "High"}}},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String, true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String, true},
    {"pet-detection", "Pet detection", "Detection", 8, 5, MiotType::Bool, true},
    {"human-detection", "Human detection", "Detection", 8, 8, MiotType::Bool, true},
    {"move-detection", "Frame change detection", "Detection", 8, 9, MiotType::Bool, true},
    {"babycry-detection", "Baby crying detection", "Detection", 8, 10, MiotType::Bool, true},
    {"pet-tracking", "Track pets", "Tracking", 8, 22, MiotType::Bool, true},
    {"human-close-up", "Human close-up", "Tracking", 8, 23, MiotType::Bool, true},
    {"warning-light-switch", "Warning light", "Lights and sound", 18, 3, MiotType::Bool, true},
    {"warning-audio-switch", "Warning siren", "Lights and sound", 18, 4, MiotType::Bool, true},
};

const std::vector<MiotProperty> kXiaomiC500aProperties = {
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"}, {1, "On"}, {2, "Auto"}}},
    {"image-rollover", "Flip image", "Image", 2, 4, MiotType::Enum, true, 0, 0, 1,
     {{0, "Normal"}, {90, "90 degrees"}, {180, "Upside down"}, {270, "270 degrees"}}},
    {"time-watermark", "Timestamp overlay", "Image", 2, 5, MiotType::Bool, true},
    {"glimmer-full-color", "Low-light color", "Image", 2, 7, MiotType::Bool, true},
    {"human-tracking", "Track people", "Tracking", 2, 9, MiotType::Bool, true},
    {"recording-mode", "Recording mode", "Recording", 2, 11, MiotType::Enum, true, 0, 0, 1,
     {{0, "Continuous"}, {1, "On motion"}, {2, "Off"}}},
    {"sd-total", "SD total (KB)", "Recording", 3, 2, MiotType::Int, false},
    {"sd-free", "SD free (KB)", "Recording", 3, 3, MiotType::Int, false},
    {"sd-used", "SD used (KB)", "Recording", 3, 4, MiotType::Int, false},
    {"indicator-light", "Status LED", "Lights and sound", 4, 1, MiotType::Bool, true},
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Low"}, {1, "Medium"}, {2, "High"}}},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String, true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String, true},
    {"pet-detection", "Pet detection", "Detection", 8, 3, MiotType::Bool, true},
    {"human-detection", "Human detection", "Detection", 8, 6, MiotType::Bool, true},
    {"move-detection", "Frame change detection", "Detection", 8, 7, MiotType::Bool, true},
    {"pet-tracking", "Track pets", "Tracking", 8, 22, MiotType::Bool, true},
    {"human-close-up", "Human close-up", "Tracking", 8, 23, MiotType::Bool, true},
    {"ai-detection-sensitivity", "AI sensitivity", "Detection", 8, 28, MiotType::Enum, true, 0, 0, 1,
     {{0, "Low"}, {1, "Medium"}, {2, "High"}}},
};

const std::vector<MiotAction> kMxiangC500Actions = {
    {"ptz-calibrate", "Calibrate pan and tilt", "Pan and tilt", 2, 1, false,
     "Re-homes the motor. The lens will sweep its full range."},
    {"restart-device", "Restart camera", "Maintenance", 2, 2, true,
     "The camera will be offline for around a minute."},
    {"sd-format", "Format SD card", "Maintenance", 3, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 3, 2, false},
};

const std::vector<MiotAction> kMxiangC500CnEnActions = {
    {"ptz-calibrate", "Calibrate pan and tilt", "Pan and tilt", 2, 1, false,
     "Re-homes the motor. The lens will sweep its full range."},
    {"restart-device", "Restart camera", "Maintenance", 2, 2, true,
     "The camera will be offline for around a minute."},
    {"sd-format", "Format SD card", "Maintenance", 4, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 4, 2, false},
};

const std::vector<MiotProperty> kNoProperties;
const std::vector<MiotAction> kNoActions;

bool usesDefaultProperties(const std::string& model) {
    return isDualLens(model) || model.find("hlc8") != std::string::npos;
}

std::vector<MiotProperty> cw300Properties(int humanDetectionPiid, int moveDetectionPiid) {
    // The Chinese moc001 and global/EU moc006 specifications agree on this
    // layout except for the two AI-detection property ids passed in here.
    return {
        // --- Image ---
        {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
        {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
         {{0, "Off"}, {1, "On"}, {2, "Auto"}}},
        {"image-rollover", "Rotate image", "Image", 2, 4, MiotType::Enum, true, 0, 0, 1,
         {{0, "Normal"}, {90, "90 degrees"}, {180, "Upside down"}, {270, "270 degrees"}}},
        {"time-watermark", "Timestamp overlay", "Image", 2, 5, MiotType::Bool, true},
        {"image-distortion-correction", "Distortion correction", "Image", 2, 14,
         MiotType::Bool, true},

        // --- Detection ---
        {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
        {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true,
         0, 0, 1, {{0, "Low"}, {1, "Medium"}, {2, "High"}}},
        {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30,
         1, {}, "Minutes to wait before raising the same alarm again."},
        {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String,
         true},
        {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String,
         true},
        {"human-detection", "Human detection", "Detection", 8, humanDetectionPiid,
         MiotType::Bool, true},
        {"move-detection", "Frame change detection", "Detection", 8, moveDetectionPiid,
         MiotType::Bool, true},

        // --- Tracking ---
        {"human-tracking", "Track people", "Tracking", 2, 9, MiotType::Bool, true},
        {"ai-frame", "Highlight detections", "Tracking", 2, 10, MiotType::Bool, true},

        // --- Lights and sound ---
        {"indicator-light", "Status LED", "Lights and sound", 4, 1, MiotType::Bool, true},
        {"hl-filllight-switch", "Fill light", "Lights and sound", 16, 1, MiotType::Bool,
         true},
        {"auto-sound-light-warning", "Automatic sound and light warning",
         "Lights and sound", 2, 15, MiotType::Bool, true},
        // Keep the established key so the live-view Alarm button uses this
        // model's manual warning switch.
        {"hl-audible-alarm", "Siren and warning light", "Lights and sound", 2, 16,
         MiotType::Bool, true},

        // --- Recording and storage ---
        {"recording-mode", "Recording mode", "Recording", 2, 11, MiotType::Enum, true, 0,
         0, 1, {{0, "Off"}, {1, "On motion"}, {2, "Continuous"}}},
        {"sd-status", "SD card status", "Recording", 3, 1, MiotType::Int, false},
        {"sd-total", "SD total (KB)", "Recording", 3, 2, MiotType::Int, false},
        {"sd-free", "SD free (KB)", "Recording", 3, 3, MiotType::Int, false},
        {"sd-used", "SD used (KB)", "Recording", 3, 4, MiotType::Int, false},
    };
}

const std::vector<MiotProperty> kCw300Moc001Properties = cw300Properties(14, 15);
const std::vector<MiotProperty> kCw300Moc006Properties = cw300Properties(22, 23);

// Property ids and enum values from the public isa.camera.700sa specification.
// Although the CW700S has wide and telephoto sensors, Xiaomi presents them as
// one hybrid-zoom camera and does not publish independent lens switches.
const std::vector<MiotProperty> kCw700sProperties = {
    // --- Image ---
    {"on", "Camera on", "Image", 2, 1, MiotType::Bool, true},
    {"night-shot", "Night vision", "Image", 2, 3, MiotType::Enum, true, 0, 0, 1,
     {{0, "Off"},
      {1, "On"},
      {2, "Auto"},
      {3, "Auto full colour"},
      {4, "Full colour"},
      {5, "Smart full colour"}}},
    {"image-rollover", "Rotate image", "Image", 2, 2, MiotType::Enum, true, 0, 0, 1,
     {{0, "Normal"}, {90, "90 degrees"}, {180, "Upside down"}, {270, "270 degrees"}}},
    {"image-distortion-correction", "Distortion correction", "Image", 2, 12,
     MiotType::Bool, true},
    {"time-watermark", "Timestamp overlay", "Image", 2, 4, MiotType::Bool, true},

    // --- Detection ---
    {"motion-detection", "Motion detection", "Detection", 5, 1, MiotType::Bool, true},
    {"detection-sensitivity", "Sensitivity", "Detection", 5, 3, MiotType::Enum, true,
     0, 0, 1, {{1, "Low"}, {2, "Medium"}, {3, "High"}}},
    {"alarm-interval", "Alarm interval", "Detection", 5, 2, MiotType::Int, true, 1, 30,
     1, {}, "Minutes to wait before raising the same alarm again."},
    {"motion-detection-start-time", "Active from", "Detection", 5, 4, MiotType::String,
     true},
    {"motion-detection-end-time", "Active until", "Detection", 5, 5, MiotType::String,
     true},
    {"human-detection", "Human detection", "Detection", 13, 4, MiotType::Bool, true},
    {"move-detection", "Frame change detection", "Detection", 13, 5, MiotType::Bool,
     true},

    // --- Tracking ---
    {"motion-tracking", "Track movement", "Tracking", 2, 8, MiotType::Bool, true},
    {"human-tracking", "Track people", "Tracking", 2, 10, MiotType::Bool, true},
    {"ai-frame", "Highlight detections", "Tracking", 2, 11, MiotType::Bool, true},

    // --- Lights and sound ---
    {"indicator-light", "Status LED", "Lights and sound", 3, 1, MiotType::Bool, true},
    {"hl-filllight-switch", "Fill light", "Lights and sound", 6, 16, MiotType::Bool,
     true},
    {"hl-filllight-lum", "Fill light brightness", "Lights and sound", 6, 11,
     MiotType::Int, true, 0, 100, 5},
    {"hl-audible-alarm", "Siren", "Lights and sound", 6, 10, MiotType::Bool, true},
    {"hl-nightvision-state", "Night vision active", "Lights and sound", 6, 17,
     MiotType::Bool, false},

    // --- Recording and storage ---
    {"recording-mode", "Recording mode", "Recording", 2, 7, MiotType::Enum, true, 0,
     0, 1, {{0, "Off"}, {1, "On motion"}, {2, "Continuous"}}},
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
    {"hl-get-location", "Current position", "Pan and tilt", 6, 12, MiotType::String,
     false},
    {"active-fav-area", "Active preset", "Pan and tilt", 9, 2, MiotType::Int, true, 0,
     64, 1, {}, "Writing a preset number moves the lens to that saved position."},
    {"fav-area", "Saved positions", "Pan and tilt", 9, 1, MiotType::String, false},
};

const std::vector<MiotAction> kCw300Actions = {
    {"ptz-calibrate", "Calibrate pan and tilt", "Pan and tilt", 2, 1, false,
     "Re-homes the motor. The lens will sweep its full range."},
    {"restart-device", "Restart camera", "Maintenance", 2, 2, true,
     "The camera will be offline for around a minute."},
    {"sd-format", "Format SD card", "Maintenance", 3, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 3, 2, false},
};

const std::vector<MiotAction> kCw700sActions = {
    {"ptz-calibrate", "Calibrate pan and tilt", "Pan and tilt", 2, 1, false,
     "Re-homes the motor. The lens will sweep its full range."},
    {"restart-device", "Restart camera", "Maintenance", 2, 2, true,
     "The camera will be offline for around a minute."},
    {"sd-format", "Format SD card", "Maintenance", 4, 1, true,
     "Erases every recording on the card. This cannot be undone."},
    {"sd-eject", "Eject SD card", "Maintenance", 4, 2, false},
};

} // namespace

const std::vector<MiotProperty>& cameraProperties(const std::string& model) {
    if (isC400(model)) {
        return kC400Properties;
    }
    if (isC500(model)) {
        if (model == "chuangmi.camera.061a03") {
            return kC500ProProperties;
        }
        if (model == "mxiang.camera.c500cn" || model == "mxiang.camera.c500en") {
            return kMxiangC500CnEnProperties;
        }
        if (model == "xiaomi.camera.c500a") {
            return kXiaomiC500aProperties;
        }
        return kMxiangC500Properties;
    }
    if (model == "mxiang.camera.moc001") {
        return kCw300Moc001Properties;
    }
    if (model == "mxiang.camera.moc006") {
        return kCw300Moc006Properties;
    }
    if (isCw700s(model)) {
        return kCw700sProperties;
    }
    return usesDefaultProperties(model) ? kDefaultProperties : kNoProperties;
}

const std::vector<MiotAction>& cameraActions(const std::string& model) {
    if (isC400(model)) {
        return kC400Actions;
    }
    if (isC500(model)) {
        if (model == "chuangmi.camera.061a03") {
            return kC500ProActions;
        }
        if (model == "mxiang.camera.c500cn" || model == "mxiang.camera.c500en") {
            return kMxiangC500CnEnActions;
        }
        return kMxiangC500Actions;
    }
    if (isCw300(model)) {
        return kCw300Actions;
    }
    if (isCw700s(model)) {
        return kCw700sActions;
    }
    return usesDefaultProperties(model) ? kDefaultActions : kNoActions;
}

bool miotSetResultOk(const Json& response, int siid, int piid, std::string& error) {
    if (!response.contains("result") || !response["result"].is_array()) {
        error = "the camera returned an unexpected property write result";
        return false;
    }

    const Json& results = response["result"];
    const Json* propertyResult = nullptr;
    for (const auto& result : results) {
        if (result.is_object() && result.value("siid", -1) == siid &&
            result.value("piid", -1) == piid) {
            propertyResult = &result;
            break;
        }
    }
    if (propertyResult == nullptr && results.size() == 1 && results[0].is_object() &&
        !results[0].contains("siid") && !results[0].contains("piid")) {
        propertyResult = &results[0];
    }
    if (propertyResult == nullptr || !propertyResult->contains("code") ||
        !(*propertyResult)["code"].is_number_integer()) {
        error = "the camera returned no result for the requested property";
        return false;
    }

    const int code = (*propertyResult)["code"].get<int>();
    if (code != 0) {
        error = "the camera rejected the setting (code " + std::to_string(code) + ")";
        return false;
    }

    error.clear();
    return true;
}

const MiotProperty* findCameraProperty(const std::string& model, std::string_view key) {
    for (const auto& property : cameraProperties(model)) {
        if (key == property.key) {
            return &property;
        }
    }
    return nullptr;
}

MiotClient::MiotClient(AccountConfig account, std::string did, std::string model)
    : account_(std::move(account)), did_(std::move(did)), model_(std::move(model)) {}

bool MiotClient::refresh(std::string& error) {
    Json props = Json::array();
    for (const auto& property : cameraProperties(model_)) {
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
    if (!miotSetResultOk(response, property.siid, property.piid, error)) {
        XV_WARN("could not set {}: {}", property.key, error);
        return false;
    }

    if (property.type == MiotType::Bool || property.type == MiotType::Enum) {
        constexpr int kVerificationAttempts = 5;
        constexpr auto kVerificationInterval = std::chrono::seconds(1);
        Json lastValue;
        bool valuePresent = false;
        std::string readError;

        for (int attempt = 0; attempt < kVerificationAttempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(kVerificationInterval);
            }
            readError.clear();
            if (!refresh(readError)) {
                continue;
            }

            const std::optional<MiotValue> current = find(property.siid, property.piid);
            if (!current.has_value() || !current->present) {
                continue;
            }
            lastValue = current->value;
            valuePresent = true;
            if (lastValue == value) {
                XV_INFO("set {} on {}", property.key, did_);
                error.clear();
                return true;
            }
        }

        if (!readError.empty()) {
            error = "the setting was accepted but could not be verified: " + readError;
        } else if (valuePresent) {
            error = "the camera kept value " + lastValue.dump() + " instead of " + value.dump();
        } else {
            error = "the setting was accepted but was absent from the camera's refreshed values";
        }
        XV_WARN("could not verify {}: {}", property.key, error);
        return false;
    }

    if (!refresh(error)) {
        error = "the setting was accepted but could not be verified: " + error;
        XV_WARN("could not verify {}: {}", property.key, error);
        return false;
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
