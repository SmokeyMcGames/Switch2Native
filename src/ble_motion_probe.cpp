#include "windows_support.hpp"
#include "ble.hpp"
#include "ble_parameters.hpp"
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;

template <class Operation> auto wait(Operation operation) {
    if (operation.wait_for(std::chrono::seconds(8)) == Windows::Foundation::AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("Bluetooth motion probe operation timed out");
    }
    return operation.get();
}

struct CaptureQueue {
    struct Packet {
        uint64_t host_us;
        std::vector<uint8_t> bytes;
    };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Packet> packets;
    uint32_t dropped{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
};

class MotionSubscription {
  public:
    MotionSubscription(uint64_t address, switch2::ControllerKind kind, std::shared_ptr<CaptureQueue> queue) {
        const guid packed_uuid{kind == switch2::ControllerKind::JoyLeft ? L"cc1bbbb5-7354-4d32-a716-a81cb241a32a" :
                               kind == switch2::ControllerKind::JoyRight ? L"d5a9e01e-2ffc-4cca-b20c-8b67142bf442" :
                               L"7492866c-ec3e-4619-8258-32755ffcc0f8"};
        const uint8_t packed_id = kind == switch2::ControllerKind::JoyLeft ? 7 :
                                  kind == switch2::ControllerKind::JoyRight ? 8 : 9;
        device_ = wait(BluetoothLEDevice::FromBluetoothAddressAsync(address));
        if (!device_ || device_.ConnectionStatus() != BluetoothConnectionStatus::Connected)
            throw std::runtime_error("The installed driver must already be connected by Bluetooth");
        const auto services = wait(device_.GetGattServicesAsync(BluetoothCacheMode::Uncached));
        if (services.Status() != GattCommunicationStatus::Success)
            throw std::runtime_error("Cannot discover controller services");
        for (auto service : services.Services()) {
            services_.push_back(service);
            const auto open_status = wait(service.OpenAsync(GattSharingMode::SharedReadAndWrite));
            std::cout << "service_open_status=" << int(open_status) << '\n';
            auto characteristics =
                wait(service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached));
            std::cout << "service=" << to_string(to_hstring(service.Uuid()))
                      << " status=" << int(characteristics.Status())
                      << " characteristics=" << characteristics.Characteristics().Size() << '\n';
            if (characteristics.Status() != GattCommunicationStatus::Success)
                continue;
            for (auto characteristic : characteristics.Characteristics()) {
                const auto uuid = characteristic.Uuid();
                std::cout << "characteristic handle=" << characteristic.AttributeHandle()
                          << " uuid=" << to_string(to_hstring(uuid))
                          << " properties=" << uint32_t(characteristic.CharacteristicProperties())
                          << '\n';
                if (uuid == packed_uuid)
                    packed_ = characteristic;
                if (uuid == guid{L"ab7de9be-89fe-49ad-828f-118f09df7fd2"})
                    common_ = characteristic;
            }
        }
        if (!packed_ || !common_)
            throw std::runtime_error("Controller motion characteristics unavailable");

