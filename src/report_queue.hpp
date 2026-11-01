#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace switch2 {
// Bounded report history: preserve bursts without allocating in the input path.
// On overflow discard the oldest report so a stalled consumer can catch up.
template <class Report, size_t Capacity> struct ReportQueue {
    static_assert(Capacity > 0);
    std::array<Report, Capacity> reports{};
    size_t head{}, count{};
    uint32_t dropped{};

    void clear() {
        head = count = 0;
    }
    template <class Predicate> void remove_if(Predicate remove) {
        size_t kept = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto &report = reports[(head + i) % Capacity];
            if (!remove(report)) reports[(head + kept++) % Capacity] = report;
        }
        count = kept;
    }
    bool empty() const {
        return count == 0;
    }
    const Report &front() const {
        return reports[head];
    }
    void push(const Report &report) {
        if (count == Capacity) {
            head = (head + 1) % Capacity;
            --count;
            ++dropped;
        }
        reports[(head + count) % Capacity] = report;
        ++count;
    }
    void pop() {
        if (count) {
            head = (head + 1) % Capacity;
            --count;
        }
    }
};
} // namespace switch2
