#include "runity/persistence/characters.hpp"

namespace runity::persistence {

const char* to_string(CharacterError e) noexcept {
    switch (e) {
        case CharacterError::NotFound: return "character not found";
        case CharacterError::Dead: return "character is dead";
        case CharacterError::SlotsFull: return "no free character slot";
        case CharacterError::ServiceUnavailable: return "character database unavailable";
        case CharacterError::StaleSave: return "stale save";
    }
    return "?";
}

std::expected<CharacterRecord, CharacterError> InMemoryCharacterRepository::load(std::int64_t account_id, std::int32_t character_id) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(CharacterError::ServiceUnavailable);
    auto it = characters_.find({account_id, character_id});
    if (it == characters_.end()) return std::unexpected(CharacterError::NotFound);
    if (it->second.is_dead) return std::unexpected(CharacterError::Dead);
    return it->second;
}

std::expected<CharacterRecord, CharacterError> InMemoryCharacterRepository::create(const CharacterRecord& initial) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(CharacterError::ServiceUnavailable);
    std::size_t living = 0;
    for (const auto& [key, c] : characters_) living += (key.first == initial.account_id && !c.is_dead) ? 1 : 0;
    if (living >= static_cast<std::size_t>(max_characters_)) return std::unexpected(CharacterError::SlotsFull);
    CharacterRecord c = initial;
    c.character_id = ++next_id_[initial.account_id];
    c.save_version = 0;
    characters_[{c.account_id, c.character_id}] = c;
    return c;
}

std::expected<std::int64_t, CharacterError> InMemoryCharacterRepository::save(const CharacterRecord& snapshot) {
    std::lock_guard lock(mutex_);
    if (unavailable_) return std::unexpected(CharacterError::ServiceUnavailable);
    auto it = characters_.find({snapshot.account_id, snapshot.character_id});
    if (it == characters_.end()) return std::unexpected(CharacterError::NotFound);
    if (snapshot.save_version <= it->second.save_version) return std::unexpected(CharacterError::StaleSave);
    it->second = snapshot;
    return snapshot.save_version;
}

void InMemoryCharacterRepository::set_unavailable(bool unavailable) {
    std::lock_guard lock(mutex_);
    unavailable_ = unavailable;
}

std::size_t InMemoryCharacterRepository::count(std::int64_t account_id) const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [key, c] : characters_) n += key.first == account_id ? 1 : 0;
    return n;
}

}  // namespace runity::persistence
