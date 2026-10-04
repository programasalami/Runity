#include "runity/persistence/postgres_characters.hpp"

#include <charconv>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

#include <libpq-fe.h>

namespace runity::persistence {

namespace {

struct ResultDeleter {
    void operator()(PGresult* r) const noexcept { PQclear(r); }
};
using Result = std::unique_ptr<PGresult, ResultDeleter>;

/// PostgreSQL's text form of an int[]: {1,2,-1}.
std::string to_pg_array(const std::vector<int>& values) {
    std::string s = "{";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) s += ',';
        s += std::to_string(values[i]);
    }
    return s + "}";
}

std::vector<int> from_pg_array(std::string_view s) {
    std::vector<int> values;
    if (s.size() < 2) return values;
    s = s.substr(1, s.size() - 2);
    while (!s.empty()) {
        const auto comma = s.find(',');
        const auto item = s.substr(0, comma);
        int v = 0;
        std::from_chars(item.data(), item.data() + item.size(), v);
        values.push_back(v);
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
    }
    return values;
}

std::int64_t to_int(const PGresult* r, int row, int col) { return std::strtoll(PQgetvalue(r, row, col), nullptr, 10); }
bool to_bool(const PGresult* r, int row, int col) { return PQgetvalue(r, row, col)[0] == 't'; }

}  // namespace

PostgresCharacterRepository::PostgresCharacterRepository(std::string conninfo, core::Logger log)
    : conninfo_(std::move(conninfo)), log_(std::move(log)) {}

PostgresCharacterRepository::~PostgresCharacterRepository() {
    if (conn_ != nullptr) PQfinish(conn_);
}

bool PostgresCharacterRepository::ensure_connected() {
    if (conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK) return true;
    if (conn_ == nullptr) {
        conn_ = PQconnectdb(conninfo_.c_str());
    } else {
        PQreset(conn_);
    }
    if (PQstatus(conn_) == CONNECTION_OK) return true;
    log_.error("PostgreSQL connection failed: {}", PQerrorMessage(conn_));
    return false;
}

std::expected<void, std::string> PostgresCharacterRepository::connect() {
    std::lock_guard lock(mutex_);
    if (!ensure_connected()) return std::unexpected(std::string(PQerrorMessage(conn_)));
    Result r(PQexec(conn_, "SELECT 1 FROM characters LIMIT 1"));
    if (PQresultStatus(r.get()) != PGRES_TUPLES_OK) {
        return std::unexpected("the characters table is missing (start the Account/API service once, or run it with 'migrate'): " +
                               std::string(PQerrorMessage(conn_)));
    }
    return {};
}

namespace {

/// Runs one statement with text parameters; nullptr (and the error logged) when it fails.
Result exec(pg_conn* conn, const core::Logger& log, const char* sql, const std::vector<std::string>& params) {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) values.push_back(p.c_str());
    Result r(PQexecParams(conn, sql, static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0));
    const auto status = PQresultStatus(r.get());
    if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK) return r;
    log.error("PostgreSQL: {}", PQerrorMessage(conn));
    return nullptr;
}

}  // namespace

std::expected<CharacterRecord, CharacterError> PostgresCharacterRepository::load(std::int64_t account_id, std::int32_t character_id) {
    std::lock_guard lock(mutex_);
    if (!ensure_connected()) return std::unexpected(CharacterError::ServiceUnavailable);
    auto r = exec(conn_, log_,
                  "SELECT class_type, skin_type, level, xp, fame, hp, mp, stats, items, health_potions, magic_potions, has_backpack, "
                  "is_dead, save_version FROM characters WHERE account_id = $1 AND character_id = $2 AND NOT is_deleted",
                  {std::to_string(account_id), std::to_string(character_id)});
    if (!r) return std::unexpected(CharacterError::ServiceUnavailable);
    if (PQntuples(r.get()) == 0) return std::unexpected(CharacterError::NotFound);
    const auto* row = r.get();
    CharacterRecord c;
    c.account_id = account_id;
    c.character_id = character_id;
    c.class_type = static_cast<std::uint16_t>(to_int(row, 0, 0));
    c.skin_type = static_cast<std::uint16_t>(to_int(row, 0, 1));
    c.level = static_cast<int>(to_int(row, 0, 2));
    c.xp = to_int(row, 0, 3);
    c.fame = to_int(row, 0, 4);
    c.hp = static_cast<int>(to_int(row, 0, 5));
    c.mp = static_cast<int>(to_int(row, 0, 6));
    c.stats = from_pg_array(PQgetvalue(row, 0, 7));
    c.items = from_pg_array(PQgetvalue(row, 0, 8));
    c.health_potions = static_cast<int>(to_int(row, 0, 9));
    c.magic_potions = static_cast<int>(to_int(row, 0, 10));
    c.has_backpack = to_bool(row, 0, 11);
    c.is_dead = to_bool(row, 0, 12);
    c.save_version = to_int(row, 0, 13);
    if (c.is_dead) return std::unexpected(CharacterError::Dead);
    return c;
}

