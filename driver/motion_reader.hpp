#pragma once
#include "hid_helpers.hpp"
#include "motion_snapshot.hpp"
#include <functional>
#include <thread>

class MotionReader {
  public:
    using Publish = std::function<void(const switch2::MotionSnapshot &)>;
    explicit MotionReader(Publish publish, std::wstring serial = L"Switch2Native-01")
        : serial_(std::move(serial)), stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!stop_event_.valid())
            switch2::fail("Create motion stop event");
        thread_ = std::jthread([this, publish = std::move(publish)] { run(publish); });
    }
    ~MotionReader() {
        SetEvent(stop_event_.get());
        thread_.join();
    }

  private:
    switch2::Handle find_stream() {
        GUID guid;
        HidD_GetHidGuid(&guid);
        for (const auto &path : switch2::device_paths(guid)) {
            switch2::Handle handle(CreateFileW(path.c_str(), GENERIC_READ,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            WCHAR serial[64]{};
            if (handle.valid() && switch2::has_hid_usage(handle.get(), 0xff00, 2) &&
                HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial)) &&
                std::wstring(serial) == serial_) {
                if (!HidD_SetNumInputBuffers(handle.get(), 128))
                    switch2::fail("Set motion stream buffers");
                return handle;
            }
        }
        return switch2::Handle{};
    }
    bool read(HANDLE handle, HANDLE event, switch2::MotionSnapshot &snapshot) {
        ResetEvent(event);
        OVERLAPPED operation{};
        operation.hEvent = event;
        DWORD received = 0;
        if (!ReadFile(handle, &snapshot, sizeof(snapshot), &received, &operation)) {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            const HANDLE events[]{stop_event_.get(), event};
            const auto result = WaitForMultipleObjects(2, events, FALSE, 2000);
            if (result != WAIT_OBJECT_0 + 1) {
                CancelIoEx(handle, &operation);
                GetOverlappedResult(handle, &operation, &received, TRUE);
                return false;
            }
            if (!GetOverlappedResult(handle, &operation, &received, FALSE))
                return false;
        }
        return received == sizeof(snapshot) && snapshot.id == 0xe4 && snapshot.version == 1;
    }
    void run(const Publish &publish) {
        switch2::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        switch2::Handle handle;
        uint32_t sequence = 0;
        bool available = false;
        publish(switch2::MotionSnapshot{});
        if (!event.valid())
            return;
        switch2::MaintenancePause maintenance;
        while (WaitForSingleObject(stop_event_.get(), 0) == WAIT_TIMEOUT) {
            if (maintenance.active()) {
                handle.reset();
                if (available) publish(switch2::MotionSnapshot{});
                available = false;
                WaitForSingleObject(stop_event_.get(), 100);
                continue;
            }
            if (!handle.valid()) {
                try {
                    handle = find_stream();
                } catch (...) {
                }
                if (!handle.valid()) {
                    WaitForSingleObject(stop_event_.get(), 1000);
                    continue;
                }
            }
            switch2::MotionSnapshot snapshot;
            if (!read(handle.get(), event.get(), snapshot)) {
                handle.reset();
                if (available)
                    publish(switch2::MotionSnapshot{});
                available = false;
                continue;
            }
            if (snapshot.ready && GetTickCount64() - snapshot.updated_ms < 2000) {
                if (!available || snapshot.sequence != sequence)
                    publish(snapshot);
                sequence = snapshot.sequence;
                available = true;
            } else if (available) {
                snapshot.ready = 0;
                publish(snapshot);
                available = false;
            }
        }
    }
    std::wstring serial_;
    switch2::Handle stop_event_;
    std::jthread thread_;
};
