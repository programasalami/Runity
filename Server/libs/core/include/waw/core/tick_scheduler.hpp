#pragma once

#include <cstdint>

#include "waw/core/clock.hpp"

namespace waw::core {

/// Fixed-rate tick bookkeeping, separated from sleeping so it can be tested with a ManualClock.
/// The simulation always advances in whole ticks of `interval`. If the process falls behind by more than
/// `max_catch_up` ticks, the rest of the backlog is dropped (and counted) instead of running a burst of ticks.
class TickScheduler {
public:
    TickScheduler(Millis interval, TimePoint start, std::uint32_t max_catch_up = 5) noexcept;

    /// Number of ticks due at `now` (0 if it is not yet time). Consumes them.
    [[nodiscard]] std::uint32_t due(TimePoint now) noexcept;
    /// When the next tick is due.
    [[nodiscard]] TimePoint next_deadline() const noexcept { return next_; }
    [[nodiscard]] std::uint64_t ticks_run() const noexcept { return ticks_; }
    [[nodiscard]] std::uint64_t ticks_dropped() const noexcept { return dropped_; }
    [[nodiscard]] Millis interval() const noexcept { return interval_; }

private:
    Millis interval_;
    TimePoint next_;
    std::uint32_t max_catch_up_;
    std::uint64_t ticks_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace waw::core
