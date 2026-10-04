// Content/Behaviors/*.json -> behaviour descriptors (runity/content/behavior.hpp). Parameter names and defaults are the original's
// (alloy-server Behaviors/Actions/*.cs, Transitions/*.cs); Tools/behaviors/transpile_behaviorlib.py documents the few renames.

#include <array>
#include <functional>

#include <nlohmann/json.hpp>

#include "runity/content/conditions.hpp"
#include "runity/content/content_db.hpp"

namespace runity::content {

namespace {

using Error = std::unexpected<std::string>;
using nlohmann::json;
namespace bh = behavior;

constexpr std::array<std::string_view, static_cast<std::size_t>(Condition::Count)> kConditionNames = {
    "Nothing", "Dead", "Quiet", "Weak", "Slowed", "Sick", "Dazed", "Stunned", "Blind", "Hallucinating", "Drunk", "Confused", "StunImmune",
    "Invisible", "Paralyzed", "Speedy", "Bleeding", "ArmorBrokenImmune", "Healing", "Damaging", "Berserk", "Paused", "Stasis",
    "StasisImmune", "Invincible", "Invulnerable", "Armored", "ArmorBroken", "Hexed", "NinjaSpeedy", "Unstable", "Darkness",
    "SlowedImmune", "DazedImmune", "ParalyzedImmune", "Petrify", "PetrifiedImmune", "PetEffectIcon", "Curse", "CurseImmune", "HpBoost",
    "MpBoost", "AttBoost", "DefBoost", "SpdBoost", "VitBoost", "WisBoost", "DexBoost", "Silenced", "Exposed", "Energized", "HpDebuff",
    "MpDebuff", "AttDebuff", "DefDebuff", "SpdDebuff", "VitDebuff", "WisDebuff", "DexDebuff", "Inspired"};

/// A JSON object's parameter with the original's default.
template <class T>
T get(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    return it->get<T>();
}

/// A list member without copying it (json::value() would copy the whole subtree).
const json& list(const json& j, const char* key) {
    static const json kEmpty = json::array();
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? kEmpty : *it;
}

bh::TargetType target_type(const json& j, bh::TargetType fallback) {
    if (auto t = j.find("targeted"); t != j.end()) return t->get<bool>() ? bh::TargetType::ClosestPlayer : bh::TargetType::FixedAngle;
    const auto name = get<std::string>(j, "targetType", "");
    if (name == "ClosestPlayer") return bh::TargetType::ClosestPlayer;
    if (name == "FixedAngle") return bh::TargetType::FixedAngle;
    if (name == "RandomPlayerPerBehavior") return bh::TargetType::RandomPlayerPerBehavior;
    if (name == "RandomPlayerPerCycle") return bh::TargetType::RandomPlayerPerCycle;
    if (name == "FarthestPlayer") return bh::TargetType::FarthestPlayer;
    if (name == "Entity") return bh::TargetType::Entity;
    return fallback;
}

bh::EntityRef entity_ref(const json& j, const char* key, std::string fallback) {
    bh::EntityRef r;
    r.id = get<std::string>(j, key, std::move(fallback));
    r.player = r.id == "player";
    return r;
}

std::vector<EffectSpec> effects(const json& j, std::vector<std::string>& unknown) {
    std::vector<EffectSpec> out;
    for (const auto& e : list(j, "effects")) {
        const auto name = e.at("effect").get<std::string>();
        if (const auto c = condition_from_string(name)) {
            out.push_back({static_cast<int>(*c), e.value("durationMs", 0)});
        } else {
            unknown.push_back("condition '" + name + "'");
        }
    }
    return out;
}

/// An inline behaviour projectile: {id, damage | minDamage + maxDamage, lifetimeMs, size, multiHit, passesCover, armorPiercing,
/// effects, path: [segment]} (the original's Shoot inline projectile + ProjectilePathSegment).
ProjectileDesc projectile(const json& j, std::vector<std::string>& notes) {
    ProjectileDesc p;
    p.object_id = get<std::string>(j, "id", "Blade");
    if (j.contains("damage")) {
        p.min_damage = p.max_damage = j.at("damage").get<int>();
    } else {
        p.min_damage = get(j, "minDamage", 0);
        p.max_damage = get(j, "maxDamage", p.min_damage);
    }
    p.lifetime_ms = get(j, "lifetimeMs", 1000);
    p.size = get(j, "size", 100);
    p.multi_hit = get(j, "multiHit", false);
    p.passes_cover = get(j, "passesCover", false);
    p.armor_piercing = get(j, "armorPiercing", false);
    p.effects = effects(j, notes);
    const auto& path = list(j, "path");
    if (path.empty()) {
        p.speed = 10.0f;
    } else {
        const auto& seg = path.front();
        const auto type = seg.at("type").get<std::string>();
        p.speed = get(seg, "speed", 10.0f);
        p.amplitude = get(seg, "amplitude", 0.0f);
        p.frequency = get(seg, "frequency", 1.0f);
        if (type == "Line") p.path = PathKind::Line;
        else if (type == "Amplitude") p.path = PathKind::Amplitude;
        else if (type == "Wavy") p.path = PathKind::Wavy;
        else if (type == "Boomerang") p.path = PathKind::Boomerang;
        else notes.push_back("projectile path '" + type + "' (flies as Line)");
        if (path.size() > 1) notes.push_back("multi-segment projectile path (only the first segment flies)");
        if (seg.value("boomerang", false)) p.path = PathKind::Boomerang;
    }
    return p;
}

bh::Script script(const json& j, std::vector<std::string>& notes);

std::vector<bh::Script> scripts(const json& list, std::vector<std::string>& notes) {
    std::vector<bh::Script> out;
    for (const auto& s : list) out.push_back(script(s, notes));
    return out;
}

bh::Follow follow(const json& j, bool away) {
    bh::Follow f;
    f.speed = get(j, "speed", 1.0f);
    f.dist_from_target = get(j, "distFromTarget", 2.0f);
    f.acquire_range = get(j, "acquireRange", 10.0f);
    f.cooldown_ms = get(j, "cooldownMs", 1000);
    f.cooldown_offset_ms = get(j, "cooldownOffsetMs", 0);
    f.follow_time_ms = get(j, "followTimeMs", 1000);
    f.target_type = target_type(j, bh::TargetType::ClosestPlayer);
    f.target = entity_ref(j, "target", "player");
    if (!f.target.player && f.target_type == bh::TargetType::ClosestPlayer) f.target_type = bh::TargetType::Entity;
    f.away = away;
    return f;
}

bh::Script script(const json& j, std::vector<std::string>& notes) {
    const auto type = j.at("type").get<std::string>();
    bh::Script out;
    if (type == "Wander") {
        out.kind = bh::Wander{get(j, "speed", 1.0f), get(j, "distance", 1.0f), get(j, "distanceFromSpawn", 5.0f), get(j, "cooldownMs", 0)};
    } else if (type == "Follow" || type == "StayAwayFrom") {
        out.kind = follow(j, type == "StayAwayFrom");
    } else if (type == "Charge") {
        out.kind = bh::Charge{get(j, "speed", 1.0f), get(j, "range", 10.0f), get(j, "cooldownMs", 1000)};
    } else if (type == "Orbit") {
        bh::Orbit o;
        o.speed = get(j, "speed", 1.0f);
        o.radius = get(j, "radius", 1.0f);
        o.acquire_range = get(j, "acquireRange", 10.0f);
        o.target = entity_ref(j, "target", "");
        o.speed_variance = get(j, "speedVariance", 0.0f);
        o.radius_variance = get(j, "radiusVariance", 0.0f);
        o.clockwise = get(j, "orbitClockwise", false);
        o.target_player = get(j, "targetPlayer", false);
        out.kind = o;
    } else if (type == "Protect") {
        out.kind = bh::Protect{get(j, "speed", 1.0f), entity_ref(j, "protectee", ""), get(j, "acquireRange", 10.0f),
                               get(j, "protectionRange", 2.0f), get(j, "reprotectRange", 1.0f)};
    } else if (type == "MoveLine") {
        out.kind = bh::MoveLine{get(j, "speed", 1.0f), get(j, "angle", 0.0f), get(j, "distance", 0.0f)};
    } else if (type == "ReturnToSpawn") {
        out.kind = bh::ReturnToSpawn{get(j, "speed", 1.0f), get(j, "distanceFromSpawn", 0.0f)};
    } else if (type == "BackAndForth") {
        out.kind = bh::BackAndForth{get(j, "speed", 1.0f), get(j, "distance", 5.0f)};
    } else if (type == "Buzz") {
        out.kind = bh::Buzz{get(j, "speed", 2.0f), get(j, "distance", 0.5f)};
    } else if (type == "Swirl") {
        out.kind = bh::Swirl{get(j, "speed", 1.0f), get(j, "radius", 8.0f), get(j, "acquireRange", 10.0f), get(j, "targeted", true)};
    } else if (type == "Shoot") {
        bh::Shoot s;
        s.max_radius = get(j, "maxRadius", 0.0f);
        s.min_radius = get(j, "minRadius", 0.0f);
        s.count = get(j, "count", 1);
        s.shoot_angle = get(j, "shootAngle", 0.0f);
        s.fixed_angle = get(j, "fixedAngle", 0.0f);
        s.rotate_angle = get(j, "rotateAngle", 0.0f);
        s.angle_offset = get(j, "angleOffset", 0.0f);
        s.predictive = get(j, "predictive", 0.0f);
        s.cooldown_offset_ms = get(j, "cooldownOffsetMs", 0);
        s.cooldown_ms = get(j, "cooldownMs", 0);
        // The original's inline-projectile Shoot defaults to ClosestPlayer only in its targetType overload; `targeted` decides
        // otherwise (default false = FixedAngle).
        s.target_type = target_type(j, bh::TargetType::FixedAngle);
        s.x_offset = get(j, "xOffset", 0.0f);
        s.y_offset = get(j, "yOffset", 0.0f);
        if (auto p = j.find("projectile"); p != j.end()) {
            s.projectile.push_back(projectile(*p, notes));
        } else {
            s.projectile_index = get(j, "projectileIndex", 0);
        }
        out.kind = std::move(s);
    } else if (type == "AOE") {
        bh::Aoe a;
        a.radius = get(j, "radius", 0.0f);
        a.damage = get(j, "damage", 0);
        a.cooldown_ms = get(j, "cooldownMs", 0);
        a.range = get(j, "range", 12.0f);
        a.cooldown_offset_ms = get(j, "cooldownOffsetMs", 0);
        a.target_type = target_type(j, bh::TargetType::ClosestPlayer);
        a.fixed_angle = get(j, "fixedAngle", 0.0f);
        a.angle_offset = get(j, "angleOffset", 0.0f);
        a.activate_count = get(j, "activateCount", 1);
        a.throw_time_ms = get(j, "throwTime", 1500);
        a.damage_cooldown_ms = get(j, "damageCooldown", 1000);
        a.rotate_angle = get(j, "rotateAngle", 0.0f);
        a.effects = effects(j, notes);
        out.kind = std::move(a);
    } else if (type == "Spawn") {
        bh::Spawn s;
        s.entity = entity_ref(j, "entityName", "");
        s.min_x = s.max_x = get(j, "x", 0.0f);
        s.min_y = s.max_y = get(j, "y", 0.0f);
        s.min_x = get(j, "minX", s.min_x);
        s.max_x = get(j, "maxX", s.max_x);
        s.min_y = get(j, "minY", s.min_y);
        s.max_y = get(j, "maxY", s.max_y);
        s.cooldown_ms = get(j, "cooldownMs", 1000);
        s.cooldown_offset_ms = get(j, "cooldownOffsetMs", 0);
        s.max_spawns_per_reset = get(j, "maxSpawnsPerReset", s.max_spawns_per_reset);
        s.min_spawn_count = get(j, "minSpawnCount", 1);
        s.max_spawn_count = get(j, "maxSpawnCount", 1);
        s.max_density = get(j, "maxDensity", 0);
        s.density_radius = get(j, "densityRadius", 10.0f);
        if (j.contains("group") && !j.at("group").is_null()) s.entity.id = "group:" + j.at("group").get<std::string>();
        out.kind = std::move(s);
    } else if (type == "Reproduce") {
        out.kind = bh::Reproduce{entity_ref(j, "entityName", ""), get(j, "cooldownMs", 60000), get(j, "maxDensity", 0),
                                 get(j, "densityRadius", 10.0f)};
    } else if (type == "TossObject") {
        bh::TossObject t;
        if (j.contains("group") && !j.at("group").is_null()) {
            t.children.push_back({"group:" + j.at("group").get<std::string>(), 0, false});
        } else {
            t.children.push_back(entity_ref(j, "child", ""));
        }
        t.range = get(j, "range", 5.0f);
        t.angle = get(j, "angle", 0.0f);
        t.cooldown_ms = get(j, "cooldownMs", 1000);
        t.cooldown_offset_ms = get(j, "cooldownOffsetMs", 0);
        t.probability = get(j, "probability", 1.0f);
        t.min_angle = get(j, "minAngle", 0.0f);
        t.max_angle = get(j, "maxAngle", 0.0f);
        t.min_range = get(j, "minRange", 0.0f);
        t.max_range = get(j, "maxRange", 0.0f);
        t.density_range = get(j, "densityRange", 0.0f);
        t.max_density = get(j, "maxDensity", 1);
        t.targeted = get(j, "targeted", false);
        if (j.contains("region")) notes.push_back("TossObject region (tossed by range and angle instead)");
        out.kind = std::move(t);
    } else if (type == "Order" || type == "OrderOnDeath") {
        out.kind = bh::Order{get(j, "range", 0.0f), entity_ref(j, "children", ""), get<std::string>(j, "targetState", ""),
                             type == "OrderOnDeath"};
    } else if (type == "HealSelf") {
        out.kind = bh::HealSelf{get(j, "cooldownMs", 0), get(j, "amount", 0), get(j, "percentage", false), get(j, "cooldownOffsetMs", 0)};
    } else if (type == "HealGroup") {
        out.kind = bh::HealGroup{get(j, "range", 0.0f), get<std::string>(j, "group", ""), get(j, "cooldownMs", 1000), get(j, "healAmount", 0)};
    } else if (type == "ConditionEffectBehavior") {
        const auto name = get<std::string>(j, "effect", "");
        const auto c = condition_from_string(name);
        if (!c) {
            notes.push_back("condition '" + name + "'");
            out.kind = bh::NoOp{type};
        } else {
            out.kind = bh::ConditionEffect{static_cast<int>(*c), get(j, "durationMs", -1), get(j, "persist", false)};
        }
    } else if (type == "Taunt") {
        bh::Taunt t;
        auto text = get<std::string>(j, "text", "");
        for (std::size_t start = 0;;) {
            const auto sep = text.find("||", start);
            t.texts.push_back(text.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
            if (sep == std::string::npos) break;
            start = sep + 2;
        }
        t.cooldown_ms = get(j, "cooldownMs", 0);
        t.probability = get(j, "probability", 1.0f);
        out.kind = std::move(t);
    } else if (type == "Suicide") {
        out.kind = bh::Suicide{get(j, "delay", 300)};
    } else if (type == "Transform") {
        out.kind = bh::Transform{entity_ref(j, "target", "")};
    } else if (type == "TransformOnDeath") {
        out.kind = bh::TransformOnDeath{entity_ref(j, "target", ""), get(j, "min", 1), get(j, "max", 1), get(j, "probability", 1.0f)};
    } else if (type == "DropPortalOnDeath") {
        out.kind = bh::DropPortalOnDeath{entity_ref(j, "portalId", ""), get(j, "probability", 1.0f), get(j, "timeout", 0)};
    } else if (type == "Timed") {
        out.kind = bh::Timed{get(j, "period", 0), scripts(list(j, "scripts"), notes)};
    } else if (type == "Duration") {
        bh::Duration d;
        d.duration = get(j, "duration", 0);
        if (auto inner = j.find("script"); inner != j.end() && !inner->is_null()) d.script.push_back(script(*inner, notes));
        out.kind = std::move(d);
    } else if (type == "Sequence") {
        out.kind = bh::Sequence{scripts(list(j, "scripts"), notes)};
    } else {
        // Flash, SetAltTexture and ChangeSize only change how a monster looks; the rest are not run by the engine yet.
        if (type != "Flash" && type != "SetAltTexture" && type != "ChangeSize") notes.push_back("script '" + type + "'");
        out.kind = bh::NoOp{type};
    }
    return out;
}

std::optional<bh::Transition> transition(const json& j, std::vector<std::string>& notes) {
    static constexpr std::array<std::pair<std::string_view, bh::TransitionKind>, 11> kKinds = {{
        {"Timed", bh::TransitionKind::Timed},
        {"EntityWithin", bh::TransitionKind::EntityWithin},
        {"EntityNotWithin", bh::TransitionKind::EntityNotWithin},
        {"EntitiesWithin", bh::TransitionKind::EntitiesWithin},
        {"EntitiesNotWithin", bh::TransitionKind::EntitiesNotWithin},
        {"HpLess", bh::TransitionKind::HpLess},
        {"EntityHpLess", bh::TransitionKind::EntityHpLess},
        {"DamageTaken", bh::TransitionKind::DamageTaken},
        {"NotMoving", bh::TransitionKind::NotMoving},
        {"OnParentDeath", bh::TransitionKind::OnParentDeath},
        {"PlayerText", bh::TransitionKind::PlayerText},
    }};
    const auto type = j.at("type").get<std::string>();
    bh::Transition t;
    const auto kind = std::find_if(kKinds.begin(), kKinds.end(), [&](const auto& k) { return k.first == type; });
    if (kind == kKinds.end()) {
        notes.push_back("transition '" + type + "'");
        return std::nullopt;
    }
    t.kind = kind->second;
    if (const auto& to = j.at("to"); to.is_array()) {
        t.to = to.get<std::vector<std::string>>();
    } else {
        t.to.push_back(to.get<std::string>());
    }
    const auto mode = get<std::string>(j, "mode", "Random");
    t.mode = mode == "Sequential" ? bh::TransitionMode::Sequential : mode == "Random7Bag" ? bh::TransitionMode::Random7Bag : bh::TransitionMode::Random;
    switch (t.kind) {
        case bh::TransitionKind::Timed: t.time_ms = get(j, "time", 0); break;
        case bh::TransitionKind::NotMoving: t.time_ms = get(j, "delay", 250); break;
        case bh::TransitionKind::EntityWithin:
        case bh::TransitionKind::EntityNotWithin:
            t.radius = get(j, "radius", 8.0f);
            t.target = entity_ref(j, "target", "player");
            break;
        case bh::TransitionKind::EntitiesWithin:
        case bh::TransitionKind::EntitiesNotWithin:
            t.radius = get(j, "radius", 0.0f);
            for (const auto& name : list(j, "targets")) t.targets.push_back({name.get<std::string>(), 0, name == "player"});
            break;
        case bh::TransitionKind::HpLess: t.threshold = get(j, "threshold", 0.0f); break;
        case bh::TransitionKind::EntityHpLess:
            t.radius = get(j, "dist", 0.0f);
            t.target = entity_ref(j, "entity", "");
            t.threshold = get(j, "threshold", 0.0f);
            break;
        case bh::TransitionKind::DamageTaken: t.damage = get(j, "damage", 0); break;
        case bh::TransitionKind::PlayerText:
            t.regex = get<std::string>(j, "regex", "");
            notes.push_back("PlayerText transition (never fires)");
            break;
        case bh::TransitionKind::OnParentDeath: break;
    }
    return t;
}

void state(const json& j, bh::Behavior& b, int parent, std::vector<std::string>& notes) {
    const int index = static_cast<int>(b.states.size());
    b.states.emplace_back();
    {
        auto& s = b.states.back();
        s.name = get<std::string>(j, "name", "");
        s.parent = parent;
        s.scripts = scripts(list(j, "scripts"), notes);
        for (const auto& t : list(j, "transitions")) {
            if (auto tr = transition(t, notes)) s.transitions.push_back(std::move(*tr));
        }
    }
    if (parent >= 0) b.states[static_cast<std::size_t>(parent)].children.push_back(index);
    if (!b.states[static_cast<std::size_t>(index)].name.empty()) {
        if (!b.state_by_name.emplace(b.states[static_cast<std::size_t>(index)].name, index).second) {
            notes.push_back("state name '" + b.states[static_cast<std::size_t>(index)].name + "' used twice");
        }
    }
    for (const auto& child : list(j, "states")) state(child, b, index, notes);
}

}  // namespace

std::optional<Condition> condition_from_string(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kConditionNames.size(); ++i) {
        if (kConditionNames[i] == name) return static_cast<Condition>(i);
    }
    return std::nullopt;
}

std::expected<void, std::string> ContentDb::add_behaviors(std::string_view json_text, std::string_view source_name) {
    const std::string src(source_name);
    try {
        const auto j = json::parse(json_text);
        if (j.value("format", 0) != 1) return Error(src + ": unsupported behaviour format (expected \"format\": 1)");
        for (const auto& [name, entry] : j.at("behaviors").items()) {
            bh::Behavior b;
            b.object_id = name;
            b.source = src;
            std::vector<std::string> notes;
            state(entry.at("root"), b, -1, notes);
            for (const auto& table : list(entry, "loot")) {
                bh::LootTable lt;
                lt.is_public = table.value("public", false);
                for (const auto& item : list(table, "items")) {
                    bh::LootItem li;
                    li.threshold = item.value("threshold", 0.0f);
                    li.chance = item.value("chance", 0.0f);
                    if (item.at("type").get<std::string>() == "Tier") {
                        li.tier = true;
                        li.tier_level = item.at("tier").get<int>();
                        const auto cls = item.at("itemType").get<std::string>();
                        li.tier_class = cls == "Armor" ? bh::TierClass::Armor : cls == "Ability" ? bh::TierClass::Ability
                                        : cls == "Ring"  ? bh::TierClass::Ring : bh::TierClass::Weapon;
                    } else {
                        li.item_id = item.at("id").get<std::string>();
                    }
                    lt.items.push_back(std::move(li));
                }
                b.loot.push_back(std::move(lt));
            }
            for (const auto& n : notes) warnings_.push_back(src + ": '" + name + "': not run: " + n);
            pending_behaviors_.push_back(std::move(b));
        }
    } catch (const json::exception& e) {
        return Error(src + ": " + e.what());
    }
    return {};
}

void ContentDb::link_behavior(bh::Behavior& b) {
    const std::string where = b.source + ": '" + b.object_id + "'";
    auto resolve = [&](bh::EntityRef& r) {
        if (r.player || r.id.empty() || r.id.starts_with("group:")) return true;
        const auto* o = object(r.id);
        if (o == nullptr) {
            warnings_.push_back(where + " names unknown object '" + r.id + "'");
            return false;
        }
        r.type = o->type;
        return true;
    };
    auto link_projectile = [&](ProjectileDesc& p) {
        const auto* o = object(p.object_id);
        if (o == nullptr || o->object_class != "Projectile") {
            warnings_.push_back(where + " fires unknown projectile '" + p.object_id + "' (Blade instead, like the original)");
            o = object("Blade");
        }
        p.object_type = o != nullptr ? o->type : 0;
    };
    std::uint16_t next_slot = 0;
    std::function<void(bh::Script&, std::vector<std::uint16_t>&)> link_script = [&](bh::Script& s, std::vector<std::uint16_t>& slots) {
        s.slot = next_slot++;
        slots.push_back(s.slot);
        std::visit(
            [&](auto& k) {
                using T = std::decay_t<decltype(k)>;
                bool ok = true;
                if constexpr (std::is_same_v<T, bh::Follow>) {
                    ok = resolve(k.target);
                } else if constexpr (std::is_same_v<T, bh::Orbit>) {
                    ok = resolve(k.target);
                } else if constexpr (std::is_same_v<T, bh::Protect>) {
                    ok = resolve(k.protectee) && k.protectee.type != 0;
                } else if constexpr (std::is_same_v<T, bh::Shoot>) {
                    for (auto& p : k.projectile) link_projectile(p);
                    if (k.projectile.empty() && !object(b.object_id)->projectiles.contains(k.projectile_index)) {
                        warnings_.push_back(where + " shoots its <Projectile id=" + std::to_string(k.projectile_index) + ">, which it lacks");
                        ok = false;
                    }
                } else if constexpr (std::is_same_v<T, bh::Spawn>) {
                    ok = resolve(k.entity);
                    if (ok && k.entity.id.starts_with("group:")) k.group = group_members(k.entity.id.substr(6));
                    ok = ok && (k.entity.type != 0 || !k.group.empty());
                } else if constexpr (std::is_same_v<T, bh::Reproduce>) {
                    if (k.entity.id.empty()) k.entity = {b.object_id, b.object_type, false};
                    ok = resolve(k.entity);
                } else if constexpr (std::is_same_v<T, bh::TossObject>) {
                    std::vector<bh::EntityRef> children;
                    for (auto& c : k.children) {
                        if (c.id.starts_with("group:")) {
                            for (const auto t : group_members(c.id.substr(6))) children.push_back({object(t)->id, t, false});
                        } else if (resolve(c)) {
                            children.push_back(c);
                        }
                    }
                    k.children = std::move(children);
                    ok = !k.children.empty();
                } else if constexpr (std::is_same_v<T, bh::Order>) {
                    ok = resolve(k.children) && k.children.type != 0;
                } else if constexpr (std::is_same_v<T, bh::Transform> || std::is_same_v<T, bh::TransformOnDeath>) {
                    ok = resolve(k.target) && k.target.type != 0;
                } else if constexpr (std::is_same_v<T, bh::DropPortalOnDeath>) {
                    ok = resolve(k.portal) && k.portal.type != 0;
                } else if constexpr (std::is_same_v<T, bh::Timed> || std::is_same_v<T, bh::Sequence>) {
                    for (auto& inner : k.scripts) link_script(inner, slots);
                } else if constexpr (std::is_same_v<T, bh::Duration>) {
                    for (auto& inner : k.script) link_script(inner, slots);
                }
                if (!ok) s.kind = bh::NoOp{"unresolved"};
            },
            s.kind);
    };
    for (auto& st : b.states) {
        for (auto& s : st.scripts) link_script(s, st.slots);
        std::erase_if(st.transitions, [&](bh::Transition& t) {
            for (const auto& name : t.to) {
                auto it = b.state_by_name.find(name);
                if (it == b.state_by_name.end()) {
                    warnings_.push_back(where + " transitions to unknown state '" + name + "'");
                    return true;
                }
                t.to_state.push_back(it->second);
            }
            if (t.to_state.empty()) return true;
            resolve(t.target);
            for (auto& r : t.targets) resolve(r);
            return false;
        });
        for (auto& t : st.transitions) {
            t.slot = next_slot++;
            st.slots.push_back(t.slot);
        }
    }
    b.slot_count = next_slot;
    for (auto& table : b.loot) {
        std::erase_if(table.items, [&](bh::LootItem& li) {
            if (li.tier) return false;
            const auto* it = item(li.item_id);
            if (it == nullptr) {
                warnings_.push_back(where + " drops unknown item '" + li.item_id + "'");
                return true;
            }
            li.item_type = it->type;
            return false;
        });
    }
}

}  // namespace runity::content
