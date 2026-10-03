#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "waw/core/clock.hpp"
#include "waw/core/log.hpp"
#include "waw/core/task_queue.hpp"
#include "waw/net/net_events.hpp"
#include "waw/persistence/account_sessions.hpp"
#include "waw/protocol/framing.hpp"

namespace waw::session {

/// Where sessions send frames. Implemented over NetServer in the app, by a recorder in tests.
class IClientLink {
public:
    virtual ~IClientLink() = default;
    virtual void send(net::ConnectionId id, std::vector<std::uint8_t> frame) = 0;
    virtual void close(net::ConnectionId id, bool flush) = 0;
};

enum class SessionState : std::uint8_t {
    AwaitingHello,    // connected, nothing received yet
    Authenticating,   // Hello accepted, ticket + lock being checked on the worker
    CharacterSelect,  // authenticated, may load or create a character
    InWorld,          // playing (set by the world layer)
    Closing,          // a fatal Failure was sent; waiting for the socket to close
};

[[nodiscard]] const char* to_string(SessionState s) noexcept;

struct SessionConfig {
    std::string server_id = "gs1";          // part of the account-lock owner value
    std::string build_version = "0.1.0";
    bool require_matching_build = true;     // the reference turned away any other client version
    std::chrono::milliseconds hello_timeout{5000};
    std::chrono::milliseconds lock_ttl{30000};
    std::chrono::milliseconds lock_refresh_interval{10000};
};

/// One connected client.
struct Session {
    std::uint32_t id = 0;
    net::ConnectionId connection;
    std::string remote_address;
    SessionState state = SessionState::AwaitingHello;
    core::TimePoint connected_at;
    std::optional<persistence::JoinIdentity> account;
    bool holds_lock = false;
    core::TimePoint next_lock_refresh;
    bool lock_refresh_pending = false;
};

/// Receives the session layer's decisions about game messages it does not handle itself (character load/create, movement...).
/// The world layer implements it; until then the default refuses them.
class IGameHandler {
public:
    virtual ~IGameHandler() = default;
    virtual void on_game_message(Session& session, const protocol::ClientMessage& message) = 0;
    virtual void on_session_closed(const Session& session) = 0;
};

/// Owns every Session and its state machine. Runs on the simulation thread only: network events arrive through
/// on_net_event(), Redis work runs on the worker, and its results come back through the completion queue.
class SessionManager {
public:
    SessionManager(SessionConfig config, IClientLink& link, persistence::IAccountSessions& accounts, core::Worker& worker,
                   core::TaskQueue& completions, const core::IClock& clock, core::Logger log);
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    void set_game_handler(IGameHandler* handler) noexcept { game_ = handler; }

    void on_net_event(net::NetEvent event);
    /// Timeouts and lock heartbeats. Call once per tick.
    void tick();
    /// Sends ServerShutdown to everyone, closes connections and releases every account lock (blocks until released).
    void shutdown();

    /// Sends a fatal Failure, then closes the connection after it is written.
    void fail(Session& session, protocol::FailureCode code, std::string message);
    /// Closes the connection after what was already sent (e.g. after PlayerDied). No Failure is sent.
    void end(Session& session);

    template <class Message>
    void send(const Session& session, const Message& m) {
        auto frame = protocol::encode_frame(m);
        if (frame.empty()) {
            log_.error("message {} to session {} could not be encoded", protocol::message_name(static_cast<std::uint16_t>(Message::kId)),
                       session.id);
            return;
        }
        link_.send(session.connection, std::move(frame));
    }

    [[nodiscard]] Session* find(std::uint32_t session_id);
    [[nodiscard]] std::size_t size() const noexcept { return by_id_.size(); }
    [[nodiscard]] std::uint32_t server_time_ms() const;

private:
    void on_connected(const net::Connected& e);
    void on_message(net::MessageReceived& e);
    void on_disconnected(const net::Disconnected& e);
    void on_hello(Session& s, const protocol::Hello& hello);
    void on_authenticated(std::uint32_t session_id, std::expected<persistence::JoinIdentity, persistence::SessionError> result);
    void release_lock(const Session& s);
    [[nodiscard]] std::string lock_owner(const Session& s) const;

    /// Posts work back to the simulation thread; it is skipped if this manager no longer exists by then.
    template <class F>
    void post_completion(F&& f) {
        completions_.post([weak = std::weak_ptr<int>(alive_), fn = std::forward<F>(f)]() mutable {
            if (!weak.expired()) fn();
        });
    }

    SessionConfig config_;
    IClientLink& link_;
    persistence::IAccountSessions& accounts_;
    core::Worker& worker_;
    core::TaskQueue& completions_;
    const core::IClock& clock_;
    core::Logger log_;
    IGameHandler* game_ = nullptr;
    core::TimePoint started_;

    std::uint32_t next_session_id_ = 1;
    std::unordered_map<std::uint32_t, std::unique_ptr<Session>> by_id_;
    std::unordered_map<net::ConnectionId, std::uint32_t> by_connection_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace waw::session