std::expected<CharacterRecord, CharacterError> PostgresCharacterRepository::create(const CharacterRecord& initial) {
    std::lock_guard lock(mutex_);
    if (!ensure_connected()) return std::unexpected(CharacterError::ServiceUnavailable);
    const auto account = std::to_string(initial.account_id);
    auto rollback = [this](CharacterError e) {
        const Result discarded(PQexec(conn_, "ROLLBACK"));
        return std::unexpected(e);
    };
    if (!exec(conn_, log_, "BEGIN", {})) return std::unexpected(CharacterError::ServiceUnavailable);

    // The account row lock serializes creates on one account, so the slot check and the id counter cannot race.
    auto acc = exec(conn_, log_, "SELECT max_characters, next_character_id FROM accounts WHERE id = $1 FOR UPDATE", {account});
    if (!acc) return rollback(CharacterError::ServiceUnavailable);
    if (PQntuples(acc.get()) == 0) return rollback(CharacterError::NotFound);
    const auto max_characters = to_int(acc.get(), 0, 0);
    const auto character_id = static_cast<std::int32_t>(to_int(acc.get(), 0, 1));

    auto living = exec(conn_, log_, "SELECT count(*) FROM characters WHERE account_id = $1 AND NOT is_dead AND NOT is_deleted", {account});
    if (!living) return rollback(CharacterError::ServiceUnavailable);
    if (to_int(living.get(), 0, 0) >= max_characters) return rollback(CharacterError::SlotsFull);

    if (!exec(conn_, log_, "UPDATE accounts SET next_character_id = next_character_id + 1 WHERE id = $1", {account})) {
        return rollback(CharacterError::ServiceUnavailable);
    }
    CharacterRecord c = initial;
    c.character_id = character_id;
    c.save_version = 0;
    if (!exec(conn_, log_,
              "INSERT INTO characters (account_id, character_id, class_type, skin_type, level, xp, fame, hp, mp, stats, items, "
              "health_potions, magic_potions, has_backpack, is_dead, save_version) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10::int[], $11::int[], $12, $13, $14, $15, 0)",
              {account, std::to_string(c.character_id), std::to_string(c.class_type), std::to_string(c.skin_type), std::to_string(c.level),
               std::to_string(c.xp), std::to_string(c.fame), std::to_string(c.hp), std::to_string(c.mp), to_pg_array(c.stats),
               to_pg_array(c.items), std::to_string(c.health_potions), std::to_string(c.magic_potions), c.has_backpack ? "true" : "false",
               c.is_dead ? "true" : "false"})) {
        return rollback(CharacterError::ServiceUnavailable);
    }
    if (!exec(conn_, log_, "COMMIT", {})) return rollback(CharacterError::ServiceUnavailable);
    return c;
}

std::expected<std::int64_t, CharacterError> PostgresCharacterRepository::save(const CharacterRecord& snapshot) {
    std::lock_guard lock(mutex_);
    if (!ensure_connected()) return std::unexpected(CharacterError::ServiceUnavailable);
    const auto account = std::to_string(snapshot.account_id);
    const auto character = std::to_string(snapshot.character_id);
    auto r = exec(conn_, log_,
                  "UPDATE characters SET level = $3, xp = $4, fame = $5, hp = $6, mp = $7, stats = $8::int[], items = $9::int[], "
                  "health_potions = $10, magic_potions = $11, has_backpack = $12, is_dead = $13, save_version = $14, updated_at = now() "
                  "WHERE account_id = $1 AND character_id = $2 AND NOT is_deleted AND save_version < $14",
                  {account, character, std::to_string(snapshot.level), std::to_string(snapshot.xp), std::to_string(snapshot.fame),
                   std::to_string(snapshot.hp), std::to_string(snapshot.mp), to_pg_array(snapshot.stats), to_pg_array(snapshot.items),
                   std::to_string(snapshot.health_potions), std::to_string(snapshot.magic_potions), snapshot.has_backpack ? "true" : "false",
                   snapshot.is_dead ? "true" : "false", std::to_string(snapshot.save_version)});
    if (!r) return std::unexpected(CharacterError::ServiceUnavailable);
    if (std::string_view(PQcmdTuples(r.get())) == "1") return snapshot.save_version;
    auto exists = exec(conn_, log_, "SELECT 1 FROM characters WHERE account_id = $1 AND character_id = $2 AND NOT is_deleted",
                       {account, character});
    if (!exists) return std::unexpected(CharacterError::ServiceUnavailable);
    return std::unexpected(PQntuples(exists.get()) == 0 ? CharacterError::NotFound : CharacterError::StaleSave);
}

}  // namespace runity::persistence
