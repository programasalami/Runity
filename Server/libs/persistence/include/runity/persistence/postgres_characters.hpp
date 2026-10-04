#pragma once

#include <expected>
#include <mutex>
#include <string>

#include "runity/core/log.hpp"
#include "runity/persistence/characters.hpp"

struct pg_conn;

namespace runity::persistence {

/// Characters in PostgreSQL (table `characters`, Database/migrations; the Account/API service owns `accounts` and creates the
/// schema). One connection, used under a mutex: every call already runs on the single persistence worker. A lost connection is
/// re-established on the next call; until then calls fail closed with ServiceUnavailable. Soft-deleted characters (the API's
/// delete) count as not found.
class PostgresCharacterRepository final : public ICharacterRepository {
public:
    /// `conninfo` is libpq "key=value" form (gameserver.json pgConnInfo). Nothing connects until connect() or the first call.
    PostgresCharacterRepository(std::string conninfo, core::Logger log);
    ~PostgresCharacterRepository() override;
    PostgresCharacterRepository(const PostgresCharacterRepository&) = delete;
    PostgresCharacterRepository& operator=(const PostgresCharacterRepository&) = delete;

    /// Connects now and checks that the schema is there, so the server can refuse to start with a clear message.
    [[nodiscard]] std::expected<void, std::string> connect();

    std::expected<CharacterRecord, CharacterError> load(std::int64_t account_id, std::int32_t character_id) override;
    std::expected<CharacterRecord, CharacterError> create(const CharacterRecord& initial) override;
    std::expected<std::int64_t, CharacterError> save(const CharacterRecord& snapshot) override;

private:
    bool ensure_connected();

    std::mutex mutex_;
    std::string conninfo_;
    core::Logger log_;
    pg_conn* conn_ = nullptr;
};

}  // namespace runity::persistence
