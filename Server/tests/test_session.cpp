#include <doctest/doctest.h>

#include "waw/session/session_manager.hpp"

using namespace waw;
using namespace std::chrono_literals;
using protocol::FailureCode;

namespace {

class RecordingLink final : public session::IClientLink {
public:
    struct Sent {
        net::ConnectionId to;
        protocol::ServerMessage message;
    };
    std::vector<Sent> sent;
    std::vector<net::ConnectionId> closed;

    void send(net::ConnectionId id, std::vector<std::uint8_t> frame) override {
        protocol::FrameDecoder d;
        d.feed(frame);
        auto f = d.next();
        REQUIRE(f.has_value());
        auto m = protocol::decode_server_message(f->id, f->payload);
        REQUIRE(m.has_value());
        sent.push_back({id, std::move(*m)});
    }
    void close(net::ConnectionId id, bool) override { closed.push_back(id); }

    template <class T>
    const T* last_to(net::ConnectionId id) const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (it->to == id) return std::get_if<T>(&it->message);
        }
        return nullptr;
    }
    bool was_closed(net::ConnectionId id) const { return std::find(closed.begin(), closed.end(), id) != closed.end(); }
};

struct Harness {
    core::MemoryLogSink sink;
    core::ManualClock clock;
    RecordingLink link;
    persistence::InMemoryAccountSessions accounts;
    core::TaskQueue completions;
    core::Worker worker;
    session::SessionConfig config;
    std::unique_ptr<session::SessionManager> sessions;

    Harness() {
        config.build_version = "1.0";
        sessions = std::make_unique<session::SessionManager>(config, link, accounts, worker, completions, clock, core::Logger(sink, "session"));
    }

    void pump() {
        worker.wait_idle();
        completions.run_pending();
    }

    net::ConnectionId connect(std::uint32_t n) {
        net::ConnectionId id(n);
        sessions->on_net_event(net::Connected{id, "127.0.0.1"});
        return id;
    }

    void receive(net::ConnectionId id, protocol::ClientMessage m) {
        sessions->on_net_event(net::MessageReceived{id, std::move(m)});
        pump();
    }

    void hello(net::ConnectionId id, std::string ticket, std::string build = "1.0", std::uint32_t version = protocol::kProtocolVersion) {
        receive(id, protocol::Hello{.protocol_version = version, .build_version = std::move(build), .token = std::move(ticket)});
    }

    FailureCode failure_code(net::ConnectionId id) const {
        const auto* f = link.last_to<protocol::Failure>(id);
        REQUIRE(f != nullptr);
        CHECK(f->fatal);
        return f->code;
    }
};

}  // namespace

TEST_CASE("a valid join ticket authenticates and takes the account lock") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 42, .name = "Bob", .rank = 0});
    auto c = h.connect(1);
    h.hello(c, "t");
    const auto* ack = h.link.last_to<protocol::HelloAck>(c);
    REQUIRE(ack != nullptr);
    CHECK(ack->account_id == 42);
    CHECK(ack->account_name == "Bob");
    CHECK(ack->protocol_version == protocol::kProtocolVersion);
    CHECK(h.accounts.is_locked(42));
    CHECK_FALSE(h.link.was_closed(c));
}

TEST_CASE("version mismatches are refused before any ticket is used") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 1, .name = "A"});
    auto a = h.connect(1);
    h.hello(a, "t", "1.0", protocol::kProtocolVersion + 1);
    CHECK(h.failure_code(a) == FailureCode::ProtocolMismatch);
    CHECK(h.link.was_closed(a));
    auto b = h.connect(2);
    h.hello(b, "t", "0.9");
    CHECK(h.failure_code(b) == FailureCode::ProtocolMismatch);
    CHECK(h.accounts.redeem_join_ticket("t").has_value());  // still unused
}

TEST_CASE("bad tickets and busy accounts are refused") {
    Harness h;
    auto a = h.connect(1);
    h.hello(a, "nope");
    CHECK(h.failure_code(a) == FailureCode::InvalidToken);

    h.accounts.add_ticket("t1", {.account_id = 7, .name = "Amy"});
    h.accounts.add_ticket("t2", {.account_id = 7, .name = "Amy"});
    auto b = h.connect(2);
    h.hello(b, "t1");
    REQUIRE(h.link.last_to<protocol::HelloAck>(b) != nullptr);
    auto c = h.connect(3);
    h.hello(c, "t2");
    CHECK(h.failure_code(c) == FailureCode::AccountInUse);
}

