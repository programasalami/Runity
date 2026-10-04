#include "runity/persistence/redis_client.hpp"

#include <charconv>

#include <asio.hpp>

namespace runity::persistence {

namespace {

// Parses one reply starting at `pos`. Returns false if more data is needed; throws std::runtime_error on invalid input.
bool parse_at(const std::string& buf, std::size_t& pos, RedisReply& out, int depth) {
    if (depth > 8) throw std::runtime_error("RESP nesting too deep");
    if (pos >= buf.size()) return false;
    const auto line_end = buf.find("\r\n", pos);
    if (line_end == std::string::npos) return false;
    const char kind = buf[pos];
    const std::string_view line(buf.data() + pos + 1, line_end - pos - 1);
    auto to_int = [&](std::string_view s) {
        long long v = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) throw std::runtime_error("bad RESP integer");
        return v;
    };
    switch (kind) {
        case '+':
        case '-':
            out.type = kind == '+' ? RedisReply::Type::Status : RedisReply::Type::Error;
            out.text = std::string(line);
            pos = line_end + 2;
            return true;
        case ':':
            out.type = RedisReply::Type::Integer;
            out.integer = to_int(line);
            pos = line_end + 2;
            return true;
        case '$': {
            const auto len = to_int(line);
            if (len < 0) {
                out.type = RedisReply::Type::Nil;
                pos = line_end + 2;
                return true;
            }
            if (len > 512 * 1024 * 1024) throw std::runtime_error("RESP bulk too large");
            const std::size_t start = line_end + 2;
            if (buf.size() < start + static_cast<std::size_t>(len) + 2) return false;
            out.type = RedisReply::Type::Bulk;
            out.text = buf.substr(start, static_cast<std::size_t>(len));
            pos = start + static_cast<std::size_t>(len) + 2;
            return true;
        }
        case '*': {
            const auto count = to_int(line);
            std::size_t p = line_end + 2;
            if (count < 0) {
                out.type = RedisReply::Type::Nil;
                pos = p;
                return true;
            }
            RedisReply array;
            array.type = RedisReply::Type::Array;
            for (long long i = 0; i < count; ++i) {
                RedisReply element;
                if (!parse_at(buf, p, element, depth + 1)) return false;
                array.elements.push_back(std::move(element));
            }
            out = std::move(array);
            pos = p;
            return true;
        }
        default:
            throw std::runtime_error("not a RESP reply");
    }
}

}  // namespace

std::expected<std::optional<RedisReply>, std::string> RespParser::next() {
    std::size_t pos = 0;
    RedisReply reply;
    try {
        if (!parse_at(buffer_, pos, reply, 0)) return std::optional<RedisReply>{};
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
    buffer_.erase(0, pos);
    return std::optional<RedisReply>{std::move(reply)};
}

std::string encode_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) {
        out += "$" + std::to_string(a.size()) + "\r\n";
        out += a;
        out += "\r\n";
    }
    return out;
}

std::expected<RedisEndpoint, std::string> RedisEndpoint::parse(std::string_view url) {
    constexpr std::string_view scheme = "redis://";
    if (!url.starts_with(scheme)) return std::unexpected("Redis URL must start with redis://");
    url.remove_prefix(scheme.size());
    if (const auto slash = url.find('/'); slash != std::string_view::npos) url = url.substr(0, slash);
    RedisEndpoint e;
    const auto colon = url.rfind(':');
    if (colon == std::string_view::npos) {
        e.host = std::string(url);
    } else {
        e.host = std::string(url.substr(0, colon));
        const auto port_text = url.substr(colon + 1);
        unsigned port = 0;
        const auto r = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (r.ec != std::errc{} || port == 0 || port > 65535) return std::unexpected("bad port in Redis URL");
        e.port = static_cast<std::uint16_t>(port);
    }
    if (e.host.empty()) return std::unexpected("missing host in Redis URL");
    return e;
}

