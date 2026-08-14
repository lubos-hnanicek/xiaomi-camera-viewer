#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bridge/Bridge.h"
#include "config/Config.h"

namespace xv {

enum class MiotType {
    Bool,
    Int,
    Enum,
    String,
};

struct MiotEnumOption {
    int value;
    const char* label;
};

// One property from the camera's MIoT specification.
struct MiotProperty {
    const char* key;
    const char* label;
    const char* group;
    int siid;
    int piid;
    MiotType type;
    bool writable;
    int minimum = 0;
    int maximum = 0;
    int step = 1;
    std::vector<MiotEnumOption> options;
    const char* help = nullptr;
};

// A callable operation, as opposed to a settable value.
struct MiotAction {
    const char* key;
    const char* label;
    const char* group;
    int siid;
    int aiid;
    bool destructive = false;
    const char* help = nullptr;
};

// The current value of one property, as last read from the camera.
struct MiotValue {
    int siid = 0;
    int piid = 0;
    bool present = false; // false when the camera rejected this property
    int code = 0;
    Json value;

    [[nodiscard]] bool asBool() const { return value.is_boolean() ? value.get<bool>() : false; }
    [[nodiscard]] int asInt() const { return value.is_number() ? value.get<int>() : 0; }
    [[nodiscard]] std::string asString() const {
        return value.is_string() ? value.get<std::string>() : std::string{};
    }
};

// Describes the properties and actions worth exposing for a camera model.
// Known models whose MIoT ids overlap with different meanings get a dedicated
// table. Unknown models retain the original CW500-derived table and hide
// properties they reject.
const std::vector<MiotProperty>& cameraProperties(const std::string& model);
const std::vector<MiotAction>& cameraActions(const std::string& model);

// One property by key, for the places that want a specific setting rather than
// the whole table. Null if the key is not in the table, which is a programming
// mistake rather than something a camera can cause.
const MiotProperty* findCameraProperty(const std::string& model, std::string_view key);

// MiotClient issues property and action calls for one camera.
//
// Every call is a cloud round trip, so refresh() runs on a worker thread while
// the UI keeps drawing the values from the last one. Reads therefore hand back
// a copy under a lock rather than a pointer into a vector that the worker is
// about to replace.
class MiotClient {
public:
    MiotClient(AccountConfig account, std::string did, std::string model);

    // Reads every property in the table. Returns false if the call itself
    // failed, as opposed to individual properties being unsupported.
    bool refresh(std::string& error);

    [[nodiscard]] std::optional<MiotValue> find(int siid, int piid) const;
    [[nodiscard]] bool supported(const MiotProperty& property) const;

    bool write(const MiotProperty& property, const Json& value, std::string& error);
    bool invoke(const MiotAction& action, std::string& error);

    [[nodiscard]] const std::string& did() const { return did_; }

private:
    AccountConfig account_;
    std::string did_;
    std::string model_;

    mutable std::mutex mutex_;
    std::vector<MiotValue> values_;
};

} // namespace xv
