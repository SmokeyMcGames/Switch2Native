#include "windows_support.hpp"
#include "motion_snapshot.hpp"
#include "joy_protocol.hpp"
#include "hid_helpers.hpp"
#include <hidsdi.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.h>
#include <cstring>
#include <iomanip>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace winrt::Windows::Devices::Bluetooth;

switch2::Handle find_interface(const wchar_t *expected_serial) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    for (const auto &path : switch2::device_paths(guid)) {
        switch2::Handle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, 0, nullptr));
        WCHAR serial[64]{};
        if (handle.valid() && HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial)) &&
            std::wstring(serial) == expected_serial &&
            (std::wstring(expected_serial) == L"Switch2Native-01"
                 ? switch2::has_hid_usage(handle.get(), 1, 5)
                 : switch2::has_hid_usage(handle.get(), 0xff00, 1)))
            return handle;
    }
    throw std::runtime_error("Switch2Native diagnostic interface not found");
}

struct Counts {
    uint8_t transport{};
    uint32_t reports{}, samples{}, errors{}, source_dropped{};
    Clock::time_point time;
};

Counts read_counts(HANDLE gamepad, HANDLE hub) {
    uint8_t status[64]{0xe1};
    switch2::JoyStatus physical;
    switch2::MotionSnapshot motion;
    if (!HidD_GetFeature(gamepad, status, sizeof(status)) ||
        !HidD_GetFeature(gamepad, &motion, sizeof(motion)) ||
        !HidD_GetFeature(hub, &physical, sizeof(physical)))
        switch2::fail("Read rate counters");
    if (physical.version != 2 || !physical.connected || motion.version != 1 || !motion.ready || !motion.transport ||
        motion.transport != status[2] || GetTickCount64() - motion.updated_ms >= 2000)
        throw std::runtime_error("Motion stream unavailable, stale, or switching transport");
    Counts counts;
    counts.transport = motion.transport;
    counts.samples = motion.sequence;
    counts.reports = physical.reports[0];
    counts.errors = physical.errors[0];
    std::memcpy(&counts.source_dropped, status + 60, 4);
    counts.time = Clock::now();
    return counts;
}

void print_rates(const char *label, const Counts &first, const Counts &last) {
    if (last.transport != first.transport || last.errors != first.errors)
        throw std::runtime_error("Transport or error count changed; repeat a stable rate capture");
    const double seconds = std::chrono::duration<double>(last.time - first.time).count();
    const auto reports = uint32_t(last.reports - first.reports);
    const auto samples = uint32_t(last.samples - first.samples);
    std::cout << label << " transport=" << (last.transport == 1 ? "USB" : "Bluetooth")
              << " elapsed_s=" << seconds << " reports=" << reports
              << " reports_hz=" << reports / seconds << " distinct_imu=" << samples
              << " distinct_imu_hz=" << samples / seconds
              << " source_overflow=" << uint32_t(last.source_dropped - first.source_dropped) << '\n';
}

void print_link(const BluetoothLEDevice &device) {
    if (!device)
        return;
    if (device.ConnectionStatus() != BluetoothConnectionStatus::Connected)
        throw std::runtime_error("Bluetooth device is not connected");
    const auto parameters = device.GetConnectionParameters();
    if (!parameters)
        throw std::runtime_error("Bluetooth connection parameters unavailable");
    // WinRT exposes BLE interval units (1.25 ms), not milliseconds.
    std::cout << "Bluetooth negotiated_interval_ms=" << parameters.ConnectionInterval() * 1.25
              << " latency_events=" << parameters.ConnectionLatency()
              << " supervision_timeout_ms=" << parameters.LinkTimeout() * 10u << '\n';
}

} // namespace

int main(int argc, char **argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::cout << std::unitbuf << std::fixed << std::setprecision(3);
        unsigned seconds = 10;
        uint64_t address = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--seconds" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("Seconds must be an integer from 1 to 3600");
                seconds = std::stoul(value);
                if (!seconds || seconds > 3600)
                    throw std::runtime_error("Seconds must be an integer from 1 to 3600");
            } else if (argument == "--address" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value.size() != 12 ||
                    value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    throw std::runtime_error("Bluetooth address must have 12 hexadecimal digits");
                address = std::stoull(value, nullptr, 16);
                if (!address)
                    throw std::runtime_error("Bluetooth address must be nonzero");
            } else {
                throw std::runtime_error("Usage: RateCheck [--seconds 1..3600] [--address HEX12]");
            }
        }
        auto gamepad = find_interface(L"Switch2Native-01");
        auto hub = switch2::open_hid_collection(0xff00, 3, L"Switch2Native-ProHub", 0);
        if (!hub.valid()) throw std::runtime_error("Pro transport coordinator unavailable");
        BluetoothLEDevice device{nullptr};
        if (address) {
            // Inspect the existing link without opening GATT services or changing its preference.
            device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
            if (!device)
                throw std::runtime_error("Bluetooth device unavailable");
            const auto preferred = BluetoothLEPreferredConnectionParameters::ThroughputOptimized();
            std::cout << "Windows throughput preset min_ms="
                      << preferred.MinConnectionInterval() * 1.25
                      << " max_ms=" << preferred.MaxConnectionInterval() * 1.25 << '\n';
            print_link(device);
        }
        std::cout << "Rates use driver counter deltas, not repeated snapshot reads.\n";
        const auto first = read_counts(gamepad.get(), hub.get());
        auto previous = first;
        for (unsigned i = 1; i <= seconds; ++i) {
            std::this_thread::sleep_until(first.time + std::chrono::seconds(i));
            const auto current = read_counts(gamepad.get(), hub.get());
            print_rates("interval", previous, current);
            previous = current;
        }
        print_rates("FINAL", first, previous);
        print_link(device);
        if (device)
            device.Close();
        return previous.samples != first.samples ? 0 : 2;
    } catch (const winrt::hresult_error &error) {
        std::cerr << "Windows Bluetooth error 0x" << std::hex << uint32_t(error.code().value)
                  << ": " << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