struct RedisClient::Impl {
    RedisEndpoint endpoint;
    std::chrono::milliseconds timeout;
    asio::io_context io;
    std::optional<asio::ip::tcp::socket> socket;
    RespParser parser;
    bool request_sent = false;  // once a request reached Redis, a failure must not be retried (GETDEL / SET NX are not idempotent)

    void disconnect() {
        if (socket) {
            asio::error_code ignored;
            socket->close(ignored);
        }
        socket.reset();
        parser = RespParser{};
    }

    // Runs the io_context until `done` or the timeout; on timeout the socket is closed (cancelling the operation).
    bool run_until(const bool& done) {
        io.restart();
        io.run_for(timeout);
        if (!done) {
            disconnect();
            io.restart();
            io.poll();  // let the cancelled handlers finish
            return false;
        }
        return true;
    }

    std::expected<void, std::string> connect() {
        if (socket) return {};
        socket.emplace(io);
        asio::ip::tcp::resolver resolver(io);
        asio::error_code ec;
        const auto endpoints = resolver.resolve(endpoint.host, std::to_string(endpoint.port), ec);
        if (ec) {
            disconnect();
            return std::unexpected("cannot resolve Redis host: " + ec.message());
        }
        bool done = false;
        asio::error_code connect_ec;
        asio::async_connect(*socket, endpoints, [&](const asio::error_code& e, const auto&) {
            connect_ec = e;
            done = true;
        });
        if (!run_until(done)) return std::unexpected("Redis connect timed out");
        if (connect_ec) {
            disconnect();
            return std::unexpected("cannot connect to Redis: " + connect_ec.message());
        }
        asio::error_code opt_ec;
        socket->set_option(asio::ip::tcp::no_delay(true), opt_ec);
        return {};
    }

    std::expected<RedisReply, std::string> roundtrip(const std::string& request) {
        bool done = false;
        asio::error_code ec;
        asio::async_write(*socket, asio::buffer(request), [&](const asio::error_code& e, std::size_t) {
            ec = e;
            done = true;
        });
        if (!run_until(done)) return std::unexpected("Redis write timed out");
        if (ec) {
            disconnect();
            return std::unexpected("Redis write failed: " + ec.message());
        }
        request_sent = true;
        std::array<char, 4096> buf{};
        while (true) {
            auto parsed = parser.next();
            if (!parsed) {
                disconnect();
                return std::unexpected("Redis protocol error: " + parsed.error());
            }
            if (*parsed) return std::move(**parsed);
            done = false;
            std::size_t n = 0;
            socket->async_read_some(asio::buffer(buf), [&](const asio::error_code& e, std::size_t got) {
                ec = e;
                n = got;
                done = true;
            });
            if (!run_until(done)) return std::unexpected("Redis read timed out");
            if (ec) {
                disconnect();
                return std::unexpected("Redis read failed: " + ec.message());
            }
            parser.feed(std::string_view(buf.data(), n));
        }
    }
};

RedisClient::RedisClient(RedisEndpoint endpoint, std::chrono::milliseconds timeout) : impl_(std::make_unique<Impl>()) {
    impl_->endpoint = std::move(endpoint);
    impl_->timeout = timeout;
}

RedisClient::~RedisClient() { impl_->disconnect(); }

std::expected<RedisReply, std::string> RedisClient::command(const std::vector<std::string>& args) {
    const std::string request = encode_command(args);
    std::string last_error;
    // One retry, only if the request never reached Redis (a kept-alive connection may have been closed since the last command).
    for (int attempt = 0; attempt < 2; ++attempt) {
        impl_->request_sent = false;
        if (auto c = impl_->connect(); !c) {
            last_error = c.error();
            continue;
        }
        auto reply = impl_->roundtrip(request);
        if (reply) return reply;
        last_error = reply.error();
        if (impl_->request_sent) break;  // the outcome is unknown: report it, never resend
    }
    return std::unexpected(last_error);
}

}  // namespace runity::persistence
