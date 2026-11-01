#pragma once
#include "joy_protocol.hpp"
#include "joy_mouse.hpp"
#include <bit>
#include <string>
#include <vector>

namespace switch2::tray {
inline std::wstring mouse_target(const JoyStatus &state) {
    return state.connected & state.right_sides
        ? L"Using right Joy-Cons. Left Joy-Cons keep normal controller input."
        : L"Using left Joy-Cons until a right Joy-Con connects.";
}
struct Notification {
    std::wstring title, body, tag;
};
class Events {
  public:
    std::vector<Notification> observe(const JoyStatus &next, bool pro) {
        std::vector<Notification> events;
        std::wstring connections;
        const auto append = [&](const std::wstring &line) {
            if (!connections.empty()) connections += L"\n";
            connections += line;
        };
        if (pro != pro_) append(pro ? L"Pro Controller connected." : L"Pro Controller disconnected.");
        for (unsigned device = 0; device < MaxJoyCons; ++device) {
            const auto bit = uint8_t(1u << device);
            if (!((previous_.connected ^ next.connected) & bit)) continue;
            const bool connected = (next.connected & bit) != 0;
            const auto &state = connected ? next : previous_;
            std::wstring name = (state.right_sides & bit) ? L"Right Joy-Con" : L"Left Joy-Con";
            for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
                if (state.groups[slot] & bit) name += L" (Player " + std::to_wstring(slot + 1) + L")";
            append(name + (connected ? L" connected." : L" disconnected."));
        }
        if (!connections.empty()) events.push_back({L"Controller connection", connections, L"connections"});
        if (known_) {
            bool groups_changed = false;
            for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
                groups_changed |= previous_.groups[slot] != next.groups[slot];
            if (next.connected == previous_.connected && std::popcount(unsigned(next.connected)) > 1 &&
                (groups_changed || (previous_.needs_choice && !next.needs_choice))) {
                const auto count = std::popcount(unsigned(next.active_slots));
                events.push_back({L"Joy-Con controller mode", count == 1 ? L"Both Joy-Cons are acting as one controller." :
                    L"Joy-Cons are acting as " + std::to_wstring(count) + L" controllers.", L"controller-mode"});
            }
            if (next.mouse_enabled != previous_.mouse_enabled)
                events.push_back({L"Mouse Mode", next.mouse_enabled ?
                    std::wstring(next.mouse_source ? L"Gyro enabled. " : L"Optical enabled. ") + mouse_target(next) : L"Disabled.", L"mouse-mode"});
            else if (next.mouse_enabled && joy_mouse_devices(next.connected, next.right_sides) !=
                     joy_mouse_devices(previous_.connected, previous_.right_sides))
                events.push_back({L"Mouse controller changed", mouse_target(next), L"mouse-target"});
            if (next.mouse_source != previous_.mouse_source)
                events.push_back({L"Mouse control type", next.mouse_source ? L"Gyro Mouse selected." : L"Optical Mouse selected.", L"mouse-source"});
            if (next.mouse_enabled && previous_.mouse_enabled && next.mouse_source && previous_.mouse_source &&
                next.gyro_paused != previous_.gyro_paused)
                events.push_back({L"Gyro mouse movement", next.gyro_paused ?
                    L"Paused. Clicks and scrolling remain active." : L"Resumed.", L"gyro-pause"});
            if (next.mouse_sensitivity != previous_.mouse_sensitivity) {
                const auto before = std::to_wstring(unsigned(previous_.mouse_sensitivity) * 25);
                const auto after = std::to_wstring(unsigned(next.mouse_sensitivity) * 25);
                events.push_back({L"Optical sensitivity: " + after + L"%", L"Changed from " + before + L"% to " +
                                  after + L"% for both Joy-Cons.", L"sensitivity"});
            }
            if (next.mouse_errors && next.mouse_errors != previous_.mouse_errors)
                events.push_back({L"Mouse Mode", L"A connected Joy-Con's mouse input could not be enabled.", L"mouse-error"});
        }
        previous_ = next;
        pro_ = pro;
        known_ = true;
        return events;
    }
  private:
    JoyStatus previous_;
    bool pro_{}, known_{};
};
} // namespace switch2::tray
