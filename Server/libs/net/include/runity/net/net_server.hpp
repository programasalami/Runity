#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "runity/core/log.hpp"
#include "runity/net/net_events.hpp"
#include "runity/protocol/framing.hpp"

namespace runity::net {

struct NetConfig {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 2050;  // 0 = pick a free port (tests)
    std::uint32_t max_connections = 1000;
    std::uint32_t max_connections_per_address = 10;
    bool loopback_exempt_from_address_limit = true;  // the reference exempts this machine too
    std::chrono::milliseconds idle_timeout{30000};
    std::size_t max_send_queue_bytes = 4 * 1024 * 1024;
    std::uint32_t max_payload_bytes = protocol::kMaxPayloadBytes;
};

/// TCP listener + connections for the game protocol. Owns its io thread.
/// - Received bytes are split into frames, decoded as client messages, and pushed to the NetEventQueue.
/// - Any malformed frame/message closes that connection (ProtocolError).
/// - send() / close() are thread-safe and may be called from the simulation thread.
class NetServer {
public:
    NetServer(NetConfig config, NetEventQueue& events, core::Logger log);
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    /// Binds and starts the io thread. Returns false (and logs) if the port cannot be bound.
    [[nodiscard]] bool start();
    /// Closes every connection and joins the io thread. Idempotent.
    void stop();

    /// The bound port (useful with port 0).
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// Queues an encoded frame for sending. Silently ignored if the connection is gone.
    void send(ConnectionId id, std::vector<std::uint8_t> frame);
    /// Closes a connection after the frames queued so far have been written (or immediately if `flush` is false).
    void close(ConnectionId id, bool flush = true);

    template <class Message>
    void send_message(ConnectionId id, const Message& m) {
        auto frame = protocol::encode_frame(m);
        if (!frame.empty()) send(id, std::move(frame));
    }

    [[nodiscard]] std::size_t connection_count() const noexcept;

    struct Impl;  // implementation detail (net_server.cpp)

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace runity::net
