// The original realm: a 2048x2048 .wmap, populated by the LEGACY terrain spawner, replicated only around players.

#include <doctest/doctest.h>

#include <cmath>
#include <map>

#include "real_fixtures.hpp"

using namespace runity;
using namespace runity::sim;
using namespace runity::test;

TEST_CASE("the realm: terrain spawner, repopulation, activity culling and tile replication near players") {
    const auto& db = real();
    const auto* config = db.world("Realm");
    REQUIRE(config != nullptr);
    const auto* map = db.map("realm1-test.wmap");
    REQUIRE(map != nullptr);
    World realm(1, *config, *map, db, 11, WorldRules{.terrain_spawner = true});

    // 1.5 % of every terrain's walkable tiles hold that terrain's monsters (LEGACY Oryx.Populate).
    std::map<content::Terrain, int> per_terrain;
    int monsters = 0;
    realm.for_each([&](Entity& e) {
        if (!e.enemy) return;
        ++monsters;
        per_terrain[e.enemy->terrain] += 1;
        const auto* desc = db.object(e.object_type);
        REQUIRE(desc != nullptr);
        CHECK(desc->terrain == e.enemy->terrain);
        CHECK(e.enemy->terrain != content::Terrain::None);
    });
    CHECK(monsters > 10000);
    for (std::size_t t = 1; t <= static_cast<std::size_t>(content::Terrain::ShorePlains); ++t) {
        const auto terrain = static_cast<content::Terrain>(t);
        const int max = static_cast<int>(static_cast<float>(realm.map().terrain(terrain).size()) * 0.015f);
        const bool spawnable = !db.terrain_spawns(terrain).empty();
        CAPTURE(content::to_string(terrain));
        if (spawnable && max > 0) CHECK(per_terrain[terrain] >= max);
    }
    // No monster stands in a wall, on water or off the map.
    int misplaced = 0;
    realm.for_each([&](Entity& e) {
        if (!e.enemy) return;
        const auto& tile = realm.map().at(static_cast<int>(e.position.x), static_cast<int>(e.position.y));
        misplaced += (tile.flags & (tile_flags::NoWalk | tile_flags::Void | tile_flags::OccupySquare)) != 0 ? 1 : 0;
    });
    CHECK(misplaced == 0);

    // Killed monsters come back on the next repopulation (every 25 s).
    std::vector<EntityId> shore;
    realm.for_each([&](Entity& e) {
        if (e.enemy && e.enemy->terrain == content::Terrain::ShoreSand && shore.size() < 500) shore.push_back(e.id);
    });
    for (const auto id : shore) realm.remove(id);
    CHECK(realm.populate_terrain() >= 450);  // the spawner may have overshot by a group before

    // A player at the realm's spawn: tiles and entities arrive only from around it, monsters only think near it.
    const auto& spawn_tiles = realm.map().region("Spawn");
    REQUIRE_FALSE(spawn_tiles.empty());
    PlayerSpawn s;
    s.session_id = 1;
    s.class_type = kWizard;
    s.name = "Wiz";
    s.account_id = 1;
    s.items = db.player_class(kWizard)->equipment;
    const auto me = realm.add_player(s);
    const Vec2 at = realm.find(me)->position;
    EntityId far_monster;
    Vec2 far_start;
    realm.for_each([&](Entity& e) {
        if (e.enemy && far_monster.value() == 0 && distance_squared(e.position, at) > 200.0f * 200.0f) {
            far_monster = e.id;
            far_start = e.position;
        }
    });
    REQUIRE(far_monster.value() != 0);
    std::vector<ViewUpdate> views;
    realm.tick(50.0f, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0].tiles.size() > 100);
    CHECK(views[0].tiles.size() < 1400);  // pi * 20^2, not 4 million
    for (const auto& t : views[0].tiles) CHECK((t.x - at.x) * (t.x - at.x) + (t.y - at.y) * (t.y - at.y) <= 22.0f * 22.0f);
    for (const auto* e : views[0].entered) CHECK(distance_squared(e->position, at) <= 20.0f * 20.0f);
    for (int i = 0; i < 40; ++i) {
        views.clear();
        realm.find(me)->hp = realm.find(me)->max_hp;
        realm.tick(50.0f, views);
    }
    REQUIRE(realm.find(far_monster) != nullptr);
    CHECK(realm.find(far_monster)->position == far_start);  // nobody near: it does not think
    CHECK(views[0].tiles.empty());                          // nothing new to send while standing still
}
