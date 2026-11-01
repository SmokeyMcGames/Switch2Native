#pragma once
#include "windows_support.hpp"
#include <winrt/Windows.Foundation.h>
#include <memory>

namespace switch2 {
template <class Operation>
auto wait_winrt(Operation operation, const std::function<void()> &check_cancelled = {},
                unsigned timeout_ms = 8000) {
    // A cancelled WinRT request may complete later. Its callback owns the
    // event until that happens, and each operation gets only one handler.
    auto ready = std::make_shared<Handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready->valid()) fail("Create Bluetooth completion event");
    operation.Completed([ready](auto const &, winrt::Windows::Foundation::AsyncStatus) {
        SetEvent(ready->get());
    });
    const auto deadline = GetTickCount64() + timeout_ms;
    try {
        for (;;) {
            const auto result = WaitForSingleObject(ready->get(), 20);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_TIMEOUT) fail("Wait for Bluetooth operation");
            if (check_cancelled) check_cancelled();
            if (GetTickCount64() >= deadline)
                throw std::runtime_error("Bluetooth operation timed out");
        }
    } catch (...) {
        try { operation.Cancel(); } catch (...) {}
        throw;
    }
    return operation.get();
}
}
