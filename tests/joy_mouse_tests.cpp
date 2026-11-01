#include "joy_mouse.hpp"
#include "mouse_shortcuts.hpp"
#include <cmath>
#include <limits>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static void relative_gyro() {
    using namespace switch2;
    JoyMouse mouse;
    check(!mouse.move_gyro(0, 40, -20), "disabled mouse cannot emit gyro movement");
    mouse.update(0, {0, 0, 1, 0}, true);
    check(!mouse.move_gyro(0, 0.5, -0.5), "sub-count motion retained");
    for (unsigned i = 0; i < 100; ++i)
        check(!mouse.move_gyro(0, 0, 0), "stationary remainder never oscillates or replays");
    auto report = mouse.move_gyro(0, 0.5, -0.5);
    check(report && report->dx == 1 && report->dy == -1, "fractional motion accumulates exactly once");
    mouse.update(0, {0, 0, 1, 1}, true);
    mouse.update(0, {0, 0, 1, 1, 1}, true);
    check(mouse.sensitivity_level() == 5, "optical gain changed for regression");
    report = mouse.move_gyro(0, 96, -96);
    check(report && report->dx == 96 && report->dy == -96 && report->buttons == 1,
          "gyro ignores optical gain and preserves a native drag");
    mouse.move_gyro(0, 0.75, 0.75);
    mouse.reset_gyro(0);
    check(!mouse.move_gyro(0, 0.25, 0.25), "pause clears old fractions without releasing drag");
    report = mouse.move_gyro(0, 1e9, -1e9);
    check(report && report->dx == 32767 && report->dy == -32768, "large relative reports clamp without overflow");
    check(!mouse.move_gyro(0, 0, 0), "clipped distance is never replayed");
    mouse.release(0);
    check(!mouse.move_gyro(0, 40, 20), "disconnect prevents subsequent gyro movement");
}
static void shortcuts() {
    using namespace switch2;
    for (bool right : {false, true}) {
        const uint32_t modifier = right ? Plus : Minus, toggle = right ? Home : Capture;
        const uint32_t stick = right ? R3 : L3;
        for (auto target : {toggle, stick}) {
            for (bool reverse : {false, true}) {
                MouseShortcuts keys;
                keys.update(0, right, 0);
                check(!keys.update(reverse ? target : modifier, right, 10).buttons, "first chord key buffered");
                const auto event = keys.update(modifier | target | A, right, 100);
                check(event.buttons == A && event.action == (target == toggle ? MouseShortcut::ToggleEnabled :
                    MouseShortcut::SwitchSource), "either-order chord consumes only shortcut keys");
                check(keys.update(modifier | target, right, 500).action == MouseShortcut::None, "held chord never repeats");
                check(!keys.update(target, right, 510).buttons, "partial release stays consumed");
                check(keys.update(modifier | target, right, 520).action == MouseShortcut::None, "repress needs full release");
                keys.update(0, right, 530);
                check(keys.update(modifier | target, right, 540).action != MouseShortcut::None, "full release rearms");
            }
        }
        MouseShortcuts keys;
        check(!keys.update(modifier | toggle, right, 0).buttons, "held wake buttons blocked");
        keys.update(0, right, 10);
        keys.update(toggle, right, 20);
        check(keys.update(0, right, 50).buttons == toggle, "short ordinary tap replayed on release");
        check(keys.update(0, right, 60).buttons == toggle, "replayed tap lasts long enough for gamepad polling");
        check(!keys.update(0, right, 90).buttons, "replayed tap releases after bounded pulse");
        keys.update(modifier, right, 100);
        check(!keys.update(modifier, right, 250).buttons, "held modifier waits for chord or release");
        const auto late = keys.update(modifier | toggle, right, 300);
        check(late.action == MouseShortcut::ToggleEnabled && !late.buttons, "held modifier accepts later second key without gamepad leak");
        keys.update(0, right, 310);
        keys.update(modifier, right, 400);
        check(!keys.update(modifier, right, 2000).buttons, "long modifier hold remains available");
        check(keys.update(modifier | stick, right, 2100).action == MouseShortcut::SwitchSource, "held modifier switches source");
        keys.update(0, right, 2200);
        keys.update(modifier, right, 2300);
        check(keys.update(0, right, 3000).buttons == modifier, "modifier alone reaches gamepad on release");
        check(keys.update(0, right, 3010).buttons == modifier, "ordinary modifier pulse remains visible");
        check(!keys.update(0, right, 3040).buttons, "ordinary modifier pulse releases");
        keys.update(toggle, right, 3100);
        check(keys.update(toggle, right, 3300).buttons == toggle, "ordinary held action forwards after window");
        check(keys.update(modifier | toggle, right, 3500).action == MouseShortcut::None,
              "already forwarded ordinary action cannot retroactively become shortcut");
        keys.reset(); keys.update(0, right, 0);
        const auto ambiguous = keys.update(modifier | toggle | stick, right, 10);
        check(ambiguous.action == MouseShortcut::None && !ambiguous.buttons, "ambiguous triple consumes without action");
    }
    MouseShortcuts right;
    right.update(0, true, 0);
    const auto c = right.update(Plus | Chat, true, 10);
    check(c.action == MouseShortcut::None && c.buttons == Chat, "Plus+C is not a mode shortcut");
}
static void gyro_click_stabilization() {
    using namespace switch2;
    JoyMouse mouse;
    const auto buttons = [&](uint8_t value, uint64_t time, bool gyro = true) {
        return mouse.update(0, {0, 0, 1, value}, true, 0, time, gyro);
    };
    buttons(1, 0);
    check(mouse.move_gyro(0, 2, 0, 1).has_value(), "held entry button does not start click hold");
    buttons(0, 10);
    auto report = buttons(1, 100);
    check(report && report->buttons == 1 && !report->dx, "left press is immediate");
    for (unsigned i = 0; i < 10; ++i) {
        check(!mouse.move_gyro(0, 10, -10, 101 + i * 2), "click tremor is suppressed");
        check(!mouse.move_gyro(0, -10, 10, 102 + i * 2), "jitter path length cannot break hold");
    }
    report = buttons(0, 130);
    check(report && !report->buttons, "release is immediate during stabilization");
    check(!mouse.move_gyro(0, 100, 0, 150), "released click holds until deadline even with large movement");
    report = buttons(1, 180);
    check(report && report->buttons == 1, "second click is immediate and rearms hold");
    buttons(0, 190);
    check(!mouse.move_gyro(0, 2, 1, 379), "double click gets its own complete hold");
    report = mouse.move_gyro(0, 2, 1, 380);
    check(report && report->dx == 2 && report->dy == 1, "deadline emits only fresh motion, never backlog");
    buttons(2, 400);
    check(!mouse.move_gyro(0, 20, 0, 410), "right click also stabilizes");
    report = mouse.move_gyro(0, 16, 0, 415);
    check(report && report->buttons == 2 && report->dx == 16, "net drag threshold breaks hold early");
    check(mouse.move_gyro(0, 1, 0, 416)->dx == 1, "held drag is not frozen again");
    buttons(0, 420);
    buttons(1, 430);
    report = mouse.move_gyro(0, 0, -40, 431);
    check(report && report->dy == -40 && report->buttons == 1, "sudden vertical drag starts immediately");
    buttons(0, 440);
    buttons(4, 450);
    check(mouse.move_gyro(0, 1, 0, 451).has_value(), "middle click remains unrestricted");
    buttons(0, 460);
    buttons(1, 470, false);
    check(mouse.move_gyro(0, 1, 0, 471).has_value(), "optical clicks do not arm gyro stabilization");
    buttons(0, 480);
    buttons(1, 490);
    mouse.reset_gyro(0);
    check(mouse.move_gyro(0, 1, 0, 491)->buttons == 1, "pause reset clears hold without releasing button");
    buttons(0, 500);
    buttons(1, 510);
    mouse.update(1, {0, 0, 1, 0}, true, 0, 510, true);
    check(mouse.move_gyro(1, 1, 0, 511)->dx == 1, "one device hold does not freeze another device");
    check(mouse.release(0)->buttons == 0, "disconnect releases held click immediately");
    check(!mouse.move_gyro(0, 50, 0, 512), "disconnected gyro cannot replay suppressed movement");
    buttons(0, 520);
    check(mouse.move_gyro(0, 1, 0, 521)->dx == 1, "reconnect starts without a stale hold");
}
static void middle_click_and_sensitivity() {
    using namespace switch2;
    check(joy_mouse_devices(0, 0x0a) == 0, "disconnected controllers are ineligible");
    check(joy_mouse_devices(1, 2) == 1, "left alone can act as mouse");
    check(joy_mouse_devices(3, 2) == 2, "right present reserves left for gamepad");
    check(joy_mouse_devices(0x0f, 5) == 5, "right preference follows device kind, not slot number");
    check(joy_mouse_devices(0x0a, 5) == 0x0a, "left fallback when all rights disconnected");
    std::array<uint8_t, 64> packet{};
    packet[0] = 8; packet[3] = 0x08; packet[4] = 1; packet[14] = 60;
    auto sample = decode_joy_mouse(packet);
    check(sample && sample->buttons == 4 && sample->sensitivity == 1, "right X and Home");
    packet[3] = 0x80;
    sample = decode_joy_mouse(packet);
    check(sample && sample->buttons == 0, "stick click remains ordinary controller input");
    packet[3] = 0x08;
    packet[4] = 0x10;
    sample = decode_joy_mouse(packet);
    check(sample && sample->sensitivity == -1, "right C lowers sensitivity");
    packet[4] = 0x11;
    sample = decode_joy_mouse(packet);
    check(sample && sample->sensitivity == 0, "opposing sensitivity buttons cancel");
    packet[4] = 1;
    packet[0] = 7;
    sample = decode_joy_mouse(packet);
    check(sample && sample->buttons == 4 && sample->sensitivity == -1, "left up-arrow and Capture");
    JoyMouse mouse;
    mouse.update(0, {0, 0, 60, 4, 1}, true);
    check(mouse.sensitivity_level() == 4, "held Home on entry does not adjust sensitivity");
    mouse.update(0, {0, 0, 60, 0}, true);
    auto report = mouse.update(0, {0, 0, 60, 4}, true);
    check(report && report->buttons == 4, "selected face button is middle button");
    mouse.update(1, {0, 0, 60, 0}, true);
    check(!mouse.update(1, {0, 0, 60, 4}, true), "middle held on two controllers");
    check(!mouse.release(0), "one release preserves another middle drag");
    report = mouse.update(1, {0, 0, 255, 4}, true);
    check(report && !report->buttons, "lifting releases middle button");
    auto press = [&](unsigned device, int8_t change) {
        mouse.update(device, {0, 0, 60, 0}, true);
        mouse.update(device, {0, 0, 60, 0, change}, true);
        mouse.update(device, {0, 0, 60, 0}, true);
    };
    press(0, 1);
    check(mouse.sensitivity_level() == 5, "Home raises by 25 percentage points");
    mouse.update(0, {0, 0, 60, 0, 1}, true);
    for (unsigned i = 0; i < 100; ++i) mouse.update(0, {0, 0, 60, 0, 1}, true);
    check(mouse.sensitivity_level() == 6, "holding Home does not repeat");
    press(1, -1);
    check(mouse.sensitivity_level() == 5, "Capture lowers shared sensitivity");
    mouse.update(0, {0, 0, 255, 0, 1}, true);
    mouse.update(1, {0, 0, 60, 0, -1}, false);
    check(mouse.sensitivity_level() == 5, "inactive controls do not change sensitivity");
    for (unsigned i = 0; i < 30; ++i) press(1, -1);
    check(mouse.sensitivity_level() == 1, "minimum sensitivity is 25 percent");
    mouse.update(0, {0, 0, 60, 0}, true);
    for (unsigned i = 0; i < 3; ++i)
        check(!mouse.update(0, {1, -1, 60, 0}, true), "fractional motion retained");
    report = mouse.update(0, {1, -1, 60, 0}, true);
    check(report && report->dx == 1 && report->dy == -1, "quarter-speed motion accumulates symmetrically");
    for (unsigned i = 0; i < 30; ++i) press(0, 1);
    check(mouse.sensitivity_level() == 16, "maximum sensitivity is 400 percent");
    report = mouse.update(1, {10, -20, 60, 0}, true);
    check(report && report->dx == 40 && report->dy == -80, "gain applies to both controllers");
    report = mouse.update(1, {32767, -32768, 60, 0}, true);
    check(report && report->dx == 32767 && report->dy == -32768, "large deltas saturate without wrapping");
}
int main() {
    try {
        using namespace switch2;
        shortcuts();
        middle_click_and_sensitivity();
        std::array<uint8_t, 64> packet{};
        packet[0] = 8; packet[3] = 0x30;
        packet[10] = 0x9d; packet[11] = 0xfe; // -355 from the hardware capture
        packet[12] = 0xa0; packet[14] = 64;
        auto sample = decode_joy_mouse(packet);
        check(sample && sample->dx == -355 && sample->dy == 160 && sample->buttons == 3, "right parser");
        packet[0] = 7;
        check(decode_joy_mouse(packet).has_value(), "left parser");
        packet[0] = 9;
        check(!decode_joy_mouse(packet), "wrong format rejected");
        check(!decode_joy_mouse(std::span(packet).first(20)), "truncated report rejected");
        JoyMouse mouse;
        check(!mouse.update(0, {0, 0, 60, 0}, false) && !mouse.active(0), "default off");
        check(!mouse.update(0, {12, 13, 0, 0}, true), "startup surface ignored");
        check(!mouse.update(0, {12, 13, 250, 0}, true), "lifted motion ignored");
        auto report = mouse.update(0, {10, -20, 60, 1}, true);
        check(report && report->dx == 10 && report->dy == -20 && report->buttons == 0, "held button blocked on entry");
        check(!mouse.update(0, {0, 0, 60, 0}, true), "release arms next click");
        report = mouse.update(0, {0, 0, 60, 1}, true);
        check(report && report->buttons == 1, "bumper left click");
        mouse.update(1, {0, 0, 60, 0}, true);
        check(!mouse.update(1, {0, 0, 60, 1}, true), "shared held button");
        check(!mouse.release(0), "disconnect preserves other drag");
        report = mouse.update(1, {0, 0, 255, 1}, true);
        check(report && report->buttons == 0, "lift releases drag");
        mouse.update(1, {0, 0, 60, 0}, true);
        report = mouse.update(1, {0, 0, 60, 2}, true);
        check(report && report->buttons == 2, "trigger right click");
        check(!mouse.update(1, {0, 0, 140, 2}, true) && mouse.active(1), "contact hysteresis");
        report = mouse.update(1, {0, 0, 60, 2}, false);
        check(report && report->buttons == 0 && !mouse.active_mask(), "toggle releases buttons");

        const AxisCalibration calibration{2000, 1000, 1500};
        check(mouse_scroll_axis(2000, calibration) == 0, "calibrated scroll center");
        check(mouse_scroll_axis(3500, calibration) == 1 && mouse_scroll_axis(1000, calibration) == -1,
              "asymmetric stick extents");
        mouse.update(0, {0, 0, 60, 0}, true, 0, 0);
        check(!mouse.update(0, {0, 0, 60, 0}, true, 0.2f, 1), "stick drift does not scroll");
        report = mouse.update(0, {0, 0, 60, 0}, true, 1, 10);
        check(report && report->wheel == 1, "up scrolls immediately");
        check(!mouse.update(0, {0, 0, 60, 0}, true, 1, 11), "packet burst does not multiply scrolling");
        check(!mouse.update(0, {0, 0, 60, 0}, true, 1, 90), "full deflection rate limited");
        report = mouse.update(0, {0, 0, 60, 0}, true, 1, 100);
        check(report && report->wheel == 1, "held stick repeats scrolling");
        report = mouse.update(0, {0, 0, 60, 0}, true, -1, 101);
        check(report && report->wheel == -1, "direction change immediate");
        report = mouse.update(0, {0, 0, 60, 0}, true, -1, 10000);
        check(report && report->wheel == -1, "long input stall does not create scroll burst");
        mouse.release(0);
        check(!mouse.update(0, {0, 0, 60, 0}, true, 1, 10001), "held stick blocked on surface entry");
        mouse.update(0, {0, 0, 60, 0}, true, 0, 10002);
        check(mouse.update(0, {0, 0, 60, 0}, true, 1, 10003)->wheel == 1, "centering rearms scroll");
        check(!mouse.update(0, {0, 0, 255, 0}, true, 1, 10100), "lifting stops scrolling");
        check(!mouse.update(0, {0, 0, 60, 0}, false, 1, 10200), "disabled mode never scrolls");

        relative_gyro();
        gyro_click_stabilization();
        std::cout << "Mouse decoding, contact, clicks, release, scrolling and relative gyro tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
