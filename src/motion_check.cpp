#include "windows_support.hpp"
#include "motion_snapshot.hpp"
#include "hid_helpers.hpp"
#include "../driver/sensor_reports.hpp"
#include <hidsdi.h>

int main() {
    try {
        GUID guid;
        HidD_GetHidGuid(&guid);
        for (auto const &path : switch2::device_paths(guid)) {
            switch2::Handle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr, OPEN_EXISTING, 0, nullptr));
            WCHAR serial[64]{};
            if (!handle.valid() ||
                !HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial)))
                continue;
            if (std::wstring(serial) == L"Switch2Native-01" &&
                switch2::has_hid_usage(handle.get(), 1, 5)) {
                switch2::MotionSnapshot snapshot;
                if (!HidD_GetFeature(handle.get(), &snapshot, sizeof(snapshot)))
                    switch2::fail("Motion feature");
                std::cout << "Gamepad motion: version=" << unsigned(snapshot.version)
                          << " transport=" << unsigned(snapshot.transport)
                          << " ready=" << unsigned(snapshot.ready)
                          << " sequence=" << snapshot.sequence
                          << " age=" << GetTickCount64() - snapshot.updated_ms
                          << " gyro=" << snapshot.angular_velocity[0] << ','
                          << snapshot.angular_velocity[1] << ',' << snapshot.angular_velocity[2]
                          << " accel=" << snapshot.acceleration[0] << ','
                          << snapshot.acceleration[1] << ',' << snapshot.acceleration[2] << '\n';
            } else if (std::wstring(serial) == L"Switch2Native-Motion-01" &&
                       switch2::has_hid_usage(handle.get(), 0xff00, 1)) {
                std::cout << "Home companion available; native motion uses the Windows SensorsCx device.\n";
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
