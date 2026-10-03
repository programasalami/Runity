#include "waw/sim/tile_map.hpp"

namespace waw::sim {

namespace {
const Tile kVoidTile{};
const std::vector<TileCoord> kNoTiles;
}  // namespace

bool TileMap::is_tile_object(const content::ObjectDesc& desc) noexcept {
    // Static decoration and walls never change: they travel with the tile data instead of as entities
    // (INTENTIONALLY DIFFERENT from the reference, which spawned every placed object as an entity - WorldSystem.md).
    return desc.is_static || desc.object_class == "Wall";
}

TileMap TileMap::build(const content::MapData& map, const content::ContentDb& content) {
    TileMap m;
    m.width_ = map.width;
    m.height_ = map.height;
    // Resolve every palette entry once: a 2048x2048 realm has ~4 million tiles but under 2000 distinct templates.
    struct Resolved {
        Tile tile;
        bool placement = false;  // the object is an entity, not part of the tile
        bool walkable = false;
    };
    std::vector<Resolved> palette(map.palette.size());
    for (std::size_t i = 0; i < map.palette.size(); ++i) {
        const auto& t = map.palette[i];
        Resolved& r = palette[i];
        r.tile.ground = t.ground_type;
        r.tile.flags = 0;
        if (const auto* ground = content.ground(t.ground_type); ground != nullptr && t.ground_type != 0xFF) {
            if (ground->no_walk) r.tile.flags |= tile_flags::NoWalk;
            r.tile.speed = ground->speed;
            r.tile.sinking = ground->sinking;
        } else {
            r.tile.flags |= tile_flags::Void | tile_flags::NoWalk;
        }
        if (t.object_type) {
            const auto* obj = content.object(*t.object_type);
            if (obj != nullptr && is_tile_object(*obj)) {
                r.tile.object = obj->type;
                if (obj->occupy_square) r.tile.flags |= tile_flags::OccupySquare;
                if (obj->full_occupy) r.tile.flags |= tile_flags::FullOccupy;
                if (obj->enemy_occupy_square) r.tile.flags |= tile_flags::EnemyOccupySquare;
                if (obj->blocks_sight) r.tile.flags |= tile_flags::BlocksSight;
            } else if (obj != nullptr) {
                r.placement = true;
            }
        }
        constexpr std::uint8_t blocked = tile_flags::NoWalk | tile_flags::Void | tile_flags::OccupySquare | tile_flags::FullOccupy;
        r.walkable = (r.tile.flags & blocked) == 0;
    }
    m.tiles_.resize(std::size_t{map.width} * map.height);
    // Raw pointers in the per-tile loop: a realm has 4 million tiles and checked iterators make debug builds crawl.
    const std::uint16_t* indices = map.tiles.data();
    Tile* out = m.tiles_.data();
    const Resolved* resolved = palette.data();
    const content::TileTemplate* templates = map.palette.data();
    for (std::uint16_t y = 0; y < map.height; ++y) {
        for (std::uint16_t x = 0; x < map.width; ++x) {
            const std::size_t index = std::size_t{y} * map.width + x;
            const auto palette_index = indices[index];
            const Resolved& r = resolved[palette_index];
            out[index] = r.tile;
            const auto& t = templates[palette_index];
            if (r.placement) m.placements_.push_back({*t.object_type, {x + 0.5f, y + 0.5f}, t.object_name});
            if (!t.regions.empty()) {
                for (const auto& region : t.regions) m.regions_[region].push_back({x, y});
            }
            if (t.terrain != content::Terrain::None && r.walkable) m.terrains_[static_cast<std::size_t>(t.terrain)].push_back({x, y});
        }
    }
    return m;
}

const Tile& TileMap::at(int x, int y) const noexcept {
    if (!in_bounds(x, y)) return kVoidTile;
    return tiles_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)];
}

const std::vector<TileCoord>& TileMap::region(const std::string& name) const {
    auto it = regions_.find(name);
    return it == regions_.end() ? kNoTiles : it->second;
}

const std::vector<TileCoord>& TileMap::terrain(content::Terrain t) const {
    const auto i = static_cast<std::size_t>(t);
    return i < terrains_.size() ? terrains_[i] : kNoTiles;
}

}  // namespace waw::sim
