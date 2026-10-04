#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "runity/content/projectile.hpp"

namespace runity::content {

// Monster behaviours as data (Content/Behaviors/*.json, written once from the original's BehaviorLib.*.cs by
// Tools/behaviors/transpile_behaviorlib.py). Every primitive keeps the original's name, parameters and defaults
// (GameServer/Game/Entities/Behaviors/Actions/*.cs, Transitions/*.cs); speeds are tiles per second, times ms, angles degrees.
// The engine that runs them is runity::sim (World, behavior/engine.cpp). Documentation/Migration/AI.md describes the format.
namespace behavior {

/// Whom a script aims at (the original BehaviorScript.TargetType; Player / Entity targets are named by a string instead).
enum class TargetType : std::uint8_t { ClosestPlayer, FixedAngle, RandomPlayerPerBehavior, RandomPlayerPerCycle, FarthestPlayer, Entity };

/// An entity named by its object id (resolved to a type after loading); "player" means any player.
struct EntityRef {
    std::string id;
    std::uint16_t type = 0;
    bool player = false;
};

struct Script;

// ---- movement
struct Wander {
    float speed = 1.0f;
    float distance = 1.0f;
    float distance_from_spawn = 5.0f;
    int cooldown_ms = 0;
};
/// Follow and StayAwayFrom share their parameters (StayAwayFrom moves away instead of closer).
struct Follow {
    float speed = 1.0f;
    float dist_from_target = 2.0f;
    float acquire_range = 10.0f;
    int cooldown_ms = 1000;
    int cooldown_offset_ms = 0;
    int follow_time_ms = 1000;
    TargetType target_type = TargetType::ClosestPlayer;
    EntityRef target{"player", 0, true};
    bool away = false;  // StayAwayFrom
};
struct Charge {
    float speed = 1.0f;
    float range = 10.0f;
    int cooldown_ms = 1000;
};
struct Orbit {
    float speed = 1.0f;
    float radius = 1.0f;
    float acquire_range = 10.0f;
    EntityRef target;  // empty id: the nearest other entity of any kind (the original GetNearestOtherEntityByName(null))
    float speed_variance = 0.0f;
    float radius_variance = 0.0f;
    bool clockwise = false;
    bool target_player = false;
};
struct Protect {
    float speed = 1.0f;
    EntityRef protectee;
    float acquire_range = 10.0f;
    float protection_range = 2.0f;
    float reprotect_range = 1.0f;
};
struct MoveLine {
    float speed = 1.0f;
    float angle = 0.0f;
    float distance = 0.0f;
};
struct ReturnToSpawn {
    float speed = 1.0f;
    float distance_from_spawn = 0.0f;
};
struct BackAndForth {
    float speed = 1.0f;
    float distance = 5.0f;
};
struct Buzz {
    float speed = 2.0f;
    float distance = 0.5f;
};
struct Swirl {
    float speed = 1.0f;
    float radius = 8.0f;
    float acquire_range = 10.0f;
    bool targeted = true;
};

// ---- attacks
struct Shoot {
    float max_radius = 0.0f;
    float min_radius = 0.0f;
    int count = 1;
    float shoot_angle = 0.0f;   // fan spacing
    float fixed_angle = 0.0f;
    float rotate_angle = 0.0f;  // added after every shot
    float angle_offset = 0.0f;
    float predictive = 0.0f;
    int cooldown_offset_ms = 0;
    int cooldown_ms = 0;
    TargetType target_type = TargetType::FixedAngle;  // targeted: true = ClosestPlayer
    float x_offset = 0.0f;
    float y_offset = 0.0f;
    int projectile_index = -1;   // >= 0: the object's own XML <Projectile id>, else `projectile`
    std::vector<ProjectileDesc> projectile;  // the inline projectile (0 or 1 element)
};
struct Aoe {
    float radius = 0.0f;
    int damage = 0;
    int cooldown_ms = 0;
    float range = 12.0f;
    int cooldown_offset_ms = 0;
    TargetType target_type = TargetType::ClosestPlayer;
    float fixed_angle = 0.0f;
    float angle_offset = 0.0f;
    int activate_count = 1;
    int throw_time_ms = 1500;
    int damage_cooldown_ms = 1000;
    float rotate_angle = 0.0f;
    std::vector<EffectSpec> effects;
};

// ---- spawning and groups
struct Spawn {
    EntityRef entity;
    std::vector<std::uint16_t> group;  // group: every object of that <Group>, one picked per child
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    int cooldown_ms = 1000;
    int cooldown_offset_ms = 0;
    int max_spawns_per_reset = std::numeric_limits<int>::max();
    int min_spawn_count = 1;
    int max_spawn_count = 1;
    int max_density = 0;
    float density_radius = 10.0f;
};
struct Reproduce {
    EntityRef entity;  // empty: the monster itself
    int cooldown_ms = 60000;
    int max_density = 0;
    float density_radius = 10.0f;
};
struct TossObject {
    std::vector<EntityRef> children;  // one, or every object of a group
    float range = 5.0f;
    float angle = 0.0f;
    int cooldown_ms = 1000;
    int cooldown_offset_ms = 0;
    float probability = 1.0f;
    float min_angle = 0.0f, max_angle = 0.0f;
    float min_range = 0.0f, max_range = 0.0f;
    float density_range = 0.0f;
    int max_density = 1;
    bool targeted = false;
};
struct Order {
    float range = 0.0f;
    EntityRef children;
    std::string target_state;
    bool on_death = false;  // OrderOnDeath
};

// ---- self and death
struct HealSelf {
    int cooldown_ms = 0;
    int amount = 0;
    bool percentage = false;
    int cooldown_offset_ms = 0;
};
struct HealGroup {
    float range = 0.0f;
    std::string group;
    int cooldown_ms = 1000;
    int heal_amount = 0;  // 0 = to full
};
struct ConditionEffect {
    int effect = 0;
    int duration_ms = -1;  // -1 = while the state lasts, 0 = remove
    bool persist = false;
};
struct Taunt {
    std::vector<std::string> texts;  // "a||b": one picked at random
    int cooldown_ms = 0;              // 0 = once when the state starts
    float probability = 1.0f;
};
struct Suicide {
    int delay_ms = 300;
};
struct Transform {
    EntityRef target;
};
struct TransformOnDeath {
    EntityRef target;
    int min = 1;
    int max = 1;
    float probability = 1.0f;
};
struct DropPortalOnDeath {
    EntityRef portal;
    float probability = 1.0f;
    int timeout_s = 0;
};

// ---- composites
struct Timed {
    int period = 0;
    std::vector<Script> scripts;
};
struct Duration {
    int duration = 0;
    std::vector<Script> script;  // exactly one
};
struct Sequence {
    std::vector<Script> scripts;
};
/// Presentation-only primitives (Flash, SetAltTexture, ChangeSize) and ones the engine does not run yet: kept by name.
struct NoOp {
    std::string what;
};

using ScriptKind = std::variant<NoOp, Wander, Follow, Charge, Orbit, Protect, MoveLine, ReturnToSpawn, BackAndForth, Buzz, Swirl, Shoot, Aoe,
                                Spawn, Reproduce, TossObject, Order, HealSelf, HealGroup, ConditionEffect, Taunt, Suicide, Transform,
                                TransformOnDeath, DropPortalOnDeath, Timed, Duration, Sequence>;

struct Script {
    ScriptKind kind;
    std::uint16_t slot = 0;  // index of this script's runtime state in an entity (numbered per behaviour)
};

enum class TransitionKind : std::uint8_t {
    Timed, EntityWithin, EntityNotWithin, EntitiesWithin, EntitiesNotWithin, HpLess, EntityHpLess, DamageTaken, NotMoving, OnParentDeath,
    PlayerText
};
/// How a transition with several targets picks one (the original TransitionType).
enum class TransitionMode : std::uint8_t { Random, Random7Bag, Sequential };

struct Transition {
    TransitionKind kind = TransitionKind::Timed;
    std::vector<std::string> to;
    std::vector<int> to_state;  // resolved state indices
    TransitionMode mode = TransitionMode::Random;
    int time_ms = 0;                 // Timed: time; NotMoving: delay
    float radius = 8.0f;             // Entity(Not)Within / Entities(Not)Within / EntityHpLess dist
    EntityRef target{"player", 0, true};
    std::vector<EntityRef> targets;  // Entities(Not)Within; empty = any entity
    float threshold = 0.0f;          // HpLess / EntityHpLess
    int damage = 0;                  // DamageTaken
    std::string regex;               // PlayerText (not run: the server has no chat hooks into worlds yet)
    std::uint16_t slot = 0;
};

struct State {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    std::vector<Script> scripts;
    std::vector<Transition> transitions;
    std::vector<std::uint16_t> slots;  // runtime slots of this state's own scripts (nested ones too) and transitions
};

/// The original's item classes for TierLoot (ItemType).
enum class TierClass : std::uint8_t { Weapon, Armor, Ability, Ring };

struct LootItem {
    bool tier = false;            // false: ItemLoot, true: TierLoot (a random item of that tier and class)
    std::uint16_t item_type = 0;  // ItemLoot
    std::string item_id;
    int tier_level = 0;           // TierLoot
    TierClass tier_class = TierClass::Weapon;
    float threshold = 0.0f;       // share of the monster's max HP a damage record must reach
    float chance = 0.0f;
};

/// The original LootDrop: rolled once per damage record; public = one shared set of bags, else soulbound bags per account.
struct LootTable {
    bool is_public = false;
    std::vector<LootItem> items;
};

/// One monster's behaviour: a tree of states, flattened (index 0 is the root).
struct Behavior {
    std::string object_id;
    std::uint16_t object_type = 0;
    std::string source;
    std::vector<State> states;
    std::unordered_map<std::string, int> state_by_name;
    std::vector<LootTable> loot;
    std::uint16_t slot_count = 0;

    /// The state an entity is in after entering `index` (the original GetDeepState: first child, recursively).
    [[nodiscard]] int deep_state(int index) const noexcept {
        while (!states[static_cast<std::size_t>(index)].children.empty()) index = states[static_cast<std::size_t>(index)].children.front();
        return index;
    }
};

}  // namespace behavior
}  // namespace runity::content
