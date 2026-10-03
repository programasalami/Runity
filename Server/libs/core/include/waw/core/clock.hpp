#pragma once

#include <chrono>

namespace waw::core {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Millis = std::chrono::milliseconds;

/// Source of the current time. Systems take an IClock& so tests can drive time by hand.
class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual TimePoint now() const = 0;
};

class SteadyClock final : public IClock {
public:
    [[nodiscard]] TimePoint now() const override { return Clock::now(); }
};

class ManualClock final : public IClock {
public:
    [[nodiscard]] TimePoint now() const override { return now_; }
    void advance(Clock::duration d) noexcept { now_ += d; }

private:
    TimePoint now_{};
};

}  // namespace waw::core
