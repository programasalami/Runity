#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace waw::content {

/// How a projectile flies (the original's ProjectileDesc + ProjectilePaths). speed in tiles per second (XML <Speed> / 10).
/// Only the single-segment paths the clients draw exist here; behaviour projectiles with other paths fly as Line (load warning).
enum class PathKind : std::uint8_t { Line, Amplitude, Wavy, Boomerang };

/// A condition effect a projectile or an AOE inflicts (the original's (ConditionEffectIndex, ms) tuples).
struct EffectSpec {
    int effect = 0;  // ConditionEffectIndex (conditions.hpp)
    int duration_ms = 0;
};

struct ProjectileDesc {
    std::string object_id;          // the projectile's look (Projectiles.xml), e.g. "Grey Missile"
    std::uint16_t object_type = 0;  // resolved after loading
    PathKind path = PathKind::Line;
    float speed = 0.0f;
    int lifetime_ms = 0;
    int min_damage = 0;
    int max_damage = 0;
    float amplitude = 0.0f;
    float frequency = 1.0f;
    bool multi_hit = false;
    bool passes_cover = false;
    bool armor_piercing = false;
    int size = 100;
    std::vector<EffectSpec> effects;
};

}  // namespace waw::content
