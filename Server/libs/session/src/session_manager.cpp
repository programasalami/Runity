#include "runity/session/session_manager.hpp"

namespace runity::session {

using protocol::FailureCode;

const char* to_string(SessionState s) noexcept {
    switch (s) {
        case SessionState::AwaitingHello: return "AwaitingHello";
        case SessionState::Authenticating: return "Authenticating";
        case SessionState::CharacterSelect: return "CharacterSelect";
        case SessionState::InWorld: return "InWorld";
        case SessionState::Closing: return "Closing";
    }
    return "?";
}

SessionManager::SessionManager(SessionConfig config, IClientLink& link, persistence::IAccountSessions& accounts, core::Worker& worker,
                               core::TaskQueue& completions, const core::IClock& clock, core::Logger log)
    : config_(std::move(config)),
      link_(link),
      accounts_(accounts),
      worker_(worker),
      completions_(completions),
      clock_(clock),
      log_(std::move(log)),
      started_(clock.now()) {}

SessionManager::~SessionManager() {
    // Worker jobs reference this object: let them finish. Completions still queued after this become no-ops (alive_).
    worker_.wait_idle();
}

std::uint32_t SessionManager::server_time_ms() const {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<core::Millis>(clock_.now() - started_).count());
}

Session* SessionManager::find(std::uint32_t session_id) {
    auto it = by_id_.find(session_id);
    return it == by_id_.end() ? nullptr : it->second.get();
}

std::string SessionManager::lock_owner(const Session& s) const { return config_.server_id + "/" + std::to_string(s.id); }

void SessionManager::on_net_event(net::NetEvent event) {
    std::visit(
        [this](auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, net::Connected>) on_connected(e);
            else if constexpr (std::is_same_v<T, net::MessageReceived>) on_message(e);
            else on_disconnected(e);
        },
        event);
}

void SessionManager::on_connected(const net::Connected& e) {
    auto s = std::make_unique<Session>();
    s->id = next_session_id_++;
    s->connection = e.id;
    s->remote_address = e.remote_address;
    s->connected_at = clock_.now();
    by_connection_[e.id] = s->id;
    log_.debug("session {} connected from {}", s->id, e.remote_address);
    by_id_.emplace(s->id, std::move(s));
}

void SessionManager::fail(Session& session, FailureCode code, std::string message) {
    if (session.state == SessionState::Closing) return;
    log_.info("session {} refused: {} ({})", session.id, protocol::to_string(code), message);
    send(session, protocol::Failure{.code = code, .fatal = true, .message = std::move(message)});
    session.state = SessionState::Closing;
    link_.close(session.connection, true);
}

void SessionManager::end(Session& session) {
    if (session.state == SessionState::Closing) return;
    session.state = SessionState::Closing;
    link_.close(session.connection, true);
}

void SessionManager::on_message(net::MessageReceived& e) {
    auto it = by_connection_.find(e.id);
    if (it == by_connection_.end()) return;
    Session& s = *by_id_.at(it->second);
    if (s.state == SessionState::Closing) return;

    if (auto* ping = std::get_if<protocol::Ping>(&e.message)) {
        if (s.state != SessionState::AwaitingHello) {
            send(s, protocol::Pong{.client_time_ms = ping->client_time_ms, .server_time_ms = server_time_ms()});
            return;
        }
    }
    if (auto* hello = std::get_if<protocol::Hello>(&e.message)) {
        if (s.state != SessionState::AwaitingHello) {
            fail(s, FailureCode::InvalidRequest, "Hello was already received.");
            return;
        }
        on_hello(s, *hello);
        return;
    }
    switch (s.state) {
        case SessionState::AwaitingHello:
        case SessionState::Authenticating:
            fail(s, FailureCode::InvalidRequest, "Not authenticated.");
            return;
        case SessionState::CharacterSelect:
        case SessionState::InWorld:
            if (game_ == nullptr) {
                fail(s, FailureCode::ServiceUnavailable, "The game world is not available.");
                return;
            }
            game_->on_game_message(s, e.message);
            return;
        case SessionState::Closing:
            return;
    }
}

void SessionManager::on_hello(Session& s, const protocol::Hello& hello) {
    if (hello.protocol_version != protocol::kProtocolVersion) {
        fail(s, FailureCode::ProtocolMismatch,
             "This server speaks protocol " + std::to_string(protocol::kProtocolVersion) + "; please update the game.");
        return;
    }
    if (config_.require_matching_build && hello.build_version != config_.build_version) {
        fail(s, FailureCode::ProtocolMismatch, "Update required: the server runs version " + config_.build_version + ".");
        return;
    }
    s.state = SessionState::Authenticating;
    const std::uint32_t id = s.id;
    const std::string owner = lock_owner(s);
    // Redis round trips happen on the worker; the result is applied on the simulation thread via the completion queue.
    worker_.submit([this, id, owner, ticket = hello.token] {
        auto identity = accounts_.redeem_join_ticket(ticket);
        if (identity) {
            if (auto locked = accounts_.acquire_lock(identity->account_id, owner, config_.lock_ttl); !locked) {
                identity = std::unexpected(locked.error());
            }
        }
        post_completion([this, id, result = std::move(identity)]() mutable { on_authenticated(id, std::move(result)); });
    });
}

