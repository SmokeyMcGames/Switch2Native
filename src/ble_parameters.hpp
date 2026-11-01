#pragma once
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.h>

namespace switch2 {
// The public interface can supply explicit values even though the runtime class
// only offers preset factories. Verified at 7.5 ms on this Windows 11 adapter;
// callers must retain the request and fall back if Windows rejects it.
struct BleConnectionParameters
    : winrt::implements<
          BleConnectionParameters,
          winrt::Windows::Devices::Bluetooth::IBluetoothLEPreferredConnectionParameters> {
    explicit BleConnectionParameters(uint16_t interval) : interval_(interval) {}
    uint16_t LinkTimeout() const {
        return 200;
    }
    uint16_t ConnectionLatency() const {
        return 0;
    }
    uint16_t MinConnectionInterval() const {
        return interval_;
    }
    uint16_t MaxConnectionInterval() const {
        return interval_;
    }

  private:
    uint16_t interval_;
};
inline auto ble_connection_parameters(uint16_t interval) {
    return winrt::make<BleConnectionParameters>(interval)
        .as<winrt::Windows::Devices::Bluetooth::BluetoothLEPreferredConnectionParameters>();
}
} // namespace switch2
