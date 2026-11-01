#include "tray_events.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        using namespace switch2;
        tray::Events events;
        JoyStatus state;
        state.connected = state.right_sides = state.groups[0] = 2;
        state.active_slots = 1;
        auto changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].body.find(L"Right Joy-Con") != std::wstring::npos,
              "initial connection is reported");
        check(events.observe(state, false).empty(), "unchanged polls never notify");
        state.mouse_sensitivity = 5;
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].title.find(L"125%") != std::wstring::npos &&
              changes[0].body.find(L"100%") != std::wstring::npos, "sensitivity includes old and new percentage");
        state.mouse_enabled = 1;
        check(events.observe(state, false).size() == 1, "mouse mode change reported");
        state.mouse_active = state.mouse_ready = 2;
        check(events.observe(state, false).empty(), "surface contact does not spam notifications");
        state.mouse_source = 1;
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].tag == L"mouse-source", "gyro source caption");
        state.gyro_paused = 1;
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].tag == L"gyro-pause" &&
              changes[0].body.find(L"Clicks") != std::wstring::npos, "pause preserves clicks caption");
        check(events.observe(state, false).empty(), "held pause has no duplicate caption");
        state.gyro_paused = 0;
        check(events.observe(state, false).size() == 1, "resume caption");
        state.mouse_source = 0;
        check(events.observe(state, false).size() == 1, "optical source caption");
        state.connected = state.active_slots = state.needs_choice = state.pairs[0] = 3;
        state.groups[1] = 1;
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].body.find(L"Left Joy-Con") != std::wstring::npos,
              "second controller arrival reported");
        state.needs_choice = 0;
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].tag == L"controller-mode", "separate choice acknowledged even if already separate");
        state.groups[0] = 3; state.groups[1] = 0; state.active_slots = 1;
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].body.find(L"one controller") != std::wstring::npos, "combined mode reported");
        state.connected = 1; state.groups[0] = 1;
        changes = events.observe(state, false);
        check(changes.size() == 2 && changes[1].tag == L"mouse-target" &&
              changes[1].body.find(L"left Joy-Cons") != std::wstring::npos, "right disconnect announces left fallback");
        state.connected = state.active_slots = 3;
        state.groups[0] = 2; state.groups[1] = 1;
        changes = events.observe(state, false);
        check(changes.size() == 2 && changes[1].tag == L"mouse-target" &&
              changes[1].body.find(L"right Joy-Cons") != std::wstring::npos, "right return announces mouse takeover");
        state.connected = state.active_slots = state.mouse_enabled = 0;
        changes = events.observe(state, false);
        check(changes.size() == 2 && changes[0].body.find(L"disconnected") != std::wstring::npos,
              "final disconnect and mouse-off reported before tray exit");
        check(events.observe(state, true).size() == 1 && events.observe(state, true).empty(), "Pro arrival only once");
        changes = events.observe(state, false);
        check(changes.size() == 1 && changes[0].body == L"Pro Controller disconnected.", "Pro disconnect reported");
        std::cout << "Tray notification transitions passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
