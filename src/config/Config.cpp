#include "config/Config.h"

#include <windows.h>

#include <dpapi.h>
#include <shlobj.h>

#include <fstream>

#include <nlohmann/json.hpp>

#include "app/Log.h"
#include "util/Encoding.h"

namespace xv {
namespace {

using Json = nlohmann::json;
using encoding::base64Decode;
using encoding::base64Encode;

constexpr const char* kTokenPrefix = "dpapi:";

// The token is protected with the current user's DPAPI key, so the config file
// is useless if copied to another machine or read by another account. That is
// the right bar here: it is a convenience cache, not a secret store.
std::string protectToken(const std::string& plaintext) {
    if (plaintext.empty()) {
        return {};
    }

    DATA_BLOB in{static_cast<DWORD>(plaintext.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB out{};

    if (!::CryptProtectData(&in, L"XiaomiViewer account token", nullptr, nullptr, nullptr, 0, &out)) {
        XV_WARN("could not encrypt the account token (error {}); it will not be saved", ::GetLastError());
        return {};
    }

    std::vector<uint8_t> bytes(out.pbData, out.pbData + out.cbData);
    ::LocalFree(out.pbData);

    return std::string(kTokenPrefix) + base64Encode(bytes);
}

std::string unprotectToken(const std::string& stored) {
    if (stored.empty()) {
        return {};
    }
    if (!stored.starts_with(kTokenPrefix)) {
        // A hand-edited plaintext token still works; it just gets re-encrypted
        // on the next save.
        return stored;
    }

    std::vector<uint8_t> bytes = base64Decode(stored.substr(std::strlen(kTokenPrefix)));
    if (bytes.empty()) {
        return {};
    }

    DATA_BLOB in{static_cast<DWORD>(bytes.size()), bytes.data()};
    DATA_BLOB out{};

    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        XV_WARN("saved account token could not be decrypted; you will need to sign in again");
        return {};
    }

    std::string plaintext(reinterpret_cast<char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return plaintext;
}

GridLayout layoutFromInt(int value) {
    switch (value) {
    case 0: return GridLayout::One;
    case 1: return GridLayout::TwoByTwo;
    case 2: return GridLayout::ThreeByThree;
    default: return GridLayout::Auto;
    }
}

} // namespace

std::string CameraConfig::label() const {
    std::string base = name.empty() ? model : name;
    if (base.empty()) {
        base = did;
    }
    // Channel 0 is the first lens, so the human-facing number is one higher.
    // Any non-zero channel selects the secondary lens on the models that have
    // one, which is why this does not try to be cleverer than "lens 2".
    if (!channel.empty() && channel != "0") {
        base += " (lens 2)";
    }
    return base;
}

bool isDualLens(const std::string& model) {
    return model.find(".hlmax") != std::string::npos ||
           model.find("cw500") != std::string::npos ||
           model.find("500dh") != std::string::npos;
}

bool hasMotor(const std::string& model) {
    // Every model here exposes the pan/tilt services in its MIoT specification
    // (a position readback and saved positions) and answers motor command 0x112.
    return model.find("hlc8") != std::string::npos ||  // CW400
           model.find("500dh") != std::string::npos || // CW500
           model.find("cw500") != std::string::npos ||
           model.find(".hlmax") != std::string::npos;
}

bool CameraConfig::motorised() const {
    if (!hasMotor(model)) {
        return false;
    }
    // On a dual-lens model the motor turns the dome; the second lens is bolted
    // in place and pointing it is not something the camera can do.
    return !isDualLens(model) || channel.empty() || channel == "0";
}

std::filesystem::path AppConfig::directory() {
    PWSTR roaming = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        std::filesystem::path path(roaming);
        ::CoTaskMemFree(roaming);
        return path / L"XiaomiViewer";
    }
    return std::filesystem::current_path();
}

std::filesystem::path AppConfig::configPath() {
    return directory() / L"config.json";
}

std::filesystem::path AppConfig::recordingsDirectory() const {
    if (!recordingsDir.empty()) {
        return std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(recordingsDir.data()), recordingsDir.size()));
    }

