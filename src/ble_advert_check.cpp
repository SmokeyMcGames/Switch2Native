#include "ble.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Storage.Streams.h>
#include <mutex>
#include <iomanip>

int main(int argc, char **argv) {
    try {
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
        using winrt::Windows::Storage::Streams::DataReader;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::cout << std::unitbuf;
        const auto seconds = argc == 2 ? std::stoul(argv[1]) : 180;
        auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        std::cout << "host=" << std::hex << adapter.BluetoothAddress() << std::dec << '\n';
        std::mutex mutex;
        BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(BluetoothLEScanningMode::Active);
        auto subscription = watcher.Received(winrt::auto_revoke, [&](auto const &, auto const &event) {
            try {
                for (auto manufacturer : event.Advertisement().ManufacturerData()) {
                    if (manufacturer.CompanyId() != 0x0553) continue;
                    std::vector<uint8_t> data(manufacturer.Data().Length());
                    DataReader::FromBuffer(manufacturer.Data()).ReadBytes(data);
                    if (data.size() < 16 || data[3] != 0x7e || data[4] != 0x05 || data[6] != 0x20 ||
                        (data[5] != 0x69 && data[5] != 0x67 && data[5] != 0x66)) continue;
                    std::lock_guard lock(mutex);
                    std::cout << "tick_ms=" << GetTickCount64() << " controller=" << std::hex << event.BluetoothAddress() << " data=";
                    for (const auto value : data) std::cout << std::setfill('0') << std::setw(2) << unsigned(value);
                    std::cout << std::dec << '\n';
                }
            } catch (...) {}
        });
        watcher.Start();
        Sleep(seconds * 1000);
        watcher.Stop();
        subscription.revoke();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
