#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "runity/content/conditions.hpp"
#include "runity/content/content_db.hpp"
#include "runity/core/strong_id.hpp"
#include "runity/sim/movement.hpp"
#include "runity/sim/projectile_path.hpp"
#include "runity/sim/rules.hpp"
#include "runity/sim/tile_map.hpp"

namespace runity::sim {

struct EntityTag;
/// 32-bit entity id: low 20 bits = slot index, high 12 bits = generation (never 0). A removed entity's id is not handed out
/// again until its slot's generation wraps (4095 reuses), unlike the reference's slot reuse bugs.
using EntityId = core::StrongId<EntityTag>;

enum class EntityKind : std::uint8_t { Other, Player, Enemy, Container, Portal, StaticObject, Npc };

/// Which replicated fields changed since the last tick.
namespace dirty {
inline constexpr std::uint32_t Position = 1 << 0;
inline constexpr std::uint32_t Hp = 1 << 1;
inline constexpr std::uint32_t MaxHp = 1 << 2;
inline constexpr std::uint32_t Level = 1 << 3;
inline constexpr std::uint32_t Skin = 1 << 4;
inline constexpr std::uint32_t Conditions = 1 << 5;
inline constexpr std::uint32_t Name = 1 << 6;
inline constexpr std::uint32_t Items = 1 << 7;
}  // namespace dirty

/// One movement input step from a client (already decoded from the wire).
struct MoveCommand {
    std::uint32_t seq = 0;
    std::uint8_t dt_ms = 0;
    std::int8_t dir_x = 0;
    std::int8_t dir_y = 0;
};

/// What a session needs to create a player in a world (also used to carry a player between worlds).
/// If the content knows the class, stats are the character's own base stats (`stats`, all zero = the class's starting values)
/// plus worn gear; hp / max_hp / speed are then ignored except hp, which is clamped to the computed maximum (tests with a
/// class-less fixture use the given values).
struct PlayerSpawn {
    std::uint32_t session_id = 0;
    std::int32_t character_id = 0;
    std::uint16_t class_type = 0;
    std::uint16_t skin_type = 0;
    std::string name;
    int level = 1;
    int hp = 100;
    int max_hp = 100;
    int speed = 0;
    int mp = -1;  // -1 = full
    std::int64_t xp = 0;
    std::int64_t fame = 0;
    std::vector<int> items;
    std::array<int, content::kStatCount> stats{};  // base stats (content::Stat order), all zero = the class's starting values
    std::optional<Vec2> position;  // default: a random "Spawn" tile
    std::int64_t account_id = 0;   // owner of soulbound loot
    bool has_backpack = false;
};

struct PlayerState {
    std::uint32_t session_id = 0;
    std::int32_t character_id = 0;
    int speed = 0;
    movement::MoverState mover;
    std::deque<MoveCommand> pending;
    std::uint32_t last_applied_seq = 0;
    bool any_applied = false;
    float time_budget_ms = 0.0f;
    std::vector<bool> sent_tiles;  // tiles this player has been told about
    std::vector<EntityId> known;   // entities this player currently sees (sorted)

