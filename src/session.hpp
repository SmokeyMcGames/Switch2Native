#pragma once
#include "protocol.hpp"
#include "controller_settings.hpp"
#include <atomic>
#include <functional>
#include <thread>
#include <string>

namespace switch2 {
enum class Transport : uint8_t { Disconnected, Usb, Bluetooth };
class Session {
  public:
    using Publish = std::function<void(const State &, const Calibration &, Transport, bool)>;
    using Log = std::function<void(const std::string &)>;
    Session(uint64_t bluetooth_address, Publish publish, Log log);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    void set_rumble(uint8_t strong, uint8_t weak) noexcept;
    ControllerSettingsState &settings() { return settings_; }

  private:
    void run(std::stop_token stop, uint64_t bluetooth_address);
    template <class Controller>
    void pump(Controller &controller, const Calibration &calibration, Transport transport,
              std::stop_token stop);
    Publish publish_;
    Log log_;
    std::atomic<uint16_t> rumble_{0};
    ControllerSettingsState settings_;
    std::jthread thread_;
};
} // namespace switch2
