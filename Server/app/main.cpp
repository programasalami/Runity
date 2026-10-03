// Runity C++ game server - process entry point. Every long-lived object is created here, owned by this stack frame,
// and destroyed in reverse order (CppMigration.md section 3).

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <print>
#include <string_view>
#include <thread>

#include <asio.hpp>

#include "config.hpp"
#include "server_app.hpp"
#include "waw/content/content_db.hpp"
#include "waw/core/clock.hpp"
#include "waw/core/env.hpp"
#include "waw/core/log.hpp"
#include "waw/core/task_queue.hpp"
#include "waw/game/game_service.hpp"
#include "waw/net/net_server.hpp"
#include "waw/persistence/account_sessions.hpp"
#include "waw/persistence/characters.hpp"
#include "waw/protocol/generated/messages.hpp"
#include "waw/session/session_manager.hpp"

namespace {

struct Args {
    std::filesystem::path config = "config/gameserver.json";
    std::optional<waw::core::Millis> run_for;
    std::optional<std::uint16_t> port;
};

std::optional<Args> parse_args(int argc, char** argv) {
    Args args;
    auto number = [](std::string_view v) -> std::optional<long long> {
        long long n = 0;
        if (std::from_chars(v.data(), v.data() + v.size(), n).ec != std::errc{} || n <= 0) return std::nullopt;
        return n;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            args.config = argv[++i];
        } else if (a == "--run-for-ms" && i + 1 < argc) {
            const auto n = number(argv[++i]);
            if (!n) return std::nullopt;
            args.run_for = waw::core::Millis(*n);
        } else if (a == "--port" && i + 1 < argc) {
            const auto n = number(argv[++i]);
            if (!n || *n > 65535) return std::nullopt;
            args.port = static_cast<std::uint16_t>(*n);
        } else {
            return std::nullopt;
        }
    }
    return args;
}

/// Sessions send through the network layer.
class NetLink final : public waw::session::IClientLink {
public:
    explicit NetLink(waw::net::NetServer& net) : net_(net) {}
    void send(waw::net::ConnectionId id, std::vector<std::uint8_t> frame) override { net_.send(id, std::move(frame)); }
    void close(waw::net::ConnectionId id, bool flush) override { net_.close(id, flush); }

private:
    waw::net::NetServer& net_;
};

}  // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    if (!args) {
        std::println(stderr, "usage: waw_gameserver [--config <file>] [--port <port>] [--run-for-ms <ms>]");
        return 2;
    }

    waw::core::ConsoleLogSink sink;
    waw::core::Logger log(sink, "main");

    auto config = waw::app::load_config(args->config);
    if (!config) {
        log.error("{}", config.error());
        return 1;
    }
    if (args->port) config->game_port = *args->port;
    const auto level = config->debug_log ? waw::core::LogLevel::Debug : waw::core::LogLevel::Info;
    log.info("Runity game server {} (protocol {}), server id {}", config->build_version, waw::protocol::kProtocolVersion,
             config->server_id);

    auto content = waw::content::ContentDb::load(config->content_root);
    if (!content) {
        log.error("content: {}", content.error());
        return 1;
    }
    log.info("content: {} grounds, {} objects, {} items, {} classes, {} maps, {} worlds, {} behaviours ({} warnings)",
             content->ground_count(), content->object_count(), content->item_count(), content->player_classes().size(),
             content->maps().size(), content->worlds().size(), content->behavior_count(), content->warnings().size());
    waw::core::Logger content_log(sink, "content", level);
    for (const auto& w : content->warnings()) content_log.debug("{}", w);

    auto redis_endpoint = waw::persistence::RedisEndpoint::parse(waw::core::env("WAW_REDIS_URL").value_or("redis://127.0.0.1:6379"));
    if (!redis_endpoint) {
        log.error("WAW_REDIS_URL: {}", redis_endpoint.error());
        return 1;
    }
    waw::persistence::RedisAccountSessions account_sessions(std::make_unique<waw::persistence::RedisClient>(*redis_endpoint),
                                                            config->redis_prefix);
    waw::persistence::InMemoryCharacterRepository characters;
    log.warn("characters are kept in memory only (characterStore InMemory): they are lost when the server stops");

    waw::core::SteadyClock clock;
    waw::core::TaskQueue completions;
    waw::core::Worker persistence_worker;

    waw::net::NetEventQueue net_events;
    waw::net::NetServer net({.bind_address = config->bind_address,
                             .port = config->game_port,
                             .max_connections = config->max_connections,
                             .max_connections_per_address = config->max_connections_per_address,
                             .idle_timeout = std::chrono::milliseconds(config->idle_timeout_ms)},
                            net_events, waw::core::Logger(sink, "net", level));
    NetLink link(net);

    waw::session::SessionManager sessions({.server_id = config->server_id,
                                           .build_version = config->build_version,
                                           .require_matching_build = config->require_matching_build,
                                           .hello_timeout = std::chrono::milliseconds(config->hello_timeout_ms)},
                                          link, account_sessions, persistence_worker, completions, clock,
                                          waw::core::Logger(sink, "session", level));
    waw::game::GameRules game_rules;
    game_rules.entry_world = config->entry_world;
    waw::game::GameService game(*content, sessions, characters, persistence_worker, completions, waw::core::Logger(sink, "game", level),
                                game_rules);
    if (auto ready = game.init(); !ready) {
        log.error("{}", ready.error());
        return 1;
    }
    sessions.set_game_handler(&game);

    if (!net.start()) return 1;

    waw::app::ServerApp app(*config, sink, clock);
    const float tick_ms = static_cast<float>(config->tick_interval().count());
    std::vector<waw::net::NetEvent> events;
    app.set_tick_handler([&](std::uint64_t) {
        events.clear();
        net_events.drain(events);
        for (auto& e : events) sessions.on_net_event(std::move(e));
        completions.run_pending();
        sessions.tick();
        game.tick(tick_ms);
    });

    // Signals are delivered through asio on its own thread; the handler only asks the run loop to stop.
    asio::io_context signal_io;
    asio::signal_set signals(signal_io, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code& ec, int signal) {
        if (ec) return;
        log.info("signal {} received, shutting down", signal);
        app.request_stop();
    });
    std::jthread signal_thread([&signal_io] { signal_io.run(); });

    const int code = app.run(args->run_for);

    // Orderly shutdown: tell every client, release every account lock, let the goodbyes reach the sockets, then stop the network.
    sessions.shutdown();
    const auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (net.connection_count() > 0 && std::chrono::steady_clock::now() < flush_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    net.stop();
    signal_io.stop();
    log.info("shutdown complete");
    return code;
}