    // Character (progression + combat).
    std::array<int, content::kStatCount> base{};  // the character's own stats (grown on level-up, raised by stat potions)
    rules::PlayerStats stats;                     // base + gear
    int mp = 0;
    std::int64_t xp = 0;  // progress within the current level
    std::int64_t fame = 0;
    std::int64_t fame_xp_carry = 0;
    std::vector<int> items;
    std::int64_t account_id = 0;
    bool has_backpack = false;
    float hp_regen_carry = 0.0f;
    float mp_regen_carry = 0.0f;
    rules::FireRateBucket fire;
    bool stats_dirty = true;
    bool inventory_dirty = true;
    bool dead = false;
    std::string killed_by;
};

/// Runtime state of one behaviour script or transition in one monster (the original StateResourceController's per-script
/// resources: ShootInfo, WanderInfo, FollowInfo ...). Each primitive gives the fields its own meaning (behavior/engine.cpp).
struct ScriptState {
    float timer = 0.0f;   // cooldown / time left
    float timer2 = 0.0f;  // a second timer (follow time, wander move time)
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    std::uint32_t target = 0;  // an EntityId value, 0 = none
    int count = 0;
    bool flag = false;
    bool fired = false;  // transitions: already fired while its state is active (the original PastTransitions)
};

/// A monster: its behaviour's state machine and what it remembers (the original EntityBehavior + DamageRecords).
struct EnemyState {
    const content::behavior::Behavior* behavior = nullptr;  // null: no AI (it stands still)
    int defense = 0;
    float xp_mult = 1.0f;
    content::Terrain terrain = content::Terrain::None;  // placed by the realm spawner on this terrain
    Vec2 spawn;           // where it entered the world (ReturnToSpawn)
    int current_state = -1;
    std::vector<ScriptState> scripts;              // by behaviour slot
    std::vector<std::uint16_t> armed_on_death;     // death scripts whose state has been entered (the original OnDeath subscriptions)
    EntityId parent;                               // who spawned it (OnParentDeath), 0 = nobody
    std::int64_t damage_taken = 0;                 // total (DamageTaken transitions)
    std::vector<std::pair<EntityId, int>> damage_by;  // damage records per attacker (XP sharing, loot shares)
};

/// A loot bag (reference LootBags): its slots, who may see / open it (0 = anyone), and when it disappears.
struct ContainerState {
    std::vector<int> items;
    std::int64_t owner_account = 0;
    double expires_ms = 0.0;
};

struct Entity {
    EntityId id;
    std::uint16_t object_type = 0;
    EntityKind kind = EntityKind::Other;
    Vec2 position;
    Vec2 previous;  // the position one tick ago (Shoot's predictive aim)
    std::string name;
    int hp = 0;
    int max_hp = 0;
    int level = 0;
    std::uint16_t skin = 0;
    std::uint64_t conditions = 0;  // bit per content::Condition
    std::vector<std::pair<std::uint8_t, double>> condition_ends;  // timed conditions: (condition, expiry ms)
    std::uint32_t dirty = 0;
    std::optional<PlayerState> player;
    std::optional<EnemyState> enemy;
    std::optional<ContainerState> container;

    [[nodiscard]] bool has(content::Condition c) const noexcept { return (conditions & content::condition_bit(c)) != 0; }
};

/// A volley of bullets fired together (sent to clients for drawing; the server simulates every bullet).
struct VolleyEvent {
    EntityId owner;
    bool enemy = false;
    std::uint16_t first_bullet = 0;
    Vec2 position;
    float angle = 0.0f;
    float angle_step = 0.0f;
    int count = 1;
    PathSpec path;
    std::uint16_t projectile_type = 0;
    int size = 100;
};

/// Floating text over an entity, for specific players (damage numbers, XP, heals).
struct NotifyEvent {
    EntityId entity;
    std::string text;
    std::uint32_t color = 0xFFFFFFFF;  // RRGGBBAA
    bool damage = false;
    std::vector<EntityId> to;  // players who should see it
};

/// A monster speaking (the original Taunt: enemy chat to the players around it).
struct TauntEvent {
    EntityId entity;
    std::string speaker;
    std::string text;
    std::vector<EntityId> to;
};

struct PlayerDeathEvent {
    EntityId player;
    std::uint32_t session_id = 0;
    std::string killed_by;
    int level = 1;
    std::int64_t fame = 0;
};

/// Everything one player learns in one tick (filled by World::tick, sent by the game layer).
struct ViewUpdate {
    EntityId viewer;
    std::uint32_t session_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t ack_input_seq = 0;
    struct TileOut {
        std::uint16_t x, y, ground, object;
    };
    std::vector<TileOut> tiles;
    std::vector<const Entity*> entered;  // valid until the next tick
    std::vector<EntityId> left;
    struct Changed {
        const Entity* entity;
        std::uint32_t fields;  // dirty bits
    };
    std::vector<Changed> changed;
    const PlayerState* stats = nullptr;   // set when the viewer's own stats changed
    const std::vector<int>* inventory = nullptr;  // set when the viewer's items changed
};

struct WorldRules {
    float sight_radius = 20.0f;                // reference SIGHT_RADIUS
    float movement_slack_ms = 250.0f;          // how much input time may run ahead of real time (lag bursts)
    float max_time_budget_ms = 1000.0f;        // input time a player may bank while idle or lagging
    std::size_t max_pending_steps = 240;       // more queued steps than this = flooding
    float hit_radius = 0.5f;                   // reference HIT_DIST_SQR 0.25
    /// Monsters farther than this from every player do not run their behaviour (a 2048x2048 realm holds ~30 000 of them);
    /// 0 = every monster always runs (small worlds, tests).
    float active_radius = 40.0f;
    int loot_bag_lifetime_ms = 60000;
    /// The original realm's terrain spawner (LEGACY Oryx.cs): keeps 1.5 % of every terrain's tiles populated with its monsters.
    bool terrain_spawner = false;
    float terrain_density = 0.015f;
    float repopulate_interval_ms = 25000.0f;   // LEGACY Oryx.Tick: Repopulate every 25 s
};

/// One world instance: tile map, entities, input application, combat, monster behaviours, spawning and replication.
/// Single-threaded: only the simulation thread calls it. No sockets, no SQL, no protocol types.
class World {
public:
    World(std::int32_t id, const content::WorldConfig& config, const content::MapData& map, const content::ContentDb& content,
          std::uint64_t seed, WorldRules rules = {});

