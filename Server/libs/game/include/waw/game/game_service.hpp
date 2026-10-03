#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "waw/content/content_db.hpp"
#include "waw/core/log.hpp"
#include "waw/core/task_queue.hpp"
#include "waw/persistence/characters.hpp"
#include "waw/session/session_manager.hpp"
#include "waw/sim/world.hpp"

namespace waw::game {

struct GameRules {
    std::string start_world = "Nexus";        // where every character enters (the original: NEXUS_ID)
    std::string realm_world = "Realm";        // where the Nexus's Realm Portals lead
    std::string entry_world;                  // where characters appear when they enter; empty = start_world (tests use "Realm")
    int realm_count = 1;                      // Realm Portals in the Nexus, one realm each (gameServerConfig.xml RealmCount)
    /// Realm instances are named after a monster (realmConfig.xml Names, RealmManager.GetNewRealmName).
    std::vector<std::string> realm_names = {
        "Lich", "Goblin", "Ghost", "Giant", "Gorgon", "Blob", "Leviathan", "Unicorn", "Minotaur", "Cube", "Pirate", "Spider", "Snake",
        "Deathmage", "Gargoyle", "Scorpion", "Djinn", "Phoenix", "Satyr", "Drake", "Orc", "Flayer", "Cyclops", "Sprite", "Chimera",
        "Kraken", "Hydra", "Slime", "Ogre", "Hobbit", "Titan", "Medusa", "Golem", "Demon", "Skeleton", "Mummy", "Imp", "Bat", "Wyrm",
        "Spectre", "Reaper", "Beholder", "Dragon", "Harpy"};
    std::size_t max_chat_bytes = 256;         // reference PlayerChat.cs:14
    std::size_t max_tiles_per_message = 4096;
    int player_slots = 20;                    // reference InventoryLayout.PlayerSlots
    float portal_use_distance = 2.0f;         // the reference had no distance check (Protocol.md U14)
    std::uint64_t world_seed = 0x5741570001ull;
};

/// The bridge between sessions (protocol) and worlds (simulation): character load / create / save, entering and changing worlds,
/// movement input, shooting, chat, and turning each world tick into messages. Runs on the simulation thread only.
class GameService final : public session::IGameHandler {
public:
    GameService(const content::ContentDb& content, session::SessionManager& sessions, persistence::ICharacterRepository& characters,
                core::Worker& worker, core::TaskQueue& completions, core::Logger log, GameRules rules = {});
    ~GameService() override;

    /// Creates the start world and its Realm Portals. A realm itself is created when its portal is first used (or at once when it
    /// is the entry world): a 2048x2048 realm with its ~30 000 monsters is the most expensive thing the server builds.
    [[nodiscard]] std::expected<void, std::string> init();

    void on_game_message(session::Session& session, const protocol::ClientMessage& message) override;
    void on_session_closed(const session::Session& session) override;

    /// One simulation tick of `dt_ms` for every world, then sends what each player learned.
    void tick(float dt_ms);

    [[nodiscard]] std::size_t players_in_world() const noexcept { return presence_.size(); }
    [[nodiscard]] sim::World* start_world() noexcept { return worlds_.empty() ? nullptr : worlds_.front().get(); }
    [[nodiscard]] sim::World* world_named(std::string_view name) noexcept;
    /// Opens the realm behind the n-th Realm Portal now (normally done by the first player using it).
    sim::World* open_realm(std::size_t index = 0);
    /// The entity a session plays, if it is in a world.
    [[nodiscard]] std::optional<std::pair<sim::World*, sim::EntityId>> presence(std::uint32_t session_id) const;

private:
    struct Presence {
        sim::World* world = nullptr;
        sim::EntityId entity;
        persistence::CharacterRecord character;
    };

    void load_character(session::Session& s, std::int32_t character_id);
    void create_character(session::Session& s, const protocol::CreateCharacter& m);
    void enter_world(std::uint32_t session_id, std::expected<persistence::CharacterRecord, persistence::CharacterError> result,
                     bool created);
    void place(session::Session& s, Presence& presence, sim::World& world, const sim::PlayerSpawn& spawn);
    void transfer(session::Session& s, sim::World& target);
    void on_move(session::Session& s, const protocol::MoveInput& m);
    void on_shoot(session::Session& s, const protocol::Shoot& m);
    void on_portal(session::Session& s, const protocol::UsePortal& m);
    void on_chat(session::Session& s, const protocol::ChatSend& m);
    void say(session::Session& s, protocol::ChatChannel channel, std::string text);
    void send_view(const sim::ViewUpdate& v);
    void send_events(sim::World& world);
    void send_deaths(sim::World& world);
    [[nodiscard]] std::expected<sim::World*, std::string> make_world(const std::string& name, std::string display_name);
    /// Copies the simulation's character state into the record and saves it on the worker.
    void save(Presence& p, const sim::Entity* entity, bool dead);
    [[nodiscard]] sim::PlayerSpawn spawn_from(const session::Session& s, const persistence::CharacterRecord& c) const;

    template <class F>
    void post_completion(F&& f) {
        completions_.post([weak = std::weak_ptr<int>(alive_), fn = std::forward<F>(f)]() mutable {
            if (!weak.expired()) fn();
        });
    }

    const content::ContentDb& content_;
    session::SessionManager& sessions_;
    persistence::ICharacterRepository& characters_;
    core::Worker& worker_;
    core::TaskQueue& completions_;
    core::Logger log_;
    GameRules rules_;

    std::vector<std::unique_ptr<sim::World>> worlds_;
    /// Where a portal leads: an open world, or a realm that opens on first use.
    struct PortalTarget {
        sim::World* world = nullptr;
        std::string config;
        std::string display_name;
    };
    std::map<std::pair<const sim::World*, std::uint32_t>, PortalTarget> portals_;  // (world, portal entity id) -> destination
    std::vector<std::pair<const sim::World*, std::uint32_t>> realm_portals_;          // in creation order
    std::mt19937_64 rng_;
    std::map<std::uint32_t, Presence> presence_;  // by session id
    std::set<std::uint32_t> loading_;             // sessions with a load / create in flight
    std::vector<sim::ViewUpdate> views_;          // reused every tick
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

/// Wire conversions (exposed for tests).
[[nodiscard]] protocol::EntityKind to_protocol(sim::EntityKind kind) noexcept;
[[nodiscard]] protocol::EntityFull to_full(const sim::Entity& e);
[[nodiscard]] protocol::EntityDelta to_delta(const sim::Entity& e, std::uint32_t dirty_fields);
[[nodiscard]] protocol::PlayerStats to_stats(const sim::Entity& e);

}  // namespace waw::game
