#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace runity::persistence {

/// One RESP2 reply.
struct RedisReply {
    enum class Type { Nil, Status, Error, Integer, Bulk, Array };
    Type type = Type::Nil;
    std::string text;           // Status, Error, Bulk
    long long integer = 0;      // Integer
    std::vector<RedisReply> elements;  // Array

    [[nodiscard]] bool is_nil() const noexcept { return type == Type::Nil; }
};

/// Incremental RESP2 parser. parse() returns a reply once a complete one is buffered.
class RespParser {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    /// A complete reply, std::nullopt if more bytes are needed, or an error string if the stream is not RESP.
    [[nodiscard]] std::expected<std::optional<RedisReply>, std::string> next();

private:
    std::string buffer_;
};

/// Encodes a command as a RESP array of bulk strings.
[[nodiscard]] std::string encode_command(const std::vector<std::string>& args);

struct RedisEndpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;

    /// Parses "redis://host:port" (no auth / TLS yet: the server runs next to Redis on the same host, as in the reference).
    [[nodiscard]] static std::expected<RedisEndpoint, std::string> parse(std::string_view url);
};

/// Minimal blocking Redis client for a single worker thread. It connects lazily and reconnects once per command after a
/// broken connection. Every call has a timeout. Not thread-safe: one instance per thread.
class RedisClient {
public:
    explicit RedisClient(RedisEndpoint endpoint, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    ~RedisClient();
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    /// Sends one command and waits for its reply. An Error reply is returned as a reply (type Error), not as a failure;
    /// failures are connection problems and timeouts.
    [[nodiscard]] std::expected<RedisReply, std::string> command(const std::vector<std::string>& args);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace runity::persistence
