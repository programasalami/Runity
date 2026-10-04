#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runity/content/content_db.hpp"

namespace runity::sim {

struct Vec2 {
    float x = 0;
    float y = 0;
    friend bool operator==(Vec2, Vec2) = default;
};

[[nodiscard]] inline float distance_squared(Vec2 a, Vec2 b) noexcept {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

struct TileCoord {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    friend bool operator==(TileCoord, TileCoord) = default;
};

/// Collision / sight flags of one tile, precomputed from its ground and its static object.
namespace tile_flags {
inline constexpr std::uint8_t NoWalk = 1 << 0;             // ground cannot be entered (water, void)
inline constexpr std::uint8_t OccupySquare = 1 << 1;       // a static object fills the tile for players and monsters
inline constexpr std::uint8_t FullOccupy = 1 << 2;         // ... and keeps a player's body half a tile away (walls)
inline constexpr std::uint8_t EnemyOccupySquare = 1 << 3;  // blocks monsters (and bullets) but not players
inline constexpr std::uint8_t BlocksSight = 1 << 4;
inline constexpr std::uint8_t Void = 1 << 5;               // no ground at all (ground type 0xFF)
}  // namespace tile_flags

struct Tile {
    std::uint16_t ground = 0xFF;
    std::uint16_t object = 0;  // static object type, 0 = none
    std::uint8_t flags = tile_flags::Void | tile_flags::NoWalk;
    float speed = 1.0f;        // ground movement multiplier
    bool sinking = false;
};

/// The authoritative tile grid of one world, built from a map and the content definitions. Immutable after construction.
class TileMap {
public:
    /// Static objects are stamped onto their tile (collision + sight); every other placed object becomes an entity placement.
    static TileMap build(const content::MapData& map, const content::ContentDb& content);

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }
    [[nodiscard]] bool in_bounds(int x, int y) const noexcept { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    /// Out-of-bounds coordinates return a void tile.
    [[nodiscard]] const Tile& at(int x, int y) const noexcept;

    /// Placed objects that are entities (portals, NPCs, containers ...), at tile centres.
    struct Placement {
        std::uint16_t object_type;
        Vec2 position;
        std::string name;
    };
    [[nodiscard]] const std::vector<Placement>& entity_placements() const noexcept { return placements_; }
    /// Tiles tagged with a region name in the map (e.g. "Spawn").
    [[nodiscard]] const std::vector<TileCoord>& region(const std::string& name) const;
    /// Walkable tiles of a realm terrain type (.wmap maps; empty for .jm maps).
    [[nodiscard]] const std::vector<TileCoord>& terrain(content::Terrain t) const;

    /// True if a static object of this definition is drawn and collided as part of the tile rather than spawned as an entity.
    [[nodiscard]] static bool is_tile_object(const content::ObjectDesc& desc) noexcept;

private:
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::vector<Tile> tiles_;
    std::vector<Placement> placements_;
    std::map<std::string, std::vector<TileCoord>> regions_;
    std::array<std::vector<TileCoord>, content::kTerrainCount> terrains_;
};

}  // namespace runity::sim