    [[nodiscard]] std::int32_t id() const noexcept { return id_; }
    [[nodiscard]] const content::WorldConfig& config() const noexcept { return config_; }
    /// The instance's name for players (a realm is named after a monster, the original RealmManager.GetNewRealmName).
    [[nodiscard]] const std::string& display_name() const noexcept { return display_name_; }
    void set_display_name(std::string name) { display_name_ = std::move(name); }
    [[nodiscard]] const TileMap& map() const noexcept { return map_; }
    [[nodiscard]] std::uint32_t tick_count() const noexcept { return tick_; }
    [[nodiscard]] double now_ms() const noexcept { return now_ms_; }
    [[nodiscard]] const WorldRules& rules() const noexcept { return rules_; }

    /// Places a player (on a random "Spawn" region tile unless a position is given).
    EntityId add_player(const PlayerSpawn& spawn);
    /// Places any object (portals, monsters from tests or behaviours). Monsters with a behaviour get their AI.
    EntityId add_object(std::uint16_t object_type, Vec2 position);
    void remove(EntityId id);
    [[nodiscard]] Entity* find(EntityId id);
    [[nodiscard]] const Entity* find(EntityId id) const;
    [[nodiscard]] std::size_t entity_count() const noexcept { return live_count_; }
    [[nodiscard]] std::size_t player_count() const noexcept { return players_.size(); }
    [[nodiscard]] std::size_t count_of(std::uint16_t object_type) const noexcept;
    template <class F>
    void for_each(F&& f) {
        for (auto& s : slots_) {
            if (s.entity) f(*s.entity);
        }
    }

    enum class InputResult { Accepted, Flooding, OutOfOrder };
    /// Queues movement steps; they are applied in tick() within the player's time budget.
    InputResult queue_input(EntityId player, std::span<const MoveCommand> steps);

    enum class ShootResult { Fired, NoWeapon, TooFast, Dead };
    /// One attack with the weapon in slot 0, aimed at `angle` (radians). `shot_id` (client-chosen) numbers the bullets so the
    /// client's wave phases match ((shot_id * projectiles + i) % 2).
    ShootResult shoot(EntityId player, std::uint16_t shot_id, float angle);

    /// Advances one tick of `dt_ms`, then builds one ViewUpdate per player.
    void tick(float dt_ms, std::vector<ViewUpdate>& out);

    /// Events produced since the last call (volleys, notifications, taunts, deaths).
    std::vector<VolleyEvent> take_volleys() { return std::exchange(volleys_, {}); }
    std::vector<NotifyEvent> take_notifications() { return std::exchange(notifications_, {}); }
    std::vector<TauntEvent> take_taunts() { return std::exchange(taunts_, {}); }
    std::vector<PlayerDeathEvent> take_deaths() { return std::exchange(deaths_, {}); }

