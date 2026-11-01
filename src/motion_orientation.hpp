#pragma once
#include "protocol.hpp"

namespace switch2 {
// Body-to-world orientation for visualization, not an absolute position sensor.
// Gravity constrains tilt; yaw remains relative and can drift without a heading reference.
class MotionOrientation {
  public:
    void update(const Motion &sample);
    std::array<float, 4> quaternion() const;

  private:
    std::array<double, 4> rotation_{1, 0, 0, 0};
    uint64_t previous_time_{};
};
} // namespace switch2
