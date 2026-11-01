#include "protocol.hpp"
#include "motion.hpp"
#include "gamepad_reports.hpp"
#include "../driver/sensor_reports.hpp"
#include <limits>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace switch2;
static unsigned checks = 0;
static void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
static uint16_t word(const std::array<uint8_t, 16> &report, size_t offset) {
    return uint16_t(report[offset] | (report[offset + 1] << 8));
}
int main() {
    try {
        // First physical USB capture from this development session, reduced to its input fields.
        std::array<uint8_t, 64> usb{5, 0xeb, 0x68, 3,    0,    0,    0,    0,   0,
                                    0, 0,    0x28, 0x58, 0x8c, 0x8e, 0x38, 0x84};
        auto first = decode_usb(usb);
        require(first && first->sticks == std::array<uint16_t, 4>{2088, 2245, 2190, 2115},
                "Real USB capture stick unpacking");
        std::array<uint8_t, 63> ble{};
        std::copy(usb.begin() + 1, usb.end(), ble.begin());
        auto wireless = decode_ble(ble);
        require(wireless && wireless->sticks == first->sticks, "USB/BLE offset equivalence");
        // Actual USB IMU tail: timestamp, temperature, acceleration, angular velocity.
        const uint8_t tail[] = {0xbe, 0xc6, 0xec, 0,    0xd3, 0xfc, 0x5c, 0, 0x7f,
                                0xfd, 0xf6, 0x0f, 0xfb, 0xff, 0xfe, 0xff, 3, 0};
        std::copy(std::begin(tail), std::end(tail), usb.begin() + 43);
        std::copy(usb.begin() + 1, usb.end(), ble.begin());
        auto raw = decode_usb(usb)->raw_motion;
        auto ble_raw = decode_ble(ble)->raw_motion;
        require(raw.timestamp == 0xecc6be &&
                    raw.acceleration == std::array<int16_t, 3>{92, -641, 4086} &&
                    raw.angular_velocity == std::array<int16_t, 3>{-5, -2, 3},
                "Real USB motion unpacking");
        require(ble_raw.timestamp == raw.timestamp && ble_raw.acceleration == raw.acceleration &&
                    ble_raw.angular_velocity == raw.angular_velocity,
                "BLE motion offset equivalence");
        std::array<uint8_t, 16> factory{};
        std::array<float, 3> bias{0.001f, -0.002f, 0.003f};
        std::memcpy(factory.data() + 4, bias.data(), sizeof(bias));
        require(parse_gyro_bias(factory) == bias, "Factory gyro calibration unpacking");
        require(!parse_gyro_bias(std::span(factory).first(15)),
                "Reject truncated gyro calibration");
        float invalid_bias = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(factory.data() + 4, &invalid_bias, 4);
        require(!parse_gyro_bias(factory), "Reject nonfinite gyro calibration");
        const auto start = std::chrono::steady_clock::time_point{};
        for (uint32_t clock : {1000000u, 45000u}) {
            MotionProcessor processor(bias);
            RawMotion sample{0xffff0000u, {0, 0, 4096}, {32767, 0, 0}};
            require(!processor.process(sample, start).ready, "Motion clock must warm up");
            sample.timestamp += clock; // Also covers wraparound.
            auto measured = processor.process(sample, start + std::chrono::seconds(1));
            const float expected = (clock == 1000000 ? 34.8f : 40.0f) - bias[0];
            require(measured.ready && std::abs(measured.angular_velocity[0] -
                                               expected * 180.0f / 3.14159265f) < 0.001f,
                    "Both IMU clock modes use calibrated degrees per second");
            require(std::abs(measured.acceleration[2] - 9.80665f) < 0.001f,
                    "Gravity uses meters per second squared");
            require(!processor.process(sample, start + std::chrono::seconds(2)).ready,
                    "Duplicate motion samples are not republished");
        }
        sensor_hid::Channel sensor;
        sensor.initialize(0x10);
        sensor.update({1, 2, 3}, 2, 100);
        require(!sensor.pending, "No sensor events without a subscriber");
        auto settings = sensor.feature;
        settings.reporting = 2;
        settings.interval = 0;
        require(sensor.set(settings) && sensor.feature.interval == 4,
                "Sensor minimum interval enforced");
        sensor.update({1, 2, 3}, 2, 101);
        require(sensor.pending && sensor.input.axes[2] == 30000,
                "Native sensor precision and delivery");
        sensor.sent(101);
        settings.interval = 16;
        require(sensor.set(settings), "Sensor interval can be changed");
        sensor.first = false;
        sensor.update({1, 2, 3}, 2, 102);
        require(!sensor.pending, "Sensor rate limiting");
        sensor.update({}, 3, 103);
        require(sensor.pending && sensor.input.state == 3 && sensor.input.event == 2,
                "Disconnect produces native sensor unavailable event");
        settings.power = 6;
        require(sensor.set(settings), "Windows HID sensor D4 power-off value is accepted");
        sensor.update({1, 2, 3}, 2, 200);
        require(!sensor.pending, "No data events while the sensor is powered off");
        require(std::abs(sensor_hid::acceleration_g({0, 0, 9.80665f})[2] - 1.0f) < 0.00001f,
                "Windows accelerometer uses g rather than meters per second squared");
        // Simulate a scheduler pause followed by a burst: every distinct sample
        // must survive both queues in order, regardless of the latest poll value.
        switch2::ReportQueue<uint32_t, 64> source;
        sensor.initialize(0x10);
        settings = sensor.feature;
        settings.reporting = 2;
        settings.interval = 4;
        require(sensor.set(settings), "Enable queued native sensor delivery");
        for (uint32_t i = 0; i < 32; ++i) {
            source.push(i);
            sensor.update({float(i), 0, 0}, 2, 1000);
        }
        for (uint32_t i = 0; i < 32; ++i) {
            require(source.front() == i && sensor.reports.front().axes[0] == int32_t(i * 10000),
                    "Motion burst retains order and every sample");
            source.pop();
            sensor.sent(1000);
        }
        require(source.empty() && !sensor.pending && !sensor.reports.dropped,
                "Burst drains completely without loss or synthetic repeats");
        // Remove one disconnected stream from a wrapped, full queue without
        // discarding or reordering reports from the remaining controllers.
        switch2::ReportQueue<uint32_t, 4> mixed;
        for (uint32_t i = 0; i < 6; ++i) mixed.push(i);
        mixed.remove_if([](uint32_t value) { return value % 2 == 0; });
        require(mixed.count == 2 && mixed.front() == 3 && mixed.dropped == 2,
                "Disconnect filters wrapped queue without counting normal cleanup as overflow");
        mixed.pop();
        require(mixed.front() == 5, "Other controller reports retain order");
        mixed.push(7);
        mixed.remove_if([](uint32_t) { return false; });
        require(mixed.count == 2 && mixed.front() == 5, "Unrelated disconnect preserves queue");
        mixed.remove_if([](uint32_t) { return true; });
        mixed.push(9);
        require(mixed.count == 1 && mixed.front() == 9, "Queue accepts a clean new session");
        for (uint32_t i = 0; i < 70; ++i)
            sensor.update({float(i), 0, 0}, 2, 1001);
        require(sensor.reports.count == 64 && sensor.reports.dropped == 6 &&
                    sensor.reports.front().axes[0] == 60000,
                "A stalled sensor retains bounded newest history and counts overflow");
        sensor.update({}, 3, 1002);
        require(sensor.reports.count == 1 && sensor.reports.front().state == 3,
                "Disconnect discards queued stale ready reports");
        settings.power = 6;
        require(sensor.set(settings) && !sensor.pending && sensor.reports.empty(),
                "Stopping the sensor discards queued reports");
        const uint32_t buttons[] = {Y,       X,    B,    A,  R,     ZR,   Minus, Plus, R3, L3, Home,
                                    Capture, Chat, Down, Up, Right, Left, L,     ZL,   GR, GL};
        for (auto button : buttons) {
            for (size_t i = 0; i < 4; ++i) {
                usb[5 + i] = uint8_t(button >> (8 * i));
                ble[4 + i] = usb[5 + i];
            }
            require(decode_usb(usb)->buttons == button, "USB button identity");
            require(decode_ble(ble)->buttons == button, "BLE button identity");
        }
        for (size_t length = 0; length < 64; ++length)
            require(!decode_usb(std::span(usb).first(length)), "Truncated USB report accepted");
        for (size_t length = 0; length < 63; ++length)
            require(!decode_ble(std::span(ble).first(length)), "Truncated BLE report accepted");
        usb[0] = 0x7f;
        require(!decode_usb(usb), "Unknown USB report accepted");
        Calibration calibration;
        // Synthetic report ID 09 identifies the Pro's packed BLE characteristic.
        // Motion decoding must not change any of its independent input fields.
        std::array<uint8_t, 64> packed{};
        packed[0] = 9;
        constexpr uint32_t packed_buttons[3][8]{{B,A,Y,X,R,ZR,Plus,R3},
                                               {Down,Right,Left,Up,L,ZL,Minus,L3},
                                               {Home,Capture,GR,GL,Chat,0,0,0}};
        for (unsigned byte = 0; byte < 3; ++byte)
            for (unsigned bit = 0; bit < 8; ++bit) {
                packed[3 + byte] = uint8_t(1u << bit);
                const auto decoded = decode_usb(packed);
                require(decoded && decoded->buttons == packed_buttons[byte][bit],
                        "Packed Pro buttons and extra inputs retain their identities");
                packed[3 + byte] = 0;
            }
        packed[6]=0x23; packed[7]=0x61; packed[8]=0x45;
        packed[9]=0x89; packed[10]=0xc7; packed[11]=0xab;
        require(decode_usb(packed)->sticks == std::array<uint16_t,4>{0x123,0x456,0x789,0xabc},
                "Packed Pro preserves both twelve-bit sticks");
        packed[5] = 1;
        const auto packed_home = xbox_controls(*decode_usb(packed), calibration);
        packed[5] = 0;
        const auto packed_released = xbox_controls(*decode_usb(packed), calibration);
        require(gamepad_report(packed_home) == gamepad_report(packed_released) &&
                    home_button_report(packed_home)[1] == 1,
                "Packed Bluetooth Home remains assignable without Xbox Guide");
        auto idle = xbox_controls(State{}, calibration);
        for (size_t i = 0; i < 4; ++i)
            require(word(idle, 2 * i) == 32768, "Neutral axis must be exact");
        State state;
        state.buttons = B | A | Y | X | ZL | ZR | Home | L3 | R3;
        auto output = xbox_controls(state, calibration);
        require(output[13] == 15, "Physical face-button mapping");
        require(word(output, 8) == 1023 && word(output, 10) == 1023, "Digital trigger conversion");
        require(output[14] == 7 && output[15] == 0, "Assignable Home and stick clicks without Guide");
        state.buttons = Home;
        auto home = xbox_controls(state, calibration);
        require(home[13] == 0 && home[14] == 4 && home[15] == 0, "Home has an independent internal button bit");
        require(gamepad_report(home) == gamepad_report(idle), "Home never reaches the Xbox report");
        require(home_button_report(home) == std::array<uint8_t, 2>{5, 1}, "Home presses the companion HID button");
        require(home_button_report(idle) == std::array<uint8_t, 2>{5, 0}, "Home releases the companion HID button");
        require(gamepad_report(output)[15] == 3, "Home masking preserves both stick clicks");
        state.buttons = 0;
        require(xbox_controls(state, calibration)[14] == 0, "Home release clears assignable button");
        state.buttons = Up | Right;
        require(xbox_controls(state, calibration)[12] == 2, "Diagonal D-pad");
        state.buttons = Up | Down | Left | Right;
        require(xbox_controls(state, calibration)[12] == 0, "Opposing directions neutral");
        state.sticks = {0, 4095, 4095, 0};
        state.profile.circular_mask = 0; // Axis mode retains independent full-range diagonals.
        output = xbox_controls(state, calibration);
        require(word(output, 0) <= 1 && word(output, 2) <= 1 && word(output, 4) == 65534 &&
                    word(output, 6) == 65534,
                "Axis endpoints and Y direction");
        std::array<uint8_t, 9> invalid{};
        require(!parse_calibration(invalid, calibration, 0), "Invalid calibration rejected");
        for (unsigned i = 0; i < 256; ++i) {
            auto u = usb_rumble(uint8_t(i), uint8_t(i), uint8_t(255 - i));
            auto b = ble_rumble(uint8_t(i), uint8_t(i), uint8_t(255 - i));
            require(u[0] == 2 && b[0] == 0 && u[1] == (0x50 | (i & 15)), "Rumble report framing");
            require(std::equal(u.begin() + 1, u.begin() + 33, b.begin() + 1),
                    "Rumble transport payload equivalence");
            const unsigned high = (unsigned(u[3] & 0xfc) << 4) | (unsigned(u[4] & 15) << 12);
            const unsigned low = (u[5] & 0xc0) | (unsigned(u[6]) << 8);
            require(high <= 16000 && low <= 16000, "Rumble amplitude ceiling");
        }
        std::cout << checks << " protocol checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
