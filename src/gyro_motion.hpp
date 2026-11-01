#pragma once
#include "protocol.hpp"

namespace switch2 {
inline constexpr double GyroCountsPerDegree = 90.0;
struct GyroDelta { double x{}, y{}; }; // Relative mouse counts, before Windows pointer processing.

class GyroMotion {
  public:
    void calibrate(const Motion &sample);
    void begin(const Motion &sample);
    std::optional<GyroDelta> update(const Motion &sample);
    void reset();
    bool needs_start() const { return !started_; }

  private:
    using Vector = std::array<double, 3>;
    static Vector cross(const Vector &a, const Vector &b);
    static double dot(const Vector &a, const Vector &b);
    static Vector unit(Vector value);
    static bool valid(const Motion &sample);
    Vector gravity_{0, 0, 1}, bias_{}, calibration_mean_{}, calibration_m2_{}, acceleration_mean_{};
    uint64_t previous_time_{}, calibration_time_{};
    unsigned calibration_count_{};
    bool started_{};
};
} // namespace switch2
