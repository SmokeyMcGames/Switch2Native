#include "packed_motion.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <vector>

static std::vector<uint8_t> parse_hex(const std::string &text) {
    if (text.size() % 2 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw std::runtime_error("Invalid hexadecimal report");
    std::vector<uint8_t> result;
    result.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
        result.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
    return result;
}

int main(int argc, char **argv) {
    try {
        std::string input_path, output_path;
        switch2::PackedMotionConfig config;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--input" && i + 1 < argc)
                input_path = argv[++i];
            else if (argument == "--output" && i + 1 < argc)
                output_path = argv[++i];
            else if (argument == "--config" && i + 1 < argc) {
                const auto parsed = switch2::parse_packed_motion_config(parse_hex(argv[++i]));
                if (!parsed)
                    throw std::runtime_error("Invalid packed-motion configuration response");
                config = *parsed;
            } else
                throw std::runtime_error(
                    "Usage: PackedMotionCheck --input CAPTURE [--output CSV] [--config HEX29]");
        }
        std::ifstream input(input_path);
        if (!input)
            throw std::runtime_error("Cannot open capture");
        std::ofstream output;
        if (!output_path.empty()) {
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.open(output_path);
            output << "host_us,end_tick,duration_ticks,encoded_duration_ticks,pattern,sample,gyro_"
                      "valid,recovered,gyro_x,gyro_y,gyro_z,accel_x,accel_y,accel_z\n"
                   << std::setprecision(12);
        }
        switch2::PackedMotionDecoder decoder(config);
        uint64_t reports = 0, encoded = 0, ready = 0, recovered = 0, invalid = 0;
        uint64_t first_us = 0, last_us = 0, duration_ticks = 0;
        std::map<unsigned, uint64_t> patterns;
        std::array<double, 3> mean{}, peak{};
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            uint64_t host_us;
            std::string hex;
            std::istringstream row(line);
            if (!(row >> host_us >> hex))
                throw std::runtime_error("Invalid capture row");
            auto report = parse_hex(hex);
            if (report.empty() || (report[0] != 7 && report[0] != 8 && report[0] != 9))
                continue;
            const size_t length_offset = report[0] == 8 ? 16 : 15;
            if (report.size() != 64 || report[length_offset] > 40) {
                ++invalid;
                continue;
            }
            if (!reports)
                first_us = host_us;
            last_us = host_us;
            ++reports;
            auto frame = decoder.decode(std::span(report).subspan(length_offset + 1, report[length_offset]), host_us);
            if (!frame) {
                ++invalid;
                continue;
            }
            if (frame->header.duration) {
                ++patterns[frame->header.pattern];
                encoded += frame->header.sample_count;
                duration_ticks += frame->header.duration;
            }
            for (size_t i = 0; i < frame->header.sample_count; ++i) {
                const auto &sample = frame->samples[i];
                if (sample.gyro_valid) {
                    ++ready;
                    recovered += sample.recovered_gap;
                    for (size_t axis = 0; axis < 3; ++axis) {
                        const auto value = sample.angular_velocity[axis] * 180 / std::numbers::pi;
                        mean[axis] += value;
                        peak[axis] = std::max(peak[axis], std::abs(value));
                    }
                }
                if (output.is_open()) {
                    output << host_us << ',' << sample.end_tick << ',' << sample.duration << ','
                           << sample.encoded_duration << ',' << unsigned(frame->header.pattern)
                           << ',' << i << ',' << sample.gyro_valid << ',' << sample.recovered_gap;
                    for (auto value : sample.angular_velocity)
                        output << ',' << value * 180 / std::numbers::pi;
                    for (auto value : sample.acceleration)
                        output << ',' << value / 9.80665;
                    output << '\n';
                }
            }
        }
        const double seconds = last_us > first_us ? (last_us - first_us) / 1e6 : 0;
        std::cout << std::fixed << std::setprecision(4) << "reports=" << reports
                  << " encoded_samples=" << encoded << " gyro_samples=" << ready
                  << " recovered=" << recovered << " invalid=" << invalid
                  << " elapsed_s=" << seconds
                  << " nominal_covered_s=" << duration_ticks / config.samples_per_second
                  << " gyro_hz=" << (seconds ? ready / seconds : 0) << '\n';
        for (const auto &[pattern, count] : patterns)
            std::cout << "pattern=" << pattern << " packets=" << count << '\n';
        for (size_t axis = 0; axis < 3; ++axis)
            std::cout << "axis=" << axis << " mean_deg_s=" << (ready ? mean[axis] / ready : 0)
                      << " peak_deg_s=" << peak[axis] << '\n';
        return ready && !invalid ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
