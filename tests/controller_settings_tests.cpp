#include "controller_settings.hpp"
#include "joy_ipc.hpp"
#include "joy_mouse.hpp"
#include "controller_button_layout.hpp"
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace switch2;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static bool approximately(double a, double b, double tolerance = 1e-4) {
    return std::abs(a - b) < tolerance;
}
static std::array<uint8_t, 32> factory(ControllerKind kind, char suffix) {
    std::array<uint8_t, 32> data{};
    const std::string serial = std::string("TESTPROFILE") + suffix;
    std::copy(serial.begin(), serial.end(), data.begin() + 2);
    data[18] = 0x7e;
    data[19] = 5;
    data[20] = uint8_t(unsigned(kind));
    data[21] = uint8_t(unsigned(kind) >> 8);
    return data;
}
static void profiles() {
    check(ControllerProfile{}.circular_mask == 3 && ControllerProfile{}.deadzone[0] == 8 &&
              ControllerProfile{}.deadzone[1] == 8,
          "new profiles and Reset default to 8 percent circular on both sticks");
    const auto bytes = factory(ControllerKind::Pro, '1');
    const auto usb = controller_identity(ControllerKind::Pro, bytes, 0);
    const auto ble = controller_identity(ControllerKind::Pro, bytes, 0x123456789abc);
    check(usb.serial == ble.serial && controller_profile_key(usb) == controller_profile_key(ble),
          "factory identity is transport independent");
    auto other = controller_identity(ControllerKind::Pro, factory(ControllerKind::Pro, '2'), 0);
    check(controller_profile_key(other) != controller_profile_key(usb),
          "different physical controllers are isolated");
    auto anonymous = controller_identity(ControllerKind::Pro, {}, 0);
    check(controller_profile_key(anonymous).empty(), "no shared USB fallback profile");
    auto fallback = controller_identity(ControllerKind::Pro, {}, 0x123456789abc);
    check(fallback.identity_source == 2 && !controller_profile_key(fallback).empty(),
          "Bluetooth fallback is unique");
    check(controller_identity(ControllerKind::JoyLeft, bytes, 0).identity_source == 0,
          "factory product identity must match");
    other.serial[0] = '\\';
    check(controller_profile_key(other).empty(), "profile path cannot contain separators");
    ControllerProfile profile;
    profile.deadzone[0] = 17;
    profile.deadzone[1] = 31;
    profile.circular_mask = 2;
    profile.gyro_counts_per_degree = 137;
    check(joy_ipc::unpack_profile(joy_ipc::pack_profile(profile)) == profile,
          "IPC profile round trip");
    check(!valid_profile(joy_ipc::unpack_profile(1u << 31)), "reserved IPC bits rejected");
    auto bad = profile;
    bad.deadzone[0] = 41;
    check(!valid_profile(bad), "dead zone range enforced");
    bad = profile;
    bad.gyro_counts_per_degree = 361;
    check(!valid_profile(bad), "gyro gain range enforced");
    ControllerSettingsState a, b;
    a.connect(ControllerKind::Pro, bytes, 0);
    b.connect(ControllerKind::Pro, factory(ControllerKind::Pro, '2'), 0);
    a.update({}, {}, {}, 1, false, 1);
    b.update({}, {}, {}, 2, false, 1);
    auto command = profile_request(0, usb, profile);
    check(a.apply(command), "matching identity accepts live settings");
    check(!b.apply(command) && b.profile() == ControllerProfile{},
          "profile cannot leak to another controller");
    check(a.profile() == profile, "live settings applied");
    a.disconnect();
    check(!a.apply(command), "disconnected controller rejects stale save");
}
static void button_preview() {
    static_assert(
        sizeof(ControllerSnapshot) == 64 && offsetof(ControllerSnapshot, transport) == 48 &&
        offsetof(ControllerSnapshot, buttons) == 50 && sizeof(joy_ipc::SettingsResponse) == 656);
    ButtonPreview taps;
    check(taps.recent(0) == 0, "unseen buttons never flash at startup");
    taps.update(A | Home, 100);
    taps.update(0, 105);
    check(taps.recent(140) == (A | Home), "a tap between display frames remains visible");
    check(taps.recent(220) == 0, "tap flash expires without replaying a press");
    taps.update(A, 250);
    taps.update(A, 400);
    check(!taps.recent(400), "held input does not repeatedly restart tap flash");
    taps.update(0, 401);
    taps.update(A, 402);
    check(taps.recent(402) == A, "release and repress rearms flash");
    taps = {};
    taps.update(B, 0xfffffff0);
    taps.update(0, 0xfffffff1);
    check(taps.recent(0x20) == B && !taps.recent(0x80), "tap timing survives tick wrap");

    const std::pair<ControllerKind, uint32_t> layouts[]{
        {ControllerKind::Pro, A | B | X | Y | Up | Down | Left | Right | L3 | R3 | Minus | Plus |
                                  Home | Capture | Chat | L | R | ZL | ZR | GL | GR},
        {ControllerKind::JoyLeft,
         Up | Down | Left | Right | L3 | Minus | Capture | L | ZL | SLLeft | SRLeft},
        {ControllerKind::JoyRight,
         A | B | X | Y | R3 | Plus | Home | Chat | R | ZR | SLRight | SRRight}};
    for (const auto &[kind, expected] : layouts) {
        uint32_t mapped = 0;
        for (const auto &button : model_buttons(kind)) {
            check(std::has_single_bit(uint32_t(button.button)) && !(mapped & button.button),
                  "each physical button maps exactly once");
            check(button.name[0] && button.width > 0 && button.height > 0,
                  "buttons have names and drawable geometry");
            mapped |= button.button;
            ControllerSnapshot snapshot;
            snapshot.kind = kind;
            snapshot.connected = 1;
            snapshot.diagnostic_flags = 1;
            snapshot.buttons = button.button;
            snapshot.input_tick_ms = 1000;
            const auto preview = model_button_state(snapshot, 1050, true);
            check(preview.held == button.button && preview.lit == button.button,
                  "each model highlights only the corresponding physical button");
        }
        check(mapped == expected, "every reported physical control has a model button");
    }
    ControllerSettingsState controller;
    controller.connect(ControllerKind::JoyRight, factory(ControllerKind::JoyRight, '3'), 0);
    State raw, suppressed;
    raw.buttons = Home | R | ZR | X | R3 | Chat;
    controller.update(raw, suppressed, {}, 1, true, 2);
    auto snapshot = controller.snapshot();
    auto preview = model_button_state(snapshot, snapshot.input_tick_ms, true);
    check(preview.held == raw.buttons,
          "preview reads physical buttons before mouse and shortcut suppression");
    check(!model_button_state(snapshot, snapshot.input_tick_ms, false).lit,
          "offline view clears highlights");
    check(!model_button_state(snapshot, snapshot.input_tick_ms + 501, true).available,
          "stale input cannot display a stuck press");
    snapshot.diagnostic_flags = 0;
    check(!model_button_state(snapshot, snapshot.input_tick_ms, true).available,
          "old reserved bytes never masquerade as button telemetry");
    controller.disconnect();
    snapshot = controller.snapshot();
    check(!snapshot.buttons && !snapshot.recent_buttons && !snapshot.diagnostic_flags,
          "disconnect clears held and flashed buttons");
    check(model_button_names(ControllerKind::JoyRight, Home | Chat) == L"Home + C",
          "names use physical Nintendo controls");
}
static void sticks() {
    Calibration calibration;
    for (auto &axis : calibration)
        axis = {2000, 1000, 1000};
    State state;
    state.profile.circular_mask = 0; // The retained Axis option keeps its original behavior.
    state.sticks = {2070, 2070, 2500, 2000};
    auto raw = stick_positions(state, calibration, false),
         filtered = stick_positions(state, calibration, true);
    check(approximately(raw[0], .07) && filtered[0] == 0 && filtered[1] == 0,
          "axis default preserves raw drift and 8 percent suppression");
    check(approximately(filtered[2], (.5 - .08) / .92), "default axis rescale unchanged");
    state.profile.circular_mask = 1;
    filtered = stick_positions(state, calibration, true);
    check(filtered[0] > 0 && approximately(filtered[0], filtered[1]),
          "diagonal outside circle remains active");
    state.sticks[0] = 2060;
    state.sticks[1] = 2050;
    filtered = stick_positions(state, calibration, true);
    check(filtered[0] == 0 && filtered[1] == 0, "circular inside radius ignored");
    state.sticks[0] = 3000;
    state.sticks[1] = 3000;
    filtered = stick_positions(state, calibration, true);
    check(approximately(std::hypot(filtered[0], filtered[1]), 1),
          "circular diagonal bounded to unit radius");
    state.profile.deadzone[0] = 0;
    state.sticks[0] = 2000;
    state.sticks[1] = 2000;
    filtered = stick_positions(state, calibration, true);
    check(std::isfinite(filtered[0]) && filtered[0] == 0,
          "zero circular dead zone finite at center");
    state.sticks = {1000, 3000, 1500, 2000};
    calibration[2].negative = 500;
    raw = stick_positions(state, calibration, false);
    check(raw[0] == -1 && raw[1] == 1 && raw[2] == -1, "asymmetric calibrated travel");
    State left, right;
    left.sticks[0] = right.sticks[2] = 2500;
    left.profile.deadzone[0] = 40;
    right.profile.deadzone[1] = 0;
    const auto combined = combined_joy_controls(left, calibration, right, calibration);
    const auto left_output = xbox_controls(left, calibration),
               right_output = xbox_controls(right, calibration);
    check(combined[0] == left_output[0] && combined[1] == left_output[1] &&
              combined[4] == right_output[4] && combined[5] == right_output[5],
          "combined pair retains each physical stick profile");
}
static void orientation() {
    MotionOrientation pose;
    Motion m;
    m.ready = true;
    m.acceleration = {0, 0, 9.80665f};
    m.sample_time_100ns = 10000000;
    m.sample_interval_100ns = 50000;
    pose.update(m);
    check(approximately(pose.quaternion()[0], 1), "face-up initialization");
    m.angular_velocity = {0, 0, 90};
    for (int i = 0; i < 200; ++i) {
        m.sample_time_100ns += 50000;
        pose.update(m);
    }
    const auto q = pose.quaternion();
    check(approximately(q[0], std::sqrt(.5)) && approximately(q[3], std::sqrt(.5)),
          "90 degree yaw integrates original sample intervals");
    pose.update(m);
    check(pose.quaternion() == q, "duplicate motion cannot rotate twice");
    m.sample_time_100ns += 2000000;
    pose.update(m);
    check(pose.quaternion() == q, "gap does not create catch-up rotation");
    MotionOrientation tilted;
    m.angular_velocity = {};
    m.acceleration = {0, 9.80665f, 0};
    tilted.update(m);
    check(approximately(tilted.quaternion()[0], std::sqrt(.5)) &&
              approximately(tilted.quaternion()[1], std::sqrt(.5)),
          "gravity initializes 90 degree pitch");
    m.acceleration = {0, 0, 9.80665f};
    for (int i = 0; i < 2000; ++i) {
        m.sample_time_100ns += 50000;
        tilted.update(m);
    }
    check(std::abs(tilted.quaternion()[1]) < .001,
          "gravity correction converges without yaw anchoring");
}
static void gain_and_clicks() {
    for (double gain : {45., 90., 180.}) {
        JoyMouse mouse;
        mouse.update(0, {0, 0, 1, 0}, true, 0, 1000, true);
        mouse.update(0, {0, 0, 1, 1}, true, 0, 1010, true);
        check(!mouse.move_gyro(0, .39 * gain, 0, 1020, gain),
              "click stabilization uses angular distance at every gain");
        const auto report = mouse.move_gyro(0, .04 * gain, 0, 1025, gain);
        check(report.has_value(), "deliberate drag breaks out at same angular distance");
    }
}
int main() {
    try {
        profiles();
        button_preview();
        sticks();
        orientation();
        gain_and_clicks();
        std::cout << "Controller profile, dead zone, orientation and gain checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