    enum class ItemResult { Done, NotAllowed, TooFar, NoItem, Full, Maxed };
    /// Moves an item between two slots: the player's own (entity = the player) or a loot bag within reach. Swaps if both hold items.
    ItemResult swap_items(EntityId player, EntityId from, int from_slot, EntityId to, int to_slot);
    /// Uses the item in a player slot: potions heal / restore magic, stat potions raise a stat up to the class maximum.
    ItemResult use_item(EntityId player, int slot);
    /// Drops the item in a player slot into a new bag at the player's feet (owned by the player if the item is soulbound).
    ItemResult drop_item(EntityId player, int slot);
    /// Puts items into bags of 8 at a spot (the original LootDrop: bag type = the highest BagType inside). owner 0 = a public bag.
    void drop_bags(Vec2 at, std::int64_t owner_account, const std::vector<int>& items);

    /// Damages an entity as if hit by `by` (tests; also the single path every hit goes through).
    void damage(Entity& target, int raw_damage, bool armor_piercing, EntityId by, const std::string& by_name);
    /// Turns a condition on for `duration_ms` (< 0: until removed).
    void apply_condition(Entity& e, content::Condition c, int duration_ms);
    void remove_condition(Entity& e, content::Condition c);

    /// Gives a player XP (already capped); handles level-ups (stats grow by the class's LevelIncrease) and fame.
    void award_xp(Entity& player, int xp);
    /// Puts a monster's behaviour into a named state (the original EntityBehavior.TransitionTo; Order uses it).
    void order(Entity& enemy, const std::string& state);
    /// The realm spawner's fill (also called every repopulate interval); returns how many monsters it placed.
    int populate_terrain();

private:
    struct Projectile {
        EntityId owner;
        bool enemy = false;
        std::uint32_t bullet_id = 0;
        Vec2 start;
        float angle = 0.0f;
        PathSpec path;
        double start_ms = 0.0;
        int damage = 0;
        bool multi_hit = false;
        bool passes_cover = false;
        bool armor_piercing = false;
        const std::vector<content::EffectSpec>* effects = nullptr;
        std::string owner_name;
        std::vector<EntityId> hit;
        bool done = false;
    };

    /// A thrown AOE waiting to land (the original AOEDamager).
    struct PendingAoe {
        EntityId owner;
        std::string owner_name;
        Vec2 at;
        float radius = 0.0f;
        int damage = 0;
        double due_ms = 0.0;
        int activations_left = 1;
        int cooldown_ms = 1000;
        const std::vector<content::EffectSpec>* effects = nullptr;
    };

    // ---- entities and the spatial grid
    EntityId allocate(Entity entity);
    Entity* slot(EntityId id);
    /// Creates an entity now, or (inside a tick) at the end of the tick; returns the entity so callers can finish it.
    Entity make_object(std::uint16_t object_type, Vec2 position);
    void spawn(Entity entity);
    void flush_spawns();
    void rebuild_grid();
    template <class F>
    void for_each_near(Vec2 at, float radius, F&& f);
    [[nodiscard]] Entity* nearest_player(Vec2 from, float radius);
    [[nodiscard]] std::vector<EntityId> players_near(Vec2 at, float radius);

    // ---- players
    void apply_inputs(Entity& e, float dt_ms);
    void regenerate(Entity& e, float dt_ms);
    void refresh_player_stats(Entity& e);
    void replicate(Entity& viewer, ViewUpdate& out);
    [[nodiscard]] bool can_see(const PlayerState& viewer, const Entity& e) const;

    // ---- combat, loot, conditions
    void tick_projectiles(float dt_ms);
    void tick_aoes();
    void tick_conditions(Entity& e);
    void fire(const VolleyEvent& v, int damage, bool multi_hit, bool passes_cover, bool armor_piercing,
              const std::vector<content::EffectSpec>* effects, const std::string& owner_name);
    void kill_enemy(Entity& e);
    void roll_loot(const Entity& enemy);
    void tick_containers();
    [[nodiscard]] bool bullet_stopped_by(int x, int y, bool passes_cover) const;

