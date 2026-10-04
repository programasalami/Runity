#include "server_app.hpp"

#include <chrono>

#include "runity/core/tick_scheduler.hpp"

namespace runity::app {

ServerApp::ServerApp(Config config, core::ILogSink& sink, const core::IClock& clock)
    : config_(std::move(config)),
      log_(sink, "server", config_.debug_log ? core::LogLevel::Debug : core::LogLevel::Info),
      clock_(clock) {}

void ServerApp::request_stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        stop_requested_.store(true);
    }
    wake_.notify_all();
}

int ServerApp::run(std::optional<core::Millis> run_for) {
    using namespace std::chrono;
    const auto start = clock_.now();
    core::TickScheduler scheduler(config_.tick_interval(), start);
    const auto stats_interval = milliseconds(config_.stats_interval_ms);
    auto next_stats = start + stats_interval;
    std::uint64_t ticks_at_last_stats = 0;
    std::uint64_t dropped_at_last_stats = 0;

    log_.info("simulation running at {} TPS ({} ms per tick)", config_.tick_rate, config_.tick_interval().count());

    while (!stop_requested_.load()) {
        const auto now = clock_.now();
        if (run_for && now - start >= *run_for) {
            log_.info("run time of {} ms reached", run_for->count());
            break;
        }

        const std::uint32_t due = scheduler.due(now);
        for (std::uint32_t i = 0; i < due && !stop_requested_.load(); ++i) {
            const std::uint64_t tick = ticks_run_.fetch_add(1) + 1;
            if (on_tick_) on_tick_(tick);
        }

        if (now >= next_stats) {
            const auto ticks = ticks_run_.load() - ticks_at_last_stats;
            const auto dropped = scheduler.ticks_dropped() - dropped_at_last_stats;
            log_.info("[STATS] ticks {} in the last {} s, dropped {}", ticks, config_.stats_interval_ms / 1000, dropped);
            ticks_at_last_stats = ticks_run_.load();
            dropped_at_last_stats = scheduler.ticks_dropped();
            next_stats += stats_interval;
        }

        // Sleep until the next tick (or until woken by request_stop). No busy spin.
        auto deadline = scheduler.next_deadline();
        if (run_for && start + *run_for < deadline) deadline = start + *run_for;
        std::unique_lock lock(mutex_);
        const auto wait_for = deadline - clock_.now();
        if (wait_for > core::Clock::duration::zero()) {
            wake_.wait_for(lock, wait_for, [this] { return stop_requested_.load(); });
        }
    }

    log_.info("simulation stopped after {} ticks", ticks_run_.load());
    return 0;
}

}  // namespace runity::app
