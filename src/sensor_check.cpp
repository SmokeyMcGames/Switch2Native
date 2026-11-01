#include <windows.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Sensors.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

using namespace winrt;
using namespace Windows::Devices::Sensors;
using Windows::Devices::Enumeration::DeviceInformation;
static bool is_controller_sensor(hstring const &id, const std::wstring &hardware_id) {
    std::wstring instance(id);
    if (instance.rfind(L"\\\\?\\", 0) != 0)
        return false;
    instance.erase(0, 4);
    auto suffix = instance.find(L"#{");
    if (suffix == std::wstring::npos)
        return false;
    instance.resize(suffix);
    std::replace(instance.begin(), instance.end(), L'#', L'\\');
    DEVINST device;
    if (CM_Locate_DevNodeW(&device, instance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return false;
    for (unsigned depth = 0; depth < 8; ++depth) {
        WCHAR hardware[512]{};
        ULONG size = sizeof(hardware), type = 0;
        if (CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_HARDWAREID, &type, hardware, &size,
                                              0) == CR_SUCCESS) {
            for (const WCHAR *value = hardware; *value; value += wcslen(value) + 1)
                if (_wcsicmp(value, hardware_id.c_str()) == 0)
                    return true;
        }
        DEVINST parent;
        if (CM_Get_Parent(&parent, device, 0) != CR_SUCCESS)
            break;
        device = parent;
    }
    return false;
}
struct Statistics {
    std::mutex lock;
    unsigned count{};
    std::array<double, 3> peak{}, sum{}, latest{};
    int64_t first{}, last{};
    int64_t first_arrival{}, last_arrival{}, minimum_delta{INT64_MAX}, maximum_delta{};
    int64_t minimum_age{INT64_MAX}, maximum_age{INT64_MIN};
    double age_sum{};
    unsigned nonmonotonic{}, five_ms{};
    bool has_samples() {
        std::lock_guard guard(lock);
        return count != 0;
    }
    void add(std::array<double, 3> values, int64_t timestamp) {
        std::lock_guard guard(lock);
        const auto arrival = std::chrono::steady_clock::now().time_since_epoch().count();
        FILETIME utc;
        GetSystemTimePreciseAsFileTime(&utc);
        const auto age = int64_t((uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime) - timestamp;
        minimum_age = std::min(minimum_age, age);
        maximum_age = std::max(maximum_age, age);
        age_sum += double(age);
        if (!count) {
            first = timestamp;
            first_arrival = arrival;
        } else {
            const auto delta = timestamp - last;
            if (delta <= 0) ++nonmonotonic;
            minimum_delta = std::min(minimum_delta, delta);
            maximum_delta = std::max(maximum_delta, delta);
            if (delta >= 49000 && delta <= 51000) ++five_ms;
        }
        last_arrival = arrival;
        last = timestamp;
        ++count;
        latest = values;
        for (size_t i = 0; i < 3; ++i) {
            peak[i] = std::max(peak[i], std::abs(values[i]));
            sum[i] += values[i];
        }
    }
    void print(const char *name, bool final) {
        std::lock_guard guard(lock);
        std::cout << name << " samples=" << count << " latest=" << latest[0] << ',' << latest[1]
                  << ',' << latest[2];
        if (final && count) {
            std::cout << " peak=" << peak[0] << ',' << peak[1] << ',' << peak[2]
                      << " mean=" << sum[0] / count << ',' << sum[1] / count << ','
                      << sum[2] / count;
            if (last > first)
                std::cout << " Hz=" << (count - 1) * 1e7 / double(last - first);
            if (count > 1) {
                const auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::duration(last_arrival - first_arrival)).count();
                std::cout << " wall_Hz=" << (count - 1) / elapsed
                          << " delta_ms=" << minimum_delta / 10000.0 << ':' << maximum_delta / 10000.0
                          << " five_ms=" << five_ms << " nonmonotonic=" << nonmonotonic;
                std::cout << " timestamp_age_ms=" << minimum_age / 10000.0 << ':' << maximum_age / 10000.0
                          << " mean_age_ms=" << age_sum / count / 10000.0;
            }
        }
        std::cout << '\n';
    }
};
int main(int argc, char **argv) {
    try {
        init_apartment(apartment_type::multi_threaded);
        std::cout << std::unitbuf << std::fixed << std::setprecision(4);
        unsigned seconds = 10, interval = 16;
        std::wstring hardware_id = L"ROOT\\SWITCH2PROSENSOR";
        for (int i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (argument == "--interval" && i + 1 < argc)
                interval = std::stoul(argv[++i]);
            else if (argument == "--joy-slot" && i + 1 < argc) {
                const std::string slot = argv[++i];
                if (slot != "1" && slot != "2" && slot != "3" && slot != "4") throw std::runtime_error("Joy-Con slot must be 1 through 4");
                hardware_id = L"ROOT\\SWITCH2JOYSENSOR" + std::wstring(slot.begin(), slot.end());
            }
            else
                throw std::runtime_error(
                    "Usage: SensorCheck [--seconds N] [--interval milliseconds] [--joy-slot 1..4]");
        }
        Gyrometer gyro{nullptr};
        Accelerometer accel{nullptr};
        for (auto const &device :
             DeviceInformation::FindAllAsync(Gyrometer::GetDeviceSelector()).get()) {
            std::cout << "Gyrometer: " << to_string(device.Name())
                      << " id=" << to_string(device.Id()) << '\n';
            if (is_controller_sensor(device.Id(), hardware_id))
                gyro = Gyrometer::FromIdAsync(device.Id()).get();
        }
        for (auto const &device :
             DeviceInformation::FindAllAsync(
                 Accelerometer::GetDeviceSelector(AccelerometerReadingType::Standard))
                 .get()) {
            std::cout << "Accelerometer: " << to_string(device.Name())
                      << " id=" << to_string(device.Id()) << '\n';
            if (is_controller_sensor(device.Id(), hardware_id))
                accel = Accelerometer::FromIdAsync(device.Id()).get();
        }
        if (!gyro || !accel) {
            std::cerr << "Switch2Native native Windows sensors not found or access unavailable\n";
            return 2;
        }
        // Use a fixed Windows display transform for comparable captures. WinRT
        // may rotate X/Y relative to the controller-frame E2 snapshot.
        gyro.ReadingTransform(Windows::Graphics::Display::DisplayOrientations::Landscape);
        accel.ReadingTransform(Windows::Graphics::Display::DisplayOrientations::Landscape);
        gyro.ReportInterval(std::max(interval, gyro.MinimumReportInterval()));
        accel.ReportInterval(std::max(interval, accel.MinimumReportInterval()));
        std::cout << "Native Windows sensors opened; gyro minimum=" << gyro.MinimumReportInterval()
                  << "ms, accel minimum=" << accel.MinimumReportInterval()
                  << "ms; requested=" << interval << "ms\n";
        auto gs = std::make_shared<Statistics>(), as = std::make_shared<Statistics>();
        auto gr = gyro.ReadingChanged(
            auto_revoke, [gs](auto const &, GyrometerReadingChangedEventArgs const &args) {
                auto r = args.Reading();
                gs->add({r.AngularVelocityX(), r.AngularVelocityY(), r.AngularVelocityZ()},
                        r.Timestamp().time_since_epoch().count());
            });
        auto ar = accel.ReadingChanged(
            auto_revoke, [as](auto const &, AccelerometerReadingChangedEventArgs const &args) {
                auto r = args.Reading();
                as->add({r.AccelerationX(), r.AccelerationY(), r.AccelerationZ()},
                        r.Timestamp().time_since_epoch().count());
            });
        for (unsigned i = 0; i < seconds; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            gs->print("gyro deg/s", false);
            as->print("accel g", false);
        }
        gr.revoke();
        ar.revoke();
        gyro.ReportInterval(0);
        accel.ReportInterval(0);
        gs->print("FINAL gyro deg/s", true);
        as->print("FINAL accel g", true);
        return gs->has_samples() && as->has_samples() ? 0 : 3;
    } catch (const hresult_error &error) {
        std::cerr << "Windows sensor error 0x" << std::hex << uint32_t(error.code().value) << ": "
                  << to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
