#include <doctest/doctest.h>

#include <thread>

#include "server_app.hpp"

using namespace std::chrono_literals;

TEST_CASE("the run loop ticks at its rate and stops after the run time") {
    runity::core::MemoryLogSink sink;
    runity::core::SteadyClock clock;
    runity::app::Config config;
    config.tick_rate = 100;  // 10 ms
    runity::app::ServerApp app(config, sink, clock);
    std::uint64_t last = 0;
    bool in_order = true;
    app.set_tick_handler([&](std::uint64_t tick) {
        in_order = in_order && tick == last + 1;
        last = tick;
    });
    CHECK(app.run(300ms) == 0);
    CHECK(in_order);
    // 30 ticks expected; allow for a loaded test machine.
    CHECK(app.ticks_run() >= 20);
    CHECK(app.ticks_run() <= 31);
    CHECK(sink.contains("simulation stopped"));
}

TEST_CASE("request_stop from another thread ends the loop promptly") {
    runity::core::MemoryLogSink sink;
    runity::core::SteadyClock clock;
    runity::app::ServerApp app(runity::app::Config{}, sink, clock);
    const auto start = std::chrono::steady_clock::now();
    std::jthread stopper([&] {
        std::this_thread::sleep_for(100ms);
        app.request_stop();
    });
    CHECK(app.run() == 0);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}
