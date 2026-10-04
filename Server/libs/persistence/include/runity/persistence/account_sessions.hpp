#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "runity/persistence/redis_client.hpp"

namespace runity::persistence {

/// Who a join ticket belongs to (written by the Account/API service; key contract: AccountService RedisSessionStore.cs).
struct JoinIdentity {
    std::int64_t account_id = 0;
    std::string name;
    int rank = 0;
    friend bool operator==(const JoinIdentity&, const JoinIdentity&) = default;
};

enum class SessionError {
    InvalidTicket,       // unknown, expired or already used
    AccountInUse,        // another session holds the account's play lock
    ServiceUnavailable,  // Redis could not be reached: fail closed (no login), never "assume ok"
};

[[nodiscard]] const char* to_string(SessionError e) noexcept;

/// Transient session state shared with the Account/API service. Blocking calls: run them on a worker thread.
class IAccountSessions {
public:
    virtual ~IAccountSessions() = default;
    /// Consumes a single-use join ticket.
    [[nodiscard]] virtual std::expected<JoinIdentity, SessionError> redeem_join_ticket(std::string_view ticket) = 0;
    /// Takes the account's play lock for `owner` (server id + session id). AccountInUse if someone else holds it.
    [[nodiscard]] virtual std::expected<void, SessionError> acquire_lock(std::int64_t account_id, std::string_view owner,
                                                                         std::chrono::milliseconds ttl) = 0;
    /// Extends the lock if `owner` still holds it. False if the lock was lost (expired or taken).
    [[nodiscard]] virtual std::expected<bool, SessionError> refresh_lock(std::int64_t account_id, std::string_view owner,
                                                                         std::chrono::milliseconds ttl) = 0;
    /// Releases the lock only if `owner` holds it.
    virtual std::expected<void, SessionError> release_lock(std::int64_t account_id, std::string_view owner) = 0;
};

/// Keys: {prefix}join:{sha256(ticket)} (GETDEL) and {prefix}lock:account:{id} (SET NX PX + owner-checked scripts).
class RedisAccountSessions final : public IAccountSessions {
public:
    RedisAccountSessions(std::unique_ptr<RedisClient> client, std::string prefix);

    std::expected<JoinIdentity, SessionError> redeem_join_ticket(std::string_view ticket) override;
    std::expected<void, SessionError> acquire_lock(std::int64_t account_id, std::string_view owner,
                                                   std::chrono::milliseconds ttl) override;
    std::expected<bool, SessionError> refresh_lock(std::int64_t account_id, std::string_view owner,
                                                   std::chrono::milliseconds ttl) override;
    std::expected<void, SessionError> release_lock(std::int64_t account_id, std::string_view owner) override;

    [[nodiscard]] std::string lock_key(std::int64_t account_id) const;
    [[nodiscard]] std::string join_key(std::string_view ticket) const;

private:
    std::unique_ptr<RedisClient> client_;
    std::string prefix_;
};

/// In-memory stand-in (tests, and running the server without Redis in development). Thread-safe. TTLs are not modelled.
class InMemoryAccountSessions final : public IAccountSessions {
public:
    void add_ticket(std::string ticket, JoinIdentity identity);
    [[nodiscard]] bool is_locked(std::int64_t account_id) const;
    void set_unavailable(bool unavailable);

    std::expected<JoinIdentity, SessionError> redeem_join_ticket(std::string_view ticket) override;
    std::expected<void, SessionError> acquire_lock(std::int64_t account_id, std::string_view owner,
                                                   std::chrono::milliseconds ttl) override;
    std::expected<bool, SessionError> refresh_lock(std::int64_t account_id, std::string_view owner,
                                                   std::chrono::milliseconds ttl) override;
    std::expected<void, SessionError> release_lock(std::int64_t account_id, std::string_view owner) override;

private:
    mutable std::mutex mutex_;
    std::map<std::string, JoinIdentity, std::less<>> tickets_;
    std::map<std::int64_t, std::string> locks_;
    bool unavailable_ = false;
};

}  // namespace runity::persistence
