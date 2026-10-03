#pragma once

#include <cstdint>

#include "waw/content/content_db.hpp"
#include "waw/sim/tile_map.hpp"

namespace waw::sim {

/// How one bullet flies; everything the closed-form path needs (the client draws the same paths: WaW.Domain ProjectilePaths).
struct PathSpec {
    content::PathKind kind = content::PathKind::Line;
    float speed = 0.0f;      // tiles per second
    int lifetime_ms = 0;
    float amplitude = 0.0f;
    float frequency = 1.0f;
};

/// Offset from the start position after `elapsed_ms` (reference ProjectilePaths: Line, Amplitude, Wavy, Boomerang).
/// `bullet_id` picks the wave phase (even 0, odd pi), so paired bullets mirror each other.
[[nodiscard]] Vec2 path_offset(const PathSpec& spec, float elapsed_ms, std::uint32_t bullet_id, float angle) noexcept;

}  // namespace waw::sim
