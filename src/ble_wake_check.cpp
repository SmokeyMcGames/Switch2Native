#include "windows_support.hpp"
#include "windows_pairing.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <array>
#include <atomic>

// Explicit, bounded diagnostic for the console's targeted wake advertisement.
// This is not part of the driver's ordinary reconnect or discovery path.
int main(int argc, char **argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: BleWakeCheck WINDOWS_PAIRED_ADDRESS");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::cout << std::unitbuf;
        const auto address = std::stoull(argv[1], nullptr, 16);
        const auto paired = switch2::windows_paired_controllers();
        if (!address || address > 0xffffffffffffull ||
            std::none_of(paired.begin(), paired.end(), [address](const auto &device) { return device.address == address; }))
            throw std::runtime_error("Target must be a recognized controller already paired in Windows");
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
        auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter || !adapter.IsPeripheralRoleSupported())
            throw std::runtime_error("Adapter does not support Bluetooth advertising");
        std::array<uint8_t, 12> payload{0x03, 0x00, 0x01, 0x00, 0x80};
        for (unsigned byte = 0; byte < 6; ++byte) payload[5 + byte] = uint8_t(address >> (8 * byte));
        payload[11] = 0x01;
        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(payload);
        BluetoothLEAdvertisementPublisher publisher;
        publisher.Advertisement().ManufacturerData().Append(BluetoothLEManufacturerData(0x0553, writer.DetachBuffer()));
        std::atomic<bool> failed{};
        auto changed = publisher.StatusChanged(winrt::auto_revoke, [&failed](auto const &, auto const &event) {
            std::cout << "publisher_status=" << int(event.Status()) << " error=" << int(event.Error()) << std::endl;
            if (event.Status() == BluetoothLEAdvertisementPublisherStatus::Aborted) failed = true;
        });
        publisher.Start();
        Sleep(2000);
        const bool started = publisher.Status() == BluetoothLEAdvertisementPublisherStatus::Started;
        publisher.Stop();
        Sleep(100);
        changed.revoke();
        std::cout << "wake_advertisement_started=" << started << '\n';
        return failed || !started ? 1 : 0;
    } catch (const winrt::hresult_error &error) { std::cerr << winrt::to_string(error.message()) << '\n'; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
}
