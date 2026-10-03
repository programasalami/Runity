#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "waw/content/behavior.hpp"
#include "waw/content/projectile.hpp"

namespace waw::content {

/// A ground tile type (Ground.xml <Ground type id>). Gameplay-relevant fields only; art keys stay in the file for the client.
struct GroundDesc {
    std::uint16_t type = 0;
    std::string id;
    bool no_walk = false;
    float speed = 1.0f;   // movement multiplier (<Speed>, default 1)
    bool sinking = false;
    int damage = 0;       // parsed; the reference never applies it (WorldSystem.md)
};

/// The original's realm terrain types (Common/Enumerables.cs TerrainType), used by the realm spawner.
enum class Terrain : std::uint8_t {
    None, Mountains, HighSand, HighPlains, HighForest, MidSand, MidPlains, MidForest, LowSand, LowPlains, LowForest, ShoreSand,
    ShorePlains, BeachTowels, Count
};
inline constexpr std::size_t kTerrainCount = static_cast<std::size_t>(Terrain::Count);
[[nodiscard]] std::string_view to_string(Terrain t) noexcept;
[[nodiscard]] std::optional<Terrain> terrain_from_string(std::string_view name) noexcept;

/// <Spawn><Mean/><StdDev/><Min/><Max/></Spawn>: how many of a terrain monster the realm spawner places together (SpawnDesc.cs).
struct SpawnGroup {
    int mean = 1;
    int std_dev = 0;
    int min = 1;
    int max = 1;
};

/// An object type (Objects.xml / StaticObjects.xml / Players.xml / Containers.xml ... <Object type id>).
struct ObjectDesc {
    std::uint16_t type = 0;
    std::string id;
    std::string object_class;  // <Class>: Character, Wall, GameObject, Player, Container, Portal, Projectile, Equipment ...
    std::string group;         // <Group>: behaviour targets by group (Spawn group:, HealGroup)
    bool is_static = false;
    bool occupy_square = false;
    bool full_occupy = false;
    bool enemy_occupy_square = false;
    bool blocks_sight = false;  // <BlocksSight/> or any wall class (the original ObjectDesc.cs)
    bool enemy = false;
    bool player = false;
    bool item = false;          // <Item/>: an item type is never an entity (the original XmlLibrary)
    int max_hp = 0;
    int defense = 0;
    int size = 100;
    float xp_mult = 1.0f;
    int level = 0;
    Terrain terrain = Terrain::None;  // <Terrain>: the realm spawner places it on that terrain
    float spawn_prob = 1.0f;          // <SpawnProb> (default 1, ObjectDesc.cs)
    std::optional<SpawnGroup> spawn;  // <Spawn>
    std::map<int, ProjectileDesc> projectiles;  // <Projectile id="N">: what Shoot(projectilePropsId: N) fires
};

/// The eight character stats, in Players.xml order.
enum class Stat : std::uint8_t { MaxHp, MaxMp, Attack, Defense, Speed, Dexterity, HpRegen, MpRegen, Count };
inline constexpr std::size_t kStatCount = static_cast<std::size_t>(Stat::Count);
[[nodiscard]] std::string_view to_string(Stat s) noexcept;
/// The stat numbers the original's XML uses in <ActivateOnEquip stat="N"> / <Activate stat="N"> (classic RotMG StatData, as the
/// original client reads them, StatsUtil.FromId): 0 MaxHP, 3 MaxMP, 20 Attack, 21 Defense, 22 Speed, 26 Vitality, 27 Wisdom,
/// 28 Dexterity. Any other number boosts nothing (the client shows "Invalid Stat!").
[[nodiscard]] std::optional<Stat> stat_from_reference_id(int id) noexcept;

/// An item (an <Object> with <Item/>). Only what the server needs to validate and simulate.
struct ItemDesc {
    std::uint16_t type = 0;
    std::string id;
    int slot_type = 0;
    int tier = -1;                       // -1 = untiered
    float rate_of_fire = 1.0f;
    int num_projectiles = 1;
    float arc_gap_degrees = 11.25f;
    std::optional<ProjectileDesc> projectile;
    bool consumable = false;
    int heal_amount = 0;                 // <Activate amount="N">Heal</Activate>
    int magic_amount = 0;                // <Activate amount="N">Magic</Activate>
    int bag_type = 0;                    // the original BagType: 0 Common, 1 Pink, 2 Cyan, 3 Blue, 4 White, 5 Purple
    bool soulbound = false;              // <Soulbound/>: dropped items stay in a bag only its owner sees
    std::array<int, kStatCount> stat_bonuses{};  // <ActivateOnEquip stat amount>IncrementStat</ActivateOnEquip> while worn
    std::array<int, kStatCount> stat_increments{};  // <Activate stat amount>IncrementStat</Activate>: a stat potion
};

struct StatRange {
    int start = 0;  // starting value (element text)
    int max = 0;    // max attribute
};

struct LevelIncrease {
    int min = 0;
    int max = 0;
};

/// A playable class (an ObjectDesc with <Player/>).
struct PlayerClassDesc {
    std::uint16_t type = 0;
    std::string id;
    std::array<StatRange, kStatCount> stats{};
    std::array<LevelIncrease, kStatCount> level_increase{};
    std::vector<int> slot_types;
    std::vector<int> equipment;  // item types, -1 = empty
};

/// One palette entry of a map.
struct TileTemplate {
    std::uint16_t ground_type = 0xFF;            // 0xFF = no ground (void), the reference's sentinel
    std::optional<std::uint16_t> object_type;    // a static object standing on the tile
    std::string object_name;                     // optional per-placement name
    std::vector<std::string> regions;            // e.g. "Spawn"
    Terrain terrain = Terrain::None;             // .wmap realm terrain
};

/// A map from a .jm file: width x height indices into `palette`, row-major (y then x).
struct MapData {
    std::string name;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<TileTemplate> palette;
    std::vector<std::uint16_t> tiles;

