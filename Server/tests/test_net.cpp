#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <thread>

#include <asio.hpp>

#include "runity/net/net_server.hpp"

using namespace runity;
using namespace std::chrono_literals;
using asio::ip::tcp;

namespace {

struct Fixture {
    core::MemoryLogSink sink;
    net::NetEventQueue events;
    std::vector<net::NetEvent> seen;
    std::unique_ptr<net::NetServer> server;

    explicit Fixture(net::NetConfig config = {}) {
        config.bind_address = "127.0.0.1";
        config.port = 0;
        server = std::make_unique<net::NetServer>(config, events, core::Logger(sink, "net"));
        REQUIRE(server->start());
    }

    /// Waits until an event of type T appears (keeps every event seen so far in `seen`).
    template <class T>
    std::optional<T> wait_for(std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::size_t checked = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            events.drain(seen);
            for (; checked < seen.size(); ++checked) {
                if (auto* e = std::get_if<T>(&seen[checked])) {
                    T copy = *e;
                    seen.erase(seen.begin() + static_cast<std::ptrdiff_t>(checked));
                    return copy;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        return std::nullopt;
    }
};

class TestClient {
public:
    explicit TestClient(std::uint16_t port) : socket_(io_) {
        socket_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    }

    void send_raw(const std::vector<std::uint8_t>& bytes) { asio::write(socket_, asio::buffer(bytes)); }

    template <class M>
    void send(const M& m) {
        send_raw(protocol::encode_frame(m));
    }

    /// Reads one frame; nullopt on EOF / error.
    std::optional<protocol::Frame> read_frame() {
        while (true) {
            if (auto f = decoder_.next()) return f;
            std::array<std::uint8_t, 4096> buf{};
            asio::error_code ec;
            const auto n = socket_.read_some(asio::buffer(buf), ec);
            if (ec) return std::nullopt;
            decoder_.feed(std::span(buf.data(), n));
        }
    }

    bool closed_by_server() {
        std::array<std::uint8_t, 64> buf{};
        asio::error_code ec;
        socket_.read_some(asio::buffer(buf), ec);
        return ec == asio::error::eof || ec == asio::error::connection_reset || ec == asio::error::connection_aborted;
    }

private:
    asio::io_context io_;
    tcp::socket socket_;
    protocol::FrameDecoder decoder_;
};

}  // namespace

TEST_CASE("a client message reaches the event queue and a server message reaches the client") {
    Fixture f;
    TestClient client(f.server->port());
    auto connected = f.wait_for<net::Connected>();
    REQUIRE(connected);
    CHECK(connected->remote_address == "127.0.0.1");

    client.send(protocol::Hello{.protocol_version = protocol::kProtocolVersion, .build_version = "test", .token = "abc"});
    auto received = f.wait_for<net::MessageReceived>();
    REQUIRE(received);
    CHECK(received->id == connected->id);
    const auto& hello = std::get<protocol::Hello>(received->message);
    CHECK(hello.token == "abc");

    f.server->send_message(connected->id, protocol::Pong{.client_time_ms = 7, .server_time_ms = 9});
    auto frame = client.read_frame();
    REQUIRE(frame);
    auto pong = protocol::decode_server_message(frame->id, frame->payload);
    REQUIRE(pong.has_value());
    CHECK(std::get<protocol::Pong>(*pong).server_time_ms == 9);
}

TEST_CASE("an unknown message id closes the connection as a protocol error") {
    Fixture f;
    TestClient client(f.server->port());
    REQUIRE(f.wait_for<net::Connected>());
    client.send_raw({0, 0, 0, 0, 0x34, 0x12});  // empty payload, id 0x1234
    auto gone = f.wait_for<net::Disconnected>();
    REQUIRE(gone);
    CHECK(gone->reason == net::DisconnectReason::ProtocolError);
    CHECK(client.closed_by_server());
}

TEST_CASE("an oversized frame closes the connection") {
    net::NetConfig config;
    config.max_payload_bytes = 1024;
    Fixture f(config);
    TestClient client(f.server->port());
    REQUIRE(f.wait_for<net::Connected>());
    client.send_raw({0x00, 0x10, 0x00, 0x00, 0x02, 0x00});  // claims 4096 bytes
    auto gone = f.wait_for<net::Disconnected>();
    REQUIRE(gone);
    CHECK(gone->reason == net::DisconnectReason::ProtocolError);
}

TEST_CASE("the per-address limit refuses extra connections") {
    net::NetConfig config;
    config.max_connections_per_address = 2;
    config.loopback_exempt_from_address_limit = false;
    Fixture f(config);
    TestClient a(f.server->port());
    TestClient b(f.server->port());
    REQUIRE(f.wait_for<net::Connected>());
    REQUIRE(f.wait_for<net::Connected>());
    TestClient c(f.server->port());
    CHECK(c.closed_by_server());
    CHECK_FALSE(f.wait_for<net::Connected>(200ms));
    CHECK(f.server->connection_count() == 2);
}

TEST_CASE("a silent connection times out") {
    net::NetConfig config;
    config.idle_timeout = 150ms;
    Fixture f(config);
    TestClient client(f.server->port());
    REQUIRE(f.wait_for<net::Connected>());
    auto gone = f.wait_for<net::Disconnected>();
    REQUIRE(gone);
    CHECK(gone->reason == net::DisconnectReason::IdleTimeout);
}

TEST_CASE("close with flush delivers queued frames first") {
    Fixture f;
    TestClient client(f.server->port());
    auto connected = f.wait_for<net::Connected>();
    REQUIRE(connected);
    f.server->send_message(connected->id, protocol::Failure{.code = protocol::FailureCode::Kicked, .fatal = true, .message = "bye"});
    f.server->close(connected->id, true);
    auto frame = client.read_frame();
    REQUIRE(frame);
    auto failure = protocol::decode_server_message(frame->id, frame->payload);
    REQUIRE(failure.has_value());
    CHECK(std::get<protocol::Failure>(*failure).message == "bye");
    CHECK(client.closed_by_server());
    auto gone = f.wait_for<net::Disconnected>();
    REQUIRE(gone);
    CHECK(gone->reason == net::DisconnectReason::ClosedByServer);
}

TEST_CASE("a client hanging up is reported") {
    Fixture f;
    {
        TestClient client(f.server->port());
        REQUIRE(f.wait_for<net::Connected>());
    }
    auto gone = f.wait_for<net::Disconnected>();
    REQUIRE(gone);
    CHECK((gone->reason == net::DisconnectReason::ClosedByPeer || gone->reason == net::DisconnectReason::SocketError));
}

TEST_CASE("stop closes every connection with ServerShutdown") {
    Fixture f;
    TestClient client(f.server->port());
    REQUIRE(f.wait_for<net::Connected>());
    f.server->stop();
    auto gone = f.wait_for<net::Disconnected>();
    REQUIRE(gone);
    CHECK(gone->reason == net::DisconnectReason::ServerShutdown);
    CHECK(f.server->connection_count() == 0);
}