        auto listener = [queue](uint8_t id) {
            return [queue, id](auto const &, const GattValueChangedEventArgs &event) {
                try {
                    CaptureQueue::Packet packet;
                    packet.host_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::steady_clock::now() - queue->start)
                                                  .count());
                    packet.bytes.resize(event.CharacteristicValue().Length() + 1);
                    packet.bytes[0] = id;
                    Windows::Storage::Streams::DataReader::FromBuffer(event.CharacteristicValue())
                        .ReadBytes(array_view<uint8_t>(packet.bytes.data() + 1,
                                                       packet.bytes.data() + packet.bytes.size()));
                    std::lock_guard lock(queue->mutex);
                    if (queue->packets.size() == 1024) {
                        queue->packets.pop_front();
                        ++queue->dropped;
                    }
                    queue->packets.push_back(std::move(packet));
                    queue->ready.notify_one();
                } catch (...) {
                    std::lock_guard lock(queue->mutex);
                    ++queue->dropped;
                }
            };
        };
        // Observe the already-enabled common stream without changing the
        // installed driver's CCCD. Cleanup revokes only our local handler.
        common_subscription_ = common_.ValueChanged(auto_revoke, listener(5));
        packed_subscription_ = packed_.ValueChanged(auto_revoke, listener(packed_id));
    }

    void start(unsigned interval_units, unsigned descriptor_value) {
        const auto descriptors = wait(packed_.GetDescriptorsAsync(BluetoothCacheMode::Uncached));
        if (descriptors.Status() == GattCommunicationStatus::Success)
            for (auto descriptor : descriptors.Descriptors()) {
                std::cout << "alternate_descriptor handle=" << descriptor.AttributeHandle()
                          << " uuid=" << to_string(to_hstring(descriptor.Uuid())) << '\n';
                if (descriptor.Uuid() == guid{L"679d5510-5a24-4dee-9557-95df80486ecb"}) {
                    rate_descriptor_ = descriptor;
                    auto result = wait(descriptor.ReadValueAsync(BluetoothCacheMode::Uncached));
                    std::cout << "rate_descriptor read_status=" << int(result.Status());
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
        if (descriptor_value) {
            if (!rate_descriptor_)
                throw std::runtime_error("Report descriptor unavailable");
            // It is not readable on this firmware. Return experimental writes
            // to the console-observed 0x0085, not an assumed saved original.
            restore_rate_ = true;
            write_rate(descriptor_value);
        }
        const auto current = wait(packed_.ReadClientCharacteristicConfigurationDescriptorAsync());
        if (current.Status() != GattCommunicationStatus::Success)
            throw std::runtime_error("Cannot read alternate stream's original CCCD");
        original_cccd_ = current.ClientCharacteristicConfigurationDescriptor();
        restore_cccd_ =
            original_cccd_ != GattClientCharacteristicConfigurationDescriptorValue::Notify;
        const auto result = wait(packed_.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue::Notify));
        if (result != GattCommunicationStatus::Success)
            throw std::runtime_error("Cannot enable alternate motion notifications");
        if (interval_units) {
            preference_ = device_.RequestPreferredConnectionParameters(
                switch2::ble_connection_parameters(uint16_t(interval_units)));
            std::cout << "interval_request_status=" << int(preference_.Status()) << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        const auto parameters = device_.GetConnectionParameters();
        std::cout << "negotiated_interval_ms=" << parameters.ConnectionInterval() * 1.25 << '\n';
    }

    void print_connection_parameters() {
        const auto parameters = device_.GetConnectionParameters();
        std::cout << "final_negotiated_interval_ms=" << parameters.ConnectionInterval() * 1.25 << '\n';
    }

    ~MotionSubscription() {
        packed_subscription_.revoke();
        common_subscription_.revoke();
        if (restore_rate_) {
            try {
                write_rate(133);
            } catch (...) {
                std::cerr << "Rate descriptor reset failed; reconnect controller\n";
            }
        }
        if (restore_cccd_) {
            try {
                auto status = wait(
                    packed_.WriteClientCharacteristicConfigurationDescriptorAsync(original_cccd_));
                std::cout << "alternate_cccd_restore_status=" << int(status) << '\n';
            } catch (...) {
                std::cerr << "Could not restore alternate CCCD; reconnect the controller\n";
            }
        }
        for (auto service : services_)
            service.Close();
        if (preference_)
            preference_.Close();
        if (device_)
            device_.Close();
    }

  private:
    void write_rate(unsigned value) {
        Windows::Storage::Streams::DataWriter writer;
        const std::array<uint8_t, 2> bytes{uint8_t(value & 255), uint8_t(value >> 8)};
        writer.WriteBytes(bytes);
        const auto status = wait(rate_descriptor_.WriteValueAsync(writer.DetachBuffer()));
        std::cout << "rate_descriptor_value=" << value << " write_status=" << int(status) << '\n';
        if (status != GattCommunicationStatus::Success)
            throw std::runtime_error("Report descriptor write rejected");
    }
    GattDescriptor rate_descriptor_{nullptr};
    bool restore_rate_{};
    BluetoothLEDevice device_{nullptr};
    BluetoothLEPreferredConnectionParametersRequest preference_{nullptr};
    std::vector<GattDeviceService> services_;
    GattCharacteristic common_{nullptr}, packed_{nullptr};
    GattCharacteristic::ValueChanged_revoker common_subscription_, packed_subscription_;
    GattClientCharacteristicConfigurationDescriptorValue original_cccd_{};
    bool restore_cccd_{};
};

int main(int argc, char **argv) {
    try {
        init_apartment(apartment_type::multi_threaded);
        std::cout << std::unitbuf;
        uint64_t address = 0;
        unsigned seconds = 10;
        unsigned interval_units = 0;
        unsigned descriptor_value = 0;
        bool standalone = false;
        switch2::ControllerKind kind = switch2::ControllerKind::Pro;
        std::string capture_path;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--address" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value.size() != 12 ||
                    value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    throw std::runtime_error("Address must contain twelve hexadecimal digits");
                address = std::stoull(value, nullptr, 16);
            } else if (argument == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (argument == "--capture" && i + 1 < argc)
                capture_path = argv[++i];
            else if (argument == "--standalone")
                standalone = true;
            else if (argument == "--kind" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "left") kind = switch2::ControllerKind::JoyLeft;
                else if (value == "right") kind = switch2::ControllerKind::JoyRight;
                else if (value != "pro") throw std::runtime_error("Kind must be pro, left or right");
            }
            else if (argument == "--interval-units" && i + 1 < argc)
                interval_units = std::stoul(argv[++i]);
            else if (argument == "--descriptor-value" && i + 1 < argc)
                descriptor_value = std::stoul(argv[++i]);
            else
                throw std::runtime_error(
                    "Usage: BleMotionProbe --address HEX12 --capture FILE "
                    "[--seconds 1..120] [--standalone] [--kind pro|left|right] [--interval-units 6..24]");
        }
        if (!address || capture_path.empty() || !seconds || seconds > 120)
            throw std::runtime_error(
                "A valid address, capture path and bounded duration are required");
        if (interval_units && (!standalone || interval_units < 6 || interval_units > 24))
            throw std::runtime_error(
                "Interval experiments require standalone mode and 6..24 units");
        if (descriptor_value &&
            (!standalone ||
             (descriptor_value != 133 && descriptor_value != 250 && descriptor_value != 500)))
            throw std::runtime_error(
                "Descriptor experiment requires standalone mode and 133, 250 or 500");
        std::ofstream capture;
        capture.exceptions(std::ios::failbit | std::ios::badbit);
        capture.open(capture_path);
        capture << "# host_us report_hex (synthetic USB report ID prefixes identify BLE "
                   "characteristics)\n";
        std::unique_ptr<switch2::BleController> owner;
        if (standalone) {
            owner =
                std::make_unique<switch2::BleController>(switch2::BleDevice{address, false, kind},
                    switch2::BleConnectionOptions{.share_services = true});
            owner->initialize(false);
            const auto config = owner->motion_configuration();
            std::cout << "motion_config=";
            capture << "# motion_config=";
            for (const auto byte : config) {
                std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
                capture << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
            }
            std::cout << std::dec << '\n';
            capture << std::dec << '\n';
        }
        auto queue = std::make_shared<CaptureQueue>();
        MotionSubscription subscription(address, kind, queue);
        subscription.start(interval_units, descriptor_value);
        {
            std::lock_guard lock(queue->mutex);
            queue->packets.clear();
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::array<uint32_t, 256> counts{};
        while (std::chrono::steady_clock::now() < until) {
            std::unique_lock lock(queue->mutex);
            queue->ready.wait_until(lock, until, [&] { return !queue->packets.empty(); });
            std::deque<CaptureQueue::Packet> packets;
            packets.swap(queue->packets);
            lock.unlock();
            for (const auto &packet : packets) {
                capture << std::dec << packet.host_us << ' ';
                for (auto value : packet.bytes)
                    capture << std::hex << std::setfill('0') << std::setw(2) << unsigned(value);
                capture << '\n';
                ++counts[packet.bytes[0]];
            }
        }
        subscription.print_connection_parameters();
        std::lock_guard lock(queue->mutex);
        const auto alternate_reports = counts[7] + counts[8] + counts[9];
        std::cout << "common_reports=" << counts[5] << " alternate_reports=" << alternate_reports
                  << " capture_dropped=" << queue->dropped << '\n';
        return alternate_reports && !queue->dropped ? 0 : 2;
    } catch (const hresult_error &error) {
        std::cerr << "Bluetooth motion error: " << to_string(error.message()) << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