    [[nodiscard]] const TileTemplate& at(std::uint16_t x, std::uint16_t y) const { return palette[tiles[std::size_t{y} * width + x]]; }
};

/// A world configuration (Content/Worlds/*.json, the original WorldConfig.cs). Fields no system reads yet stay in raw_json.
struct WorldConfig {
    int id = 0;
    std::string name;
    std::string display_name;
    std::string music;
    int difficulty = 0;
    int max_players = -1;
    bool blocks_sight = false;
    bool long_lasting = false;
    std::vector<std::string> maps;  // one is picked at random for each instance (the original World.Load)
    std::string raw_json;
};

/// A monster the realm spawner may place on a terrain (an object with <Terrain>).
struct TerrainSpawn {
    std::uint16_t object_type = 0;
    float probability = 1.0f;
    SpawnGroup group;
};

/// Every gameplay definition, loaded once at start and immutable afterwards (pass by const reference).
class ContentDb {
public:
    /// Loads Definitions/*.xml, Maps/*.jm|*.wmap, Worlds/*.json and Behaviors/*.json under `root`. A malformed file is an error.
    /// Like the original (XmlLibrary TryAdd), the first definition of a type or id wins and later duplicates are skipped; those,
    /// map tiles naming unknown grounds / objects and behaviour details that cannot run are collected in warnings() instead.
    [[nodiscard]] static std::expected<ContentDb, std::string> load(const std::filesystem::path& root);

