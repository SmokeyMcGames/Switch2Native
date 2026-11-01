#pragma once
#include "protocol.hpp"
#include <vector>

namespace switch2 {
struct WindowsPairedController {
    uint64_t address{};
    ControllerKind kind{ControllerKind::Pro};
};
// Enumerates Windows' saved associations without opening a GATT connection.
std::vector<WindowsPairedController> windows_paired_controllers();
}
