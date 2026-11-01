#pragma once
#include <array>
#include <cstdint>
#include <initializer_list>

namespace sensor_hid {
// HID Sensor usages, per USB-IF HUT Sensors and Microsoft's Sensor HID driver.
// One-based array selectors match the HID Sensor enumeration values.
struct Descriptor {
    std::array<uint8_t, 2048> bytes{};
    size_t length{};
    constexpr void add(std::initializer_list<uint8_t> values) {
        for (auto value : values)
            bytes[length++] = value;
    }
    constexpr void usage(uint16_t value) {
        add({0x0a, uint8_t(value), uint8_t(value >> 8)});
    }
    constexpr void selection(uint16_t property, uint16_t first, uint8_t count, bool feature) {
        usage(property);
        add({0x15, 1, 0x25, count, 0x75, 8, 0x95, 1, 0xa1, 2});
        for (uint16_t i = 0; i < count; ++i)
            usage(first + i);
        add({uint8_t(feature ? 0xb1 : 0x81), 0, 0xc0});
    }
    constexpr void sensor(uint8_t id, bool gyro) {
        add({0x05, 0x20, 0x09, uint8_t(gyro ? 0x76 : 0x73), 0xa1, 0, 0x85, id, 0x35, 0, 0x45, 0,
             0x65, 0, 0x55, 0});
        selection(0x309, 0x830, 3, true); // External connection
        selection(0x316, 0x840, 6, true); // Reporting state
        selection(0x319, 0x850, 6, true); // Power state
        selection(0x201, 0x800, 7, true); // Sensor state
        usage(gyro ? 0x1456 : 0x1452);    // Absolute change sensitivity
        add({0x15, 0, 0x27, 0xff, 0xff, 0xff, 0x7f, 0x75, 32, 0x95, 1, 0x66,
             uint8_t(gyro ? 0x14 : 0), uint8_t(gyro ? 0xf0 : 0), 0x55, 0x0c, 0xb1,
             2}); // Degrees/s or the acceleration usage's default g, 10^-4
        usage(0x30e);
        add({0x65, 0, 0x55, 0, 0xb1, 2}); // Report interval (ms)
        usage(0x304);
        add({0xb1, 2}); // Minimum report interval (ms)
        selection(0x201, 0x800, 7, false);
        selection(0x202, 0x810, 6, false);
        for (uint16_t axis = 0; axis < 3; ++axis) {
            usage(uint16_t((gyro ? 0x457 : 0x453) + axis));
            add({0x17,
                 1,
                 0,
                 0,
                 0x80,
                 0x27,
                 0xff,
                 0xff,
                 0xff,
                 0x7f,
                 0x75,
                 32,
                 0x95,
                 1,
                 0x66,
                 uint8_t(gyro ? 0x14 : 0),
                 uint8_t(gyro ? 0xf0 : 0),
                 0x55,
                 0x0c,
                 0x81,
                 2});
        }
        add({0xc0});
    }
};
} // namespace sensor_hid
