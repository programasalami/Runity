#include <doctest/doctest.h>

#include "config.hpp"

using runity::app::parse_config;

TEST_CASE("an empty object gives the defaults") {
    auto c = parse_config("{}");
    REQUIRE(c.has_value());
    CHECK(c->game_port == 2050);
    CHECK(c->tick_rate == 20);
    CHECK(c->tick_interval().count() == 50);
}

TEST_CASE("values and comments are read") {
    auto c = parse_config(R"({ // comment
        "gamePort": 3000, "tickRate": 10, "debugLog": true })");
    REQUIRE(c.has_value());
    CHECK(c->game_port == 3000);
    CHECK(c->tick_interval().count() == 100);
    CHECK(c->debug_log);
}

TEST_CASE("unknown keys, wrong types and bad values are errors") {
    CHECK_FALSE(parse_config(R"({"gamePrt": 1})").has_value());
    CHECK_FALSE(parse_config(R"({"gamePort": "x"})").has_value());
    CHECK_FALSE(parse_config(R"({"tickRate": 0})").has_value());
    CHECK_FALSE(parse_config("[1]").has_value());
    CHECK_FALSE(parse_config("{").has_value());
}
