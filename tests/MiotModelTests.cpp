#include <cassert>
#include <string_view>
#include <vector>

#include "cloud/Miot.h"

namespace {

const xv::MiotProperty* property(const std::vector<xv::MiotProperty>& properties,
                                 std::string_view key) {
    for (const auto& candidate : properties) {
        if (key == candidate.key) {
            return &candidate;
        }
    }
    return nullptr;
}

const xv::MiotAction* action(const std::vector<xv::MiotAction>& actions, std::string_view key) {
    for (const auto& candidate : actions) {
        if (key == candidate.key) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    using namespace xv;

    std::string error;
    assert(miotSetResultOk(
        Json{{"result", Json::array({Json{{"siid", 2}, {"piid", 3}, {"code", 0}}})}},
        2, 3, error));
    assert(!miotSetResultOk(
        Json{{"result", Json::array({Json{{"siid", 2}, {"piid", 3}, {"code", 23}}})}},
        2, 3, error));
    assert(error.find("23") != std::string::npos);
    assert(!miotSetResultOk(Json{{"result", Json::array()}}, 2, 3, error));

    const auto& c400 = cameraProperties("chuangmi.camera.039a04");
    const MiotProperty* c400Night = property(c400, "night-shot");
    assert(c400Night != nullptr && c400Night->siid == 2 && c400Night->piid == 3);
    const MiotProperty* c400Total = property(c400, "sd-total");
    assert(c400Total != nullptr && c400Total->siid == 4 && c400Total->piid == 2);
    assert(c400Total->unit == MiotUnit::Bytes);
    assert(property(c400, "baby-wake-switch")->siid == 7);
    assert(action(cameraActions("chuangmi.camera.039a04"), "sd-format")->aiid == 1);

    const auto& c500Cn = cameraProperties("mxiang.camera.c500cn");
    assert(property(c500Cn, "recording-mode")->piid == 7);
    assert(property(c500Cn, "sd-total")->siid == 4);
    assert(action(cameraActions("mxiang.camera.c500en"), "sd-format")->siid == 4);

    const auto& c500Pro = cameraProperties("chuangmi.camera.061a03");
    assert(isC500("chuangmi.camera.061a03"));
    assert(property(c500Pro, "recording-mode")->piid == 7);
    assert(property(c500Pro, "human-tracking")->piid == 11);
    assert(property(c500Pro, "sd-total")->siid == 4);
    assert(property(c500Pro, "cruise-mode")->siid == 11);
    assert(property(c500Pro, "motion-tracking") == nullptr);
    assert(action(cameraActions("chuangmi.camera.061a03"), "sd-format")->aiid == 1);

    const auto& c500Global = cameraProperties("mxiang.camera.c500os");
    assert(property(c500Global, "recording-mode")->piid == 11);
    assert(property(c500Global, "sd-total")->siid == 3);
    assert(property(c500Global, "human-detection")->piid == 6);
    assert(action(cameraActions("mxiang.camera.c500s"), "sd-format")->siid == 3);

    const auto& c500a = cameraProperties("xiaomi.camera.c500a");
    assert(property(c500a, "ai-detection-sensitivity")->piid == 28);

    assert(cameraProperties("xiaomi.camera.unverified").empty());
    assert(cameraActions("xiaomi.camera.unverified").empty());
    return 0;
}