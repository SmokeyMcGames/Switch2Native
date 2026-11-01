#include "joy_protocol.hpp"
#include <algorithm>

namespace switch2 {
bool JoyRouting::connect(unsigned device, bool connected) {
    return connect(device, connected, device % 2 != 0);
}
bool JoyRouting::connect(unsigned device, bool connected, bool right) {
    if (device >= MaxJoyCons)
        return false;
    const uint8_t bit = uint8_t(1u << device);
    if (bool(connected_ & bit) == connected)
        return false;
    ++generation_;
    if (!connected) {
        connected_ &= uint8_t(~bit);
        const auto partner = partners_[device];
        if (partner < MaxJoyCons) {
            partners_[partner] = MaxJoyCons;
            combined_[partner] = pending_[partner] = false;
        }
        partners_[device] = MaxJoyCons;
        combined_[device] = pending_[device] = false;
        return true;
    }
    connected_ |= bit;
    right_sides_ = right ? uint8_t(right_sides_ | bit) : uint8_t(right_sides_ & ~bit);
    arrival_[device] = ++next_arrival_;
    unsigned partner = MaxJoyCons;
    for (unsigned candidate = 0; candidate < MaxJoyCons; ++candidate) {
        if (candidate == device || !(connected_ & (1u << candidate)) ||
            partners_[candidate] < MaxJoyCons || bool(right_sides_ & (1u << candidate)) == right)
            continue;
        if (partner == MaxJoyCons || arrival_[candidate] < arrival_[partner])
            partner = candidate;
    }
    if (partner < MaxJoyCons) {
        partners_[device] = uint8_t(partner);
        partners_[partner] = uint8_t(device);
        pending_[device] = pending_[partner] = true;
    }
    return true;
}
bool JoyRouting::choose(uint32_t generation, JoyMode mode, uint8_t pair) {
    const auto current = status();
    if (!pair)
        pair = current.needs_choice ? current.needs_choice : current.pairs[0];
    if (!pair || generation != generation_ ||
        (pair != current.pairs[0] && pair != current.pairs[1]) ||
        (mode != JoyMode::Combined && mode != JoyMode::Separate))
        return false;
    for (unsigned device = 0; device < MaxJoyCons; ++device)
        if (pair & (1u << device)) {
            combined_[device] = mode == JoyMode::Combined;
            pending_[device] = false;
        }
    return true;
}
JoyStatus JoyRouting::status() const {
    JoyStatus result;
    result.connected = connected_;
    result.right_sides = right_sides_;
    result.generation = generation_;
    std::array<unsigned, MaxJoyCons> order{0, 1, 2, 3};
    std::sort(order.begin(), order.end(),
              [&](unsigned a, unsigned b) { return arrival_[a] < arrival_[b]; });
    unsigned slots = 0, pairs = 0, count = 0;
    uint8_t visited = 0, paired = 0;
    bool combined = false;
    for (auto device : order) {
        const auto bit = uint8_t(1u << device);
        if (!(connected_ & bit))
            continue;
        if (!count++)
            result.first_side = uint8_t(device);
        const auto partner = partners_[device];
        if (partner < MaxJoyCons && !(paired & bit)) {
            const auto pair = uint8_t(bit | (1u << partner));
            paired |= pair;
            result.pairs[pairs++] = pair;
            if (!result.needs_choice && pending_[device])
                result.needs_choice = pair;
        }
        if (visited & bit)
            continue;
        uint8_t group = bit;
        if (partner < MaxJoyCons && combined_[device]) {
            group |= uint8_t(1u << partner);
            combined = true;
        }
        visited |= group;
        result.groups[slots++] = group;
    }
    result.active_slots = uint8_t((1u << slots) - 1);
    result.mode = combined ? JoyMode::Combined : count > 1 ? JoyMode::Separate : JoyMode::Single;
    return result;
}
unsigned JoyRouting::slot_for(unsigned device) const {
    if (device >= MaxJoyCons)
        return MaxJoyCons;
    const auto current = status();
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
        if (current.groups[slot] & (1u << device))
            return slot;
    return MaxJoyCons;
}
uint8_t JoyRouting::physical_mask(unsigned slot) const {
    return slot < MaxJoyCons ? status().groups[slot] : 0;
}
uint8_t joy_player_lights(bool right, unsigned player, bool sideways) {
    if (player < 1 || player > MaxJoyCons)
        return 0;
    const unsigned position = sideways && right ? MaxJoyCons - player : player - 1;
    return uint8_t(1u << position);
}

std::array<uint8_t, 16> single_joy_controls(unsigned side, const State &state,
                                            const Calibration &calibration) {
    State mapped;
    const auto buttons = state.buttons;
    auto map = [&](Button from, Button to) {
        if (buttons & from)
            mapped.buttons |= to;
    };
    if (side == 0) {
        // Common report 05 uses this order for the left Joy-Con's four buttons.
        map(Down, Y);
        map(Up, X);
        map(Right, B);
        map(Left, A);
        map(SLLeft, L);
        map(SRLeft, R);
        map(Minus, Plus);
        map(L3, L3);
        map(Capture, Home);
        map(L, ZL);
        map(ZL, ZR);
    } else {
        for (auto button : {Y, X, B, A, Home, Plus})
            map(button, button);
        map(SLRight, L);
        map(SRRight, R);
        map(R3, L3);
        map(Chat, Minus);
        map(R, ZL);
        map(ZR, ZR);
    }
    auto result = xbox_controls(mapped, Calibration{});
    const auto vertical = xbox_controls(state, calibration);
    const unsigned offset = side == 0 ? 0 : 4;
    const int x = int(vertical[offset] | (vertical[offset + 1] << 8)) - 32768;
    const int y = int(vertical[offset + 2] | (vertical[offset + 3] << 8)) - 32768;
    const int horizontal_x = side == 0 ? y : -y;
    const int horizontal_y = side == 0 ? -x : x;
    const auto put = [&](unsigned index, int value) {
        const auto encoded = uint16_t(std::clamp(32768 + value, 0, 65534));
        result[index] = uint8_t(encoded);
        result[index + 1] = uint8_t(encoded >> 8);
    };
    put(0, horizontal_x);
    put(2, horizontal_y);
    return result;
}
std::array<uint8_t, 16> combined_joy_controls(const State &left,
                                              const Calibration &left_calibration,
                                              const State &right,
                                              const Calibration &right_calibration) {
    State state = left;
    state.profile.deadzone[1] = right.profile.deadzone[1];
    state.profile.circular_mask = uint8_t((left.profile.circular_mask & 1) | (right.profile.circular_mask & 2));
    state.buttons = left.buttons | right.buttons;
    std::copy_n(right.sticks.begin() + 2, 2, state.sticks.begin() + 2);
    Calibration calibration = left_calibration;
    std::copy_n(right_calibration.begin() + 2, 2, calibration.begin() + 2);
    return xbox_controls(state, calibration);
}
Motion sideways_motion(unsigned side, Motion motion) {
    for (auto *axes : {&motion.angular_velocity, &motion.acceleration}) {
        const auto x = (*axes)[0], y = (*axes)[1];
        (*axes)[0] = side == 0 ? -y : y;
        (*axes)[1] = side == 0 ? x : -x;
    }
    return motion;
}
} // namespace switch2
