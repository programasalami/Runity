#include "waw/sim/world.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace waw::sim {

namespace {

constexpr std::uint32_t kIndexBits = 20;
constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1;
constexpr std::uint16_t kMaxGeneration = 0xFFF;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::uint32_t kDamageColor = 0xFF4040FF;
constexpr std::uint32_t kGoodColor = 0x40FF60FF;
constexpr std::uint32_t kLevelColor = 0x60D0FFFF;

using content::Condition;

std::uint32_t index_of(EntityId id) noexcept { return id.value() & kIndexMask; }
std::uint16_t generation_of(EntityId id) noexcept { return static_cast<std::uint16_t>(id.value() >> kIndexBits); }
EntityId make_id(std::uint32_t index, std::uint16_t generation) noexcept {
    return EntityId((static_cast<std::uint32_t>(generation) << kIndexBits) | index);
}

EntityKind kind_of(const content::ObjectDesc& d) noexcept {
    if (d.player) return EntityKind::Player;
    if (d.enemy) return EntityKind::Enemy;
    if (d.object_class.find("Portal") != std::string::npos) return EntityKind::Portal;
    if (d.object_class == "Container" || d.object_class == "OneWayContainer" || d.object_class == "Storage" ||
        d.object_class == "StorageChest") {
        return EntityKind::Container;
    }
    if (d.object_class == "Character") return EntityKind::Npc;
    return EntityKind::Other;
}

/// Distance from point p to the segment a-b.
float segment_distance(Vec2 a, Vec2 b, Vec2 p) noexcept {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len_sq = dx * dx + dy * dy;
    float t = len_sq > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = a.x + t * dx - p.x;
    const float cy = a.y + t * dy - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

std::string display_name(const content::ContentDb& content, const Entity& e) {
    if (!e.name.empty()) return e.name;
    const auto* d = content.object(e.object_type);
    return d != nullptr ? d->id : "something";
}

}  // namespace

World::World(std::int32_t id, const content::WorldConfig& config, const content::MapData& map, const content::ContentDb& content,
             std::uint64_t seed, WorldRules rules)
    : id_(id),
      config_(config),
      content_(content),
      display_name_(config.display_name),
      map_(TileMap::build(map, content)),
      rules_(rules),
      rng_(seed) {
    grid_w_ = (map_.width() + kChunk - 1) / kChunk;
    grid_h_ = (map_.height() + kChunk - 1) / kChunk;
    grid_start_.assign(static_cast<std::size_t>(grid_w_) * static_cast<std::size_t>(grid_h_) + 1, 0);
    for (const auto& placement : map_.entity_placements()) {
        const EntityId e = add_object(placement.object_type, placement.position);
        if (!placement.name.empty()) find(e)->name = placement.name;
    }
    if (rules_.terrain_spawner) {
        (void)populate_terrain();
        next_repopulate_ms_ = rules_.repopulate_interval_ms;
    }
    rebuild_grid();
}

// ------------------------------------------------------------------------------------------------------------ entities

EntityId World::allocate(Entity entity) {
    std::uint32_t index;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    Slot& s = slots_[index];
    s.generation = static_cast<std::uint16_t>(s.generation >= kMaxGeneration ? 1 : s.generation + 1);
    entity.id = make_id(index, s.generation);
    s.entity = std::move(entity);
    ++live_count_;
    if (s.entity->player) players_.push_back(s.entity->id);
    if (s.entity->container) containers_.push_back(s.entity->id);
    return s.entity->id;
}

Entity* World::slot(EntityId id) {
    const auto index = index_of(id);
    if (index >= slots_.size()) return nullptr;
    Slot& s = slots_[index];
    if (!s.entity || s.generation != generation_of(id)) return nullptr;
    return &*s.entity;
}

Entity* World::find(EntityId id) { return slot(id); }
const Entity* World::find(EntityId id) const { return const_cast<World*>(this)->slot(id); }

std::size_t World::count_of(std::uint16_t object_type) const noexcept {
    std::size_t n = 0;
    for (const auto& s : slots_) n += (s.entity && s.entity->object_type == object_type) ? 1 : 0;
    return n;
}

Entity World::make_object(std::uint16_t object_type, Vec2 position) {
    const auto* desc = content_.object(object_type);
    Entity e;
    e.object_type = object_type;
    e.kind = desc != nullptr ? kind_of(*desc) : EntityKind::Other;
    e.position = e.previous = position;
    e.max_hp = e.hp = desc != nullptr ? desc->max_hp : 0;
    const auto* behavior = content_.behavior(object_type);
    if (behavior != nullptr || (desc != nullptr && desc->enemy)) {
        EnemyState es;
        es.behavior = behavior;
        es.defense = desc != nullptr ? desc->defense : 0;
        es.xp_mult = desc != nullptr ? desc->xp_mult : 1.0f;
        es.spawn = position;
        if (behavior != nullptr) es.scripts.resize(behavior->slot_count);
        e.enemy = std::move(es);
        if (desc != nullptr && desc->enemy) e.kind = EntityKind::Enemy;
    }
    return e;
}

void World::spawn(Entity entity) {
    if (in_tick_) {
        spawn_queue_.push_back(std::move(entity));
        return;
    }
    const EntityId id = allocate(std::move(entity));
    if (Entity* e = slot(id); e != nullptr && e->enemy) start_behavior(*e);
}

EntityId World::add_object(std::uint16_t object_type, Vec2 position) {
    const EntityId id = allocate(make_object(object_type, position));
    if (Entity* e = slot(id); e != nullptr && e->enemy) start_behavior(*e);
    return id;
}

void World::flush_spawns() {
    auto queue = std::exchange(spawn_queue_, {});
    for (auto& e : queue) spawn(std::move(e));
}

void World::remove(EntityId id) {
    Entity* e = slot(id);
    if (e == nullptr) return;
    if (e->player) std::erase(players_, id);
    if (e->container) std::erase(containers_, id);
    const auto index = index_of(id);
    slots_[index].entity.reset();
    free_slots_.push_back(index);
    --live_count_;
}

void World::rebuild_grid() {
    const std::size_t cells = grid_start_.size() - 1;
    std::fill(grid_start_.begin(), grid_start_.end(), 0u);
    grid_scratch_.clear();
    for (const auto& s : slots_) {
        if (!s.entity) continue;
        const int cx = std::clamp(static_cast<int>(s.entity->position.x) / kChunk, 0, grid_w_ - 1);
        const int cy = std::clamp(static_cast<int>(s.entity->position.y) / kChunk, 0, grid_h_ - 1);
        const auto cell = static_cast<std::uint32_t>(cy * grid_w_ + cx);
        grid_scratch_.push_back(cell);
        grid_start_[cell + 1] += 1;
    }
    for (std::size_t c = 0; c < cells; ++c) grid_start_[c + 1] += grid_start_[c];
    grid_ids_.resize(grid_scratch_.size());
    std::vector<std::uint32_t> cursor(grid_start_.begin(), grid_start_.end() - 1);
    std::size_t i = 0;
    for (const auto& s : slots_) {
        if (!s.entity) continue;
        grid_ids_[cursor[grid_scratch_[i++]]++] = s.entity->id;
    }
}

Entity* World::nearest_player(Vec2 from, float radius) {
    Entity* best = nullptr;
    float best_d = radius * radius;
    for (const EntityId id : players_) {
        Entity* p = slot(id);
        if (p == nullptr || p->player->dead) continue;
        const float d = distance_squared(p->position, from);
        if (d <= best_d) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

std::vector<EntityId> World::players_near(Vec2 at, float radius) {
    std::vector<EntityId> out;
    for (const EntityId id : players_) {
        const Entity* p = slot(id);
        if (p != nullptr && distance_squared(p->position, at) <= radius * radius) out.push_back(id);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------ players

void World::refresh_player_stats(Entity& e) {
    PlayerState& p = *e.player;
    if (content_.player_class(e.object_type) != nullptr) {
        p.stats = rules::with_gear(p.base, p.items, content_);
        e.max_hp = p.stats.max_hp;
        p.speed = p.stats.speed;
    }
    e.hp = std::min(e.hp, e.max_hp);
    p.mp = std::min(p.mp, p.stats.max_mp);
    p.stats_dirty = true;
    e.dirty |= dirty::Hp | dirty::MaxHp;
}

EntityId World::add_player(const PlayerSpawn& spawn) {
    Entity e;
    e.object_type = spawn.class_type;
    e.kind = EntityKind::Player;
    e.name = spawn.name;
    e.level = spawn.level;
    e.skin = spawn.skin_type;

    PlayerState p;
    p.session_id = spawn.session_id;
    p.character_id = spawn.character_id;
    p.xp = spawn.xp;
    p.fame = spawn.fame;
    p.items = spawn.items;
    p.items.resize(rules::kPlayerSlots, -1);
    p.account_id = spawn.account_id;
    p.has_backpack = spawn.has_backpack;
    if (const auto* cls = content_.player_class(spawn.class_type)) {
        const bool unset = std::all_of(spawn.stats.begin(), spawn.stats.end(), [](int v) { return v == 0; });
        p.base = unset ? rules::starting_stats(*cls) : spawn.stats;
        p.stats = rules::with_gear(p.base, p.items, content_);
        e.max_hp = p.stats.max_hp;
        e.hp = spawn.hp <= 0 ? e.max_hp : std::min(spawn.hp, e.max_hp);
        p.speed = p.stats.speed;
        p.mp = spawn.mp < 0 ? p.stats.max_mp : std::min(spawn.mp, p.stats.max_mp);
    } else {
        p.stats.max_hp = spawn.max_hp;
        p.stats.speed = spawn.speed;
        e.max_hp = spawn.max_hp;
        e.hp = spawn.hp;
        p.speed = spawn.speed;
        p.mp = std::max(0, spawn.mp);
    }

    if (spawn.position) {
        e.position = *spawn.position;
    } else if (const auto& spawns = map_.region("Spawn"); !spawns.empty()) {
        std::uniform_int_distribution<std::size_t> pick(0, spawns.size() - 1);
        const auto& t = spawns[pick(rng_)];
        e.position = {t.x + 0.5f, t.y + 0.5f};
    } else {
        e.position = {map_.width() / 2.0f, map_.height() / 2.0f};
    }
    e.previous = e.position;
    p.mover.position = e.position;
    p.sent_tiles.assign(std::size_t{map_.width()} * map_.height(), false);
    e.player = std::move(p);
    return allocate(std::move(e));
}

World::InputResult World::queue_input(EntityId player, std::span<const MoveCommand> steps) {
    Entity* e = slot(player);
    if (e == nullptr || !e->player) return InputResult::Accepted;
    PlayerState& p = *e->player;
    for (const auto& step : steps) {
        const std::uint32_t last = p.pending.empty() ? p.last_applied_seq : p.pending.back().seq;
        const bool first_ever = !p.any_applied && p.pending.empty();
        if (!first_ever && step.seq <= last) return InputResult::OutOfOrder;
        p.pending.push_back(step);
    }
    return p.pending.size() > rules_.max_pending_steps ? InputResult::Flooding : InputResult::Accepted;
}

World::ShootResult World::shoot(EntityId player, std::uint16_t shot_id, float angle) {
    Entity* e = slot(player);
    if (e == nullptr || !e->player || e->player->dead) return ShootResult::Dead;
    PlayerState& p = *e->player;
    if (p.items.empty() || p.items[0] < 0) return ShootResult::NoWeapon;
    const auto* weapon = content_.item(static_cast<std::uint16_t>(p.items[0]));
    if (weapon == nullptr || !weapon->projectile) return ShootResult::NoWeapon;
    if (!std::isfinite(angle)) return ShootResult::NoWeapon;
    if (!p.fire.try_attack(now_ms_, rules::attack_period_ms(p.stats.dexterity, weapon->rate_of_fire))) return ShootResult::TooFast;

    const auto& pd = *weapon->projectile;
    const int count = std::max(1, weapon->num_projectiles);
    const float step = weapon->arc_gap_degrees * kPi / 180.0f;
    VolleyEvent v;
    v.owner = e->id;
    v.enemy = false;
    v.first_bullet = static_cast<std::uint16_t>(shot_id * count);
    v.position = e->position;
    v.angle = angle - step * static_cast<float>(count - 1) / 2.0f;
    v.angle_step = step;
    v.count = count;
    v.path = {pd.path, pd.speed, pd.lifetime_ms, pd.amplitude, pd.frequency};
    v.projectile_type = pd.object_type;
    v.size = pd.size;
    volleys_.push_back(v);
    std::uniform_int_distribution<int> roll(pd.min_damage, pd.max_damage);  // max included (the original's Next(min, max) never rolled it)
    for (int i = 0; i < count; ++i) {
        Projectile b;
        b.owner = e->id;
        b.bullet_id = static_cast<std::uint16_t>(v.first_bullet + i);
        b.start = v.position;
        b.angle = v.angle + v.angle_step * static_cast<float>(i);
        b.path = v.path;
        b.start_ms = now_ms_;
        b.damage = rules::damage_with_attack(roll(rng_), p.stats.attack, e->has(Condition::Weak), e->has(Condition::Damaging));
        b.multi_hit = pd.multi_hit;
        b.passes_cover = pd.passes_cover;
        b.armor_piercing = pd.armor_piercing;
        b.owner_name = e->name;
        projectiles_.push_back(std::move(b));
    }
    return ShootResult::Fired;
}

void World::fire(const VolleyEvent& v, int damage, bool multi_hit, bool passes_cover, bool armor_piercing,
                 const std::vector<content::EffectSpec>* effects, const std::string& owner_name) {
    volleys_.push_back(v);
    for (int i = 0; i < v.count; ++i) {
        Projectile b;
        b.owner = v.owner;
        b.enemy = v.enemy;
        b.bullet_id = static_cast<std::uint16_t>(v.first_bullet + i);
        b.start = v.position;
        b.angle = v.angle + v.angle_step * static_cast<float>(i);
        b.path = v.path;
        b.start_ms = now_ms_;
        b.damage = damage;
        b.multi_hit = multi_hit;
        b.passes_cover = passes_cover;
        b.armor_piercing = armor_piercing;
        b.effects = effects != nullptr && !effects->empty() ? effects : nullptr;
        b.owner_name = owner_name;
        projectiles_.push_back(std::move(b));
    }
}

void World::apply_inputs(Entity& e, float dt_ms) {
    PlayerState& p = *e.player;
    // Speed-hack guard: a client can only spend input time it has received in real time (plus slack for lag bursts).
    p.time_budget_ms = std::min(p.time_budget_ms + dt_ms, rules_.max_time_budget_ms);
    const Vec2 before = e.position;
    while (!p.pending.empty()) {
        const MoveCommand& c = p.pending.front();
        const float step_ms = std::min(static_cast<float>(c.dt_ms), movement::kMaxStepMs);
        if (step_ms > p.time_budget_ms + rules_.movement_slack_ms) break;  // wait for real time to catch up
        p.time_budget_ms -= step_ms;
        movement::step(map_, p.mover, movement::direction_from_input(c.dir_x, c.dir_y), step_ms, p.speed);
        p.last_applied_seq = c.seq;
        p.any_applied = true;
        p.pending.pop_front();
    }
    e.position = p.mover.position;
    if (!(e.position == before)) e.dirty |= dirty::Position;
}

void World::regenerate(Entity& e, float dt_ms) {
    PlayerState& p = *e.player;
    if (p.dead) return;
    if (e.hp < e.max_hp && !e.has(Condition::Sick)) {
        p.hp_regen_carry += rules::hp_regen_per_second(p.stats.vitality) * dt_ms / 1000.0f;
        const int whole = static_cast<int>(p.hp_regen_carry);
        if (whole > 0) {
            e.hp = std::min(e.max_hp, e.hp + whole);
            p.hp_regen_carry -= static_cast<float>(whole);
            e.dirty |= dirty::Hp;
            p.stats_dirty = true;
        }
    } else {
        p.hp_regen_carry = 0.0f;
    }
    if (p.mp < p.stats.max_mp && !e.has(Condition::Quiet)) {
        p.mp_regen_carry += rules::mp_regen_per_second(p.stats.wisdom) * dt_ms / 1000.0f;
        const int whole = static_cast<int>(p.mp_regen_carry);
        if (whole > 0) {
            p.mp = std::min(p.stats.max_mp, p.mp + whole);
            p.mp_regen_carry -= static_cast<float>(whole);
            p.stats_dirty = true;
        }
    } else {
        p.mp_regen_carry = 0.0f;
    }
}

// ------------------------------------------------------------------------------------------------------------ conditions

void World::apply_condition(Entity& e, Condition c, int duration_ms) {
    if (c == Condition::Nothing || static_cast<int>(c) >= 64) return;
    e.conditions |= content::condition_bit(c);
    e.dirty |= dirty::Conditions;
    const auto key = static_cast<std::uint8_t>(c);
    std::erase_if(e.condition_ends, [key](const auto& t) { return t.first == key; });
    if (duration_ms >= 0) e.condition_ends.emplace_back(key, now_ms_ + duration_ms);
}

void World::remove_condition(Entity& e, Condition c) {
    if (static_cast<int>(c) >= 64) return;
    const auto key = static_cast<std::uint8_t>(c);
    std::erase_if(e.condition_ends, [key](const auto& t) { return t.first == key; });
    if (!e.has(c)) return;
    e.conditions &= ~content::condition_bit(c);
    e.dirty |= dirty::Conditions;
}

void World::tick_conditions(Entity& e) {
    if (e.condition_ends.empty()) return;
    for (std::size_t i = 0; i < e.condition_ends.size();) {
        if (e.condition_ends[i].second <= now_ms_) {
            e.conditions &= ~(std::uint64_t{1} << e.condition_ends[i].first);
            e.dirty |= dirty::Conditions;
            e.condition_ends.erase(e.condition_ends.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// ------------------------------------------------------------------------------------------------------------ combat

bool World::bullet_stopped_by(int x, int y, bool passes_cover) const {
    if (!map_.in_bounds(x, y)) return true;
    const auto flags = map_.at(x, y).flags;
    if ((flags & (tile_flags::Void | tile_flags::EnemyOccupySquare)) != 0) return true;
    return !passes_cover && (flags & tile_flags::OccupySquare) != 0;
}

void World::damage(Entity& target, int raw_damage, bool armor_piercing, EntityId by, const std::string& by_name) {
    if (target.hp <= 0) return;
    if (target.player && target.player->dead) return;
    if (target.has(Condition::Invulnerable) || target.has(Condition::Invincible) || target.has(Condition::Stasis)) return;
    const int defense = target.player ? target.player->stats.defense : target.enemy ? target.enemy->defense : 0;
    const bool pierce = armor_piercing || target.has(Condition::ArmorBroken);
    const int amount = rules::after_defense(raw_damage, defense, pierce, target.has(Condition::Armored));
    if (amount <= 0) return;
    target.hp -= amount;
    target.dirty |= dirty::Hp;
    std::vector<EntityId> to;
    if (target.player) {
        target.player->stats_dirty = true;
        to.push_back(target.id);
    }
    if (const Entity* attacker = find(by); attacker != nullptr && attacker->player) to.push_back(by);
    notifications_.push_back({target.id, "-" + std::to_string(amount), kDamageColor, true, std::move(to)});
    if (target.enemy) {
        target.enemy->damage_taken += amount;
        auto& records = target.enemy->damage_by;
        auto it = std::find_if(records.begin(), records.end(), [by](const auto& r) { return r.first == by; });
        if (it == records.end()) records.emplace_back(by, amount);
        else it->second += amount;
    }

    if (target.hp > 0) return;
    if (target.enemy) {
        kill_enemy(target);
    } else if (target.player) {
        target.hp = 0;
        PlayerState& p = *target.player;
        p.dead = true;
        p.killed_by = by_name;
        deaths_.push_back({target.id, p.session_id, by_name, target.level, p.fame});
        to_remove_.push_back(target.id);
    } else {
        to_remove_.push_back(target.id);
    }
}

void World::tick_projectiles(float dt_ms) {
    constexpr int kSubSteps = 4;
    const double previous_ms = now_ms_ - dt_ms;
    for (auto& b : projectiles_) {
        if (b.done) continue;
        const float life = static_cast<float>(b.path.lifetime_ms);
        const float from_t = std::clamp(static_cast<float>(std::max(previous_ms, b.start_ms) - b.start_ms), 0.0f, life);
        const float to_t = std::clamp(static_cast<float>(now_ms_ - b.start_ms), 0.0f, life);
        if (from_t >= life) {
            b.done = true;
            continue;
        }
        auto at = [&](float t) {
            const Vec2 o = path_offset(b.path, t, b.bullet_id, b.angle);
            return Vec2{b.start.x + o.x, b.start.y + o.y};
        };
        Vec2 last = at(from_t);
        for (int k = 1; k <= kSubSteps && !b.done; ++k) {
            const float t = from_t + (to_t - from_t) * static_cast<float>(k) / kSubSteps;
            const Vec2 pos = at(t);
            const float reach = rules_.hit_radius + std::sqrt(distance_squared(last, pos));
            Entity* victim = nullptr;
            for_each_near(pos, reach, [&](Entity& target) {
                if (victim != nullptr) return;
                const bool hittable = b.enemy ? (target.player && !target.player->dead) : (target.enemy && target.hp > 0 && target.kind == EntityKind::Enemy);
                if (!hittable) return;
                if (std::find(b.hit.begin(), b.hit.end(), target.id) != b.hit.end()) return;
                if (segment_distance(last, pos, target.position) > rules_.hit_radius) return;
                victim = &target;
            });
            while (victim != nullptr) {
                b.hit.push_back(victim->id);
                if (b.effects != nullptr && victim->player) {
                    for (const auto& eff : *b.effects) apply_condition(*victim, static_cast<Condition>(eff.effect), eff.duration_ms);
                }
                damage(*victim, b.damage, b.armor_piercing, b.owner, b.owner_name);
                if (!b.multi_hit) {
                    b.done = true;
                    break;
                }
                victim = nullptr;  // a multi-hit bullet looks for the next one on the same stretch
                for_each_near(pos, reach, [&](Entity& target) {
                    if (victim != nullptr) return;
                    const bool hittable = b.enemy ? (target.player && !target.player->dead) : (target.enemy && target.hp > 0 && target.kind == EntityKind::Enemy);
                    if (!hittable || std::find(b.hit.begin(), b.hit.end(), target.id) != b.hit.end()) return;
                    if (segment_distance(last, pos, target.position) <= rules_.hit_radius) victim = &target;
                });
            }
            if (!b.done && pos.x >= 0.0f && pos.y >= 0.0f && bullet_stopped_by(static_cast<int>(pos.x), static_cast<int>(pos.y), b.passes_cover)) {
                b.done = true;
            } else if (pos.x < 0.0f || pos.y < 0.0f) {
                b.done = true;
            }
            last = pos;
        }
        if (to_t >= life) b.done = true;
    }
    std::erase_if(projectiles_, [](const Projectile& b) { return b.done; });
}

void World::tick_aoes() {
    for (auto& a : aoes_) {
        if (a.due_ms > now_ms_ || a.activations_left <= 0) continue;
        for (const EntityId id : players_near(a.at, a.radius)) {
            Entity* p = slot(id);
            if (p == nullptr || p->player->dead) continue;
            if (a.effects != nullptr) {
                for (const auto& eff : *a.effects) apply_condition(*p, static_cast<Condition>(eff.effect), eff.duration_ms);
            }
            damage(*p, a.damage, false, a.owner, a.owner_name);
        }
        a.activations_left -= 1;
        a.due_ms = now_ms_ + a.cooldown_ms;
    }
    std::erase_if(aoes_, [](const PendingAoe& a) { return a.activations_left <= 0; });
}

// ------------------------------------------------------------------------------------------------------------ kills and loot

void World::kill_enemy(Entity& e) {
    EnemyState& es = *e.enemy;
    run_death_scripts(e);
    roll_loot(e);
    // XP: every player near the monster, not only its damagers (LEGACY CharacterEntity.HandleXpGain).
    const int base_xp = rules::xp_for_kill(e.max_hp, es.xp_mult);
    if (base_xp > 0) {
        const float share = rules::xp_share_distance_sq(rules_.sight_radius);
        for (const EntityId id : players_) {
            Entity* player = slot(id);
            if (player == nullptr || player->player->dead || distance_squared(player->position, e.position) >= share) continue;
            award_xp(*player, rules::capped_xp(base_xp, player->level));
        }
    }
    to_remove_.push_back(e.id);
}

void World::roll_loot(const Entity& enemy) {
    const auto* behavior = enemy.enemy->behavior;
    if (behavior == nullptr || behavior->loot.empty() || enemy.max_hp <= 0) return;
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);
    // The original LootDrop.HandleLoot: every damage record rolls every entry; public tables pool all rolls into one set of bags,
    // the others give each account its own soulbound bags.
    for (const auto& table : behavior->loot) {
        std::map<std::int64_t, std::vector<int>> drops;  // account (0 = public) -> items
        for (const auto& [attacker, dealt] : enemy.enemy->damage_by) {
            const Entity* p = find(attacker);
            if (p == nullptr || !p->player) continue;
            const std::int64_t key = table.is_public ? 0 : p->player->account_id;
            if (!table.is_public && key == 0) continue;
            auto& list = drops[key];
            const float share = static_cast<float>(dealt) / static_cast<float>(enemy.max_hp);
            for (const auto& item : table.items) {
                if (share < item.threshold || roll(rng_) > item.chance) continue;
                if (!item.tier) {
                    list.push_back(item.item_type);
                    continue;
                }
                const auto& pool = content_.tier_items(item.tier_class, item.tier_level);
                if (pool.empty()) continue;
                std::uniform_int_distribution<std::size_t> pick(0, pool.size() - 1);
                list.push_back(pool[pick(rng_)]);
            }
        }
        for (const auto& [account, items] : drops) {
            constexpr std::size_t kBagSlots = 8;
            std::uniform_real_distribution<float> offset(0.0f, 1.5f);
            for (std::size_t start = 0; start < items.size(); start += kBagSlots) {
                const std::vector<int> chunk(items.begin() + static_cast<std::ptrdiff_t>(start),
                                             items.begin() + static_cast<std::ptrdiff_t>(std::min(items.size(), start + kBagSlots)));
                drop_bags({enemy.position.x + offset(rng_), enemy.position.y + offset(rng_)}, account, chunk);
            }
        }
    }
}

void World::drop_bags(Vec2 at, std::int64_t owner_account, const std::vector<int>& items) {
    constexpr std::size_t kBagSlots = 8;
    std::vector<int> pending;
    for (const int it : items) {
        if (it >= 0) pending.push_back(it);
    }
    std::uniform_real_distribution<float> spread(-0.75f, 0.75f);
    for (std::size_t start = 0; start < pending.size(); start += kBagSlots) {
        ContainerState c;
        c.owner_account = owner_account;
        c.expires_ms = now_ms_ + rules_.loot_bag_lifetime_ms;
        int bag_type = 0;
        for (std::size_t i = start; i < std::min(pending.size(), start + kBagSlots); ++i) {
            c.items.push_back(pending[i]);
            if (const auto* d = content_.item(static_cast<std::uint16_t>(pending[i]))) bag_type = std::max(bag_type, d->bag_type);
        }
        c.items.resize(kBagSlots, -1);
        Entity e;
        e.object_type = content_.loot_bag_type(bag_type);
        e.kind = EntityKind::Container;
        const bool first = start == 0;
        Vec2 pos{at.x + (first ? 0.0f : spread(rng_)), at.y + (first ? 0.0f : spread(rng_))};
        if (pos.x < 0.0f || pos.y < 0.0f || !map_.in_bounds(static_cast<int>(pos.x), static_cast<int>(pos.y))) pos = at;
        e.position = e.previous = pos;
        e.container = std::move(c);
        spawn(std::move(e));
    }
}

bool World::can_see(const PlayerState& viewer, const Entity& e) const {
    return !e.container || e.container->owner_account == 0 || e.container->owner_account == viewer.account_id;
}

void World::tick_containers() {
    for (const EntityId id : containers_) {
        const Entity* e = slot(id);
        if (e == nullptr) continue;
        const auto& c = *e->container;
        const bool empty = std::all_of(c.items.begin(), c.items.end(), [](int i) { return i < 0; });
        if (empty || now_ms_ >= c.expires_ms) to_remove_.push_back(id);
    }
}

// ------------------------------------------------------------------------------------------------------------ items

World::ItemResult World::swap_items(EntityId player, EntityId from, int from_slot, EntityId to, int to_slot) {
    constexpr float kReach = 1.5f;  // the original allowed 3 tiles without any prediction slack; bags are opened standing on them
    Entity* me = slot(player);
    if (me == nullptr || !me->player || me->player->dead) return ItemResult::NotAllowed;
    struct Side {
        std::vector<int>* items = nullptr;
        bool is_player = false;
    };
    auto side = [&](EntityId id, int slot_index, Side& out) -> ItemResult {
        if (id == player) {
            if (slot_index < 0 || slot_index >= rules::kPlayerSlots) return ItemResult::NotAllowed;
            if (slot_index >= rules::kBackpackSlot && !me->player->has_backpack) return ItemResult::NotAllowed;
            out = {&me->player->items, true};
            return ItemResult::Done;
        }
        Entity* other = slot(id);
        if (other == nullptr || !other->container) return ItemResult::NotAllowed;
        if (!can_see(*me->player, *other)) return ItemResult::NotAllowed;
        if (distance_squared(other->position, me->position) > kReach * kReach) return ItemResult::TooFar;
        if (slot_index < 0 || slot_index >= static_cast<int>(other->container->items.size())) return ItemResult::NotAllowed;
        out = {&other->container->items, false};
        return ItemResult::Done;
    };
    Side a;
    Side b;
    if (auto r = side(from, from_slot, a); r != ItemResult::Done) return r;
    if (auto r = side(to, to_slot, b); r != ItemResult::Done) return r;
    int& item_a = (*a.items)[static_cast<std::size_t>(from_slot)];
    int& item_b = (*b.items)[static_cast<std::size_t>(to_slot)];
    if (item_a < 0) return ItemResult::NoItem;
    const auto* cls = content_.player_class(me->object_type);
    auto fits = [&](const Side& s, int slot_index, int item) {
        if (item < 0 || !s.is_player) return true;  // anything may go into a bag (reference CanPutNormalObjects)
        const auto* d = content_.item(static_cast<std::uint16_t>(item));
        return d != nullptr && cls != nullptr && rules::slot_fits(slot_index, cls->slot_types, d->slot_type);
    };
    if (!fits(b, to_slot, item_a) || !fits(a, from_slot, item_b)) return ItemResult::NotAllowed;
    std::swap(item_a, item_b);
    if (a.is_player || b.is_player) {
        me->player->inventory_dirty = true;
        refresh_player_stats(*me);  // gear may have changed
    }
    for (EntityId id : {from, to}) {
        if (Entity* e = slot(id); e != nullptr && e->container) e->dirty |= dirty::Items;
    }
    return ItemResult::Done;
}

World::ItemResult World::use_item(EntityId player, int slot_index) {
    Entity* me = slot(player);
    if (me == nullptr || !me->player || me->player->dead) return ItemResult::NotAllowed;
    PlayerState& p = *me->player;
    if (slot_index < 0 || slot_index >= rules::kPlayerSlots) return ItemResult::NotAllowed;
    int& item = p.items[static_cast<std::size_t>(slot_index)];
    if (item < 0) return ItemResult::NoItem;
    const auto* d = content_.item(static_cast<std::uint16_t>(item));
    if (d == nullptr) return ItemResult::NotAllowed;
    const bool raises = std::any_of(d->stat_increments.begin(), d->stat_increments.end(), [](int v) { return v != 0; });
    if (d->heal_amount <= 0 && d->magic_amount <= 0 && !raises) return ItemResult::NotAllowed;
    if (raises) {
        // A stat potion: +amount, never above the class maximum; a potion that cannot raise anything is kept.
        const auto* cls = content_.player_class(me->object_type);
        if (cls == nullptr) return ItemResult::NotAllowed;
        bool changed = false;
        for (std::size_t i = 0; i < content::kStatCount; ++i) {
            if (d->stat_increments[i] == 0) continue;
            const int raised = std::min(p.base[i] + d->stat_increments[i], cls->stats[i].max);
            changed = changed || raised != p.base[i];
            p.base[i] = raised;
        }
        if (!changed) return ItemResult::Maxed;
        refresh_player_stats(*me);
    }
    if (d->heal_amount > 0) {
        me->hp = std::min(me->max_hp, me->hp + d->heal_amount);
        me->dirty |= dirty::Hp;
        notifications_.push_back({me->id, "+" + std::to_string(d->heal_amount), kGoodColor, false, {me->id}});
    }
    if (d->magic_amount > 0) p.mp = std::min(p.stats.max_mp, p.mp + d->magic_amount);
    if (d->consumable) {
        item = -1;
        p.inventory_dirty = true;
    }
    p.stats_dirty = true;
    return ItemResult::Done;
}

World::ItemResult World::drop_item(EntityId player, int slot_index) {
    Entity* me = slot(player);
    if (me == nullptr || !me->player || me->player->dead) return ItemResult::NotAllowed;
    PlayerState& p = *me->player;
    if (slot_index < 0 || slot_index >= rules::kPlayerSlots) return ItemResult::NotAllowed;
    const int item = p.items[static_cast<std::size_t>(slot_index)];
    if (item < 0) return ItemResult::NoItem;
    const auto* d = content_.item(static_cast<std::uint16_t>(item));
    const bool soulbound = d != nullptr && d->soulbound;
    p.items[static_cast<std::size_t>(slot_index)] = -1;
    p.inventory_dirty = true;
    refresh_player_stats(*me);
    // At the player's feet (the original InvDrop created the bag without ever moving it there).
    drop_bags(me->position, soulbound ? p.account_id : 0, {item});
    return ItemResult::Done;
}

// ------------------------------------------------------------------------------------------------------------ progression

void World::award_xp(Entity& e, int xp) {
    PlayerState& p = *e.player;
    if (xp <= 0) return;
    p.xp += xp;
    p.fame_xp_carry += xp;
    while (p.fame_xp_carry >= rules::kXpPerFame) {
        p.fame += 1;
        p.fame_xp_carry -= rules::kXpPerFame;
    }
    notifications_.push_back({e.id, "+" + std::to_string(xp) + " XP", kGoodColor, false, {e.id}});
    const auto* cls = content_.player_class(e.object_type);
    bool levelled = false;
    while (e.level < rules::kMaxLevel && p.xp >= rules::xp_to_next_level(e.level)) {
        p.xp -= rules::xp_to_next_level(e.level);
        e.level += 1;
        if (cls != nullptr) rules::level_up(p.base, *cls, rng_);
        levelled = true;
    }
    if (levelled) {
        refresh_player_stats(e);
        e.hp = e.max_hp;  // a level-up heals fully (LEGACY Player.GainXP)
        p.mp = p.stats.max_mp;
        e.dirty |= dirty::Level | dirty::Hp;
        notifications_.push_back({e.id, "Level Up!", kLevelColor, false, {e.id}});
    }
    p.stats_dirty = true;
}

// ------------------------------------------------------------------------------------------------------------ realm spawner

int World::spawn_terrain_group(const content::TerrainSpawn& sp, content::Terrain terrain) {
    const auto& tiles = map_.terrain(terrain);
    if (tiles.empty()) return 0;
    int num = 1;
    if (const auto* desc = content_.object(sp.object_type); desc != nullptr && desc->spawn) {
        // LEGACY Oryx.SpawnTerrainEnemy: (int)normal(mean, stddev), clamped to [min, max].
        int rolled = sp.group.mean;
        if (sp.group.std_dev > 0) {
            std::normal_distribution<double> normal(sp.group.mean, sp.group.std_dev);
            rolled = static_cast<int>(normal(rng_));
        }
        num = std::clamp(rolled, std::max(1, sp.group.min), std::max({1, sp.group.min, sp.group.max}));
    }
    std::uniform_int_distribution<std::size_t> pick(0, tiles.size() - 1);
    std::uniform_real_distribution<float> scatter(-5.0f, 5.0f);
    const auto tile = tiles[pick(rng_)];
    for (int k = 0; k < num; ++k) {
        Vec2 pos{tile.x + 0.5f + scatter(rng_), tile.y + 0.5f + scatter(rng_)};
        if (pos.x < 0.0f || pos.y < 0.0f || !enemy_can_stand(static_cast<int>(pos.x), static_cast<int>(pos.y))) pos = {tile.x + 0.5f, tile.y + 0.5f};
        Entity e = make_object(sp.object_type, pos);
        if (e.enemy) e.enemy->terrain = terrain;
        spawn(std::move(e));
    }
    return num;
}

int World::populate_terrain() {
    // LEGACY Oryx.Populate / Repopulate: per terrain, at most 1.5 % of its tiles hold monsters; every monster of that terrain is
    // rolled against its <SpawnProb> in turn until the count is reached.
    std::array<int, content::kTerrainCount> alive{};
    for (const auto& s : slots_) {
        if (s.entity && s.entity->enemy && s.entity->hp > 0) alive[static_cast<std::size_t>(s.entity->enemy->terrain)] += 1;
    }
    for (const auto& e : spawn_queue_) {
        if (e.enemy) alive[static_cast<std::size_t>(e.enemy->terrain)] += 1;
    }
    std::uniform_real_distribution<double> roll(0.0, 1.0);
    int placed = 0;
    for (std::size_t t = 1; t <= static_cast<std::size_t>(content::Terrain::ShorePlains); ++t) {
        const auto terrain = static_cast<content::Terrain>(t);
        terrain_max_[t] = static_cast<int>(static_cast<float>(map_.terrain(terrain).size()) * rules_.terrain_density);
        const auto& spawns = content_.terrain_spawns(terrain);
        const bool any = std::any_of(spawns.begin(), spawns.end(), [](const auto& s) { return s.probability > 0.0f; });
        if (!any) continue;
        for (int round = 0; alive[t] < terrain_max_[t] && round < 1000000; ++round) {
            for (const auto& sp : spawns) {
                if (roll(rng_) > sp.probability) continue;
                const int n = spawn_terrain_group(sp, terrain);
                alive[t] += n;
                placed += n;
            }
        }
    }
    return placed;
}

void World::tick_terrain_spawner() {
    if (!rules_.terrain_spawner || now_ms_ < next_repopulate_ms_) return;
    next_repopulate_ms_ = now_ms_ + rules_.repopulate_interval_ms;
    (void)populate_terrain();
}

// ------------------------------------------------------------------------------------------------------------ tick and replication

void World::replicate(Entity& viewer, ViewUpdate& out) {
    PlayerState& p = *viewer.player;
    out.viewer = viewer.id;
    out.session_id = p.session_id;
    out.server_tick = tick_;
    out.ack_input_seq = p.last_applied_seq;

    // Tiles: only the ones near the player that it has not been sent yet (a realm has 4 million).
    const float r = rules_.sight_radius;
    const int r_tiles = static_cast<int>(std::ceil(r));
    const int cx = static_cast<int>(viewer.position.x);
    const int cy = static_cast<int>(viewer.position.y);
    for (int y = cy - r_tiles; y <= cy + r_tiles; ++y) {
        for (int x = cx - r_tiles; x <= cx + r_tiles; ++x) {
            if (!map_.in_bounds(x, y)) continue;
            const float dx = static_cast<float>(x - cx);
            const float dy = static_cast<float>(y - cy);
            if (dx * dx + dy * dy > r * r) continue;
            const std::size_t i = static_cast<std::size_t>(y) * map_.width() + static_cast<std::size_t>(x);
            if (p.sent_tiles[i]) continue;
            p.sent_tiles[i] = true;
            const Tile& t = map_.at(x, y);
            out.tiles.push_back({static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y), t.ground, t.object});
        }
    }

    std::vector<EntityId> visible;
    for_each_near(viewer.position, r, [&](Entity& e) {
        if (can_see(p, e)) visible.push_back(e.id);  // an owned loot bag is only sent to its owner
    });
    std::sort(visible.begin(), visible.end());
    for (const auto id : visible) {
        Entity& e = *slot(id);
        if (std::binary_search(p.known.begin(), p.known.end(), id)) {
            if (e.dirty != 0) out.changed.push_back({&e, e.dirty});
        } else {
            out.entered.push_back(&e);
        }
    }
    for (const auto id : p.known) {
        if (!std::binary_search(visible.begin(), visible.end(), id)) out.left.push_back(id);
    }
    p.known = std::move(visible);
    if (p.stats_dirty) out.stats = &p;
    if (p.inventory_dirty) out.inventory = &p.items;
}

void World::tick(float dt_ms, std::vector<ViewUpdate>& out) {
    ++tick_;
    now_ms_ += dt_ms;
    rebuild_grid();
    in_tick_ = true;
    for (const EntityId id : players_) {
        Entity* e = slot(id);
        if (e == nullptr) continue;
        e->previous = e->position;
        apply_inputs(*e, dt_ms);
        regenerate(*e, dt_ms);
        tick_conditions(*e);
    }

    // Only monsters near a player think (activity culling, see WorldRules::active_radius).
    std::vector<EntityId> thinkers;
    if (rules_.active_radius > 0.0f) {
        active_cells_.assign(grid_start_.size() - 1, 0);
        const int reach = static_cast<int>(std::ceil(rules_.active_radius / kChunk));
        for (const EntityId id : players_) {
            const Entity* p = slot(id);
            if (p == nullptr) continue;
            const int pcx = static_cast<int>(p->position.x) / kChunk;
            const int pcy = static_cast<int>(p->position.y) / kChunk;
            for (int y = std::max(0, pcy - reach); y <= std::min(grid_h_ - 1, pcy + reach); ++y) {
                for (int x = std::max(0, pcx - reach); x <= std::min(grid_w_ - 1, pcx + reach); ++x) {
                    const auto cell = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid_w_) + static_cast<std::size_t>(x);
                    if (std::exchange(active_cells_[cell], char{1}) != 0) continue;
                    for (auto i = grid_start_[cell]; i < grid_start_[cell + 1]; ++i) thinkers.push_back(grid_ids_[i]);
                }
            }
        }
    } else {
        for (const auto& s : slots_) {
            if (s.entity && s.entity->enemy) thinkers.push_back(s.entity->id);
        }
    }
    for (const EntityId id : thinkers) {
        Entity* e = slot(id);
        if (e == nullptr || !e->enemy) continue;
        tick_conditions(*e);
        tick_enemy(*e, dt_ms);
        if (Entity* still = slot(id)) still->previous = still->position;
    }
    tick_projectiles(dt_ms);
    tick_aoes();
    for (auto& [due, id] : timed_removals_) {
        if (due <= now_ms_) to_remove_.push_back(id);
    }
    std::erase_if(timed_removals_, [this](const auto& t) { return t.first <= now_ms_; });
    tick_containers();
    for (const auto id : to_remove_) remove(id);
    to_remove_.clear();
    in_tick_ = false;
    flush_spawns();
    tick_terrain_spawner();
    for (const EntityId id : players_) {
        Entity* e = slot(id);
        if (e == nullptr) continue;
        ViewUpdate update;
        replicate(*e, update);
        out.push_back(std::move(update));
    }
    // Clear the change bits of everything a player may have been sent (monsters far from every player are not replicated, and
    // a 2048x2048 realm is too big to sweep every tick).
    auto clear = [](Entity& e) {
        e.dirty = 0;
        if (e.player) {
            e.player->stats_dirty = false;
            e.player->inventory_dirty = false;
        }
    };
    if (rules_.active_radius > 0.0f) {
        for (const EntityId id : thinkers) {
            if (Entity* e = slot(id)) clear(*e);
        }
        for (const EntityId id : players_) {
            Entity* p = slot(id);
            if (p == nullptr) continue;
            clear(*p);
            for_each_near(p->position, rules_.sight_radius, clear);
        }
        for (const EntityId id : containers_) {
            if (Entity* e = slot(id)) clear(*e);
        }
    } else {
        for (auto& s : slots_) {
            if (s.entity) clear(*s.entity);
        }
    }
}

}  // namespace waw::sim
