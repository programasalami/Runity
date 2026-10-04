#include <doctest/doctest.h>

#include <unordered_set>

#include "runity/core/clock.hpp"
#include "runity/core/log.hpp"
#include "runity/core/strong_id.hpp"
#include "runity/core/tick_scheduler.hpp"

using namespace runity::core;
using namespace std::chrono_literals;

TEST_CASE("tick scheduler runs one tick per interval") {
    ManualClock clock;
    TickScheduler s(50ms, clock.now());
    CHECK(s.due(clock.now()) == 0);
    clock.advance(49ms);
    CHECK(s.due(clock.now()) == 0);
    clock.advance(1ms);
    CHECK(s.due(clock.now()) == 1);
    CHECK(s.due(clock.now()) == 0);
    clock.advance(50ms);
    CHECK(s.due(clock.now()) == 1);
    CHECK(s.ticks_run() == 2);
}

TEST_CASE("tick scheduler catches up a little, then drops the backlog") {
    ManualClock clock;
    TickScheduler s(50ms, clock.now(), 5);
    clock.advance(150ms);
    CHECK(s.due(clock.now()) == 3);
    clock.advance(1000ms);  // 20 ticks late
    CHECK(s.due(clock.now()) == 5);
    CHECK(s.ticks_dropped() == 15);
    // The next deadline is in the future again, not in the past.
    CHECK(s.next_deadline() > clock.now());
}

TEST_CASE("strong ids of different tags are distinct types") {
    struct ATag;
    struct BTag;
    using A = StrongId<ATag>;
    using B = StrongId<BTag>;
    static_assert(!std::is_convertible_v<A, B>);
    static_assert(!std::is_convertible_v<std::uint32_t, A>);
    std::unordered_set<A> set{A(1), A(2), A(1)};
    CHECK(set.size() == 2);
    CHECK(A(1) < A(2));
}

TEST_CASE("logger respects its minimum level") {
    MemoryLogSink sink;
    Logger log(sink, "test", LogLevel::Info);
    log.debug("hidden {}", 1);
    log.info("shown {}", 2);
    log.child("other").error("bad {}", 3);
    const auto lines = sink.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].message == "shown 2");
    CHECK(lines[1].category == "other");
    CHECK(lines[1].level == LogLevel::Error);
}
