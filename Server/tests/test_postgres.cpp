#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <libpq-fe.h>

#include "runity/core/log.hpp"
#include "runity/persistence/postgres_characters.hpp"

using namespace runity;
using persistence::CharacterError;
using persistence::CharacterRecord;

namespace {

class NullSink final : public core::ILogSink {
public:
    void write(core::LogLevel, std::string_view, std::string_view) override {}
};

/// A throwaway schema in the local runity_test database (Database/setup/create_database.sql), built from the real migrations
/// like the Account/API service's PostgreSQL tests do, and dropped afterwards. Not available without a local PostgreSQL.
struct TestSchema {
    std::string conninfo;
    std::string schema;
    PGconn* admin = nullptr;

    TestSchema() {
        conninfo = "host=localhost port=5432 dbname=runity_test user=runity password=runitypass connect_timeout=2";
        admin = PQconnectdb(conninfo.c_str());
        if (PQstatus(admin) != CONNECTION_OK) {
            PQfinish(admin);
            admin = nullptr;
            return;
        }
        std::random_device rd;
        schema = "test_" + std::to_string(rd()) + std::to_string(rd());
        std::string sql = "CREATE SCHEMA " + schema + "; SET search_path TO " + schema + ";\n";
        for (const auto& file : std::filesystem::directory_iterator(std::filesystem::path(RUNITY_REPO_ROOT) / "Database" / "migrations")) {
            std::ifstream in(file.path());
            std::stringstream text;
            text << in.rdbuf();
            sql += text.str() + "\n";
        }
        exec(sql);
    }

    ~TestSchema() {
        if (admin == nullptr) return;
        exec("DROP SCHEMA " + schema + " CASCADE");
        PQfinish(admin);
    }

    [[nodiscard]] bool available() const { return admin != nullptr; }
    [[nodiscard]] std::string repository_conninfo() const { return conninfo + " options='-csearch_path=" + schema + "'"; }

    std::string exec(const std::string& sql) {
        PGresult* r = PQexec(admin, sql.c_str());
        const auto status = PQresultStatus(r);
        CHECK_MESSAGE((status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK), PQerrorMessage(admin));
        std::string first = PQntuples(r) > 0 ? PQgetvalue(r, 0, 0) : "";
        PQclear(r);
        return first;
    }
};

CharacterRecord wizard(std::int64_t account_id) {
    CharacterRecord c;
    c.account_id = account_id;
    c.class_type = 782;
    c.level = 1;
    c.hp = 100;
    c.mp = 100;
    c.stats = {100, 100, 12, 0, 10, 15, 12, 12};
    c.items = {2711, 2737, -1, 2763, -1, -1, -1, -1, -1, -1, -1, -1};
    c.health_potions = 1;
    c.magic_potions = 1;
    return c;
}

}  // namespace

TEST_CASE("postgres character repository: create, load, save, slots, death and soft delete") {
    TestSchema db;
    if (!db.available()) {
        MESSAGE("skipped: no local PostgreSQL runity_test database");
        return;
    }
    NullSink sink;
    persistence::PostgresCharacterRepository repo(db.repository_conninfo(), core::Logger(sink, "postgres"));
    REQUIRE(repo.connect().has_value());
    const auto account = std::stoll(db.exec("INSERT INTO " + db.schema + ".accounts (name, password_hash) VALUES ('Tester', 'x') RETURNING id"));

    // Ids come from the account's counter; the account's max_characters (2) limits living characters.
    auto first = repo.create(wizard(account));
    REQUIRE(first.has_value());
    CHECK(first->character_id == 1);
    CHECK(first->save_version == 0);
    auto second = repo.create(wizard(account));
    REQUIRE(second.has_value());
    CHECK(second->character_id == 2);
    CHECK(repo.create(wizard(account)).error() == CharacterError::SlotsFull);
    CHECK(repo.create(wizard(account + 1000)).error() == CharacterError::NotFound);

    // Everything the game saves comes back unchanged.
    CHECK(repo.load(account, 1).value() == *first);
    auto progressed = *first;
    progressed.level = 7;
    progressed.xp = 12345;
    progressed.fame = 24;
    progressed.hp = 55;
    progressed.mp = 3;
    progressed.stats = {300, 200, 20, 3, 14, 19, 16, 17};
    progressed.items[4] = 2595;
    progressed.health_potions = 4;
    progressed.has_backpack = true;
    progressed.save_version = 1;
    CHECK(repo.save(progressed).value() == 1);
    CHECK(repo.load(account, 1).value() == progressed);

    // An older or equal save_version is refused; unknown characters are not found.
    CHECK(repo.save(progressed).error() == CharacterError::StaleSave);
    auto unknown = progressed;
    unknown.character_id = 99;
    CHECK(repo.save(unknown).error() == CharacterError::NotFound);
    CHECK(repo.load(account, 99).error() == CharacterError::NotFound);

    // Permadeath: a dead character cannot be played and frees its slot.
    auto dead = progressed;
    dead.is_dead = true;
    dead.save_version = 2;
    CHECK(repo.save(dead).has_value());
    CHECK(repo.load(account, 1).error() == CharacterError::Dead);
    auto third = repo.create(wizard(account));
    REQUIRE(third.has_value());
    CHECK(third->character_id == 3);

    // The Account/API service's delete (a soft delete) makes the character disappear for the game server too.
    db.exec("UPDATE " + db.schema + ".characters SET is_deleted = TRUE WHERE account_id = " + std::to_string(account) + " AND character_id = 2");
    CHECK(repo.load(account, 2).error() == CharacterError::NotFound);
    auto late_save = *second;
    late_save.save_version = 1;
    CHECK(repo.save(late_save).error() == CharacterError::NotFound);
}

TEST_CASE("postgres character repository fails closed when the database cannot be reached") {
    NullSink sink;
    persistence::PostgresCharacterRepository repo("host=127.0.0.1 port=1 dbname=none user=none connect_timeout=2", core::Logger(sink, "postgres"));
    CHECK_FALSE(repo.connect().has_value());
    CHECK(repo.load(1, 1).error() == CharacterError::ServiceUnavailable);
    CHECK(repo.create(wizard(1)).error() == CharacterError::ServiceUnavailable);
    CHECK(repo.save(wizard(1)).error() == CharacterError::ServiceUnavailable);
}