TEST_CASE("disconnecting releases the lock so the account can log in again") {
    Harness h;
    h.accounts.add_ticket("t1", {.account_id = 7, .name = "Amy"});
    h.accounts.add_ticket("t2", {.account_id = 7, .name = "Amy"});
    auto a = h.connect(1);
    h.hello(a, "t1");
    h.sessions->on_net_event(net::Disconnected{a, net::DisconnectReason::ClosedByPeer, ""});
    h.pump();
    CHECK_FALSE(h.accounts.is_locked(7));
    CHECK(h.sessions->size() == 0);
    auto b = h.connect(2);
    h.hello(b, "t2");
    CHECK(h.link.last_to<protocol::HelloAck>(b) != nullptr);
}

TEST_CASE("a client that leaves during authentication does not keep the lock") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 3, .name = "Cy"});
    auto a = h.connect(1);
    h.sessions->on_net_event(net::MessageReceived{a, protocol::Hello{.protocol_version = protocol::kProtocolVersion,
                                                                      .build_version = "1.0", .token = "t"}});
    h.sessions->on_net_event(net::Disconnected{a, net::DisconnectReason::ClosedByPeer, ""});
    h.pump();  // the auth result arrives for a session that is gone
    h.pump();  // the release job runs
    CHECK_FALSE(h.accounts.is_locked(3));
}

TEST_CASE("anything but Hello before authentication is refused") {
    Harness h;
    auto a = h.connect(1);
    h.receive(a, protocol::Escape{});
    CHECK(h.failure_code(a) == FailureCode::InvalidRequest);
    auto b = h.connect(2);
    h.receive(b, protocol::Ping{.client_time_ms = 1});
    CHECK(h.failure_code(b) == FailureCode::InvalidRequest);
}

TEST_CASE("a second Hello is refused") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 1, .name = "A"});
    auto a = h.connect(1);
    h.hello(a, "t");
    h.hello(a, "t");
    CHECK(h.failure_code(a) == FailureCode::InvalidRequest);
}

TEST_CASE("a silent connection is timed out") {
    Harness h;
    auto a = h.connect(1);
    h.clock.advance(4s);
    h.sessions->tick();
    CHECK_FALSE(h.link.was_closed(a));
    h.clock.advance(2s);
    h.sessions->tick();
    CHECK(h.failure_code(a) == FailureCode::Timeout);
}

TEST_CASE("ping is answered after authentication") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 1, .name = "A"});
    auto a = h.connect(1);
    h.hello(a, "t");
    h.clock.advance(1234ms);
    h.receive(a, protocol::Ping{.client_time_ms = 99});
    const auto* pong = h.link.last_to<protocol::Pong>(a);
    REQUIRE(pong != nullptr);
    CHECK(pong->client_time_ms == 99);
    CHECK(pong->server_time_ms == 1234);
}

TEST_CASE("losing the lock to another server kicks the session; a Redis outage does not") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 5, .name = "E"});
    auto a = h.connect(1);
    h.hello(a, "t");

    h.accounts.set_unavailable(true);
    h.clock.advance(11s);
    h.sessions->tick();
    h.pump();
    CHECK_FALSE(h.link.was_closed(a));
    CHECK(h.sink.contains("lock heartbeat failed"));

    h.accounts.set_unavailable(false);
    REQUIRE(h.accounts.release_lock(5, "gs1/1").has_value());
    REQUIRE(h.accounts.acquire_lock(5, "other/9", 30s).has_value());
    h.clock.advance(11s);
    h.sessions->tick();
    h.pump();
    CHECK(h.failure_code(a) == FailureCode::Kicked);
}

TEST_CASE("an unavailable session store refuses logins") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 1, .name = "A"});
    h.accounts.set_unavailable(true);
    auto a = h.connect(1);
    h.hello(a, "t");
    CHECK(h.failure_code(a) == FailureCode::ServiceUnavailable);
}

TEST_CASE("game messages without a world are refused, not ignored") {
    Harness h;
    h.accounts.add_ticket("t", {.account_id = 1, .name = "A"});
    auto a = h.connect(1);
    h.hello(a, "t");
    h.receive(a, protocol::LoadCharacter{.character_id = 1});
    CHECK(h.failure_code(a) == FailureCode::ServiceUnavailable);
}

TEST_CASE("shutdown tells every client and releases every lock") {
    Harness h;
    h.accounts.add_ticket("t1", {.account_id = 1, .name = "A"});
    h.accounts.add_ticket("t2", {.account_id = 2, .name = "B"});
    auto a = h.connect(1);
    auto b = h.connect(2);
    h.hello(a, "t1");
    h.hello(b, "t2");
    h.sessions->shutdown();
    CHECK(h.failure_code(a) == FailureCode::ServerShutdown);
    CHECK(h.failure_code(b) == FailureCode::ServerShutdown);
    CHECK_FALSE(h.accounts.is_locked(1));
    CHECK_FALSE(h.accounts.is_locked(2));
}
