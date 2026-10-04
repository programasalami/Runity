#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "runity/core/strong_id.hpp"
#include "runity/protocol/generated/messages.hpp"

namespace runity::net {

struct ConnectionTag;
using ConnectionId = core::StrongId<ConnectionTag>;

enum class DisconnectReason : std::uint8_t {
    ClosedByPeer,
    ClosedByServer,
    ProtocolError,      // malformed frame or message: the peer is not speaking our protocol
    IdleTimeout,
    SendQueueOverflow,  // the peer does not read fast enough
    SocketError,
    ServerShutdown,
};

[[nodiscard]] const char* to_string(DisconnectReason r) noexcept;

struct Connected {
    ConnectionId id;
    std::string remote_address;
};
struct MessageReceived {
    ConnectionId id;
    protocol::ClientMessage message;
};
struct Disconnected {
    ConnectionId id;
    DisconnectReason reason;
    std::string detail;
};

using NetEvent = std::variant<Connected, MessageReceived, Disconnected>;

/// Multi-producer queue from the network thread to the simulation thread. The simulation drains it once per tick.
class NetEventQueue {
public:
    void push(NetEvent e) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(e));
    }

    /// Moves every queued event into `out` (appending) and returns how many were taken.
    std::size_t drain(std::vector<NetEvent>& out) {
        std::lock_guard lock(mutex_);
        const std::size_t n = events_.size();
        for (auto& e : events_) out.push_back(std::move(e));
        events_.clear();
        return n;
    }

private:
    std::mutex mutex_;
    std::deque<NetEvent> events_;
};

}  // namespace runity::net