    // ---- monster behaviours (behavior/engine.cpp)
    void start_behavior(Entity& e);
    void tick_enemy(Entity& e, float dt_ms);
    void enter_state(Entity& e, int state);
    void exit_state(Entity& e, int state);
    void transition_to(Entity& e, int target);
    enum class ScriptResult : std::uint8_t { OnCooldown, Failed, Active, Activate, Deactivate };
    void start_script(Entity& e, const content::behavior::Script& s);
    void end_script(Entity& e, const content::behavior::Script& s);
    ScriptResult tick_script(Entity& e, const content::behavior::Script& s, float dt_ms);
    void start_transition(Entity& e, const content::behavior::Transition& t);
    [[nodiscard]] int tick_transition(Entity& e, const content::behavior::Transition& t, float dt_ms);
    void run_death_scripts(Entity& e);
    [[nodiscard]] bool enemy_can_stand(int x, int y) const;
    void move_enemy(Entity& e, Vec2 target);
    void move_towards(Entity& e, Vec2 target, float speed, float dt_ms);
    [[nodiscard]] Entity* find_target(const Entity& host, content::behavior::TargetType type, float radius,
                                      const content::behavior::EntityRef& target);
    [[nodiscard]] Entity* nearest_named(const Entity& host, const content::behavior::EntityRef& ref, float radius);
    [[nodiscard]] int count_named(Vec2 at, std::uint16_t type, float radius);
    void tick_terrain_spawner();
    int spawn_terrain_group(const content::TerrainSpawn& spawn, content::Terrain terrain);

    std::int32_t id_;
    const content::WorldConfig& config_;
    const content::ContentDb& content_;
    std::string display_name_;
    TileMap map_;
    WorldRules rules_;
    std::mt19937_64 rng_;
    std::uint32_t tick_ = 0;
    double now_ms_ = 0.0;
    struct Slot {
        std::uint16_t generation = 0;  // 0 = never used
        std::optional<Entity> entity;
    };
    // A deque: adding an entity never moves the existing ones, so references held during a tick stay valid. Entities created
    // while the tick iterates (loot bags, spawned monsters) wait in spawn_queue_ until the iteration is over.
    std::deque<Slot> slots_;
    std::vector<Entity> spawn_queue_;
    bool in_tick_ = false;
    std::vector<std::uint32_t> free_slots_;
    std::size_t live_count_ = 0;
    std::vector<EntityId> players_;
    std::vector<EntityId> containers_;  // loot bags (they expire)
    // Spatial grid of kChunk x kChunk tiles, rebuilt once at the start of every tick (a counting sort of the entities by cell):
    // ids of cell c are grid_ids_[grid_start_[c] .. grid_start_[c + 1]). Entities created during a tick join at the next one.
    static constexpr int kChunk = 16;
    int grid_w_ = 0;
    int grid_h_ = 0;
    std::vector<std::uint32_t> grid_start_;
    std::vector<EntityId> grid_ids_;
    std::vector<std::uint32_t> grid_scratch_;
    std::vector<char> active_cells_;
    std::vector<Projectile> projectiles_;
    std::vector<PendingAoe> aoes_;
    std::vector<std::pair<double, EntityId>> timed_removals_;  // Suicide, portal timeouts
    std::uint16_t next_enemy_bullet_ = 0;
    std::array<int, content::kTerrainCount> terrain_max_{};
    double next_repopulate_ms_ = 0.0;
    std::vector<VolleyEvent> volleys_;
    std::vector<NotifyEvent> notifications_;
    std::vector<TauntEvent> taunts_;
    std::vector<PlayerDeathEvent> deaths_;
    std::vector<EntityId> to_remove_;
};

template <class F>
inline void World::for_each_near(Vec2 at, float radius, F&& f) {
    // Entities move a little between grid rebuilds: look one chunk further than the radius needs.
    const int x0 = std::clamp(static_cast<int>((at.x - radius) / kChunk) - 1, 0, grid_w_ - 1);
    const int x1 = std::clamp(static_cast<int>((at.x + radius) / kChunk) + 1, 0, grid_w_ - 1);
    const int y0 = std::clamp(static_cast<int>((at.y - radius) / kChunk) - 1, 0, grid_h_ - 1);
    const int y1 = std::clamp(static_cast<int>((at.y + radius) / kChunk) + 1, 0, grid_h_ - 1);
    const float r2 = radius * radius;
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const auto cell = static_cast<std::size_t>(cy) * static_cast<std::size_t>(grid_w_) + static_cast<std::size_t>(cx);
            for (auto i = grid_start_[cell]; i < grid_start_[cell + 1]; ++i) {
                Entity* e = slot(grid_ids_[i]);
                if (e != nullptr && distance_squared(e->position, at) <= r2) f(*e);
            }
        }
    }
}

}  // namespace runity::sim
