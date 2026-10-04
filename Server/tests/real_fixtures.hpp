#pragma once

// The REAL content (Content/: the original Alloy's definitions, maps, worlds and transpiled behaviours) for gameplay tests.
// Loaded ONCE for the whole test run (every test file shares real()): it is the slowest thing the tests do.

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "runity/sim/world.hpp"

namespace runity::test {

using namespace runity::sim;

inline const content::ContentDb& real() {
    static const content::ContentDb db = [] {
        auto loaded = content::ContentDb::load(RUNITY_CONTENT_ROOT);
        if (!loaded) FAIL("content failed to load: " << loaded.error());
        return std::move(*loaded);
    }();
    return db;
}

inline constexpr std::uint16_t kWizard = 0x030e;

inline std::uint16_t type_of(const char* id) {
    const auto* o = real().object(id);
    REQUIRE_MESSAGE(o != nullptr, id);
    return o->type;
}

inline int item_type(const char* id) {
    const auto* d = real().item(id);
    REQUIRE_MESSAGE(d != nullptr, id);
    return d->type;
}

/// A world on real content: an open field of Light Grass with optional walls ('#', Grey Wall) and a Spawn tile ('S').
/// Every monster thinks, however far from a player (WorldRules::active_radius 0), unless the rules say otherwise.
struct Arena {
    content::MapData map;
    content::WorldConfig config;
    std::unique_ptr<World> world;

    explicit Arena(std::vector<std::string> rows, WorldRules rules = {.active_radius = 0.0f}) {
        map.name = "arena";
        map.height = static_cast<std::uint16_t>(rows.size());
        map.width = static_cast<std::uint16_t>(rows[0].size());
        const auto grass = real().ground("Light Grass")->type;
        const auto wall = type_of("Grey Wall");
        for (const auto& row : rows) {
            for (char c : row) {
                content::TileTemplate t;
                t.ground_type = grass;
                if (c == '#') t.object_type = wall;
                if (c == 'S') t.regions.push_back("Spawn");
                map.palette.push_back(t);
                map.tiles.push_back(static_cast<std::uint16_t>(map.palette.size() - 1));
            }
        }
        config.id = 7;
        config.name = "Arena";
        config.display_name = "Arena";
        world = std::make_unique<World>(7, config, map, real(), 99, rules);
    }

    EntityId wizard(Vec2 at, int level = 1, std::int64_t account = 1, std::uint32_t session = 1) {
        PlayerSpawn s;
        s.session_id = session;
        s.character_id = 1;
        s.class_type = kWizard;
        s.name = "Wiz" + std::to_string(session);
        s.account_id = account;
        s.level = level;
        s.hp = 0;  // full
        s.items = real().player_class(kWizard)->equipment;
        s.position = at;
        return world->add_player(s);
    }

    void run(float ms) {
        std::vector<ViewUpdate> views;
        for (float t = 0; t < ms; t += 50.0f) {
            views.clear();
            world->tick(50.0f, views);
        }
    }

    /// Every live entity of a type.
    std::vector<Entity*> all(std::uint16_t type) {
        std::vector<Entity*> out;
        world->for_each([&](Entity& e) {
            if (e.object_type == type) out.push_back(&e);
        });
        return out;
    }
};

}  // namespace runity::test
