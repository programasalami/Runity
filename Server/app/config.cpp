#include "config.hpp"

#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace runity::app {

namespace {

template <class T>
void read_field(const nlohmann::json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end()) out = it->get<T>();
}

}  // namespace

std::expected<Config, std::string> parse_config(const std::string& json_text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text, nullptr, true, /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(std::string("config is not valid JSON: ") + e.what());
    }
    if (!j.is_object()) return std::unexpected("config must be a JSON object");

    static const std::set<std::string> known = {"bindAddress", "gamePort", "tickRate", "maxConnections",
                                                "maxConnectionsPerAddress", "helloTimeoutMs", "idleTimeoutMs",
                                                "statsIntervalMs", "buildVersion", "contentRoot", "debugLog",
                                                "serverId", "redisPrefix", "requireMatchingBuild", "characterStore", "pgConnInfo", "entryWorld"};
    for (const auto& [key, _] : j.items()) {
        if (!known.contains(key)) return std::unexpected("unknown config key: " + key);
    }

    Config c;
    try {
        read_field(j, "bindAddress", c.bind_address);
        read_field(j, "gamePort", c.game_port);
        read_field(j, "tickRate", c.tick_rate);
        read_field(j, "maxConnections", c.max_connections);
        read_field(j, "maxConnectionsPerAddress", c.max_connections_per_address);
        read_field(j, "helloTimeoutMs", c.hello_timeout_ms);
        read_field(j, "idleTimeoutMs", c.idle_timeout_ms);
        read_field(j, "statsIntervalMs", c.stats_interval_ms);
        read_field(j, "buildVersion", c.build_version);
        std::string content_root = c.content_root.string();
        read_field(j, "contentRoot", content_root);
        c.content_root = content_root;
        read_field(j, "debugLog", c.debug_log);
        read_field(j, "serverId", c.server_id);
        read_field(j, "redisPrefix", c.redis_prefix);
        read_field(j, "requireMatchingBuild", c.require_matching_build);
        read_field(j, "characterStore", c.character_store);
        read_field(j, "pgConnInfo", c.pg_conninfo);
        read_field(j, "entryWorld", c.entry_world);
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(std::string("config value has the wrong type: ") + e.what());
    }
    if (c.tick_rate == 0 || c.tick_rate > 1000) return std::unexpected("tickRate must be 1..1000");
    if (c.game_port == 0) return std::unexpected("gamePort must not be 0");
    if (c.server_id.empty() || c.server_id.find('/') != std::string::npos) return std::unexpected("serverId must be non-empty, without '/'");
    if (c.character_store != "InMemory" && c.character_store != "Postgres") return std::unexpected("characterStore must be Postgres or InMemory");
    if (c.character_store == "Postgres" && c.pg_conninfo.empty()) return std::unexpected("characterStore Postgres needs pgConnInfo");
    return c;
}

std::expected<Config, std::string> load_config(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::unexpected("cannot open config file " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    auto config = parse_config(ss.str());
    if (config) {
        // Relative paths in the config are relative to the config file, not the working directory.
        if (config->content_root.is_relative()) config->content_root = path.parent_path() / config->content_root;
    }
    return config;
}

}  // namespace runity::app
