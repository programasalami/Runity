#include "runity/net/net_server.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <thread>
#include <unordered_map>

#include <asio.hpp>

#include "runity/protocol/framing.hpp"

namespace runity::net {

using asio::ip::tcp;

const char* to_string(DisconnectReason r) noexcept {
    switch (r) {
        case DisconnectReason::ClosedByPeer: return "closed by peer";
        case DisconnectReason::ClosedByServer: return "closed by server";
        case DisconnectReason::ProtocolError: return "protocol error";
        case DisconnectReason::IdleTimeout: return "idle timeout";
        case DisconnectReason::SendQueueOverflow: return "send queue overflow";
        case DisconnectReason::SocketError: return "socket error";
        case DisconnectReason::ServerShutdown: return "server shutdown";
    }
    return "?";
}

namespace detail {
class Connection;
}
using detail::Connection;

struct NetServer::Impl {
    Impl(NetConfig c, NetEventQueue& e, core::Logger l) : config(std::move(c)), events(e), log(std::move(l)) {}

    NetConfig config;
    NetEventQueue& events;
    core::Logger log;

    asio::io_context io;
    tcp::acceptor acceptor{io};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::jthread thread;
    std::atomic<std::uint16_t> bound_port{0};
    std::atomic<std::size_t> count{0};
    bool started = false;

    // Everything below is touched on the io thread only.
    std::unordered_map<ConnectionId, std::shared_ptr<Connection>> connections;
    std::unordered_map<std::string, std::uint32_t> per_address;
    std::uint32_t next_id = 1;
    bool stopping = false;

    void accept();
    void on_closed(ConnectionId id, const std::string& address, bool counted_address);
};

namespace detail {

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(NetServer::Impl& server, tcp::socket socket, ConnectionId id, std::string address, bool counted_address)
        : server_(server),
          socket_(std::move(socket)),
          id_(id),
          address_(std::move(address)),
          counted_address_(counted_address),
          decoder_(server.config.max_payload_bytes),
          idle_timer_(server.io) {}

    void start() {
        arm_idle_timer();
        read();
    }

    void send(std::vector<std::uint8_t> frame) {
        if (closed_ || close_after_flush_) return;
        queued_bytes_ += frame.size();
        if (queued_bytes_ > server_.config.max_send_queue_bytes) {
            finish(DisconnectReason::SendQueueOverflow, "");
            return;
        }
        write_queue_.push_back(std::move(frame));
        if (!writing_) write_next();
    }

    void close(bool flush) {
        if (closed_) return;
        if (flush && (writing_ || !write_queue_.empty())) {
            close_after_flush_ = true;
            return;
        }
        finish(DisconnectReason::ClosedByServer, "");
    }

    void finish(DisconnectReason reason, std::string detail) {
        if (closed_) return;
        closed_ = true;
        idle_timer_.cancel();
        asio::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        server_.events.push(Disconnected{id_, reason, std::move(detail)});
        server_.on_closed(id_, address_, counted_address_);
    }

private:
    void arm_idle_timer() {
        idle_timer_.expires_after(server_.config.idle_timeout);
        idle_timer_.async_wait([self = shared_from_this()](const asio::error_code& ec) {
            if (!ec) self->finish(DisconnectReason::IdleTimeout, "");
        });
    }

    void read() {
        socket_.async_read_some(asio::buffer(read_buffer_), [self = shared_from_this()](const asio::error_code& ec, std::size_t n) {
            self->on_read(ec, n);
        });
    }

    void on_read(const asio::error_code& ec, std::size_t n) {
        if (closed_) return;
        if (ec) {
            const bool peer = ec == asio::error::eof || ec == asio::error::connection_reset;
            finish(peer ? DisconnectReason::ClosedByPeer : DisconnectReason::SocketError, ec.message());
            return;
        }
        decoder_.feed(std::span(read_buffer_.data(), n));
        while (auto frame = decoder_.next()) {
            auto message = protocol::decode_client_message(frame->id, frame->payload);
            if (!message) {
                finish(DisconnectReason::ProtocolError, std::string(protocol::to_string(message.error())) + " in message " +
                                                            std::string(protocol::message_name(frame->id)));
                return;
            }
            server_.events.push(MessageReceived{id_, std::move(*message)});
        }
        if (decoder_.error()) {
            finish(DisconnectReason::ProtocolError, "frame larger than the limit");
            return;
        }
        arm_idle_timer();
        read();
    }

    void write_next() {
        writing_ = true;
        asio::async_write(socket_, asio::buffer(write_queue_.front()),
                          [self = shared_from_this()](const asio::error_code& ec, std::size_t) { self->on_written(ec); });
    }

    void on_written(const asio::error_code& ec) {
        if (closed_) return;
        if (ec) {
            finish(DisconnectReason::SocketError, ec.message());
            return;
        }
        queued_bytes_ -= write_queue_.front().size();
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            write_next();
            return;
        }
        writing_ = false;
        if (close_after_flush_) finish(DisconnectReason::ClosedByServer, "");
    }

