#include "usb.hpp"
#include "protocol.hpp"
#include "ble.hpp"
#include "session.hpp"
#include <winrt/base.h>
#include <fstream>
#include <iomanip>
#include <map>

int main(int argc, char **argv) {
    std::cout << std::unitbuf;
    try {
        unsigned seconds = 10;
        bool wake = false;
        bool bluetooth = false, scan = false, pair = false;
        bool run = false;
        uint64_t address = 0;
        auto kind = switch2::ControllerKind::Pro;
        std::ofstream capture;
        for (int i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--wake")
                wake = true;
            else if (argument == "--ble")
                bluetooth = true;
            else if (argument == "--scan")
                scan = true;
            else if (argument == "--pair")
                pair = true;
            else if (argument == "--run")
                run = true;
            else if (argument == "--address" && i + 1 < argc)
                address = std::stoull(argv[++i], nullptr, 16);
            else if (argument == "--kind" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "left") kind = switch2::ControllerKind::JoyLeft;
                else if (value == "right") kind = switch2::ControllerKind::JoyRight;
                else if (value != "pro") throw std::runtime_error("Controller kind must be pro, left or right");
            }
            else if (argument == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (argument == "--capture" && i + 1 < argc) {
                capture.open(argv[++i]);
                if (!capture)
                    throw std::runtime_error("Cannot open capture");
            } else
                throw std::runtime_error("Usage: Switch2Native [--wake | --ble [--pair] | --scan] "
                                         "[--kind pro|left|right] [--address MAC] [--seconds N] [--capture reports.txt]");
        }
        if (kind != switch2::ControllerKind::Pro && (run || (!bluetooth && !scan)))
            throw std::runtime_error("Joy-Con diagnostics require --ble or --scan; --run is Pro-only");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (run) {
            std::atomic<unsigned> count = 0;
            switch2::Session session(
                address,
                [&](const switch2::State &state, const switch2::Calibration &calibration,
                    switch2::Transport transport, bool physical_report) {
                    if (transport == switch2::Transport::Disconnected) {
                        std::cout << "Disconnected: neutral output\n";
                        return;
                    }
                    if (!physical_report) return;
                    const auto n = ++count;
                    if (n == 1 || n % 500 == 0) {
                        auto report = switch2::xbox_controls(state, calibration);
                        std::cout << "reports=" << n << " transport=" << unsigned(transport)
                                  << " buttons=" << std::hex << state.buttons << std::dec
                                  << " calibrated sticks=";
                        for (size_t i = 0; i < 4; ++i)
                            std::cout << (report[i * 2] | (report[i * 2 + 1] << 8)) << ' ';
                        std::cout << '\n';
                    }
                },
                [](const std::string &message) { std::cout << message << '\n'; });
            Sleep(seconds * 1000);
            std::cout << "Session reports=" << count << '\n';
            return count ? 0 : 3;
        }
        std::unique_ptr<switch2::UsbController> usb;
        std::unique_ptr<switch2::BleController> ble;
        if (bluetooth || scan) {
            std::cout << "Scanning for Switch 2 controller product=" << std::hex << uint16_t(kind) << std::dec << std::endl;
            auto device = address ? std::optional<switch2::BleDevice>{{address, false, kind}}
                                  : switch2::scan_ble(60000, [] { return false; }, kind);
            if (!device) {
                std::cout
                    << "No matching BLE advertisement. Unplug USB and hold the sync button.\n";
                return 2;
            }
            std::cout << "BLE address=" << std::hex << device->address << std::dec
                      << " pairing=" << device->pairing << std::endl;
            if (scan)
                return 0;
            if (device->pairing && !pair)
                throw std::runtime_error("Controller needs bonding; rerun with --ble --pair");
            ble = std::make_unique<switch2::BleController>(*device);
            ble->initialize(pair && device->pairing);
            std::cout << "Bluetooth subscribed and initialized" << std::endl;
            if (kind != switch2::ControllerKind::Pro) {
                for (const auto offset : {0x130a8u, 0x130e8u, 0x1fc040u, 0x13040u}) {
                    const auto data = ble->read_memory(offset, 16);
                    std::cout << "calibration_address=" << std::hex << offset << " data=";
                    for (auto value : data) std::cout << std::setfill('0') << std::setw(2) << unsigned(value);
                    std::cout << std::dec << '\n';
                }
            }
        } else {
            auto devices = switch2::inspect_usb();
            std::cout << "USB interfaces: " << devices.size() << '\n';
            for (const auto &device : devices) {
                std::wcout << device.path << '\n';
                std::cout << "HID usage=" << device.usage_page << ':' << device.usage
                          << " input=" << device.input_length << " output=" << device.output_length
                          << " feature=" << device.feature_length << '\n';
            }
            if (devices.empty())
                return 2;
            usb = std::make_unique<switch2::UsbController>(devices.front());
            if (wake) {
                usb->initialize();
                std::cout << "USB initialization sent\n";
            }
        }
        unsigned count = 0, decoded = 0;
        std::map<unsigned, unsigned> ids;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            auto report = usb ? usb->read(100) : ble->read(100);
            if (report.empty())
                continue;
            ++count;
            if (usb)
                ++ids[report.front()];
            if (capture || count <= 3) {
                std::ostringstream line;
                for (auto byte : report)
                    line << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
                if (capture)
                    capture << line.str() << '\n';
                if (count <= 3)
                    std::cout << "report=" << line.str() << '\n';
            }
            if (auto state = usb ? switch2::decode_usb(report) : switch2::decode_ble(report)) {
                ++decoded;
                if (decoded <= 3 || decoded % 250 == 0)
                    std::cout << "buttons=" << std::hex << state->buttons << std::dec
                              << " sticks=" << state->sticks[0] << ',' << state->sticks[1] << ','
                              << state->sticks[2] << ',' << state->sticks[3] << '\n';
            }
        }
        std::cout << "Received=" << count << " decoded=" << decoded << " seconds=" << seconds
                  << '\n';
        for (const auto &[id, total] : ids)
            std::cout << "ID=0x" << std::hex << id << std::dec << " count=" << total << '\n';
        return decoded ? 0 : 3;
    } catch (const winrt::hresult_error &error) {
        std::cerr << "Windows Bluetooth error: " << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
