#include "runity/core/tick_scheduler.hpp"

namespace runity::core {

TickScheduler::TickScheduler(Millis interval, TimePoint start, std::uint32_t max_catch_up) noexcept
    : interval_(interval), next_(start + interval), max_catch_up_(max_catch_up) {}

std::uint32_t TickScheduler::due(TimePoint now) noexcept {
    if (now < next_) return 0;
    const auto behind = static_cast<std::uint64_t>((now - next_) / interval_) + 1;
    std::uint64_t run = behind;
    if (run > max_catch_up_) {
        dropped_ += run - max_catch_up_;
        run = max_catch_up_;
    }
    next_ += interval_ * static_cast<Millis::rep>(behind);
    ticks_ += run;
    return static_cast<std::uint32_t>(run);
}

}  // namespace runity::core
