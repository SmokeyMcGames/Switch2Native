#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace switch2 {
// A short diagnostic-only flash lets a 25 Hz window show taps between refreshes.
// It never feeds gamepad, mouse or sensor reports.
class ButtonPreview {
  public:
    void update(uint32_t buttons, uint32_t now_ms) {
        auto pressed = buttons & ~held_;
        seen_ |= pressed;
        while (pressed) {
            const unsigned bit = std::countr_zero(pressed);
            pressed_at_[bit] = now_ms;
            pressed &= pressed - 1;
        }
        held_ = buttons;
    }
    uint32_t recent(uint32_t now_ms) const {
        uint32_t result = 0, candidates = seen_;
        while (candidates) {
            const unsigned bit = std::countr_zero(candidates);
            if (uint32_t(now_ms - pressed_at_[bit]) < 120)
                result |= 1u << bit;
            candidates &= candidates - 1;
        }
        return result;
    }

  private:
    uint32_t held_{}, seen_{};
    std::array<uint32_t, 32> pressed_at_{};
};
} // namespace switch2
