#include "gyro_motion.hpp"
#include "gamepad_reports.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace switch2;
static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static bool near(double a, double b, double tolerance = 1e-7) { return std::abs(a - b) < tolerance; }
static GyroDelta turn(GyroMotion &tracker, Motion &sample, std::array<float, 3> rate,
                      unsigned count, uint64_t host_step = 50000) {
    GyroDelta total;
    sample.angular_velocity = rate;
    for (unsigned i = 0; i < count; ++i) {
        sample.sample_time_100ns += host_step;
        const auto delta = tracker.update(sample);
        check(delta.has_value(), "genuine motion sample accepted");
        total.x += delta->x; total.y += delta->y;
    }
    return total;
}
int main() {
    try {
        GyroMotion tracker;
        Motion sample{true, {0, 0, 1}, {}, 10000000, 50000};
        tracker.begin(sample);
        auto slow = turn(tracker, sample, {0, 0, -3}, 1000, 49000);
        auto fast = turn(tracker, sample, {0, 0, 300}, 10, 51000);
        check(near(slow.x, 15 * GyroCountsPerDegree) && near(slow.y, 0), "fixed rotation-to-count response");
        check(near(slow.x + fast.x, 0) && near(fast.y, 0), "slow outward and fast return match despite host-clock slew");
        check(!tracker.update(sample), "duplicate sample cannot move twice");
        const auto timestamp = sample.sample_time_100ns;
        sample.sample_time_100ns -= 10000;
        check(!tracker.update(sample), "backward sample ignored");
        sample.sample_time_100ns = timestamp + 2000000;
        check(!tracker.update(sample) && tracker.needs_start(), "stalled transport cannot create catch-up jump");

        tracker = {};
        sample = {true, {0, 0, 1}, {}, 10000000, 100000};
        tracker.begin(sample);
        auto slower_packets = turn(tracker, sample, {0, 0, -3}, 500, 100000);
        check(near(slower_packets.x, slow.x), "response independent of report rate");
        auto pitch = turn(tracker, sample, {30, 0, 0}, 100, 100000);
        check(near(pitch.x, 0) && near(pitch.y, -30 * GyroCountsPerDegree), "pitch response has no orientation pull");
        for (unsigned i = 0; i < 200; ++i) {
            sample.acceleration = {float(std::sin(i * 0.02)), 0, float(std::cos(i * 0.02))};
            const auto still = turn(tracker, sample, {}, 1, 100000);
            check(near(still.x, 0) && near(still.y, 0), "gravity correction cannot move stationary pointer");
        }

        tracker = {};
        sample = {true, {0, 0, 1}, {0.3f, -0.2f, -0.6f}, 10000000, 50000};
        for (unsigned i = 0; i < 200; ++i) { sample.sample_time_100ns += 50000; tracker.calibrate(sample); }
        tracker.begin(sample);
        auto still = turn(tracker, sample, {0.3f, -0.2f, -0.6f}, 400);
        check(near(still.x, 0) && near(still.y, 0), "resting bias does not move pointer");
        still = turn(tracker, sample, {0.4f, -0.2f, -0.7f}, 200);
        check(near(still.x, 0) && near(still.y, 0), "small residual noise is gated");
        auto precise = turn(tracker, sample, {0.3f, -0.2f, -1.6f}, 200);
        check(near(precise.x, GyroCountsPerDegree, 1e-5), "slow deliberate motion keeps its full scale");
        tracker.reset();
        sample.sample_time_100ns += 10000000;
        tracker.begin(sample);
        still = turn(tracker, sample, {0.3f, -0.2f, -0.6f}, 20);
        check(near(still.x, 0) && near(still.y, 0), "pause/resume cannot replay accumulated time");

        tracker = {};
        sample = {true, {-1, 0, 0}, {}, 10000000, 50000};
        tracker.begin(sample);
        const auto rolled = turn(tracker, sample, {30, 0, 0}, 200);
        check(near(rolled.x, 30 * GyroCountsPerDegree) && near(rolled.y, 0), "sideways grip preserves yaw direction");
        std::cout << "Direct gyro response, device timing, drift gate and relative motion tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
