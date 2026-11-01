#include "settings_backend.hpp"
#include "hid_helpers.hpp"

namespace switch2 {
void SettingsBackend::refresh() {
    joy_ipc::SettingsResponse next;
    next.generation = current_.generation;
    for (unsigned hub = 0; hub < 2; ++hub) {
        if (!hubs_[hub].valid() && GetTickCount64() >= retry_at_[hub]) {
            retry_at_[hub] = GetTickCount64() + 1000;
            try {
                hubs_[hub] = open_hid_collection(
                    0xff00, 3, hub ? L"Switch2Native-JoyHub" : L"Switch2Native-ProHub", 0);
            } catch (...) {
            }
        }
        if (!hubs_[hub].valid())
            continue;
        for (unsigned device = 0; device < (hub ? 4u : 1u); ++device) {
            const unsigned index = hub ? device + 1 : 0;
            auto &controller = next.controllers[index];
            auto &motion = next.motion[index];
            controller.id = uint8_t(0xf0 + device);
            motion.id = uint8_t(0xf4 + device);
            if (!HidD_GetFeature(hubs_[hub].get(), &controller, sizeof(controller)) ||
                controller.version != 1 ||
                !HidD_GetFeature(hubs_[hub].get(), &motion, sizeof(motion)) ||
                motion.version != 1) {
                controller = {};
                motion = {};
                hubs_[hub].reset();
                break;
            }
        }
    }
    bool changed = false;
    for (unsigned i = 0; i < 5; ++i) {
        const auto &a = next.controllers[i], &b = current_.controllers[i];
        changed |= a.connected != b.connected || a.kind != b.kind || a.serial != b.serial ||
                   a.identity_source != b.identity_source;
    }
    if (changed)
        ++next.generation;
    current_ = next;
}
joy_ipc::SettingsResponse SettingsBackend::process(const joy_ipc::Request &request,
                                                   bool interactive) {
    refresh();
    auto response = current_;
    if (request.magic != response.magic || request.version != 3)
        response.result = ERROR_INVALID_DATA;
    else if (request.command == joy_ipc::Command::SettingsSave) {
        const auto profile = joy_ipc::unpack_profile(request.pair);
        if (!interactive)
            response.result = ERROR_ACCESS_DENIED;
        else if (request.generation != response.generation)
            response.result = ERROR_RETRY;
        else if (request.mode >= 5 || !valid_profile(profile))
            response.result = ERROR_INVALID_PARAMETER;
        else {
            const auto &controller = response.controllers[request.mode];
            const auto handle = hubs_[request.mode ? 1 : 0].get();
            if (!controller.connected || controller_profile_key(controller).empty())
                response.result = ERROR_NOT_FOUND;
            else {
                const auto previous = load_controller_profile(controller);
                if (!save_controller_profile(controller, profile))
                    response.result = ERROR_WRITE_FAULT;
                else {
                    auto command =
                        profile_request(request.mode ? request.mode - 1 : 0, controller, profile);
                    if (!HidD_SetFeature(handle, &command, sizeof(command))) {
                        response.result = ERROR_DEVICE_NOT_CONNECTED;
                        // A disconnect during save must not leave an apparently failed edit on
                        // disk.
                        if (!save_controller_profile(controller, previous))
                            response.result = ERROR_WRITE_FAULT;
                    } else {
                        current_.controllers[request.mode].profile = profile;
                        response.controllers[request.mode].profile = profile;
                    }
                }
            }
        }
    } else if (request.command != joy_ipc::Command::SettingsRead)
        response.result = ERROR_INVALID_FUNCTION;
    return response;
}
} // namespace switch2
