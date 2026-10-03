// The monster behaviour engine: runs the state machines of Content/Behaviors/*.json (waw/content/behavior.hpp).
//
// Semantics follow the original Alloy server (GameServer/Game/Entities/Behaviors):
//  * State.Tick: the root ticks first; per active state (root -> current) its transitions are checked in order (the first that
//    fires wins, once per entry of its state: PastTransitions), then EVERY script of that state ticks. So a root Wander and a child
//    Follow both move the monster in the same tick.
//  * EntityBehavior.Load / TransitionTo: entering a state enters its first child recursively (GetDeepState); a transition exits the
//    current state and every ancestor that is not an ancestor of the target, then enters the target's chain top-down.
//  * Script.Start runs on entry, End on exit; per-entity per-script runtime state lives in EnemyState::scripts (the original
//    StateResourceController) and is reset on entry.
// Deliberate differences (Documentation/Migration/AI.md): monsters collide with walls / water; several original queries pass a
// radius where a squared radius is expected - the radii here are the ones the behaviour names; Spawn stops at maxSpawnsPerReset
// (the original allowed one more); HealGroup heals the named <Group> (the original compared it with object names and never healed).

#include <algorithm>
#include <cmath>
#include <numbers>

#include "waw/sim/world.hpp"

namespace waw::sim {

namespace {

namespace bh = content::behavior;
using content::Condition;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegToRad = kPi / 180.0f;
constexpr std::uint32_t kHealColor = 0x40FF60FF;
constexpr int kPredictTicks = 10;  // the original Shoot.PREDICT_NUM_TICKS

bool is_death_script(const bh::Script& s) {
    return std::holds_alternative<bh::TransformOnDeath>(s.kind) || std::holds_alternative<bh::DropPortalOnDeath>(s.kind) ||
           (std::holds_alternative<bh::Order>(s.kind) && std::get<bh::Order>(s.kind).on_death);
}

/// Finds a script by its runtime slot (death scripts are looked up when the monster dies).
const bh::Script* find_slot(const std::vector<bh::Script>& scripts, std::uint16_t slot) {
    for (const auto& s : scripts) {
        if (s.slot == slot) return &s;
        const std::vector<bh::Script>* inner = nullptr;
        if (const auto* t = std::get_if<bh::Timed>(&s.kind)) inner = &t->scripts;
        if (const auto* q = std::get_if<bh::Sequence>(&s.kind)) inner = &q->scripts;
        if (const auto* d = std::get_if<bh::Duration>(&s.kind)) inner = &d->script;
        if (inner != nullptr) {
            if (const auto* found = find_slot(*inner, slot)) return found;
        }
    }
    return nullptr;
}

float angle_to(Vec2 from, Vec2 to) noexcept { return std::atan2(to.y - from.y, to.x - from.x); }

/// Intersections of two circles (the original Utils.FindCircleCircleIntersections). Returns how many (0, 1 or 2).
int circle_intersections(Vec2 c0, float r0, Vec2 c1, float r1, Vec2& i1, Vec2& i2) noexcept {
    const float dx = c0.x - c1.x;
    const float dy = c0.y - c1.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > r0 + r1 || dist < std::abs(r0 - r1) || (dist == 0.0f && r0 == r1)) return 0;
    const float a = (r0 * r0 - r1 * r1 + dist * dist) / (2.0f * dist);
    const float h = std::sqrt(std::max(0.0f, r0 * r0 - a * a));
    const float cx = c0.x + a * (c1.x - c0.x) / dist;
    const float cy = c0.y + a * (c1.y - c0.y) / dist;
    i1 = {cx + h * (c1.y - c0.y) / dist, cy - h * (c1.x - c0.x) / dist};
    i2 = {cx - h * (c1.y - c0.y) / dist, cy + h * (c1.x - c0.x) / dist};
    return dist == r0 + r1 ? 1 : 2;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ movement helpers

bool World::enemy_can_stand(int x, int y) const {
    if (!map_.in_bounds(x, y)) return false;
    constexpr std::uint8_t blocked = tile_flags::NoWalk | tile_flags::Void | tile_flags::FullOccupy | tile_flags::OccupySquare |
                                     tile_flags::EnemyOccupySquare;
    return (map_.at(x, y).flags & blocked) == 0;
}

void World::move_enemy(Entity& e, Vec2 target) {
    if (e.has(Condition::Paralyzed) || e.has(Condition::Stasis) || !std::isfinite(target.x) || !std::isfinite(target.y)) return;
    Vec2 next = e.position;
    if (target.x >= 0.0f && target.y >= 0.0f && enemy_can_stand(static_cast<int>(target.x), static_cast<int>(target.y))) {
        next = target;
    } else if (target.x >= 0.0f && enemy_can_stand(static_cast<int>(target.x), static_cast<int>(e.position.y))) {
        next = {target.x, e.position.y};  // a blocked diagonal slides along the free axis
    } else if (target.y >= 0.0f && enemy_can_stand(static_cast<int>(e.position.x), static_cast<int>(target.y))) {
        next = {e.position.x, target.y};
    }
    if (!(next == e.position)) {
        e.position = next;
        e.dirty |= dirty::Position;
    }
}

void World::move_towards(Entity& e, Vec2 target, float speed, float dt_ms) {
    // The original EntityStats.MoveTowards: step towards the point, never past it.
    const float step = speed * dt_ms / 1000.0f;
    const float d2 = distance_squared(e.position, target);
    if (d2 <= step * step) {
        move_enemy(e, target);
        return;
    }
    const float a = angle_to(e.position, target);
    move_enemy(e, {e.position.x + std::cos(a) * step, e.position.y + std::sin(a) * step});
}

Entity* World::nearest_named(const Entity& host, const bh::EntityRef& ref, float radius) {
    if (ref.player) return nearest_player(host.position, radius);
    Entity* best = nullptr;
    float best_d = radius * radius;
    for_each_near(host.position, radius, [&](Entity& e) {
        if (e.id == host.id || e.player || (ref.type != 0 && e.object_type != ref.type) || e.hp < 0) return;
        if (ref.type == 0 && !e.enemy) return;  // "any entity": other monsters, not bags or portals
        const float d = distance_squared(e.position, host.position);
        if (d <= best_d) {
            best_d = d;
            best = &e;
        }
    });
    return best;
}

int World::count_named(Vec2 at, std::uint16_t type, float radius) {
    int n = 0;
    for_each_near(at, radius, [&](Entity& e) { n += e.object_type == type ? 1 : 0; });
    for (const auto& queued : spawn_queue_) {
        n += queued.object_type == type && distance_squared(queued.position, at) <= radius * radius ? 1 : 0;
    }
    return n;
}

Entity* World::find_target(const Entity& host, bh::TargetType type, float radius, const bh::EntityRef& target) {
    switch (type) {
        case bh::TargetType::ClosestPlayer: return nearest_player(host.position, radius);
        case bh::TargetType::FixedAngle: return nullptr;
        case bh::TargetType::RandomPlayerPerBehavior:
        case bh::TargetType::RandomPlayerPerCycle: {
            const auto near = players_near(host.position, radius);
            if (near.empty()) return nullptr;
            std::uniform_int_distribution<std::size_t> pick(0, near.size() - 1);
            return slot(near[pick(rng_)]);
        }
        case bh::TargetType::FarthestPlayer: {
            Entity* best = nullptr;
            float best_d = -1.0f;
            for (const EntityId id : players_near(host.position, radius)) {
                Entity* p = slot(id);
                const float d = distance_squared(p->position, host.position);
                if (d > best_d) {
                    best_d = d;
                    best = p;
                }
            }
            return best;
        }
        case bh::TargetType::Entity: return nearest_named(host, target, radius);
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------------------ the state machine

void World::start_behavior(Entity& e) {
    EnemyState& es = *e.enemy;
    if (es.behavior == nullptr || es.behavior->states.empty()) return;
    const int deep = es.behavior->deep_state(0);
    std::vector<int> chain;
    for (int s = deep; s >= 0; s = es.behavior->states[static_cast<std::size_t>(s)].parent) chain.push_back(s);
    es.current_state = deep;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) enter_state(e, *it);
}

void World::enter_state(Entity& e, int index) {
    EnemyState& es = *e.enemy;
    const auto& st = es.behavior->states[static_cast<std::size_t>(index)];
    for (const auto slot_index : st.slots) es.scripts[slot_index] = ScriptState{};
    for (const auto& t : st.transitions) start_transition(e, t);
    for (const auto& s : st.scripts) start_script(e, s);
}

void World::exit_state(Entity& e, int index) {
    EnemyState& es = *e.enemy;
    const auto& st = es.behavior->states[static_cast<std::size_t>(index)];
    for (const auto& s : st.scripts) end_script(e, s);
    for (const auto slot_index : st.slots) es.scripts[slot_index] = ScriptState{};
}

void World::transition_to(Entity& e, int target) {
    EnemyState& es = *e.enemy;
    const auto& states = es.behavior->states;
    auto parent = [&](int s) { return states[static_cast<std::size_t>(s)].parent; };
    auto is_strict_ancestor_of_target = [&](int s) {
        for (int p = parent(target); p >= 0; p = parent(p)) {
            if (p == s) return true;
        }
        return false;
    };
    int s = es.current_state;
    while (s >= 0 && !is_strict_ancestor_of_target(s)) {
        exit_state(e, s);
        s = parent(s);
    }
    const int deep = es.behavior->deep_state(target);
    std::vector<int> chain;
    for (int x = deep; x >= 0 && x != s; x = parent(x)) chain.push_back(x);
    es.current_state = deep;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        enter_state(e, *it);
        if (es.current_state != deep) return;  // an Order in a Start moved it elsewhere already
    }
}

void World::order(Entity& enemy, const std::string& state) {
    if (!enemy.enemy || enemy.enemy->behavior == nullptr || enemy.hp <= 0) return;
    const auto& names = enemy.enemy->behavior->state_by_name;
    auto it = names.find(state);
    if (it == names.end()) return;
    transition_to(enemy, it->second);
}

void World::tick_enemy(Entity& e, float dt_ms) {
    EnemyState& es = *e.enemy;
    if (es.behavior == nullptr || es.current_state < 0 || e.hp <= 0) return;
    const auto& states = es.behavior->states;
    std::array<int, 32> chain{};
    std::size_t depth = 0;
    for (int s = es.current_state; s >= 0 && depth < chain.size(); s = states[static_cast<std::size_t>(s)].parent) chain[depth++] = s;
    for (std::size_t i = depth; i-- > 0;) {
        const auto& st = states[static_cast<std::size_t>(chain[i])];
        for (const auto& t : st.transitions) {
            const int target = tick_transition(e, t, dt_ms);
            if (target < 0) continue;
            ScriptState& ts = es.scripts[t.slot];
            if (ts.fired) continue;
            ts.fired = true;
            transition_to(e, target);
            return;
        }
        for (const auto& s : st.scripts) {
            (void)tick_script(e, s, dt_ms);
            if (e.hp <= 0 || es.current_state < 0) return;
        }
    }
}

void World::run_death_scripts(Entity& e) {
    EnemyState& es = *e.enemy;
    if (es.behavior == nullptr) return;
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);
    for (const auto slot_index : es.armed_on_death) {
        const bh::Script* s = nullptr;
        for (const auto& st : es.behavior->states) {
            if ((s = find_slot(st.scripts, slot_index)) != nullptr) break;
        }
        if (s == nullptr) continue;
        if (const auto* t = std::get_if<bh::TransformOnDeath>(&s->kind)) {
            if (roll(rng_) > t->probability) continue;
            std::uniform_int_distribution<int> count(t->min, std::max(t->min, t->max));
            for (int n = count(rng_); n > 0; --n) spawn(make_object(t->target.type, e.position));
        } else if (const auto* d = std::get_if<bh::DropPortalOnDeath>(&s->kind)) {
            if (roll(rng_) > d->probability) continue;
            std::uniform_real_distribution<float> offset(0.0f, 1.5f);
            Entity portal = make_object(d->portal.type, {e.position.x + offset(rng_), e.position.y + offset(rng_)});
            if (d->timeout_s > 0) {
                // The portal's id exists once it is allocated: allocate now (bags and portals have no behaviour to start).
                const bool was = std::exchange(in_tick_, false);
                const EntityId id = allocate(std::move(portal));
                in_tick_ = was;
                timed_removals_.emplace_back(now_ms_ + d->timeout_s * 1000.0, id);
            } else {
                spawn(std::move(portal));
            }
        } else if (const auto* o = std::get_if<bh::Order>(&s->kind)) {
            for_each_near(e.position, o->range, [&](Entity& other) {
                if (other.id != e.id && other.object_type == o->children.type) order(other, o->target_state);
            });
        }
    }
}

// ------------------------------------------------------------------------------------------------------------ transitions

void World::start_transition(Entity& e, const bh::Transition& t) {
    ScriptState& s = e.enemy->scripts[t.slot];
    switch (t.kind) {
        case bh::TransitionKind::Timed: s.timer = static_cast<float>(t.time_ms); break;
        case bh::TransitionKind::DamageTaken: s.a = static_cast<float>(e.enemy->damage_taken); break;
        case bh::TransitionKind::NotMoving:
            s.a = e.position.x;
            s.b = e.position.y;
            s.timer = static_cast<float>(t.time_ms);
            break;
        default: break;
    }
}

int World::tick_transition(Entity& e, const bh::Transition& t, float dt_ms) {
    ScriptState& s = e.enemy->scripts[t.slot];
    bool fire = false;
    switch (t.kind) {
        case bh::TransitionKind::Timed:
            s.timer -= dt_ms;
            if (s.timer <= 0.0f) {
                s.timer = static_cast<float>(t.time_ms);
                fire = true;
            }
            break;
        case bh::TransitionKind::EntityWithin:
        case bh::TransitionKind::EntityNotWithin: {
            const bool near = nearest_named(e, t.target, t.radius) != nullptr;
            fire = (t.kind == bh::TransitionKind::EntityWithin) == near;
            break;
        }
        case bh::TransitionKind::EntitiesWithin:
        case bh::TransitionKind::EntitiesNotWithin: {
            bool near = false;
            if (t.targets.empty()) {
                for_each_near(e.position, t.radius, [&](Entity& other) { near = near || (other.id != e.id && (other.enemy || other.player)); });
            } else {
                for (const auto& ref : t.targets) near = near || nearest_named(e, ref, t.radius) != nullptr;
            }
            fire = (t.kind == bh::TransitionKind::EntitiesWithin) == near;
            break;
        }
        case bh::TransitionKind::HpLess:
            fire = e.max_hp > 0 && static_cast<float>(e.hp) / static_cast<float>(e.max_hp) < t.threshold;
            break;
        case bh::TransitionKind::EntityHpLess: {
            const Entity* other = nearest_named(e, t.target, t.radius);
            fire = other != nullptr && other->max_hp > 0 && static_cast<float>(other->hp) / static_cast<float>(other->max_hp) <= t.threshold;
            break;
        }
        case bh::TransitionKind::DamageTaken:
            fire = static_cast<float>(e.enemy->damage_taken) - s.a >= static_cast<float>(t.damage);
            break;
        case bh::TransitionKind::NotMoving:
            if (s.timer > 0.0f) {
                s.timer -= dt_ms;
                break;
            }
            if (e.position.x == s.a && e.position.y == s.b) {
                fire = true;
            } else {
                s.a = e.position.x;
                s.b = e.position.y;
                s.timer = static_cast<float>(t.time_ms);
            }
            break;
        case bh::TransitionKind::OnParentDeath: {
            const Entity* p = find(e.enemy->parent);
            fire = p == nullptr || p->hp <= 0;
            break;
        }
        case bh::TransitionKind::PlayerText: break;
    }
    if (!fire) return -1;
    // The original BehaviorTransition.GetTargetState: Random, Random7Bag (each once before repeating) or Sequential.
    const auto n = t.to_state.size();
    if (n == 1) return t.to_state.front();
    auto used = static_cast<std::uint32_t>(s.count);
    if (std::popcount(used) >= static_cast<int>(std::min<std::size_t>(n, 32))) used = 0;
    std::size_t pick = 0;
    if (t.mode == bh::TransitionMode::Random) {
        std::uniform_int_distribution<std::size_t> any(0, n - 1);
        pick = any(rng_);
    } else if (t.mode == bh::TransitionMode::Sequential) {
        pick = static_cast<std::size_t>(std::popcount(used)) % n;
    } else {
        std::vector<std::size_t> free;
        for (std::size_t i = 0; i < n && i < 32; ++i) {
            if ((used & (1u << i)) == 0) free.push_back(i);
        }
        std::uniform_int_distribution<std::size_t> any(0, free.size() - 1);
        pick = free[any(rng_)];
    }
    if (pick < 32) used |= 1u << pick;
    s.count = static_cast<int>(used);
    return t.to_state[pick];
}

// ------------------------------------------------------------------------------------------------------------ scripts

void World::start_script(Entity& e, const bh::Script& script) {
    EnemyState& es = *e.enemy;
    ScriptState& s = es.scripts[script.slot];
    if (is_death_script(script)) {
        if (std::find(es.armed_on_death.begin(), es.armed_on_death.end(), script.slot) == es.armed_on_death.end()) {
            es.armed_on_death.push_back(script.slot);
        }
        return;
    }
    std::visit(
        [&](const auto& k) {
            using T = std::decay_t<decltype(k)>;
            if constexpr (std::is_same_v<T, bh::Wander>) {
                s.timer = static_cast<float>(k.cooldown_ms);
                s.c = e.position.x;  // the original WanderInfo.InitialPos: where the state began
                s.d = e.position.y;
                s.flag = false;
            } else if constexpr (std::is_same_v<T, bh::Follow>) {
                s.timer = static_cast<float>(k.cooldown_offset_ms == 0 ? k.cooldown_ms : k.cooldown_offset_ms);
                s.target = 0;
            } else if constexpr (std::is_same_v<T, bh::Orbit>) {
                std::uniform_real_distribution<float> var(-1.0f, 1.0f);
                s.count = k.clockwise ? 1 : -1;
                s.a = k.speed + k.speed_variance * var(rng_);
                s.b = k.radius + k.radius_variance * var(rng_);
            } else if constexpr (std::is_same_v<T, bh::MoveLine>) {
                s.a = k.distance;
            } else if constexpr (std::is_same_v<T, bh::BackAndForth>) {
                s.a = k.distance;
            } else if constexpr (std::is_same_v<T, bh::Swirl>) {
                s.a = e.position.x;
                s.b = e.position.y;
            } else if constexpr (std::is_same_v<T, bh::Shoot>) {
                s.timer = static_cast<float>(k.cooldown_offset_ms);
                s.a = 0.0f;
            } else if constexpr (std::is_same_v<T, bh::Aoe>) {
                s.timer = static_cast<float>(k.cooldown_offset_ms);
                s.a = 0.0f;
            } else if constexpr (std::is_same_v<T, bh::Spawn>) {
                s.timer = static_cast<float>(k.cooldown_offset_ms);
                s.count = 0;
            } else if constexpr (std::is_same_v<T, bh::TossObject>) {
                s.timer = static_cast<float>(k.cooldown_offset_ms);
            } else if constexpr (std::is_same_v<T, bh::HealSelf>) {
                s.timer = static_cast<float>(k.cooldown_offset_ms);
            } else if constexpr (std::is_same_v<T, bh::Order>) {
                for_each_near(e.position, k.range, [&](Entity& other) {
                    if (other.id != e.id && other.object_type == k.children.type) order(other, k.target_state);
                });
            } else if constexpr (std::is_same_v<T, bh::ConditionEffect>) {
                if (k.duration_ms == 0) remove_condition(e, static_cast<Condition>(k.effect));
                else apply_condition(e, static_cast<Condition>(k.effect), k.duration_ms);
            } else if constexpr (std::is_same_v<T, bh::Taunt>) {
                std::uniform_real_distribution<float> roll(0.0f, 1.0f);
                if (k.cooldown_ms == 0 && !k.texts.empty() && roll(rng_) < k.probability) {
                    std::uniform_int_distribution<std::size_t> pick(0, k.texts.size() - 1);
                    taunts_.push_back({e.id, content_.object(e.object_type)->id, k.texts[pick(rng_)], players_near(e.position, rules_.sight_radius)});
                }
            } else if constexpr (std::is_same_v<T, bh::Suicide>) {
                timed_removals_.emplace_back(now_ms_ + k.delay_ms, e.id);
            } else if constexpr (std::is_same_v<T, bh::Transform>) {
                spawn(make_object(k.target.type, e.position));
                to_remove_.push_back(e.id);
            } else if constexpr (std::is_same_v<T, bh::Timed>) {
                s.timer = static_cast<float>(k.period);
                s.flag = false;
            } else if constexpr (std::is_same_v<T, bh::Duration>) {
                s.timer = static_cast<float>(k.duration);
                for (const auto& inner : k.script) start_script(e, inner);
            } else if constexpr (std::is_same_v<T, bh::Sequence>) {
                s.count = 0;
                for (const auto& inner : k.scripts) start_script(e, inner);
            }
        },
        script.kind);
}

void World::end_script(Entity& e, const bh::Script& script) {
    if (const auto* c = std::get_if<bh::ConditionEffect>(&script.kind); c != nullptr && !c->persist) {
        remove_condition(e, static_cast<Condition>(c->effect));
    }
    const std::vector<bh::Script>* inner = nullptr;
    if (const auto* t = std::get_if<bh::Timed>(&script.kind)) inner = &t->scripts;
    if (const auto* q = std::get_if<bh::Sequence>(&script.kind)) inner = &q->scripts;
    if (const auto* d = std::get_if<bh::Duration>(&script.kind)) inner = &d->script;
    if (inner != nullptr) {
        for (const auto& s : *inner) end_script(e, s);
    }
}

World::ScriptResult World::tick_script(Entity& e, const bh::Script& script, float dt_ms) {
    EnemyState& es = *e.enemy;
    ScriptState& s = es.scripts[script.slot];
    const float dt_s = dt_ms / 1000.0f;
    return std::visit(
        [&](const auto& k) -> ScriptResult {
            using T = std::decay_t<decltype(k)>;
            if constexpr (std::is_same_v<T, bh::Wander>) {
                // Pick a direction that keeps the monster within distanceFromSpawn of where the state began, walk `distance`,
                // then wait cooldownMs (the original Wander, incl. its circle-intersection direction choice).
                if (k.speed <= 0.0f || k.distance <= 0.0f) return ScriptResult::Failed;
                const float move_time = k.distance / k.speed;
                bool first = false;
                if (!s.flag && s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    if (s.timer > 0.0f) return ScriptResult::OnCooldown;
                } else if (!s.flag) {
                    // The original: intersect the circle around where the state began (radius distanceFromSpawn) with the circle
                    // the step can reach (radius distance); pick a whole-degree angle between the two crossings.
                    Vec2 i1;
                    Vec2 i2;
                    const int crossings = circle_intersections({s.c, s.d}, k.distance_from_spawn, e.position, k.distance, i1, i2);
                    float angle_deg = s.a / kDegToRad;
                    if (crossings == 2) {
                        float a1 = angle_to(e.position, i1) / kDegToRad;
                        float a2 = angle_to(e.position, i2) / kDegToRad;
                        if (a2 < 0.0f) a2 += 360.0f;
                        while (a1 < a2) a1 += 360.0f;
                        std::uniform_int_distribution<int> arc(static_cast<int>(a2), std::max(static_cast<int>(a2), static_cast<int>(a1) - 1));
                        angle_deg = static_cast<float>(arc(rng_));
                    } else if (crossings == 0) {
                        std::uniform_int_distribution<int> any(0, 359);
                        angle_deg = static_cast<float>(any(rng_));
                    }
                    s.a = angle_deg * kDegToRad;
                    s.timer = move_time * 1000.0f - dt_ms;
                    s.flag = true;
                    first = true;
                }
                const float fraction = dt_s / move_time;
                move_enemy(e, {e.position.x + std::cos(s.a) * fraction * k.distance, e.position.y + std::sin(s.a) * fraction * k.distance});
                if (first) return ScriptResult::Activate;
                s.timer -= dt_ms;
                if (s.timer <= 0.0f) {
                    s.flag = false;
                    s.timer = static_cast<float>(k.cooldown_ms);
                    return ScriptResult::Deactivate;
                }
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Follow>) {
                // Follow / StayAwayFrom: re-acquire every cooldown, chase (or flee) for followTime.
                if (k.cooldown_ms >= 0) {
                    s.timer -= dt_ms;
                    if (s.timer <= 0.0f) {
                        const Entity* t = find_target(e, k.target_type, k.acquire_range, k.target);
                        s.target = t != nullptr ? t->id.value() : 0;
                        s.timer = static_cast<float>(t != nullptr ? k.follow_time_ms : k.cooldown_ms);
                        if (t == nullptr) return ScriptResult::Deactivate;
                    }
                }
                if (s.target == 0) return ScriptResult::OnCooldown;
                const Entity* t = find(EntityId(s.target));
                if (t == nullptr || (t->player && t->player->dead)) {
                    const Entity* again = find_target(e, k.target_type, k.acquire_range, k.target);
                    s.target = again != nullptr ? again->id.value() : 0;
                    return ScriptResult::Failed;
                }
                const float d2 = distance_squared(e.position, t->position);
                const float limit = k.dist_from_target * k.dist_from_target;
                if (d2 == 0.0f || (k.away ? d2 > limit : d2 < limit)) return ScriptResult::Failed;
                const float a = angle_to(e.position, t->position) + (k.away ? kPi : 0.0f);
                const float step = k.speed * dt_s;
                move_enemy(e, {e.position.x + std::cos(a) * step, e.position.y + std::sin(a) * step});
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Charge>) {
                ScriptResult status = ScriptResult::Active;
                if (s.timer <= 0.0f) {
                    if (s.a == 0.0f && s.b == 0.0f) {
                        const Entity* p = nearest_player(e.position, k.range);
                        if (p == nullptr) return status;
                        if (p->position.x != e.position.x && p->position.y != e.position.y) {
                            const float dx = p->position.x - e.position.x;
                            const float dy = p->position.y - e.position.y;
                            const float d = std::sqrt(dx * dx + dy * dy);
                            s.a = dx / d;
                            s.b = dy / d;
                            s.timer = d / std::max(0.01f, k.speed) * 1000.0f;
                            status = ScriptResult::Activate;
                        }
                    } else {
                        s.a = s.b = 0.0f;
                        s.timer = static_cast<float>(k.cooldown_ms);
                        status = ScriptResult::Deactivate;
                    }
                }
                if (s.a != 0.0f || s.b != 0.0f) {
                    const float d = k.speed * dt_s;
                    move_enemy(e, {e.position.x + s.a * d, e.position.y + s.b * d});
                }
                s.timer -= dt_ms;
                return status;
            } else if constexpr (std::is_same_v<T, bh::Orbit>) {
                const Entity* t = nullptr;
                if (k.target_player) {
                    t = nearest_player(e.position, k.acquire_range);
                } else {
                    t = s.target != 0 ? find(EntityId(s.target)) : nullptr;
                    if (t == nullptr) t = nearest_named(e, k.target, k.acquire_range);
                }
                s.target = t != nullptr ? t->id.value() : 0;
                if (t == nullptr || s.b <= 0.0f) return ScriptResult::Failed;
                float angle;
                if (e.position == t->position) {
                    std::uniform_real_distribution<float> jitter(-1.0f, 1.0f);
                    angle = std::atan2(jitter(rng_), jitter(rng_));
                } else {
                    angle = angle_to(t->position, e.position);
                }
                angle += static_cast<float>(s.count) * s.a / s.b * dt_s;
                const Vec2 goal{t->position.x + std::cos(angle) * s.b, t->position.y + std::sin(angle) * s.b};
                const float dx = goal.x - e.position.x;
                const float dy = goal.y - e.position.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.0f) {
                    const float step = s.a * dt_s;
                    move_enemy(e, {e.position.x + dx / len * step, e.position.y + dy / len * step});
                }
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Protect>) {
                // States: 0 don't know where, 1 protecting (going back), 2 protected (close enough).
                const Entity* p = nearest_named(e, k.protectee, k.acquire_range);
                if (p == nullptr) {
                    s.count = 0;
                    return ScriptResult::Active;
                }
                const float d = std::sqrt(distance_squared(e.position, p->position));
                if (s.count == 2 && d > k.protection_range) s.count = 1;
                if (s.count == 0) s.count = 1;
                if (s.count == 1) {
                    if (d > k.reprotect_range) {
                        const float step = k.speed * dt_s;
                        const float a = angle_to(e.position, p->position);
                        move_enemy(e, {e.position.x + std::cos(a) * step, e.position.y + std::sin(a) * step});
                    } else {
                        s.count = 2;
                    }
                }
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::MoveLine>) {
                // The original moves along the angle forever (its distance is counted but never checked): a distance > 0 stops it.
                if (k.distance > 0.0f && s.a <= 0.0f) return ScriptResult::Deactivate;
                const float step = k.speed * dt_s;
                s.a -= step;
                const float a = k.angle * kDegToRad;
                move_enemy(e, {e.position.x + std::cos(a) * step, e.position.y + std::sin(a) * step});
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::ReturnToSpawn>) {
                const float d = std::sqrt(distance_squared(e.position, es.spawn));
                if (d <= k.distance_from_spawn + k.speed / 50.0f) return ScriptResult::Deactivate;
                move_towards(e, es.spawn, k.speed, dt_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::BackAndForth>) {
                const float step = k.speed * dt_s;
                if (s.a > 0.0f) {
                    move_enemy(e, {e.position.x + step, e.position.y});
                    s.a -= step;
                    if (s.a <= 0.0f) s.a = -k.distance;
                } else {
                    move_enemy(e, {e.position.x - step, e.position.y});
                    s.a += step;
                    if (s.a >= 0.0f) s.a = k.distance;
                }
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Buzz>) {
                if (s.c <= 0.0f) {
                    std::uniform_int_distribution<int> axis(-1, 1);
                    int x = 0;
                    int y = 0;
                    while (x == 0 && y == 0) {
                        x = axis(rng_);
                        y = axis(rng_);
                    }
                    const float len = std::sqrt(static_cast<float>(x * x + y * y));
                    s.a = static_cast<float>(x) / len;
                    s.b = static_cast<float>(y) / len;
                    s.c = k.distance;
                }
                const float step = k.speed * dt_s;
                move_enemy(e, {e.position.x + s.a * step, e.position.y + s.b * step});
                s.c -= step;
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Swirl>) {
                // Circles the point where the state began (the original's targeted variant mixes seconds and milliseconds).
                const Vec2 centre{s.a, s.b};
                if (k.radius <= 0.0f) return ScriptResult::Failed;
                float angle = e.position == centre ? 0.0f : angle_to(centre, e.position);
                angle += k.speed / k.radius * dt_s;
                const Vec2 goal{centre.x + std::cos(angle) * k.radius, centre.y + std::sin(angle) * k.radius};
                move_towards(e, goal, k.speed, dt_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Shoot>) {
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    if (s.timer > 0.0f) return ScriptResult::OnCooldown;
                }
                if (e.has(Condition::Stunned)) return ScriptResult::Failed;
                const content::ProjectileDesc* pd = nullptr;
                if (!k.projectile.empty()) {
                    pd = &k.projectile.front();
                } else if (const auto* desc = content_.object(e.object_type)) {
                    auto it = desc->projectiles.find(k.projectile_index);
                    if (it != desc->projectiles.end()) pd = &it->second;
                }
                if (pd == nullptr) return ScriptResult::Failed;
                float angle = k.fixed_angle * kDegToRad;
                const float fan = k.shoot_angle * kDegToRad;
                if (k.target_type != bh::TargetType::FixedAngle) {
                    const Entity* t = find_target(e, k.target_type, k.max_radius, {"player", 0, true});
                    if (t == nullptr) return ScriptResult::Failed;  // no cooldown: try again next tick
                    if (k.min_radius > 0.0f && distance_squared(t->position, e.position) < k.min_radius * k.min_radius) {
                        return ScriptResult::Failed;
                    }
                    if (k.predictive > 0.0f) {
                        const Vec2 ahead{t->position.x + kPredictTicks * (t->position.x - t->previous.x),
                                         t->position.y + kPredictTicks * (t->position.y - t->previous.y)};
                        angle = angle_to(e.position, ahead);
                    } else {
                        angle = angle_to(e.position, t->position);
                    }
                    angle -= (static_cast<float>(k.count) / 2.0f - 0.5f) * fan;
                }
                angle += k.angle_offset * kDegToRad;
                if (k.rotate_angle != 0.0f) {
                    angle += s.a;
                    s.a += k.rotate_angle * kDegToRad;
                }
                VolleyEvent v;
                v.owner = e.id;
                v.enemy = true;
                v.first_bullet = next_enemy_bullet_;
                next_enemy_bullet_ = static_cast<std::uint16_t>(next_enemy_bullet_ + k.count);
                v.position = {e.position.x + k.x_offset, e.position.y + k.y_offset};
                v.angle = angle;
                v.angle_step = fan;
                v.count = std::max(1, k.count);
                v.path = {pd->path, pd->speed, pd->lifetime_ms, pd->amplitude, pd->frequency};
                v.projectile_type = pd->object_type;
                v.size = pd->size;
                std::uniform_int_distribution<int> roll(pd->min_damage, std::max(pd->min_damage, pd->max_damage));
                fire(v, roll(rng_), pd->multi_hit, pd->passes_cover, pd->armor_piercing, &pd->effects, content_.object(e.object_type)->id);
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Aoe>) {
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    return ScriptResult::OnCooldown;
                }
                float angle = k.fixed_angle * kDegToRad;
                float throw_distance = k.range;
                if (k.target_type != bh::TargetType::FixedAngle) {
                    const Entity* t = find_target(e, k.target_type, k.range, {"player", 0, true});
                    if (t == nullptr) return ScriptResult::Failed;
                    angle = angle_to(e.position, t->position);
                    throw_distance = std::min(k.range, std::sqrt(distance_squared(e.position, t->position)));
                }
                s.a += k.rotate_angle * kDegToRad;
                angle += k.angle_offset * kDegToRad + s.a;
                PendingAoe a;
                a.owner = e.id;
                a.owner_name = content_.object(e.object_type)->id;
                a.at = {e.position.x + std::cos(angle) * throw_distance, e.position.y + std::sin(angle) * throw_distance};
                a.radius = k.radius;
                a.damage = k.damage;
                a.due_ms = now_ms_ + k.throw_time_ms;
                a.activations_left = std::max(1, k.activate_count);
                a.cooldown_ms = k.damage_cooldown_ms;
                a.effects = k.effects.empty() ? nullptr : &k.effects;
                aoes_.push_back(std::move(a));
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Spawn>) {
                if (s.count >= k.max_spawns_per_reset) return ScriptResult::Failed;
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    if (s.timer > 0.0f) return ScriptResult::OnCooldown;
                }
                std::uniform_int_distribution<int> how_many(k.min_spawn_count, std::max(k.min_spawn_count, k.max_spawn_count));
                std::uniform_real_distribution<float> unit(0.0f, 1.0f);
                for (int n = how_many(rng_); n > 0 && s.count < k.max_spawns_per_reset; --n) {
                    std::uint16_t type = k.entity.type;
                    if (!k.group.empty()) {
                        std::uniform_int_distribution<std::size_t> pick(0, k.group.size() - 1);
                        type = k.group[pick(rng_)];
                    }
                    if (k.max_density != 0 && count_named(e.position, type, k.density_radius) >= k.max_density) continue;
                    const Vec2 at{e.position.x + unit(rng_) * (k.max_x - k.min_x) + k.min_x, e.position.y + unit(rng_) * (k.max_y - k.min_y) + k.min_y};
                    Entity child = make_object(type, at);
                    if (child.enemy) child.enemy->parent = e.id;
                    spawn(std::move(child));
                    s.count += 1;
                }
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Reproduce>) {
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    if (s.timer > 0.0f) return ScriptResult::OnCooldown;
                }
                if (k.max_density != 0 && count_named(e.position, k.entity.type, k.density_radius) >= k.max_density) return ScriptResult::Failed;
                Entity child = make_object(k.entity.type, e.position);
                if (child.enemy) child.enemy->parent = e.id;
                spawn(std::move(child));
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::TossObject>) {
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    return ScriptResult::Active;
                }
                std::uniform_real_distribution<float> unit(0.0f, 1.0f);
                if (unit(rng_) > k.probability) {
                    s.timer = static_cast<float>(k.cooldown_ms);
                    return ScriptResult::Deactivate;
                }
                if (k.density_range != 0.0f && k.max_density != 0) {
                    int near = 0;
                    for (const auto& c : k.children) near += count_named(e.position, c.type, k.density_range);
                    if (near >= k.max_density) {
                        s.timer = static_cast<float>(k.cooldown_ms);
                        return ScriptResult::Deactivate;
                    }
                }
                float r = k.range;
                if (k.min_range != 0.0f && k.max_range != 0.0f) r = k.min_range + unit(rng_) * (k.max_range - k.min_range);
                float a = k.angle * kDegToRad;
                if (k.angle == 0.0f && k.min_angle != 0.0f && k.max_angle != 0.0f) {
                    a = (k.min_angle + unit(rng_) * (k.max_angle - k.min_angle)) * kDegToRad;
                }
                Vec2 at{e.position.x + r * std::cos(a), e.position.y + r * std::sin(a)};
                if (k.targeted) {
                    if (const Entity* p = nearest_player(e.position, k.range)) at = p->position;
                }
                if (at.x < 0.0f || at.y < 0.0f || !enemy_can_stand(static_cast<int>(at.x), static_cast<int>(at.y))) return ScriptResult::Failed;
                std::uniform_int_distribution<std::size_t> pick(0, k.children.size() - 1);
                spawn(make_object(k.children[pick(rng_)].type, at));
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::HealSelf>) {
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    return ScriptResult::OnCooldown;
                }
                const int amount = k.percentage ? static_cast<int>(k.amount * e.max_hp / 100.0) : k.amount;
                const int healed = std::min(amount, e.max_hp - e.hp);
                if (healed > 0) {
                    e.hp += healed;
                    e.dirty |= dirty::Hp;
                    notifications_.push_back({e.id, "+" + std::to_string(healed), kHealColor, false, players_near(e.position, rules_.sight_radius)});
                }
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::HealGroup>) {
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    return ScriptResult::Active;
                }
                for_each_near(e.position, k.range, [&](Entity& other) {
                    if (!other.enemy || other.hp <= 0) return;
                    const auto* d = content_.object(other.object_type);
                    if (d == nullptr || d->group != k.group) return;
                    const int target = k.heal_amount != 0 ? std::min(other.max_hp, other.hp + k.heal_amount) : other.max_hp;
                    if (target <= other.hp) return;
                    notifications_.push_back({other.id, "+" + std::to_string(target - other.hp), kHealColor, false,
                                              players_near(other.position, rules_.sight_radius)});
                    other.hp = target;
                    other.dirty |= dirty::Hp;
                });
                s.timer = static_cast<float>(k.cooldown_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Taunt>) {
                if (k.cooldown_ms == 0 || k.texts.empty()) return ScriptResult::Failed;
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    return ScriptResult::OnCooldown;
                }
                s.timer = static_cast<float>(k.cooldown_ms);
                std::uniform_real_distribution<float> roll(0.0f, 1.0f);
                if (roll(rng_) < k.probability) {
                    std::uniform_int_distribution<std::size_t> pick(0, k.texts.size() - 1);
                    taunts_.push_back({e.id, content_.object(e.object_type)->id, k.texts[pick(rng_)], players_near(e.position, rules_.sight_radius)});
                }
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Timed>) {
                // The original Timed: wait `period`, then start the children once and run them every tick.
                if (s.timer > 0.0f) {
                    s.timer -= dt_ms;
                    return ScriptResult::OnCooldown;
                }
                if (!s.flag) {
                    s.flag = true;
                    for (const auto& inner : k.scripts) start_script(e, inner);
                }
                for (const auto& inner : k.scripts) (void)tick_script(e, inner, dt_ms);
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Duration>) {
                if (s.timer <= 0.0f) return ScriptResult::Failed;
                for (const auto& inner : k.script) (void)tick_script(e, inner, dt_ms);
                s.timer -= dt_ms;
                return ScriptResult::Active;
            } else if constexpr (std::is_same_v<T, bh::Sequence>) {
                if (k.scripts.empty()) return ScriptResult::Failed;
                const auto index = static_cast<std::size_t>(s.count) % k.scripts.size();
                const auto status = tick_script(e, k.scripts[index], dt_ms);
                if (status == ScriptResult::Active || status == ScriptResult::Failed) s.count = static_cast<int>((index + 1) % k.scripts.size());
                return ScriptResult::Active;
            } else {
                // Start-only scripts (Order, ConditionEffect, Suicide, Transform), death scripts and presentation no-ops.
                return ScriptResult::OnCooldown;
            }
        },
        script.kind);
}

}  // namespace waw::sim
