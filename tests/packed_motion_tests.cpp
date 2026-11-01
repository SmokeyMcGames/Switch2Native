#include "packed_motion.hpp"
#include "packed_motion_processor.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using namespace switch2;
namespace {
unsigned checks{};
void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char *message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
std::vector<uint8_t> hex(const std::string &value) {
    std::vector<uint8_t> result;
    for (size_t i = 0; i < value.size(); i += 2)
        result.push_back(uint8_t(std::stoul(value.substr(i, 2), nullptr, 16)));
    return result;
}
// Independent test encoder: explicit bit positions and analytic Z rotations.
void put(std::vector<uint8_t> &bytes, size_t &position, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i, ++position)
        bytes.at(position / 8) |= uint8_t(((value >> i) & 1) << (position % 8));
}
std::vector<uint8_t> single(uint16_t end, uint16_t duration, double angle, unsigned axis = 2) {
    std::vector<uint8_t> bytes(30);
    size_t bit = 0;
    put(bytes, bit, end, 12);
    put(bytes, bit, duration, 12);
    put(bytes, bit, 0, 8);
    const double w = std::cos(angle / 2), z = std::sin(angle / 2);
    const unsigned largest = std::abs(w) >= std::abs(z) ? 0 : axis + 1;
    std::array<double, 4> q{w, 0, 0, 0};
    q[axis + 1] = z;
    put(bytes, bit, largest, 2);
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned width = i == 0 ? 32 : 31;
        const double ratio = q[(largest + i + 1) % 4] / q[largest];
        const auto value = uint64_t(std::llround((ratio + 1) * double(uint64_t{1} << (width - 1))));
        put(bytes, bit, std::min(value, (uint64_t{1} << width) - 1), width);
    }
    put(bytes, bit, 0, 32);
    put(bytes, bit, 0, 32);
    put(bytes, bit, uint64_t{1} << 28, 32); // 1/8 of the configured 8g range.
    return bytes;
}
void analytic_rotations() {
    for (double initial : {0.0, 1.56, -1.58, 3.13, -3.15}) {
        PackedMotionDecoder decoder;
        const auto seed = decoder.decode(single(4094, 4, initial), 100000);
        require(seed && !seed->samples[0].gyro_valid, "First orientation seeds the decoder");
        const auto frame = decoder.decode(single(2, 4, initial + 0.004), 104167);
        require(frame && frame->samples[0].gyro_valid, "Timestamp wrapping keeps a valid interval");
        near(frame->samples[0].angular_velocity[0], 0, 1e-5, "No spurious X rotation");
        near(frame->samples[0].angular_velocity[1], 0, 1e-5, "No spurious Y rotation");
        near(frame->samples[0].angular_velocity[2], 0.96, 1e-5,
             "Analytical angular velocity across quaternion sign/index changes");
        near(frame->samples[0].acceleration[2], 9.80665, 1e-6, "Acceleration uses SI units");
        require(!decoder.decode(single(2, 4, initial + 0.004), 105000),
                "Duplicate timestamp is rejected");
        require(!decoder.decode(single(1, 4, initial), 106000), "Reordered packet is rejected");
        const auto gap = decoder.decode(single(10, 4, initial + 0.012), 112500);
        require(gap && gap->samples[0].recovered_gap && gap->samples[0].duration == 8 &&
                    gap->samples[0].encoded_duration == 4,
                "Missing packet produces one marked interval average");
        near(gap->samples[0].angular_velocity[2], 0.96, 1e-5,
             "Recovered rotation uses the entire missing duration");
        const auto restarted = decoder.decode(single(14, 4, initial), 3000000);
        require(restarted && !restarted->samples[0].gyro_valid,
                "Long host gap reseeds orientation");
        decoder.reset();
        require(!decoder.decode(single(18, 4, initial), 3004000)->samples[0].gyro_valid,
                "Explicit reset reseeds orientation");
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        PackedMotionDecoder decoder;
        decoder.decode(single(100, 4, std::numbers::pi / 2, axis), 0);
        const auto frame = decoder.decode(single(104, 4, std::numbers::pi / 2 + 0.004, axis), 4167);
        require(frame && frame->samples[0].gyro_valid, "Largest-component tie remains continuous");
        for (unsigned component = 0; component < 3; ++component)
            near(frame->samples[0].angular_velocity[component], component == axis ? 0.96 : 0, 1e-5,
                 "Analytic rotation preserves all three axis assignments");
    }
}
void captured_patterns() {
    // Genuine controller, USB/Bluetooth captures on 2026-10-31, before factory bias correction.
    const std::array<std::string, 4> payloads{
        "c154000c015a977301c743340060bdc48752450044cf52fd60fae90f9eff",
        "c984000da761ce850fd130f812ef170170523daafa73a5ffcb0c006101005f11f026d5a3a53f9eff",
        "bc04010e079273dd108daa4b7c0450f5e90fff0f00016004a87afaa3ff130001801130d5af3f9fff",
        "ad57010f679d74dd130d5b547c044cf5e88ff97f020001000950f5f427fc5f012001c00898ead39f"};
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
        const auto bytes = hex(payloads[pattern]);
        PackedMotionDecoder decoder;
        const auto frame = decoder.decode(bytes, 1000000);
        require(frame && frame->header.pattern == pattern, "Captured pattern recognized");
        const size_t count = pattern == 0 ? 1 : pattern == 1 ? 2 : 3;
        require(frame->header.sample_count == count, "Encoded intervals counted exactly once");
        unsigned total = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto &sample = frame->samples[i];
            total += sample.duration;
            near(sample.acceleration[0] / 9.80665, 0.017, 0.002, "Captured acceleration X");
            near(sample.acceleration[1] / 9.80665, -0.167, 0.002, "Captured acceleration Y");
            near(sample.acceleration[2] / 9.80665, 0.9945, 0.002, "Captured acceleration Z");
            require(sample.gyro_valid == (i != 0), "Only the initial orientation lacks a rate");
            if (i) {
                near(sample.angular_velocity[0] * 180 / std::numbers::pi, -0.44, 0.16,
                     "Captured stationary gyro X");
                near(sample.angular_velocity[1] * 180 / std::numbers::pi, 0.17, 0.16,
                     "Captured stationary gyro Y");
                near(sample.angular_velocity[2] * 180 / std::numbers::pi, 0.11, 0.16,
                     "Captured stationary gyro Z");
            }
        }
        require(total == frame->header.duration, "Intervals cover the payload duration exactly");
        require(frame->samples[count - 1].end_tick == frame->header.end_tick,
                "Last interval ends at header timestamp");
        for (size_t length = 0; length < bytes.size(); ++length)
            require(!decoder.decode(std::span(bytes).first(length), 1010000),
                    "Every truncated captured payload is rejected");
    }
    PackedMotionDecoder decoder;
    decoder.decode(hex(payloads[2]), 1000000);
    const auto one = decoder.decode(hex(payloads[0]), 1007500);
    const auto two = decoder.decode(hex(payloads[1]), 1015000);
    require(one && two && !one->samples[0].recovered_gap && !two->samples[0].recovered_gap,
            "Consecutive captured patterns share orientation history without gaps");
    near(two->samples[0].angular_velocity[0] * 180 / std::numbers::pi, -0.44, 0.16,
         "Quaternion interval agrees with following directly encoded gyro");
}
void sample_timing() {
    const auto config = parse_packed_motion_config(hex(
        "01200300000ae81c3b797d8b3a0ae89c4258a00b420ae89c4158a00b41"));
    require(config && config->samples_per_second == 800, "Joy-Con uses its own 800 Hz clock");
    constexpr uint64_t start = 130000000000000000;
    PackedSampleClock clock(800);
    require(clock.end_time(4092, 100000, start) == start, "Clock anchors first packet");
    require(clock.end_time(4, 112000, start + 120000) == start + 100000,
            "Clock wrap and arrival jitter preserve ten milliseconds of hardware time");
    require(clock.end_time(12, 123000, start + 230000) == start + 200000,
            "Arrival jitter does not become sample jitter");
    require(clock.end_time(20, 3000000, start + 29000000) == start + 29000000,
            "Long disconnect starts a new clock anchor");
    PackedMotionProcessor processor(*config, {0, 0, 0.1f});
    auto batch = processor.process(single(4094, 4, 0), 100000, start);
    require(batch && !batch->count, "Initial quaternion does not invent a gyro sample");
    batch = processor.process(single(2, 4, 0.004), 106000, start + 60000);
    require(batch && batch->count == 1, "Exactly one encoded gyro interval is published");
    require(batch->samples[0].sample_time_100ns == start + 50000,
            "Four Joy-Con ticks produce a five millisecond sample interval");
    require(batch->samples[0].sample_interval_100ns == 50000,
            "Mouse integration receives the original device interval separately from mapped UTC");
    near(batch->samples[0].angular_velocity[2], 0.7 * 180 / std::numbers::pi, 0.001,
         "Factory bias and Joy-Con clock produce calibrated degrees per second");
    near(batch->samples[0].acceleration[2], 9.80665, 1e-5, "Processor preserves acceleration SI units");
    require(!processor.process(single(2, 4, 0.004), 107000, start + 70000),
            "Duplicate packets cannot inflate the native sensor rate");
    batch = processor.process(single(10, 4, 0.012), 116000, start + 160000);
    require(batch && batch->count == 1 && batch->samples[0].sample_time_100ns == start + 150000,
            "A lost packet leaves a timestamp gap without interpolation");
    require(batch->samples[0].sample_interval_100ns == 100000,
            "Recovered rotation retains its actual duration rather than just the encoded acceleration interval");
    batch = processor.process(single(14, 4, 0), 3000000, start + 29000000);
    require(batch && !batch->count, "Reconnect reseeds orientation without a spurious rate");
    batch = processor.process(single(18, 4, 0.004), 3005000, start + 29050000);
    require(batch && batch->count == 1 && batch->samples[0].sample_time_100ns == start + 29050000,
            "Samples resume at the new clock anchor");
    auto packed_pair = hex("c984000da761ce850fd130f812ef170170523daafa73a5ffcb0c006101005f11f026d5a3a53f9eff");
    PackedMotionProcessor paired(*config, {});
    batch = paired.process(packed_pair, 100000, start);
    require(batch && batch->count == 1, "First paired packet publishes only its directly encoded gyro interval");
    packed_pair[0] += 8;
    batch = paired.process(packed_pair, 111000, start + 110000);
    require(batch && batch->count == 2, "One subsequent packet publishes both distinct motion intervals");
    require(batch->samples[0].sample_time_100ns == start + 50000 &&
                batch->samples[1].sample_time_100ns == start + 100000,
            "Batched samples retain separate five millisecond timestamps");
    const auto pro_config = parse_packed_motion_config(hex(
        "01c0030000e7d01c3b7922a03a0ae89c4258a00b420ae89c4158a00b41"));
    require(pro_config && pro_config->samples_per_second == 960, "Native Pro uses its own reported clock");
    PackedMotionProcessor pro(*pro_config, {0,0,0.1f});
    batch = pro.process(single(4094,4,0),100000,start);
    require(batch && !batch->count, "Native Pro seeds without inventing a sample");
    batch = pro.process(single(2,4,0.004),104500,start+45000);
    require(batch && batch->count == 1 && batch->samples[0].sample_time_100ns == start+41667,
            "Pro tick wrap preserves its 960 Hz timing rather than Joy-Con timing");
    near(batch->samples[0].angular_velocity[2],0.86*180/std::numbers::pi,0.001,
         "Native Pro gyro uses factory bias and its own clock scale");
    for (unsigned frequency : {800u,960u}) for (int skew : {-70, 70}) {
        PackedSampleClock drifting(frequency);
        const auto nominal_period = 80000000ull / frequency;
        uint64_t previous = 0;
        for (unsigned packet = 0; packet < frequency * 600 / 8; ++packet) {
            const auto sensor_ticks = uint64_t(packet) * 8;
            const auto real_us = uint64_t(packet) * 8 * 1000000 * (10000 + skew) / frequency / 10000;
            // Periodic radio delays must not alter the sample count or cause
            // long-term lag; the simulated capture spans ten minutes.
            const auto jitter = packet % 17 == 16 ? 2000ull : 0ull;
            const auto arrival = start + (real_us + jitter) * 10;
            const auto mapped = drifting.end_time(uint16_t(sensor_ticks & 4095), real_us + jitter, arrival);
            require(!packet || mapped > previous, "Drifting clock stays monotonic through repeated wraps");
            require(mapped <= arrival, "Mapped sample never lies after its observed arrival");
            require(arrival - mapped < 100000, "Crystal skew cannot accumulate more than ten milliseconds of lag");
            if (packet)
                require(mapped - previous >= nominal_period * 95 / 100 && mapped - previous <= nominal_period * 102 / 100 + 1,
                        "Clock correction stays gradual despite arrival jitter");
            previous = mapped;
        }
    }
}
void validation() {
    auto info = hex("01c0030000e7d01c3b7922a03a0ae89c4258a00b420ae89c4158a00b41");
    const auto config = parse_packed_motion_config(info);
    require(config && config->samples_per_second == 960, "Read actual controller ODR metadata");
    near(config->angular_velocity_range * 180 / std::numbers::pi, 2000, 0.001,
         "Read actual gyro range");
    for (size_t i = 0; i < info.size(); ++i)
        require(!parse_packed_motion_config(std::span(info).first(i)), "Reject truncated metadata");
    info[0] = 2;
    require(!parse_packed_motion_config(info), "Unknown metadata version is rejected");
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        PackedMotionConfig bad;
        bad.dense_angular_velocity_range = invalid;
        require(!PackedMotionDecoder(bad).decode(single(100, 4, 0), 1),
                "Invalid ranges are rejected");
    }
    const std::array<uint8_t, 4> empty{0, 0, 0, 0};
    const auto frame = PackedMotionDecoder().decode(empty, 0);
    require(frame && frame->header.sample_count == 0,
            "Zero duration contains no synthetic samples");
    auto sparse =
        hex("ad57010f679d74dd130d5b547c044cf5e88ff97f020001000950f5f427fc5f012001c00898ead39f");
    const unsigned end = 1000, duration = 400;
    sparse[0] = uint8_t(end & 255);
    sparse[1] = uint8_t((end >> 8) | ((duration & 15) << 4));
    sparse[2] = uint8_t(duration >> 4);
    const auto history = PackedMotionDecoder().decode(sparse, 0);
    require(history && history->samples[0].end_tick == 736 && history->samples[1].end_tick == 880 &&
                history->samples[2].end_tick == 1000,
            "Sparse history selects available ticks and clamps its oldest state");
    require(history->samples[0].duration + history->samples[1].duration +
                    history->samples[2].duration ==
                duration,
            "Sparse history preserves elapsed sensor time");
}
} // namespace
int main() {
    try {
        analytic_rotations();
        captured_patterns();
        validation();
        sample_timing();
        std::cout << checks << " packed motion checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "After " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