    [[nodiscard]] const GroundDesc* ground(std::uint16_t type) const;
    [[nodiscard]] const GroundDesc* ground(std::string_view id) const;
    [[nodiscard]] const ObjectDesc* object(std::uint16_t type) const;
    [[nodiscard]] const ObjectDesc* object(std::string_view id) const;
    [[nodiscard]] const PlayerClassDesc* player_class(std::uint16_t type) const;
    [[nodiscard]] const MapData* map(std::string_view name) const;
    [[nodiscard]] const WorldConfig* world(std::string_view name) const;
    [[nodiscard]] const ItemDesc* item(std::uint16_t type) const;
    [[nodiscard]] const ItemDesc* item(std::string_view id) const;
    /// The AI of a monster type, if any behaviour file defines one.
    [[nodiscard]] const behavior::Behavior* behavior(std::uint16_t object_type) const;
    /// The original's loot bag object for a bag type (InventoryUtils.GetBagIdFromType: 0 Common 0x500, 1 Pink 0x506, 2 Cyan 0x508,
    /// 3 Blue 0x509, 4 White 0x510, 5 Purple 0x507); unknown bag types use the common bag.
    [[nodiscard]] std::uint16_t loot_bag_type(int bag_type) const noexcept;
    /// Items a TierLoot(tier, class) picks from: every item with that <Tier> whose slot type belongs to the class.
    [[nodiscard]] const std::vector<std::uint16_t>& tier_items(behavior::TierClass cls, int tier) const;
    /// The realm spawner's monsters for a terrain (every object with that <Terrain>, in load order).
    [[nodiscard]] const std::vector<TerrainSpawn>& terrain_spawns(Terrain terrain) const;
    /// Objects of a <Group> (behaviours that spawn or heal "a group").
    [[nodiscard]] std::vector<std::uint16_t> group_members(std::string_view group) const;
    [[nodiscard]] std::size_t item_count() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t behavior_count() const noexcept { return behaviors_.size(); }
    /// Content oddities that did not stop the load (duplicate definitions, unknown names in maps, unusable projectiles).
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    [[nodiscard]] std::size_t ground_count() const noexcept { return grounds_.size(); }
    [[nodiscard]] std::size_t object_count() const noexcept { return objects_.size(); }
    [[nodiscard]] const std::map<std::uint16_t, PlayerClassDesc>& player_classes() const noexcept { return classes_; }
    [[nodiscard]] const std::map<std::string, MapData, std::less<>>& maps() const noexcept { return maps_; }
    [[nodiscard]] const std::map<std::string, WorldConfig, std::less<>>& worlds() const noexcept { return worlds_; }

    // Loaders, public for tests.
    [[nodiscard]] std::expected<void, std::string> add_xml(std::string_view xml_text, std::string_view source_name);
    [[nodiscard]] std::expected<void, std::string> add_map(std::string_view name, std::string_view jm_text);
    /// The original's binary realm map (MapData.LoadWMap): version byte, then zlib { i16 template count; per template u16 ground
    /// type, u8-length object id, u8-length object config, u8 terrain, u8 region, (+u8 elevation in version 1); i32 width,
    /// i32 height; i16 index per tile; (+u8 elevation per tile in version 2) }, all little-endian.
    [[nodiscard]] std::expected<void, std::string> add_wmap(std::string_view name, std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::expected<void, std::string> add_world(std::string_view json_text, std::string_view source_name);
    /// One Content/Behaviors/*.json file (format 1, Documentation/Migration/AI.md).
    [[nodiscard]] std::expected<void, std::string> add_behaviors(std::string_view json_text, std::string_view source_name);
    /// Resolves names between files (projectile looks, behaviour targets, loot items). Called by load(); tests call it after add_*.
    [[nodiscard]] std::expected<void, std::string> link();

private:
    void link_behavior(behavior::Behavior& b);

    std::map<std::uint16_t, GroundDesc> grounds_;
    std::unordered_map<std::string, std::uint16_t> ground_by_id_;
    std::map<std::uint16_t, ObjectDesc> objects_;
    std::unordered_map<std::string, std::uint16_t> object_by_id_;
    std::map<std::uint16_t, PlayerClassDesc> classes_;
    std::map<std::string, MapData, std::less<>> maps_;
    std::map<std::string, WorldConfig, std::less<>> worlds_;
    std::vector<std::string> warnings_;
    std::map<std::uint16_t, ItemDesc> items_;
    std::unordered_map<std::string, std::uint16_t> item_by_id_;
    std::vector<behavior::Behavior> pending_behaviors_;  // by object id until link()
    std::map<std::uint16_t, behavior::Behavior> behaviors_;
    std::map<std::pair<int, int>, std::vector<std::uint16_t>> tier_items_;  // (TierClass, tier) -> items
    std::array<std::vector<TerrainSpawn>, kTerrainCount> terrain_spawns_;
};

}  // namespace waw::content
