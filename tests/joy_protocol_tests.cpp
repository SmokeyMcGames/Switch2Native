#include "joy_protocol.hpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>
using namespace switch2;
namespace {
unsigned checks{};
void require(bool value, const char *why) {
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
int axis(const std::array<uint8_t, 16> &report, unsigned index) {
    return int(report[2 * index] | (report[2 * index + 1] << 8)) - 32768;
}
void packed_controls() {
    constexpr uint32_t expected[2][8]{{Down, Right, Left, Up, L, ZL, Minus, L3},
                                     {B, A, Y, X, R, ZR, Plus, R3}};
    for (unsigned side = 0; side < 2; ++side) {
        std::array<uint8_t, 64> report{};
        report[0] = uint8_t(7 + side);
        report[6] = 0x23; report[7] = 0x61; report[8] = 0x45;
        for (unsigned bit = 0; bit < 8; ++bit) {
            report[3] = uint8_t(1u << bit);
            const auto state = decode_packed_joy_controls(report);
            require(state && state->buttons == expected[side][bit], "Packed face and edge buttons retain their mapping");
            require(state->sticks[side * 2] == 0x123 && state->sticks[side * 2 + 1] == 0x456,
                    "Packed stick is decoded on the correct physical side");
        }
        report[3] = 0;
        report[4] = 0xd1;
        const auto state = decode_packed_joy_controls(report);
        require(state && state->buttons == (side ? (Home | Chat | SLRight | SRRight) : (Capture | SLLeft | SRLeft)),
                "Packed Home, Capture, C and rail buttons remain independently assignable");
        for (size_t size = 0; size < report.size(); ++size)
            require(!decode_packed_joy_controls(std::span(report).first(size)), "Reject truncated packed controls");
        report[0] = 9;
        require(!decode_packed_joy_controls(report), "Do not decode Pro reports as Joy-Con controls");
    }
}
} // namespace
int main() {
    try {
        packed_controls();
        for (unsigned first = 0; first < 2; ++first) {
            JoyRouting routing;
            require(routing.status().active_slots == 0, "No phantom gamepad without a Joy-Con");
            require(!routing.connect(4, true), "Reject invalid physical side");
            require(routing.connect(first, true), "First connection changes topology");
            const auto single = routing.status();
            require(single.active_slots == 1 && single.mode == JoyMode::Single &&
                        !single.needs_choice,
                    "One Joy-Con is one controller without a prompt");
            require(routing.physical_mask(0) == (1u << first) && routing.physical_mask(1) == 0,
                    "First-connected side owns the first logical slot");
            require(!routing.connect(first, true) &&
                        routing.status().generation == single.generation,
                    "Reports do not retrigger connection prompts");
            routing.connect(1 - first, true);
            const auto both = routing.status();
            require(both.active_slots == 3 && both.needs_choice && both.mode == JoyMode::Separate,
                    "Second connection remains playable separately while waiting for choice");
            require(routing.slot_for(first) == 0 && routing.slot_for(1 - first) == 1,
                    "Second connection preserves the first controller's slot");
            require(!routing.choose(single.generation, JoyMode::Combined),
                    "Reject stale dialog generation");
            require(!routing.choose(both.generation, JoyMode::Single),
                    "Reject invalid two-controller choice");
            require(routing.choose(both.generation, JoyMode::Combined), "Accept combine choice");
            require(routing.status().active_slots == 1 && !routing.status().needs_choice &&
                        routing.physical_mask(0) == 3 && routing.physical_mask(1) == 0,
                    "Combined controller owns both physical inputs and only one slot");
            require(routing.choose(both.generation, JoyMode::Separate),
                    "Allow later tray mode change");
            require(routing.physical_mask(0) == (1u << first) &&
                        routing.physical_mask(1) == (1u << (1 - first)),
                    "Separate rumble routes to exactly one physical controller");
            routing.connect(first, false);
            require(routing.status().active_slots == 1 &&
                        routing.status().mode == JoyMode::Single &&
                        routing.slot_for(1 - first) == 0,
                    "Survivor falls back to single sideways controller");
            require(!routing.choose(both.generation, JoyMode::Combined),
                    "Disconnect invalidates an open popup");
            routing.connect(first, true);
            require(routing.status().needs_choice && routing.status().generation != both.generation,
                    "Reconnection creates a fresh choice");
            routing.connect(0, false);
            routing.connect(1, false);
            require(routing.status().active_slots == 0 && routing.physical_mask(0) == 0,
                    "Last disconnect removes every logical controller");
        }
        for (bool right : {false, true})
            for (unsigned player = 1; player <= 4; ++player) {
                require(joy_player_lights(right, player, true) ==
                            (1u << (right ? 4 - player : player - 1)),
                        "Sideways LEDs run from visual left to right on either side");
                require(joy_player_lights(right, player, false) == (1u << (player - 1)),
                        "Combined upright LEDs identify the pair's shared player");
            }
        require(!joy_player_lights(false, 0, true) && !joy_player_lights(true, 5, true),
                "Reject out-of-range player LEDs");
        JoyRouting four;
        for (unsigned i = 0; i < 4; ++i)
            four.connect(i, true);
        auto all = four.status();
        require(
            all.connected == 15 && all.active_slots == 15 && all.pairs[0] == 3 &&
                all.pairs[1] == 12,
            "Four Joy-Cons create four separate controllers and two independent pairing choices");
        require(all.needs_choice == 3 && four.choose(all.generation, JoyMode::Combined, 3),
                "First pair can combine");
        require(four.status().active_slots == 7 && four.physical_mask(0) == 3 &&
                    four.physical_mask(1) == 4 && four.physical_mask(2) == 8 &&
                    four.status().needs_choice == 12,
                "Combining one pair preserves the other two singles");
        require(four.choose(all.generation, JoyMode::Combined, 12),
                "Second pair can combine independently");
        require(four.status().active_slots == 3 && four.physical_mask(0) == 3 &&
                    four.physical_mask(1) == 12,
                "Two combined pairs expose exactly two controllers");
        require(!four.choose(all.generation, JoyMode::Combined, 5),
                "Same-side or unassigned pairs cannot combine");
        four.connect(0, false);
        require(four.status().active_slots == 3 && four.physical_mask(0) == 2 &&
                    four.physical_mask(1) == 12,
                "One pair's disconnect leaves the other combined pair intact");
        four.connect(0, true);
        require(four.status().needs_choice == 3 &&
                    !four.choose(all.generation, JoyMode::Combined, 3),
                "Reconnection prompts again and rejects an older four-controller choice");
        JoyRouting same_side;
        for (unsigned i = 0; i < 4; ++i)
            same_side.connect(i, true, true);
        require(same_side.status().active_slots == 15 && !same_side.status().needs_choice &&
                    !same_side.status().pairs[0],
                "Four right Joy-Cons work singly without an invalid pair prompt");
        std::array<unsigned, 4> order{0, 1, 2, 3};
        do {
            JoyRouting ordered;
            for (auto device : order)
                ordered.connect(device, true, true);
            for (unsigned player = 0; player < 4; ++player)
                require(ordered.slot_for(order[player]) == player &&
                            ordered.physical_mask(player) == (1u << order[player]),
                        "All 24 connection orders assign players in arrival order");
        } while (std::next_permutation(order.begin(), order.end()));
        Calibration left{}, right{};
        // Factory stick calibration read from these two physical Joy-Con 2s.
        const uint8_t factory_left[]{0xe6, 0xa7, 0x82, 0x9a, 0x84, 0x48, 0x6b, 0xe4, 0x4b};
        const uint8_t factory_right[]{0x0a, 0x58, 0x7e, 0x89, 0x34, 0x4e, 0xbb, 0xf4, 0x46};
        require(parse_calibration(factory_left, left, 0) &&
                    parse_calibration(factory_right, right, 2),
                "Both sides decode their single-stick factory block");
        State l, r;
        l.sticks[0] = left[0].center;
        l.sticks[1] = left[1].center;
        r.sticks[2] = right[2].center;
        r.sticks[3] = right[3].center;
        require(axis(single_joy_controls(0, l, left), 0) == 0 &&
                    axis(single_joy_controls(1, r, right), 1) == 0,
                "Independent calibration produces centered single controllers");
        for (unsigned side = 0; side < 2; ++side) {
            State state;
            state.buttons = side ? SLRight | SRRight : SLLeft | SRLeft;
            auto report = single_joy_controls(side, state, {});
            require(report[13] == 0x30 && !report[8] && !report[10],
                    "SL/SR are shoulders, not triggers");
            state.buttons = side ? R | ZR : L | ZL;
            report = single_joy_controls(side, state, {});
            require(report[8] == 0xff && report[9] == 3 && report[10] == 0xff && report[11] == 3,
                    "Outer shoulder/trigger pair remains available as digital triggers");
            state.buttons = side ? Home : Capture;
            report = single_joy_controls(side, state, {});
            require(report[14] == 4 && report[15] == 0, "Single-controller Home/Capture is assignable without Guide");
            state.buttons = side ? R3 : L3;
            require(single_joy_controls(side, state, {})[14] == 1,
                    "Single stick click maps to left-stick click");
            state.buttons = side ? Plus : Minus;
            require(single_joy_controls(side, state, {})[13] == 0x80,
                    "Each single controller has Start");
            state = {};
            state.sticks[side * 2] = 3848;
            report = single_joy_controls(side, state, {});
            require(axis(report, 0) == 0 &&
                        (side ? axis(report, 1) > 32000 : axis(report, 1) < -32000),
                    "Sideways vertical axis rotates the physical X axis in opposite directions");
            state = {};
            state.sticks[side * 2 + 1] = 3848;
            report = single_joy_controls(side, state, {});
            require(axis(report, 1) == 0 &&
                        (side ? axis(report, 0) > 32000 : axis(report, 0) < -32000),
                    "Sideways horizontal axis rotates the physical Y axis in opposite directions");
            require(axis(report, 2) == 0 && axis(report, 3) == 0,
                    "Single controller's absent right stick is neutral");
        }
        l.buttons = Left | L | ZL | Minus | L3;
        r.buttons = A | R | ZR | Plus | R3 | Home;
        auto combined = combined_joy_controls(l, left, r, right);
        require(combined[12] == 7 && combined[13] == 0xf2 && combined[14] == 7 && combined[15] == 0,
                "Combined mapping preserves both sides, D-pad, Start/Back and assignable Home");
        require(combined[8] == 255 && combined[10] == 255,
                "Both combined triggers are independent");
        for (unsigned i = 0; i < 4; ++i)
            require(axis(combined, i) == 0, "Combined sticks use each controller's calibration");
        Motion motion{true, {1, 2, 3}, {4, 5, 6}};
        auto a = sideways_motion(0, motion), b = sideways_motion(1, motion);
        require(a.acceleration == std::array<float, 3>{-2, 1, 3} &&
                    b.acceleration == std::array<float, 3>{2, -1, 3},
                "Motion axes rotate with sideways orientation");
        require(a.angular_velocity == std::array<float, 3>{-5, 4, 6} &&
                    b.angular_velocity == std::array<float, 3>{5, -4, 6},
                "Gyro uses the same right-handed rotation as acceleration");
        std::cout << checks << " Joy-Con checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
