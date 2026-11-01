#pragma once
#include "protocol.hpp"

namespace switch2 {
enum class MouseShortcut { None, ToggleEnabled, SwitchSource };
struct MouseShortcutResult {
    uint32_t buttons{};
    MouseShortcut action{};
};

// Keep Plus/Minus pending until release so it can be held as a modifier. Other
// shortcut buttons get a brief either-order window before ordinary forwarding.
class MouseShortcuts {
  public:
    MouseShortcutResult update(uint32_t raw, bool right, uint64_t now_ms) {
        const uint32_t modifier = right ? Plus : Minus;
        const uint32_t toggle = right ? Home : Capture;
        const uint32_t stick = right ? R3 : L3;
        const uint32_t candidates = modifier | toggle | stick;
        const uint32_t held = raw & candidates;
        MouseShortcutResult result{raw & ~candidates};
        if (!initialized_) {
            initialized_ = true;
            blocked_ = held; // A wake/arrival with a held button is not a shortcut.
        }
        if (!(held & blocked_)) blocked_ = 0;
        forwarded_ &= held;
        if (now_ms >= replay_until_) replayed_ = 0;
        const auto fresh = held & ~(pending_ | forwarded_ | blocked_ | replayed_);
        if (fresh && !pending_) deadline_ = now_ms + 150;
        pending_ |= fresh;
        if (pending_ && ((pending_ & modifier) || now_ms <= deadline_) && !(forwarded_ | blocked_ | replayed_)) {
            const bool enable_chord = (held & (modifier | toggle)) == (modifier | toggle);
            const bool source_chord = (held & (modifier | stick)) == (modifier | stick);
            if (enable_chord || source_chord) {
                if (enable_chord != source_chord)
                    result.action = enable_chord ? MouseShortcut::ToggleEnabled : MouseShortcut::SwitchSource;
                blocked_ = pending_ | held;
                pending_ = 0;
            }
        }
        const auto released = pending_ & ~held;
        const auto ready = released | (now_ms >= deadline_ ? (pending_ & ~modifier) : 0);
        if (released) {
            replayed_ |= released;
            replay_until_ = now_ms + 40; // Give ordinary gamepad polls time to see the released tap.
        }
        forwarded_ |= ready & held;
        pending_ &= ~ready;
        result.buttons |= forwarded_ | replayed_;
        return result;
    }
    void reset() { *this = {}; }

  private:
    bool initialized_{};
    uint32_t pending_{}, forwarded_{}, blocked_{}, replayed_{};
    uint64_t deadline_{}, replay_until_{};
};
} // namespace switch2
