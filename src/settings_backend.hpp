#pragma once
#include "joy_ipc.hpp"

namespace switch2 {
// Owned by the service's pipe thread. No device I/O runs on the tray UI thread.
class SettingsBackend {
  public:
    joy_ipc::SettingsResponse process(const joy_ipc::Request &request, bool interactive);

  private:
    void refresh();
    std::array<Handle, 2> hubs_;
    std::array<uint64_t, 2> retry_at_{};
    joy_ipc::SettingsResponse current_;
};
} // namespace switch2
