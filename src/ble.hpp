#pragma once
#include "windows_support.hpp"
#include "protocol.hpp"
#include <memory>
#include <optional>

namespace switch2 {
struct BleDevice {
    uint64_t address{};
    bool pairing{};
    ControllerKind kind{ControllerKind::Pro};
};
std::optional<BleDevice>
scan_ble(unsigned milliseconds, const std::function<bool()> &stop = [] { return false; },
         ControllerKind kind = ControllerKind::Pro, bool pairing_only = false,
         uint64_t address = 0, uint64_t required_host = 0);
struct BleConnectionOptions {
    bool share_services{};
    bool wait_until_available{};
    bool reuse_windows_connection{};
    std::function<bool()> cancelled;
    std::function<void(const std::string &)> trace;
};
class BleController {
  public:
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("Bluetooth operation cancelled") {} };
    explicit BleController(BleDevice device, BleConnectionOptions options = {});
    ~BleController();
    BleController(const BleController &) = delete;
    BleController &operator=(const BleController &) = delete;
    void initialize(bool pair);
    void set_player_lights(uint8_t mask);
    std::vector<uint8_t> read(unsigned timeout_ms);
    void write(std::span<const uint8_t> report);
    std::vector<uint8_t> read_memory(uint32_t address, uint8_t length);
    std::vector<uint8_t> motion_configuration();
    void enable_packed_motion();
    void set_mouse_sensor(bool enabled);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace switch2
