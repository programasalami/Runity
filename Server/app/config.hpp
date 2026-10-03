#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace waw::app {

/// Non-secret server settings (Server/config/gameserver.json). Secrets come from the environment, never from this file.
struct Config {
    std::string bind_address = "0.0.0.0";
    std::uint16_t game_port = 2050;
    std::uint32_t tick_rate = 20;              // ticks per second (the reference runs 20 TPS)
    std::uint32_t max_connections = 1000;
    std::uint32_t max_connections_per_address = 10;
    std::uint32_t hello_timeout_ms = 5000;
    std::uint32_t idle_timeout_ms = 30000;
    std::uint32_t stats_interval_ms = 10000;   // the reference prints a [STATS] line every 10 s
    std::string build_version = "0.1.0";
    std::filesystem::path content_root = "../Content";
    bool debug_log = false;
    std::string server_id = "gs1";             // part of every account-lock owner value; unique per running server
    std::string redis_prefix = "waw:";         // must equal the Account/API service's Service:RedisPrefix
    bool require_matching_build = true;        // the reference turned away any other client version
    std::string character_store = "InMemory";  // "InMemory" until the PostgreSQL character repository is migrated
    std::string entry_world;                   // development / tests: the world characters enter (default: the Nexus)

    [[nodiscard]] std::chrono::milliseconds tick_interval() const { return std::chrono::milliseconds(1000 / tick_rate); }
};

/// Reads a JSON config. Unknown keys are an error (a typo must not silently fall back to a default).
[[nodiscard]] std::expected<Config, std::string> load_config(const std::filesystem::path& path);
/// Parses JSON text (tests).
[[nodiscard]] std::expected<Config, std::string> parse_config(const std::string& json_text);

}  // namespace waw::app
