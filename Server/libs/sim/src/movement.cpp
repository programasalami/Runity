#include "runity/sim/movement.hpp"

#include <algorithm>
#include <cmath>

namespace runity::sim::movement {

float move_speed(int speed_stat, float tile_multiplier) noexcept {
    // Integer division on purpose: reference Player.GetMoveSpeed.
    const int steps = speed_stat / kSpeedDivisor;
    const float speed = kMinMoveSpeed + static_cast<float>(steps) * (kMaxMoveSpeed - kMinMoveSpeed);
    return speed * tile_multiplier;
}

float tile_multiplier(const Tile& tile, float& sink_level, float dt_ms) noexcept {
    if (tile.sinking) {
        sink_level = std::min(sink_level + dt_ms / kSinkStepMs, kMaxSinkLevel);
        return 0.1f + (1.0f - sink_level / kMaxSinkLevel) * (tile.speed - 0.1f);
    }
    sink_level = 0.0f;
    return tile.speed;
}

Vec2 direction_from_input(std::int8_t dir_x, std::int8_t dir_y) noexcept {
    Vec2 d{static_cast<float>(dir_x) / 127.0f, static_cast<float>(dir_y) / 127.0f};
    const float len_sq = d.x * d.x + d.y * d.y;
    if (len_sq > 1.0f) {
        const float len = std::sqrt(len_sq);
        d.x /= len;
        d.y /= len;
    }
    return d;
}

namespace {

bool is_full_occupy(const TileMap& map, float x, float y) noexcept {
    const int tx = static_cast<int>(x);
    const int ty = static_cast<int>(y);
    if (x < 0.0f || y < 0.0f || !map.in_bounds(tx, ty)) return true;  // no tile
    const Tile& t = map.at(tx, ty);
    return (t.flags & (tile_flags::Void | tile_flags::FullOccupy)) != 0;
}

bool is_walkable(const Tile& t) noexcept { return (t.flags & (tile_flags::NoWalk | tile_flags::OccupySquare)) == 0; }

}  // namespace

bool is_valid_position(const TileMap& map, Vec2 current, float x, float y) noexcept {
    if (x < 0.0f || y < 0.0f) return false;
    const int tx = static_cast<int>(x);
    const int ty = static_cast<int>(y);
    const bool same_tile = tx == static_cast<int>(current.x) && ty == static_cast<int>(current.y);
    if (!same_tile && (!map.in_bounds(tx, ty) || !is_walkable(map.at(tx, ty)))) return false;

    const float x_frac = x - static_cast<float>(tx);
    const float y_frac = y - static_cast<float>(ty);
    if (x_frac < 0.5f) {
        if (is_full_occupy(map, x - 1, y)) return false;
        if (y_frac < 0.5f) {
            if (is_full_occupy(map, x, y - 1) || is_full_occupy(map, x - 1, y - 1)) return false;
        } else if (y_frac > 0.5f) {
            if (is_full_occupy(map, x, y + 1) || is_full_occupy(map, x - 1, y + 1)) return false;
        }
    } else if (x_frac > 0.5f) {
        if (is_full_occupy(map, x + 1, y)) return false;
        if (y_frac < 0.5f) {
            if (is_full_occupy(map, x, y - 1) || is_full_occupy(map, x + 1, y - 1)) return false;
        } else if (y_frac > 0.5f) {
            if (is_full_occupy(map, x, y + 1) || is_full_occupy(map, x + 1, y + 1)) return false;
        }
    } else if (y_frac < 0.5f) {
        if (is_full_occupy(map, x, y - 1)) return false;
    } else if (y_frac > 0.5f) {
        if (is_full_occupy(map, x, y + 1)) return false;
    }
    return true;
}

namespace {

// Reference ModifyStep: crossing a half-tile border into an invalid position stops just before the border.
Vec2 modify_step(const TileMap& map, Vec2 pos, float x, float y) noexcept {
    const bool x_cross = (std::fmod(pos.x, 0.5f) == 0.0f && x != pos.x) || static_cast<int>(pos.x / 0.5f) != static_cast<int>(x / 0.5f);
    const bool y_cross = (std::fmod(pos.y, 0.5f) == 0.0f && y != pos.y) || static_cast<int>(pos.y / 0.5f) != static_cast<int>(y / 0.5f);

    if ((!x_cross && !y_cross) || is_valid_position(map, pos, x, y)) return {x, y};

    float next_x_border = 0.0f;
    float next_y_border = 0.0f;
    if (x_cross) {
        next_x_border = x > pos.x ? static_cast<float>(static_cast<int>(x * 2)) / 2.0f : static_cast<float>(static_cast<int>(pos.x * 2)) / 2.0f;
        if (static_cast<int>(next_x_border) > static_cast<int>(pos.x)) next_x_border -= 0.01f;
    }
    if (y_cross) {
        next_y_border = y > pos.y ? static_cast<float>(static_cast<int>(y * 2)) / 2.0f : static_cast<float>(static_cast<int>(pos.y * 2)) / 2.0f;
        if (static_cast<int>(next_y_border) > static_cast<int>(pos.y)) next_y_border -= 0.01f;
    }
    if (!x_cross) return {x, next_y_border};
    if (!y_cross) return {next_x_border, y};

    const float x_border_dist = x > pos.x ? x - next_x_border : next_x_border - x;
    const float y_border_dist = y > pos.y ? y - next_y_border : next_y_border - y;
    if (x_border_dist > y_border_dist) {
        if (is_valid_position(map, pos, x, next_y_border)) return {x, next_y_border};
        if (is_valid_position(map, pos, next_x_border, y)) return {next_x_border, y};
    } else {
        if (is_valid_position(map, pos, next_x_border, y)) return {next_x_border, y};
        if (is_valid_position(map, pos, x, next_y_border)) return {x, next_y_border};
    }
    return {next_x_border, next_y_border};
}

}  // namespace

Vec2 resolve_move(const TileMap& map, Vec2 current, float x, float y) noexcept {
    const float dx = x - current.x;
    const float dy = y - current.y;
    if (dx < kMoveThreshold && dx > -kMoveThreshold && dy < kMoveThreshold && dy > -kMoveThreshold) {
        return modify_step(map, current, x, y);
    }
    // Long move: sub-steps of at most kMoveThreshold. Each sub-step is resolved from where the previous one ended
    // (the reference measured every sub-step from the frame's start position; moves this long do not happen at fixed steps).
    Vec2 result = current;
    float step_size = kMoveThreshold / std::max(std::abs(dx), std::abs(dy));
    float done_fraction = 0.0f;
    bool done = false;
    while (!done) {
        if (done_fraction + step_size >= 1.0f) {
            step_size = 1.0f - done_fraction;
            done = true;
        }
        result = modify_step(map, result, result.x + dx * step_size, result.y + dy * step_size);
        done_fraction += step_size;
    }
    return result;
}

void step(const TileMap& map, MoverState& state, Vec2 direction, float dt_ms, int speed_stat) noexcept {
    dt_ms = std::clamp(dt_ms, 0.0f, kMaxStepMs);
    const Tile& tile = map.at(static_cast<int>(state.position.x), static_cast<int>(state.position.y));
    const float multiplier = tile_multiplier(tile, state.sink_level, dt_ms);
    if (direction.x == 0.0f && direction.y == 0.0f) return;
    const float speed = move_speed(speed_stat, multiplier);
    state.position = resolve_move(map, state.position, state.position.x + direction.x * speed * dt_ms,
                                  state.position.y + direction.y * speed * dt_ms);
}

}  // namespace runity::sim::movement