    NetServer::Impl& server_;
    tcp::socket socket_;
    ConnectionId id_;
    std::string address_;
    bool counted_address_;
    protocol::FrameDecoder decoder_;
    asio::steady_timer idle_timer_;
    std::array<std::uint8_t, 16 * 1024> read_buffer_{};
    std::deque<std::vector<std::uint8_t>> write_queue_;
    std::size_t queued_bytes_ = 0;
    bool writing_ = false;
    bool close_after_flush_ = false;
    bool closed_ = false;
};

}  // namespace detail

void NetServer::Impl::accept() {
    acceptor.async_accept([this](const asio::error_code& ec, tcp::socket socket) {
        if (stopping) return;
        if (ec) {
            if (ec != asio::error::operation_aborted) log.warn("accept failed: {}", ec.message());
            if (acceptor.is_open()) accept();
            return;
        }
        asio::error_code endpoint_ec;
        const auto endpoint = socket.remote_endpoint(endpoint_ec);
        if (endpoint_ec) {
            accept();
            return;
        }
        const auto address = endpoint.address();
        const std::string address_text = address.to_string();
        const bool counted = !(config.loopback_exempt_from_address_limit && address.is_loopback());

        if (connections.size() >= config.max_connections) {
            log.warn("refused {}: server full ({} connections)", address_text, connections.size());
        } else if (counted && per_address[address_text] >= config.max_connections_per_address) {
            log.warn("refused {}: too many connections from this address", address_text);
        } else {
            asio::error_code opt_ec;
            socket.set_option(tcp::no_delay(true), opt_ec);
            const ConnectionId id(next_id++);
            if (counted) ++per_address[address_text];
            auto connection = std::make_shared<Connection>(*this, std::move(socket), id, address_text, counted);
            connections.emplace(id, connection);
            count.store(connections.size());
            events.push(Connected{id, address_text});
            connection->start();
        }
        accept();
    });
}

void NetServer::Impl::on_closed(ConnectionId id, const std::string& address, bool counted_address) {
    if (counted_address) {
        if (auto it = per_address.find(address); it != per_address.end() && --it->second == 0) per_address.erase(it);
    }
    // Deferred erase: on_closed runs inside the connection's own handler.
    asio::post(io, [this, id] {
        connections.erase(id);
        count.store(connections.size());
    });
}

NetServer::NetServer(NetConfig config, NetEventQueue& events, core::Logger log)
    : impl_(std::make_unique<Impl>(std::move(config), events, std::move(log))) {}

NetServer::~NetServer() { stop(); }

bool NetServer::start() {
    auto& s = *impl_;
    if (s.started) return true;
    asio::error_code ec;
    const auto address = asio::ip::make_address(s.config.bind_address, ec);
    if (ec) {
        s.log.error("invalid bind address {}: {}", s.config.bind_address, ec.message());
        return false;
    }
    const tcp::endpoint endpoint(address, s.config.port);
    s.acceptor.open(endpoint.protocol(), ec);
    if (!ec) s.acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) s.acceptor.bind(endpoint, ec);
    if (!ec) s.acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        s.log.error("cannot listen on {}:{}: {}", s.config.bind_address, s.config.port, ec.message());
        return false;
    }
    s.bound_port.store(s.acceptor.local_endpoint().port());
    s.work.emplace(asio::make_work_guard(s.io));
    s.accept();
    s.thread = std::jthread([&s] { s.io.run(); });
    s.started = true;
    s.log.info("listening on {}:{}", s.config.bind_address, s.bound_port.load());
    return true;
}

void NetServer::stop() {
    auto& s = *impl_;
    if (!s.started) return;
    s.started = false;
    asio::post(s.io, [&s] {
        s.stopping = true;
        asio::error_code ignored;
        s.acceptor.close(ignored);
        auto connections = s.connections;  // finish() mutates the map through posted erases
        for (auto& [id, c] : connections) c->finish(DisconnectReason::ServerShutdown, "");
    });
    s.work.reset();
    if (s.thread.joinable()) s.thread.join();
    s.connections.clear();
    s.count.store(0);
}

std::uint16_t NetServer::port() const noexcept { return impl_->bound_port.load(); }

std::size_t NetServer::connection_count() const noexcept { return impl_->count.load(); }

void NetServer::send(ConnectionId id, std::vector<std::uint8_t> frame) {
    auto& s = *impl_;
    asio::post(s.io, [&s, id, frame = std::move(frame)]() mutable {
        if (auto it = s.connections.find(id); it != s.connections.end()) it->second->send(std::move(frame));
    });
}

void NetServer::close(ConnectionId id, bool flush) {
    auto& s = *impl_;
    asio::post(s.io, [&s, id, flush] {
        if (auto it = s.connections.find(id); it != s.connections.end()) it->second->close(flush);
    });
}

}  // namespace runity::net
