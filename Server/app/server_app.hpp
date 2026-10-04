#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "config.hpp"
#include "runity/core/clock.hpp"
#include "runity/core/log.hpp"

namespace runity::app {

/// Owns the simulation run loop: a fixed-rate tick on the calling thread, a periodic stats line, and an orderly stop.
/// request_stop() may be called from any thread (signal handler thread, tests).
class ServerApp {
public:
    /// Called once per simulation tick with the tick number (starting at 1).
    using TickFn = std::function<void(std::uint64_t tick)>;

    ServerApp(Config config, core::ILogSink& sink, const core::IClock& clock);

    /// Runs until request_stop() or until `run_for` has elapsed. Returns the process exit code.
    int run(std::optional<core::Millis> run_for = std::nullopt);
    void request_stop() noexcept;

    void set_tick_handler(TickFn fn) { on_tick_ = std::move(fn); }
    [[nodiscard]] std::uint64_t ticks_run() const noexcept { return ticks_run_.load(); }

private:
    Config config_;
    core::Logger log_;
    const core::IClock& clock_;
    TickFn on_tick_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> ticks_run_{0};
};

}  // namespace runity::app