    PWSTR videos = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
        std::filesystem::path path(videos);
        ::CoTaskMemFree(videos);
        return path / L"XiaomiViewer";
    }
    return directory() / L"Recordings";
}

std::filesystem::path AppConfig::logPath() {
    return directory() / L"xiaomi-viewer.log";
}

AppConfig AppConfig::load() {
    AppConfig config;

    const auto path = configPath();
    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    Json root = Json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        XV_WARN("{} is not valid JSON; starting from defaults", path.string());
        return config;
    }

    if (const auto it = root.find("account"); it != root.end() && it->is_object()) {
        config.account.userId = it->value("user_id", std::string{});
        config.account.region = it->value("region", std::string{});
        config.account.token = unprotectToken(it->value("token", std::string{}));
    }

    if (const auto it = root.find("cameras"); it != root.end() && it->is_array()) {
        for (const auto& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            CameraConfig camera;
            camera.did = entry.value("did", std::string{});
            camera.model = entry.value("model", std::string{});
            camera.name = entry.value("name", std::string{});
            camera.ip = entry.value("ip", std::string{});
            camera.channel = entry.value("channel", std::string{});
            camera.quality = entry.value("quality", std::string{});
            camera.transport = entry.value("transport", std::string{});
            camera.enabled = entry.value("enabled", true);
            camera.audio = entry.value("audio", false);
            if (!camera.did.empty()) {
                config.cameras.push_back(std::move(camera));
            }
        }
    }

    if (const auto it = root.find("window"); it != root.end() && it->is_object()) {
        config.window.x = it->value("x", 0);
        config.window.y = it->value("y", 0);
        config.window.width = it->value("width", 0);
        config.window.height = it->value("height", 0);
        config.window.maximized = it->value("maximized", false);
    }

    config.layout = layoutFromInt(root.value("layout", 3));
    config.uiScale = root.value("ui_scale", 1.0f);
    config.recordingsDir = root.value("recordings_dir", std::string{});

    XV_INFO("loaded config with {} camera(s) from {}", config.cameras.size(), path.string());
    return config;
}

bool AppConfig::save() const {
    Json root;

    root["account"] = Json{
        {"user_id", account.userId},
        {"region", account.region},
        {"token", protectToken(account.token)},
    };

    Json list = Json::array();
    for (const auto& camera : cameras) {
        list.push_back(Json{
            {"did", camera.did},
            {"model", camera.model},
            {"name", camera.name},
            {"ip", camera.ip},
            {"channel", camera.channel},
            {"quality", camera.quality},
            {"transport", camera.transport},
            {"enabled", camera.enabled},
            {"audio", camera.audio},
        });
    }
    root["cameras"] = std::move(list);
    root["layout"] = static_cast<int>(layout);
    root["ui_scale"] = uiScale;

    // Only written when it has been changed from the default, so the default can
    // still move with the user's Videos folder.
    if (!recordingsDir.empty()) {
        root["recordings_dir"] = recordingsDir;
    }

    // Left out entirely until there is a position worth restoring, so a config
    // written before the window ever opened does not claim a zero-sized one.
    if (window.valid()) {
        root["window"] = Json{
            {"x", window.x},
            {"y", window.y},
            {"width", window.width},
            {"height", window.height},
            {"maximized", window.maximized},
        };
    }

    const auto path = configPath();

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Write beside the target and rename, so an interrupted save cannot leave a
    // truncated config behind.
    const auto temporary = std::filesystem::path(path).concat(L".tmp");
    {
        std::ofstream file(temporary, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            XV_ERROR("could not write {}", temporary.string());
            return false;
        }
        file << root.dump(2) << '\n';
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        XV_ERROR("could not replace {}: {}", path.string(), ec.message());
        return false;
    }

    return true;
}

CameraConfig* AppConfig::findCamera(const std::string& did, const std::string& channel) {
    for (auto& camera : cameras) {
        if (camera.did == did && camera.channel == channel) {
            return &camera;
        }
    }
    return nullptr;
}

} // namespace xv