void SessionManager::on_authenticated(std::uint32_t session_id,
                                      std::expected<persistence::JoinIdentity, persistence::SessionError> result) {
    Session* s = find(session_id);
    if (s == nullptr || s->state != SessionState::Authenticating) {
        // The client left while we were checking: give the lock back, or it would block the account until its TTL ends.
        if (result) {
            const auto account_id = result->account_id;
            const std::string owner = config_.server_id + "/" + std::to_string(session_id);
            worker_.submit([this, account_id, owner] { (void)accounts_.release_lock(account_id, owner); });
        }
        return;
    }
    if (!result) {
        switch (result.error()) {
            case persistence::SessionError::InvalidTicket:
                fail(*s, FailureCode::InvalidToken, "Your login has expired. Please sign in again.");
                break;
            case persistence::SessionError::AccountInUse:
                fail(*s, FailureCode::AccountInUse, "This account is already playing.");
                break;
            case persistence::SessionError::ServiceUnavailable:
                log_.warn("session {}: session store unavailable during login", s->id);
                fail(*s, FailureCode::ServiceUnavailable, "Login is unavailable right now. Please try again shortly.");
                break;
        }
        return;
    }
    s->account = std::move(*result);
    s->holds_lock = true;
    s->next_lock_refresh = clock_.now() + config_.lock_refresh_interval;
    s->state = SessionState::CharacterSelect;
    log_.info("session {} authenticated as account {} ({})", s->id, s->account->account_id, s->account->name);
    send(*s, protocol::HelloAck{.protocol_version = protocol::kProtocolVersion,
                                .session_id = s->id,
                                .account_id = s->account->account_id,
                                .account_name = s->account->name,
                                .server_time_ms = server_time_ms()});
}

void SessionManager::on_disconnected(const net::Disconnected& e) {
    auto it = by_connection_.find(e.id);
    if (it == by_connection_.end()) return;
    const std::uint32_t id = it->second;
    by_connection_.erase(it);
    auto node = by_id_.extract(id);
    Session& s = *node.mapped();
    log_.debug("session {} disconnected: {} {}", s.id, net::to_string(e.reason), e.detail);
    if (game_ != nullptr && s.account) game_->on_session_closed(s);
    release_lock(s);
}

void SessionManager::release_lock(const Session& s) {
    if (!s.holds_lock || !s.account) return;
    const auto account_id = s.account->account_id;
    const std::string owner = lock_owner(s);
    worker_.submit([this, account_id, owner] {
        if (auto r = accounts_.release_lock(account_id, owner); !r) {
            log_.warn("account {}: lock release failed ({}); it expires on its own", account_id, persistence::to_string(r.error()));
        }
    });
}

void SessionManager::tick() {
    const auto now = clock_.now();
    for (auto& [id, ptr] : by_id_) {
        Session& s = *ptr;
        if (s.state == SessionState::AwaitingHello && now - s.connected_at > config_.hello_timeout) {
            fail(s, FailureCode::Timeout, "No Hello received in time.");
            continue;
        }
        if (s.holds_lock && !s.lock_refresh_pending && now >= s.next_lock_refresh) {
            s.lock_refresh_pending = true;
            s.next_lock_refresh = now + config_.lock_refresh_interval;
            const auto account_id = s.account->account_id;
            const std::string owner = lock_owner(s);
            const std::uint32_t session_id = s.id;
            worker_.submit([this, account_id, owner, session_id] {
                auto refreshed = accounts_.refresh_lock(account_id, owner, config_.lock_ttl);
                post_completion([this, session_id, refreshed] {
                    Session* live = find(session_id);
                    if (live == nullptr) return;
                    live->lock_refresh_pending = false;
                    if (!refreshed) {
                        // Redis is down: keep playing (saves go to PostgreSQL); the next heartbeat retries.
                        log_.warn("session {}: lock heartbeat failed ({})", session_id, persistence::to_string(refreshed.error()));
                    } else if (!*refreshed) {
                        live->holds_lock = false;
                        fail(*live, FailureCode::Kicked, "Your account was signed in elsewhere.");
                    }
                });
            });
        }
    }
}

void SessionManager::shutdown() {
    for (auto& [id, ptr] : by_id_) {
        Session& s = *ptr;
        if (s.state != SessionState::Closing) {
            send(s, protocol::Failure{.code = FailureCode::ServerShutdown, .fatal = true, .message = "The server is restarting."});
            s.state = SessionState::Closing;
            link_.close(s.connection, true);
        }
        if (game_ != nullptr && s.account) game_->on_session_closed(s);
        release_lock(s);
        s.holds_lock = false;
    }
    worker_.wait_idle();
    completions_.run_pending();
    by_connection_.clear();
    by_id_.clear();
}

}  // namespace runity::session
