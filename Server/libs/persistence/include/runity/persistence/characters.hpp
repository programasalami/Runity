#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace runity::persistence {

/// A character as stored (table `characters`, Database/migrations). Like the original (CharacterStats), the character's own base
/// stats are stored; what gear adds is derived by the simulation.
struct CharacterRecord {
    std::int64_t account_id = 0;
    std::int32_t character_id = 0;
    std::uint16_t class_type = 0;
    std::uint16_t skin_type = 0;
    int level = 1;
    std::int64_t xp = 0;
    std::int64_t fame = 0;
    int hp = 0;
    int mp = 0;
    std::vector<int> items;        // one per player slot, -1 = empty
    std::vector<int> stats;        // base stats: max HP, max MP, attack, defense, speed, dexterity, vitality, wisdom (empty = class start)
    int health_potions = 0;        // the original's potion stacks (newCharsConfig: 1 each)
    int magic_potions = 0;
    bool has_backpack = false;
    bool is_dead = false;
    std::int64_t save_version = 0;
    friend bool operator==(const CharacterRecord&, const CharacterRecord&) = default;
};

enum class CharacterError {
    NotFound,            // no such character on this account (or deleted)
    Dead,                // dead characters cannot be played
    SlotsFull,           // the account has no free character slot
    ServiceUnavailable,  // the database could not be reached: fail closed
    StaleSave,           // a newer save already exists (save_version check)
};

[[nodiscard]] const char* to_string(CharacterError e) noexcept;

/// Character data access. Blocking: call it on the persistence worker, never on the simulation thread.
class ICharacterRepository {
public:
    virtual ~ICharacterRepository() = default;
    [[nodiscard]] virtual std::expected<CharacterRecord, CharacterError> load(std::int64_t account_id, std::int32_t character_id) = 0;
    /// Creates a character from `initial` (account_id, class, skin, starting values). Assigns character_id from the
    /// account's counter and checks the account's slot limit in the same transaction.
    [[nodiscard]] virtual std::expected<CharacterRecord, CharacterError> create(const CharacterRecord& initial) = 0;
    /// Saves a snapshot if its save_version is newer than the stored one; returns the stored version.
    [[nodiscard]] virtual std::expected<std::int64_t, CharacterError> save(const CharacterRecord& snapshot) = 0;
};

/// In-memory repository (tests, and running without PostgreSQL in development). Thread-safe.
class InMemoryCharacterRepository final : public ICharacterRepository {
public:
    explicit InMemoryCharacterRepository(int max_characters_per_account = 2) : max_characters_(max_characters_per_account) {}

    std::expected<CharacterRecord, CharacterError> load(std::int64_t account_id, std::int32_t character_id) override;
    std::expected<CharacterRecord, CharacterError> create(const CharacterRecord& initial) override;
    std::expected<std::int64_t, CharacterError> save(const CharacterRecord& snapshot) override;

    void set_unavailable(bool unavailable);
    [[nodiscard]] std::size_t count(std::int64_t account_id) const;

private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::int64_t, std::int32_t>, CharacterRecord> characters_;
    std::map<std::int64_t, std::int32_t> next_id_;
    int max_characters_;
    bool unavailable_ = false;
};

}  // namespace runity::persistence
