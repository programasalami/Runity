#pragma once

#include <cstdint>

#include "waw/sim/tile_map.hpp"

namespace waw::sim::movement {

// Player movement rules. The Unity client runs the SAME function (WaW.Domain MovementRules.cs) to predict the local player;
// Protocol/vectors/movement.json pins both implementations to identical results. Reference: Movement.md (Player.cs:508-767).

inline constexpr float kMinMoveSpeed = 0.004f;    // tiles per ms (4 tiles/s)
inline constexpr float kMaxMoveSpeed = 0.0096f;   // tiles per ms (9.6 tiles/s)
inline constexpr int kSpeedDivisor = 75;
inline constexpr float kMoveThreshold = 0.4f;     // longer moves are split into sub-steps
inline constexpr float kMaxSinkLevel = 18.0f;
inline constexpr float kSinkStepMs = 50.0f;       // the reference raised the sink level once per server tick (50 ms)
inline constexpr float kMaxStepMs = 50.0f;        // a single input step never covers more than this

/// The movement state that persists between steps.
struct MoverState {
    Vec2 position;
    float sink_level = 0.0f;
};

/// Tiles per ms. Kept EXACTLY as the reference computes it, including the integer division `speed / 75`
/// (every Speed below 75 moves at the minimum speed). Changing it is an open decision for the project owner (MigrationStatus.md).
[[nodiscard]] float move_speed(int speed_stat, float tile_multiplier) noexcept;

/// The ground speed multiplier for a tile, updating the sink level (sinking ground slows a player down over time).
[[nodiscard]] float tile_multiplier(const Tile& tile, float& sink_level, float dt_ms) noexcept;

/// The input direction: a quantized vector, normalised if longer than 1 (a longer vector must not mean faster movement).
[[nodiscard]] Vec2 direction_from_input(std::int8_t dir_x, std::int8_t dir_y) noexcept;

/// Can a player whose current position is `current` stand at (x, y)? (reference IsValidPosition)
[[nodiscard]] bool is_valid_position(const TileMap& map, Vec2 current, float x, float y) noexcept;

/// Moves from the current position towards (x, y), stopping at walls the way the reference does (ModifyMove + ModifyStep).
[[nodiscard]] Vec2 resolve_move(const TileMap& map, Vec2 current, float x, float y) noexcept;

/// One fixed input step: applies tile speed / sinking, then moves with collision.
void step(const TileMap& map, MoverState& state, Vec2 direction, float dt_ms, int speed_stat) noexcept;

}  // namespace waw::sim::movement
