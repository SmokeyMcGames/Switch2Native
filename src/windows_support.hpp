#pragma once
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace switch2 {
inline void fail(const char *operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (valid())
            CloseHandle(value_);
        value_ = value;
    }
    bool valid() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept {
        return value_;
    }

  private:
    HANDLE value_;
};
std::vector<std::wstring> device_paths(const GUID &guid);
inline bool is_controller(const std::wstring &path) {
    std::wstring lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    return lower.find(L"vid_057e&pid_2069") != std::wstring::npos;
}
} // namespace switch2
