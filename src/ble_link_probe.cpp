#include "windows_support.hpp"
#include "ble_parameters.hpp"
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <iomanip>
#include <thread>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;

template <class Operation> auto wait(Operation operation) {
    if (operation.wait_for(std::chrono::seconds(8)) == Windows::Foundation::AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("Bluetooth probe operation timed out");
    }
    return operation.get();
}

static void print_link(const BluetoothLEDevice &device) {
    const auto parameters = device.GetConnectionParameters();
    if (!parameters || device.ConnectionStatus() != BluetoothConnectionStatus::Connected)
        throw std::runtime_error("The controller must already be connected over Bluetooth");
    std::cout << "negotiated_interval_ms=" << parameters.ConnectionInterval() * 1.25
              << " latency_events=" << parameters.ConnectionLatency() << '\n';
}

static void inspect_descriptor(const BluetoothLEDevice &device) {
    const auto services = wait(device.GetGattServicesAsync(BluetoothCacheMode::Uncached));
    if (services.Status() != GattCommunicationStatus::Success)
        throw std::runtime_error("Cannot enumerate GATT services");
    std::cout << "GATT services=" << services.Services().Size() << '\n';
    bool found = false;
    for (auto service : services.Services()) {
        try {
            const auto inputs = wait(service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached));
            if (inputs.Status() == GattCommunicationStatus::Success) {
                for (auto input : inputs.Characteristics()) {
                    if (input.Uuid() != guid{L"ab7de9be-89fe-49ad-828f-118f09df7fd2"})
                        continue;
                    const auto descriptors =
                        wait(input.GetDescriptorsAsync(BluetoothCacheMode::Uncached));
                    if (descriptors.Status() != GattCommunicationStatus::Success)
                        continue;
                    for (auto descriptor : descriptors.Descriptors()) {
                        if (descriptor.Uuid() != guid{L"679d5510-5a24-4dee-9557-95df80486ecb"})
                            continue;
                        found = true;
                        const auto result =
                            wait(descriptor.ReadValueAsync(BluetoothCacheMode::Uncached));
                        std::cout << "Common-input rate descriptor handle="
                                  << descriptor.AttributeHandle()
                                  << " read_status=" << int(result.Status());
                        if (result.Status() == GattCommunicationStatus::Success) {
                            std::vector<uint8_t> bytes(result.Value().Length());
                            Windows::Storage::Streams::DataReader::FromBuffer(result.Value())
                                .ReadBytes(bytes);
                            std::cout << " value=";
                            for (auto value : bytes)
                                std::cout << std::hex << std::setfill('0') << std::setw(2)
                                          << unsigned(value);
                            std::cout << std::dec;
                        }
                        std::cout << '\n';
                    }
                }
            }
        } catch (...) {
            service.Close();
            throw;
        }
        service.Close();
    }
    if (!found)
        std::cout << "Rate descriptor not exposed by this Windows GATT enumeration\n";
}

int main(int argc, char **argv) {
    BluetoothLEPreferredConnectionParametersRequest request{nullptr};
    BluetoothLEDevice device{nullptr};
    try {
        init_apartment(apartment_type::multi_threaded);
        std::cout << std::unitbuf;
        uint64_t address = 0;
        unsigned units = 0, seconds = 10;
        bool inspect = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--address" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value.size() != 12 ||
                    value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    throw std::runtime_error("Address must contain 12 hexadecimal digits");
                address = std::stoull(value, nullptr, 16);
            } else if (argument == "--interval-units" && i + 1 < argc)
                units = std::stoul(argv[++i]);
            else if (argument == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (argument == "--inspect-descriptor")
                inspect = true;
            else
                throw std::runtime_error("Usage: BleLinkProbe --address HEX12 [--interval-units "
                                         "2..24] [--seconds 1..30] [--inspect-descriptor]");
        }
        if (!address || !seconds || seconds > 30 || (units && (units < 2 || units > 24)))
            throw std::runtime_error("Invalid address, duration, or BLE interval");
        device = wait(BluetoothLEDevice::FromBluetoothAddressAsync(address));
        if (!device)
            throw std::runtime_error("Bluetooth device unavailable");
        print_link(device);
        if (inspect)
            inspect_descriptor(device);
        if (units) {
            auto parameters = switch2::ble_connection_parameters(uint16_t(units));
            std::cout << "Requesting explicit interval_ms=" << units * 1.25 << '\n';
            request = device.RequestPreferredConnectionParameters(parameters);
            std::cout << "request_status=" << int(request.Status()) << '\n';
        }
        for (unsigned i = 0; i < seconds; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            print_link(device);
        }
        if (request) {
            request.Close();
            request = nullptr;
        }
        device.Close();
        return 0;
    } catch (const hresult_error &error) {
        std::cerr << "Windows rejected probe: 0x" << std::hex << uint32_t(error.code().value) << ' '
                  << to_string(error.message()) << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    if (request)
        request.Close();
    if (device)
        device.Close();
    return 1;
}
