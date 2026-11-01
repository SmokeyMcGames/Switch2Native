// Vendor protocol constants and bonding sequence: CareyScott/switch2controllerpc (MIT).
// Uses public Windows Bluetooth APIs; no private Bluetooth service interfaces.
#include "ble.hpp"
#include "ble_parameters.hpp"
#include "winrt_wait.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
#include <condition_variable>
#include <mutex>
#include <deque>

namespace switch2 {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
namespace {
template <class T> auto wait(T operation, const std::function<bool()> &cancelled = {}) {
    return wait_winrt(operation, [&] {
        if (cancelled && cancelled()) throw BleController::Cancelled();
    });
}
void check(GattCommunicationStatus status, const char *context) {
    if (status != GattCommunicationStatus::Success)
        throw std::runtime_error(std::string(context) + " (GATT status " +
                                 std::to_string(int(status)) + ")");
}
std::vector<uint8_t> bytes(const IBuffer &buffer) {
    std::vector<uint8_t> result(buffer.Length());
    DataReader::FromBuffer(buffer).ReadBytes(result);
    return result;
}
IBuffer buffer(std::span<const uint8_t> data) {
    DataWriter writer;
    writer.WriteBytes(array_view<const uint8_t>(data.data(), data.data() + data.size()));
    return writer.DetachBuffer();
}
struct Notifications {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<uint8_t>> input;
    std::vector<uint8_t> response;
    uint8_t command{};
    bool waiting{};
};
} // namespace
std::optional<BleDevice> scan_ble(unsigned milliseconds, const std::function<bool()> &stop, ControllerKind kind, bool pairing_only, uint64_t address, uint64_t required_host) {
    struct Scan {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<BleDevice> device;
    };
    auto state = std::make_shared<Scan>();
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    auto subscription = watcher.Received(
        auto_revoke, [state, kind, pairing_only, address, required_host](auto const &, const BluetoothLEAdvertisementReceivedEventArgs &event) {
            try {
                if (address && event.BluetoothAddress() != address) return;
                for (auto manufacturer : event.Advertisement().ManufacturerData()) {
                    if (manufacturer.CompanyId() != 0x0553)
                        continue;
                    auto data = bytes(manufacturer.Data());
                    if (data.size() < 16 || data[3] != 0x7e || data[4] != 0x05 ||
                        uint16_t(data[5] | (data[6] << 8)) != uint16_t(kind))
                        continue;
                    const bool pairing = std::all_of(data.begin() + 10, data.begin() + 16,
                                                     [](auto value) { return value == 0; });
                    if (pairing_only && !pairing) continue;
                    uint64_t host = 0;
                    for (unsigned i = 0; i < 6; ++i) host |= uint64_t(data[10 + i]) << (8 * i);
                    if (required_host && host != required_host) continue;
                    std::lock_guard lock(state->mutex);
                    state->device = BleDevice{event.BluetoothAddress(), pairing, kind};
                    state->ready.notify_all();
                }
            } catch (...) { /* An invalid advertisement must not escape the OS callback. */
            }
        });
    watcher.Start();
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    std::unique_lock lock(state->mutex);
    while (!state->device && !stop() && std::chrono::steady_clock::now() < until)
        state->ready.wait_for(lock, std::chrono::milliseconds(100));
    auto found = state->device;
    lock.unlock();
    watcher.Stop();
    return found;
}
struct BleController::Impl {
    std::function<bool()> cancelled;
    std::function<void(const std::string &)> trace;
    bool share_services{};
    void check_cancelled() const {
        if (cancelled && cancelled()) throw Cancelled();
    }
    template <class T> auto await(T operation) { return wait(operation, cancelled); }
    template <class T> auto await(T operation, const char *phase) {
        if (trace) trace(std::string("trace: BT ") + phase);
        try { return await(operation); }
        catch (const Cancelled &) { throw; }
        catch (const std::exception &error) {
            throw std::runtime_error(std::string(phase) + ": " + error.what());
        }
    }
    ControllerKind kind{ControllerKind::Pro};
    BluetoothLEDevice device{nullptr};
    BluetoothLEPreferredConnectionParametersRequest connection_request{nullptr};
    GattSession session{nullptr};
    std::vector<GattDeviceService> services;
    GattCharacteristic input{nullptr}, packed{nullptr}, command{nullptr}, response{nullptr}, rumble{nullptr};
    GattCharacteristic::ValueChanged_revoker input_subscription, packed_subscription, response_subscription;
    bool packed_enabled{};
    std::shared_ptr<Notifications> notifications = std::make_shared<Notifications>();
    std::chrono::steady_clock::time_point last_parameters_check{};
    uint16_t preferred_interval{6};
    void prefer_low_latency() {
        // Close the old request first: closing it after the new request can reset
        // the link to the OS default, even while another request remains alive.
        if (connection_request)
            connection_request.Close();
        connection_request = nullptr;
        if (preferred_interval == 6 || preferred_interval == 8) {
            try {
                connection_request =
                    device.RequestPreferredConnectionParameters(ble_connection_parameters(preferred_interval));
                if (connection_request.Status() ==
                    BluetoothLEPreferredConnectionParametersRequestStatus::Success)
                    return;
                connection_request.Close();
                connection_request = nullptr;
            } catch (const hresult_error &) {
            }
            preferred_interval = 12;
        }
        connection_request = device.RequestPreferredConnectionParameters(
            BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
    }
    void maintain_parameters() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_parameters_check < std::chrono::seconds(5))
            return;
        last_parameters_check = now;
        try {
            const auto parameters = device.GetConnectionParameters();
            if (parameters && parameters.ConnectionInterval() != preferred_interval)
                prefer_low_latency();
        } catch (const hresult_error &) {
            // Link loss is handled by read(); a preference failure must not stop input.
        }
    }
    ~Impl() {
        packed_subscription.revoke();
        input_subscription.revoke();
        response_subscription.revoke();
        // Production connections are closed below. Never queue a radio write
        // during USB handoff or link loss; an unreachable CCCD can take seconds.
        // Shared standalone probes still restore their explicit subscription.
        if (packed_enabled && share_services && device &&
            device.ConnectionStatus() == BluetoothConnectionStatus::Connected) {
            try {
                wait(packed.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::None));
            } catch (...) {}
        }
        for (auto service : services)
            service.Close();
        if (connection_request)
            connection_request.Close();
        if (session) {
            session.MaintainConnection(false);
            session.Close();
        }
        if (device)
            device.Close();
    }
    void send(GattCharacteristic characteristic, std::span<const uint8_t> data) {
        auto properties = characteristic.CharacteristicProperties();
        auto mode = (properties & GattCharacteristicProperties::WriteWithoutResponse) !=
                            GattCharacteristicProperties::None
                        ? GattWriteOption::WriteWithoutResponse
                        : GattWriteOption::WriteWithResponse;
        check_cancelled();
        check(await(characteristic.WriteValueAsync(buffer(data), mode)), "Bluetooth write");
    }
    std::vector<uint8_t> transact(uint8_t id, uint8_t sub, std::span<const uint8_t> payload) {
        if (payload.size() > 255)
            throw std::runtime_error("Vendor command too long");
        std::vector<uint8_t> packet{id, 0x91, 1, sub, 0, uint8_t(payload.size()), 0, 0};
        packet.insert(packet.end(), payload.begin(), payload.end());
        {
            std::lock_guard lock(notifications->mutex);
            notifications->command = id;
            notifications->response.clear();
            notifications->waiting = true;
        }
        send(command, packet);
        std::unique_lock lock(notifications->mutex);
        const auto deadline = GetTickCount64() + 3000;
        while (notifications->response.empty()) {
            lock.unlock();
            check_cancelled();
            lock.lock();
            if (GetTickCount64() >= deadline) {
                notifications->waiting = false;
                throw std::runtime_error("Controller did not acknowledge Bluetooth command " +
                                         std::to_string(id));
            }
            notifications->ready.wait_for(lock, std::chrono::milliseconds(20),
                                            [&] { return !notifications->response.empty(); });
        }
        auto reply = std::move(notifications->response);
        notifications->waiting = false;
        if (reply.size() < 8 || reply[0] != id || reply[1] != 1)
            throw std::runtime_error("Invalid Bluetooth command response");
        return {reply.begin() + 8, reply.end()};
    }
};
BleController::BleController(BleDevice controller, BleConnectionOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->cancelled = std::move(options.cancelled);
    impl_->trace = std::move(options.trace);
    impl_->share_services = options.share_services;
    impl_->kind = controller.kind;
    impl_->device = impl_->await(BluetoothLEDevice::FromBluetoothAddressAsync(controller.address), "device");
    if (!impl_->device)
        throw std::runtime_error("Bluetooth controller unavailable");
    if (!impl_->device.DeviceInformation().Pairing().IsPaired())
        throw std::runtime_error("Pair this controller in Windows Bluetooth settings first");
    // Do not queue GATT operations against a sleeping controller. Windows can
    // leave those requests ahead of the fresh connection after Home is pressed.
    // Only initial adoption may reuse a link Windows has already established.
    // Ordinary reconnects require an advertisement directed to this PC, never
    // an open SYNC advertisement or a wake intended for a different host.
    if (options.wait_until_available && !(options.reuse_windows_connection &&
        impl_->device.ConnectionStatus() == BluetoothConnectionStatus::Connected)) {
        const auto adapter = impl_->await(BluetoothAdapter::GetDefaultAsync(), "adapter");
        if (!adapter) throw std::runtime_error("No Bluetooth adapter");
        if (impl_->trace) impl_->trace("trace: BT awaiting this PC's advertisement");
        auto stop = [&] {
            return (impl_->cancelled && impl_->cancelled()) ||
                   (options.reuse_windows_connection &&
                    impl_->device.ConnectionStatus() == BluetoothConnectionStatus::Connected);
        };
        while (!stop() && !scan_ble(60000, stop, controller.kind, false, controller.address,
                                   adapter.BluetoothAddress())) {}
        impl_->check_cancelled();
        if (!impl_->device.DeviceInformation().Pairing().IsPaired())
            throw std::runtime_error("Controller was removed from Windows Bluetooth settings");
    }
    impl_->session = impl_->await(GattSession::FromDeviceIdAsync(impl_->device.BluetoothDeviceId()), "session");
    if (!impl_->session)
        throw std::runtime_error("Cannot create Bluetooth GATT session");
    impl_->session.MaintainConnection(true);
    // Cached discovery and the notification subscription below actively request
    // the connection. Waiting only for MaintainConnection's passive reconnect
    // left this adapter idle for about 18 seconds after USB removal.
    // Keep the request alive for the connection; releasing it restores the OS default.
    try {
        impl_->connection_request = impl_->device.RequestPreferredConnectionParameters(
            BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
    } catch (const hresult_error &) {
        OutputDebugStringW(L"Switch2Native: Bluetooth throughput preference unavailable; retaining "
                           L"default connection parameters\n");
    }
    const auto cache = controller.pairing ? BluetoothCacheMode::Uncached : BluetoothCacheMode::Cached;
    // Every input, command, reply and rumble characteristic we use is in this
    // service. Avoid probing the controller's unrelated/empty GATT services.
    const guid controller_service{L"ab7de9be-89fe-49ad-828f-118f09df7fd0"};
    auto services = impl_->await(impl_->device.GetGattServicesForUuidAsync(controller_service, cache), "services");
    check(services.Status(), "Discover Bluetooth services");
    const guid input_uuid{L"ab7de9be-89fe-49ad-828f-118f09df7fd2"};
    const guid packed_uuid{controller.kind == ControllerKind::JoyLeft ? L"cc1bbbb5-7354-4d32-a716-a81cb241a32a" :
                          controller.kind == ControllerKind::JoyRight ? L"d5a9e01e-2ffc-4cca-b20c-8b67142bf442" :
                          L"7492866c-ec3e-4619-8258-32755ffcc0f8"};
    const guid command_uuid{L"649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"};
    const guid response_uuid{L"c765a961-d9d8-4d36-a20a-5315b111836a"};
    const guid rumble_uuid{controller.kind == ControllerKind::JoyLeft ? L"289326cb-a471-485d-a8f4-240c14f18241" :
                          controller.kind == ControllerKind::JoyRight ? L"fa19b0fb-cd1f-46a7-84a1-bbb09e00c149" :
                          L"cc483f51-9258-427d-a939-630c31f72b05"};
    for (auto service : services.Services()) {
        impl_->services.push_back(service);
        if (options.share_services) {
            // A standalone capture can observe the alternate characteristic in
            // the same process while this owner performs normal initialization.
            const auto status = impl_->await(service.OpenAsync(GattSharingMode::SharedReadAndWrite));
            if (status != GattOpenStatus::Success && status != GattOpenStatus::AlreadyOpened)
                continue;
        }
        auto chars = impl_->await(service.GetCharacteristicsAsync(cache), "characteristics");
        if (chars.Status() == GattCommunicationStatus::Unreachable)
            check(chars.Status(), "Discover Bluetooth characteristics");
        if (chars.Status() != GattCommunicationStatus::Success)
            continue;
        for (auto characteristic : chars.Characteristics()) {
            auto id = characteristic.Uuid();
            if (id == input_uuid)
                impl_->input = characteristic;
            if (id == packed_uuid)
                impl_->packed = characteristic;
            if (id == command_uuid)
                impl_->command = characteristic;
            if (id == response_uuid)
                impl_->response = characteristic;
            if (id == rumble_uuid)
                impl_->rumble = characteristic;
        }
    }
    if (!impl_->input || !impl_->command || !impl_->response)
        throw std::runtime_error(
            "Nintendo Bluetooth characteristics unavailable; close other controller software");
    auto notifications = impl_->notifications;
    impl_->response_subscription = impl_->response.ValueChanged(
        auto_revoke, [notifications](auto const &, const GattValueChangedEventArgs &event) {
            try {
                auto packet = bytes(event.CharacteristicValue());
                std::lock_guard lock(notifications->mutex);
                if (packet.size() >= 8 && notifications->waiting &&
                    packet[0] == notifications->command) {
                    notifications->response = std::move(packet);
                    notifications->ready.notify_all();
                }
            } catch (...) {
            }
        });
    check(impl_->await(impl_->response.WriteClientCharacteristicConfigurationDescriptorAsync(
              GattClientCharacteristicConfigurationDescriptorValue::Notify), "reply subscription"),
          "Subscribe to Bluetooth replies");
    impl_->input_subscription = impl_->input.ValueChanged(
        auto_revoke, [notifications](auto const &, const GattValueChangedEventArgs &event) {
            try {
                auto packet = bytes(event.CharacteristicValue());
                std::lock_guard lock(notifications->mutex);
                if (notifications->input.size() >= 64)
                    notifications->input.pop_front();
                notifications->input.push_back(std::move(packet));
                notifications->ready.notify_all();
            } catch (...) {
            }
        });
    check(impl_->await(impl_->input.WriteClientCharacteristicConfigurationDescriptorAsync(
              GattClientCharacteristicConfigurationDescriptorValue::Notify), "input subscription"),
          "Subscribe to Bluetooth input");
    // Apply explicit parameters only after establishing the physical connection.
    try {
        impl_->prefer_low_latency();
        impl_->last_parameters_check = std::chrono::steady_clock::now();
    } catch (const hresult_error &) {
        OutputDebugStringW(L"Switch2Native: cannot request low-latency Bluetooth parameters\n");
    }
}
BleController::~BleController() = default;
void BleController::initialize(bool pair) {
    if (pair) {
        auto adapter = wait(BluetoothAdapter::GetDefaultAsync());
        if (!adapter)
            throw std::runtime_error("No Bluetooth adapter");
        auto address = adapter.BluetoothAddress();
        std::vector<uint8_t> mac{0, 2};
        for (int copy = 0; copy < 2; ++copy)
            for (unsigned i = 0; i < 6; ++i)
                mac.push_back(uint8_t(address >> (8 * i)));
        impl_->transact(0x15, 1, mac);
        const uint8_t key1[] = {0,    0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
                                0x79, 0xee, 7,    0xf2, 0x53, 0x2c, 0x6c, 0x31};
        const uint8_t key2[] = {0,    0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
                                0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73};
        const uint8_t finish[] = {0};
        impl_->transact(0x15, 4, key1);
        impl_->transact(0x15, 2, key2);
        impl_->transact(0x15, 3, finish);
    }
    const uint8_t features[] = {0x27, 0, 0, 0};
    impl_->transact(0x0c, 2, features);
    if (impl_->kind != ControllerKind::Pro)
        impl_->transact(0x11, 1, {});
    impl_->transact(0x0c, 4, features);
    set_player_lights(1);
}
void BleController::set_player_lights(uint8_t mask) {
    if (mask & 0xf0) throw std::invalid_argument("Player LED mask exceeds four LEDs");
    const uint8_t lights[] = {mask, 0, 0, 0};
    impl_->transact(9, 7, lights);
}
void BleController::set_mouse_sensor(bool enabled) {
    if (impl_->kind != ControllerKind::JoyLeft && impl_->kind != ControllerKind::JoyRight)
        throw std::invalid_argument("Optical mouse input requires a Joy-Con");
    const uint8_t feature_mask[]{0x37, 0, 0, 0};
    const uint8_t mouse_feature[]{0x10, 0, 0, 0};
    impl_->transact(0x0c, 2, feature_mask);
    impl_->transact(0x0c, enabled ? 4 : 5, mouse_feature);
}
std::vector<uint8_t> BleController::read(unsigned timeout_ms) {
    impl_->check_cancelled();
    impl_->maintain_parameters();
    auto &shared = *impl_->notifications;
    std::unique_lock lock(shared.mutex);
    if (!shared.ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return !shared.input.empty(); })) {
        if (impl_->device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
            throw std::runtime_error("Bluetooth link disconnected");
        return {};
    }
    auto result = std::move(shared.input.front());
    shared.input.pop_front();
    return result;
}
void BleController::write(std::span<const uint8_t> report) {
    if (!impl_->rumble)
        throw std::runtime_error("Controller rumble characteristic unavailable");
    impl_->send(impl_->rumble, report);
}
std::vector<uint8_t> BleController::motion_configuration() {
    return impl_->transact(0x11, 3, {});
}
void BleController::enable_packed_motion() {
    if (!impl_->packed)
        throw std::runtime_error("Packed controller input unavailable");
    auto notifications = impl_->notifications;
    const uint8_t report_id = impl_->kind == ControllerKind::JoyLeft ? 7 :
                              impl_->kind == ControllerKind::JoyRight ? 8 : 9;
    impl_->packed_subscription = impl_->packed.ValueChanged(
        auto_revoke, [notifications, report_id](auto const &, const GattValueChangedEventArgs &event) {
            try {
                auto packet = bytes(event.CharacteristicValue());
                packet.insert(packet.begin(), report_id);
                std::lock_guard lock(notifications->mutex);
                if (notifications->input.size() >= 64) notifications->input.pop_front();
                notifications->input.push_back(std::move(packet));
                notifications->ready.notify_all();
            } catch (...) {}
        });
    check(impl_->await(impl_->packed.WriteClientCharacteristicConfigurationDescriptorAsync(
                   GattClientCharacteristicConfigurationDescriptorValue::Notify)),
          "Subscribe to packed controller motion");
    impl_->packed_enabled = true;
    // An 800 Hz Joy-Con IMU usually encodes two samples at a 10 ms connection
    // interval. At 7.5 ms many reports contain only one sample instead.
    // The Pro's 960 Hz clock already supplies about 200 samples/s at 7.5 ms.
    impl_->preferred_interval = impl_->kind == ControllerKind::Pro ? 6 : 8;
    try { impl_->prefer_low_latency(); } catch (const hresult_error &) {}
    impl_->last_parameters_check = std::chrono::steady_clock::now();
}
std::vector<uint8_t> BleController::read_memory(uint32_t address, uint8_t length) {
    const uint8_t payload[] = {length,
                               0x7e,
                               0,
                               0,
                               uint8_t(address),
                               uint8_t(address >> 8),
                               uint8_t(address >> 16),
                               uint8_t(address >> 24)};
    auto data = impl_->transact(2, 4, payload);
    if (data.size() < size_t(8 + length) || data[0] != length ||
        !std::equal(data.begin() + 4, data.begin() + 8, payload + 4))
        throw std::runtime_error("Invalid Bluetooth memory read response");
    return {data.begin() + 8, data.begin() + 8 + length};
}
} // namespace switch2
