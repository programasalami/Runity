#include "waw/persistence/account_sessions.hpp"

#include <nlohmann/json.hpp>

#include "waw/core/sha256.hpp"

namespace waw::persistence {

namespace {

// Atomic owner-checked operations (a plain GET + DEL could release a lock that another session took in between).
constexpr const char* kRefreshScript =
    "if redis.call('get', KEYS[1]) == ARGV[1] then return redis.call('pexpire', KEYS[1], ARGV[2]) else return 0 end";
constexpr const char* kReleaseScript =
    "if redis.call('get', KEYS[1]) == ARGV[1] then return redis.call('del', KEYS[1]) else return 0 end";

}  // namespace

const char* to_string(SessionError e) noexcept {
    switch (e) {
        case SessionError::InvalidTicket: return "invalid join ticket";
        case SessionError::AccountInUse: return "account in use";
        case SessionError::ServiceUnavailable: return "session service unavailable";
    }
    return "?";
}

RedisAccountSessions::RedisAccountSessions(std::unique_ptr<RedisClient> client, std::string prefix)
    : client_(std::move(client)), prefix_(std::move(prefix)) {}

std::string RedisAccountSessions::lock_key(std::int64_t account_id) const {
    return prefix_ + "lock:account:" + std::to_string(account_id);
}

std::string RedisAccountSessions::join_key(std::string_view ticket) const { return prefix_ + "join:" + core::Sha256::hex(ticket); }

std::expected<JoinIdentity, SessionError> RedisAccountSessions::redeem_join_ticket(std::string_view ticket) {
    if (ticket.empty() || ticket.size() > 256) return std::unexpected(SessionError::InvalidTicket);
    auto reply = client_->command({"GETDEL", join_key(ticket)});
    if (!reply || reply->type == RedisReply::Type::Error) return std::unexpected(SessionError::ServiceUnavailable);
    if (reply->is_nil()) return std::unexpected(SessionError::InvalidTicket);
    try {
        const auto j = nlohmann::json::parse(reply->text);
        JoinIdentity id;
        id.account_id = j.at("accountId").get<std::int64_t>();
        id.name = j.at("name").get<std::string>();
        id.rank = j.value("rank", 0);
        if (id.account_id <= 0) return std::unexpected(SessionError::InvalidTicket);
        return id;
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(SessionError::InvalidTicket);
    }
}

std::expected<void, SessionError> RedisAccountSessions::acquire_lock(std::int64_t account_id, std::string_view owner,
                                                                     std::chrono::milliseconds ttl) {
    auto reply = client_->command({"SET", lock_key(account_id), std::string(owner), "NX", "PX", std::to_string(ttl.count())});
    if (!reply || reply->type == RedisReply::Type::Error) return std::unexpected(SessionError::ServiceUnavailable);
    if (reply->is_nil()) return std::unexpected(SessionError::AccountInUse);
    return {};
}

std::expected<bool, SessionError> RedisAccountSessions::refresh_lock(std::int64_t account_id, std::string_view owner,
                                                                     std::chrono::milliseconds ttl) {
    auto reply = client_->command({"EVAL", kRefreshScript, "1", lock_key(account_id), std::string(owner), std::to_string(ttl.count())});
    if (!reply || reply->type != RedisReply::Type::Integer) return std::unexpected(SessionError::ServiceUnavailable);
    return reply->integer == 1;
}

std::expected<void, SessionError> RedisAccountSessions::release_lock(std::int64_t account_id, std::string_view owner) {
    auto reply = client_->command({"EVAL", kReleaseScript, "1", lock_key(account_id), std::string(owner)});
    if (!reply || reply->type != RedisReply::Type::Integer) return std::unexpected(SessionError::ServiceUnavailable);
    return {};
}

void InMemoryAccountSessions::add_ticket(std::string ticket, JoinIdentity identity) {
    std::lock_guard lock(mutex_);
    tickets_[std::move(ticket)] = std::move(identity);
}

bool InMemoryAccountSessions::is_locked(std::int64_t account_id) const {
    std::lock_guard lock(mutex_);
    return locks_.contains(account_id);
}

void InMemoryAccountSessions::set_unavailable(bool unavailable) {
    std::lock_guard lock(mutex_);
    unavailable_ = unavailable;
}

std::expected<JoinIdentity, SessionError> InMemoryAccountSessions::redeem_join_ticket(std::string_view ticket) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(SessionError::ServiceUnavailable);
    auto it = tickets_.find(ticket);
    if (it == tickets_.end()) return std::unexpected(SessionError::InvalidTicket);
    auto identity = std::move(it->second);
    tickets_.erase(it);
    return identity;
}

std::expected<void, SessionError> InMemoryAccountSessions::acquire_lock(std::int64_t account_id, std::string_view owner,
                                                                        std::chrono::milliseconds) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(SessionError::ServiceUnavailable);
    if (locks_.contains(account_id)) return std::unexpected(SessionError::AccountInUse);
    locks_[account_id] = std::string(owner);
    return {};
}

std::expected<bool, SessionError> InMemoryAccountSessions::refresh_lock(std::int64_t account_id, std::string_view owner,
                                                                        std::chrono::milliseconds) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(SessionError::ServiceUnavailable);
    auto it = locks_.find(account_id);
    return it != locks_.end() && it->second == owner;
}

std::expected<void, SessionError> InMemoryAccountSessions::release_lock(std::int64_t account_id, std::string_view owner) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(SessionError::ServiceUnavailable);
    if (auto it = locks_.find(account_id); it != locks_.end() && it->second == owner) locks_.erase(it);
    return {};
}

}  // namespace waw::persistence
