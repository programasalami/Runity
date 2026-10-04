#include <doctest/doctest.h>

#include "real_fixtures.hpp"
#include "runity/content/content_db.hpp"

using namespace runity::content;
using runity::test::real;

TEST_CASE("the original content loads: 14 classes, 2250 grounds, maps, worlds and realm behaviours") {
    const auto& db = real();
    CHECK(db.ground_count() == 2250);
    CHECK(db.item_count() >= 3500);
    CHECK(db.player_classes().size() == 14);
    CHECK(db.maps().contains("Nexus.jm"));
    CHECK(db.maps().contains("realm1-test.wmap"));
    CHECK(db.world("Nexus") != nullptr);
    CHECK(db.world("Realm") != nullptr);
    CHECK(db.world("Vault") != nullptr);
    CHECK(db.behavior_count() >= 140);
    // The original's XML has duplicates and dangling names: they are warnings, not errors (first definition wins).
    CHECK_FALSE(db.warnings().empty());
}

TEST_CASE("every class has its original starting gear, slot types and level increases") {
    const auto& db = real();
    for (const auto& [type, cls] : db.player_classes()) {
        CAPTURE(cls.id);
        REQUIRE(cls.equipment.size() == 12);
        CHECK(cls.equipment[4] == 0xa22);  // a Health Potion in the first inventory slot (Players.xml)
        CHECK(cls.slot_types.size() == 12);
        CHECK(cls.level_increase[static_cast<std::size_t>(Stat::MaxHp)].min == 20);
        CHECK(cls.level_increase[static_cast<std::size_t>(Stat::MaxHp)].max == 30);
    }
    const auto* wizard = db.player_class(0x030e);
    REQUIRE(wizard != nullptr);
    CHECK(wizard->id == "Wizard");
    CHECK(wizard->stats[static_cast<std::size_t>(Stat::MaxHp)].start == 100);
    CHECK(wizard->stats[static_cast<std::size_t>(Stat::MaxHp)].max == 670);
    CHECK(wizard->stats[static_cast<std::size_t>(Stat::MaxMp)].max == 385);
    CHECK(wizard->slot_types[0] == 17);  // staff
    CHECK(db.item(static_cast<std::uint16_t>(wizard->equipment[0]))->id == "Energy Staff");
    CHECK(db.item(static_cast<std::uint16_t>(wizard->equipment[1]))->id == "Fire Spray Spell");
    const auto* knight = db.player_class(0x031e);
    REQUIRE(knight != nullptr);
    CHECK(knight->stats[static_cast<std::size_t>(Stat::Defense)].max == 40);
}

TEST_CASE("item stats use the original's stat numbers; potions raise stats") {
    const auto& db = real();
    const auto* ring = db.item("Ring of Vitality");
    REQUIRE(ring != nullptr);
    CHECK(ring->stat_bonuses[static_cast<std::size_t>(Stat::HpRegen)] == 3);  // stat="26" is vitality
    const auto* life = db.item("Potion of Life");
    REQUIRE(life != nullptr);
    CHECK(life->stat_increments[static_cast<std::size_t>(Stat::MaxHp)] == 5);
    CHECK(life->bag_type == 5);
    CHECK(db.item("Health Potion")->heal_amount == 100);
    CHECK(db.item("Health Potion")->consumable);
    CHECK(db.loot_bag_type(0) == 0x0500);
    CHECK(db.loot_bag_type(5) == 0x0507);  // purple (the original InventoryUtils.GetBagIdFromType)
    CHECK(db.loot_bag_type(99) == 0x0500);
    CHECK_FALSE(db.tier_items(behavior::TierClass::Weapon, 1).empty());
    for (const auto t : db.tier_items(behavior::TierClass::Ring, 1)) CHECK(db.item(t)->slot_type == 9);
}

TEST_CASE("the Nexus is the original's, with spawn and realm portal regions") {
    const auto& db = real();
    const auto* nexus = db.map("Nexus.jm");
    REQUIRE(nexus != nullptr);
    CHECK(nexus->width == 142);
    std::size_t spawns = 0;
    std::size_t portals = 0;
    for (std::uint16_t y = 0; y < nexus->height; ++y) {
        for (std::uint16_t x = 0; x < nexus->width; ++x) {
            for (const auto& r : nexus->at(x, y).regions) {
                spawns += r == "Spawn" ? 1 : 0;
                portals += r == "Realm Portals" ? 1 : 0;
            }
        }
    }
    CHECK(spawns > 0);
    CHECK(portals > 0);
    const auto* world = db.world("Nexus");
    REQUIRE(world != nullptr);
    CHECK(world->id == -1);
    CHECK(world->maps == std::vector<std::string>{"Nexus.jm"});
}

TEST_CASE("the realm .wmap: 2048x2048, terrains, a spawn region, and only static objects") {
    const auto& db = real();
    const auto* realm = db.world("Realm");
    REQUIRE(realm != nullptr);
    CHECK(realm->maps.size() == 3);
    const auto* map = db.map("realm1-test.wmap");
    REQUIRE(map != nullptr);
    CHECK(map->width == 2048);
    CHECK(map->height == 2048);
    CHECK(map->tiles.size() == 2048u * 2048u);
    std::array<std::size_t, kTerrainCount> terrain{};
    std::size_t spawn_templates = 0;
    for (const auto& t : map->palette) {
        for (const auto& r : t.regions) spawn_templates += r == "Spawn" ? 1 : 0;
        if (t.object_type) {
            const auto* o = db.object(*t.object_type);
            REQUIRE(o != nullptr);
            CHECK(o->is_static);  // trees, rocks, flowers: no monster is placed in the map (the realm spawner places them)
        }
    }
    for (const auto index : map->tiles) terrain[static_cast<std::size_t>(map->palette[index].terrain)] += 1;
    CHECK(spawn_templates > 0);
    CHECK(terrain[static_cast<std::size_t>(Terrain::ShoreSand)] > 100000);
    CHECK(terrain[static_cast<std::size_t>(Terrain::Mountains)] > 100000);
}

TEST_CASE("a broken .wmap is a load error") {
    ContentDb db;
    const std::array<std::uint8_t, 1> empty{1};
    CHECK_FALSE(db.add_wmap("x.wmap", empty).has_value());
    const std::array<std::uint8_t, 6> garbage{1, 2, 3, 4, 5, 6};
    auto r = db.add_wmap("y.wmap", garbage);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("zlib") != std::string::npos);
    const std::array<std::uint8_t, 2> future{9, 0};
    CHECK_FALSE(db.add_wmap("z.wmap", future).has_value());
}

TEST_CASE("terrain monsters for the realm spawner come from <Terrain>, <SpawnProb> and <Spawn>") {
    const auto& db = real();
    const auto& low_sand = db.terrain_spawns(Terrain::LowSand);
    REQUIRE_FALSE(low_sand.empty());
    const auto* archer = db.object("Sandsman Archer");
    REQUIRE(archer != nullptr);
    CHECK(archer->terrain == Terrain::LowSand);
    CHECK(std::any_of(low_sand.begin(), low_sand.end(), [&](const TerrainSpawn& s) { return s.object_type == archer->type; }));
    bool grouped = false;
    for (std::size_t t = 1; t < kTerrainCount; ++t) {
        for (const auto& s : db.terrain_spawns(static_cast<Terrain>(t))) {
            CHECK(db.object(s.object_type)->enemy);
            grouped = grouped || s.group.max > 1;
        }
    }
    CHECK(grouped);  // e.g. <Spawn><Mean>5</Mean><StdDev>2</StdDev><Min>3</Min><Max>10</Max></Spawn>
    // Monsters keep their own XML projectiles (Shoot with a projectile index fires them).
    REQUIRE(archer->projectiles.contains(0));
    CHECK(archer->projectiles.at(0).object_id == "Green Arrow");
    CHECK(archer->projectiles.at(0).min_damage == 8);
}

TEST_CASE("realm behaviours are transpiled from the original: states, inline projectiles, loot") {
    const auto& db = real();
    const auto* pirate = db.behavior(db.object("Pirate")->type);
    REQUIRE(pirate != nullptr);
    REQUIRE(pirate->loot.size() == 1);
    CHECK(pirate->loot[0].is_public);  // the original's one active LootDrop
    REQUIRE(pirate->loot[0].items.size() == 1);
    CHECK(pirate->loot[0].items[0].item_type == 0xa22);
    CHECK(pirate->loot[0].items[0].threshold == doctest::Approx(0.03f));
    CHECK(pirate->loot[0].items[0].chance == doctest::Approx(0.9f));

    const auto* mage = db.behavior(db.object("Hobbit Mage")->type);
    REQUIRE(mage != nullptr);
    REQUIRE(mage->state_by_name.contains("ring1"));
    CHECK(mage->deep_state(0) == mage->state_by_name.at("idle"));
    const auto& ring1 = mage->states[static_cast<std::size_t>(mage->state_by_name.at("ring1"))];
    REQUIRE(ring1.scripts.size() == 1);
    const auto& shoot = std::get<behavior::Shoot>(ring1.scripts[0].kind);
    CHECK(shoot.count == 15);
    CHECK(shoot.shoot_angle == 24.0f);
    REQUIRE(shoot.projectile.size() == 1);
    CHECK(shoot.projectile[0].object_id == "Dark Blue Magic");
    CHECK(shoot.projectile[0].object_type == db.object("Dark Blue Magic")->type);
    CHECK(shoot.projectile[0].min_damage == 10);
    CHECK(shoot.projectile[0].speed == 5.0f);
    // The commented-out CharacterLoot table of the original became soulbound loot (TierLoot = a random item of a tier).
    REQUIRE_FALSE(mage->loot.empty());
    CHECK_FALSE(mage->loot[0].is_public);
    CHECK(std::any_of(mage->loot[0].items.begin(), mage->loot[0].items.end(), [](const behavior::LootItem& i) { return i.tier; }));
    // Every realm monster of the five realm files that the original gives a behaviour has one here.
    for (const char* id : {"Hobbit Archer", "Sumo Master", "Orc King", "Ogre King", "Medusa", "Ent God", "Snake", "Scorpion Queen"}) {
        CAPTURE(id);
        CHECK(db.behavior(db.object(id)->type) != nullptr);
    }
}

TEST_CASE("duplicate types and ids are skipped with a warning; malformed XML is an error") {
    ContentDb db;
    REQUIRE(db.add_xml(R"(<Objects><Object type="0x10" id="A"><Class>GameObject</Class></Object></Objects>)", "a.xml").has_value());
    REQUIRE(db.add_xml(R"(<Objects><Object type="0x10" id="B"/></Objects>)", "b.xml").has_value());
    CHECK(db.object(0x10)->id == "A");
    CHECK(db.object("B") == nullptr);
    CHECK(db.warnings().size() == 1);
    CHECK_FALSE(db.add_xml(R"(<Objects><Object type="zz" id="C"/></Objects>)", "d.xml").has_value());
    CHECK_FALSE(db.add_xml("<Objects><Object", "e.xml").has_value());
}

TEST_CASE("a map whose tile data does not match its size is a load error; unknown grounds are warnings") {
    ContentDb db;
    REQUIRE(db.add_xml(R"(<GroundTypes><Ground type="0x01" id="Grass"/></GroundTypes>)", "g.xml").has_value());
    // data holds one tile but the map claims 2x2.
    CHECK_FALSE(db.add_map("x.jm", R"({"width":2,"height":2,"dict":[{"ground":"Grass"}],"data":"eJxjYAAAAAIAAQ=="})").has_value());
    auto ok = db.add_map("y.jm", R"({"width":1,"height":1,"dict":[{"ground":"Grass"}],"data":"eJxjYAAAAAIAAQ=="})");
    REQUIRE(ok.has_value());
    CHECK(db.map("y.jm")->at(0, 0).ground_type == 1);
    REQUIRE(db.add_map("z.jm", R"({"width":1,"height":1,"dict":[{"ground":"Nope"}],"data":"eJxjYAAAAAIAAQ=="})").has_value());
    CHECK_FALSE(db.warnings().empty());
}

TEST_CASE("behaviour files: unknown primitives and states are warnings, the rest still runs") {
    ContentDb db;
    REQUIRE(db.add_xml(R"(<Objects>
        <Object type="0x20" id="Bolt"><Class>Projectile</Class></Object>
        <Object type="0x21" id="Imp"><Class>Character</Class><Enemy/><MaxHitPoints>10</MaxHitPoints></Object>
    </Objects>)", "o.xml").has_value());
    REQUIRE(db.add_behaviors(R"({"format": 1, "behaviors": {"Imp": {"root": {
        "scripts": [ {"type": "Wander", "speed": 2}, {"type": "Dance"} ],
        "states": [ {"name": "a", "transitions": [ {"type": "Timed", "time": 100, "to": "b"}, {"type": "Timed", "time": 1, "to": "nowhere"} ]},
                    {"name": "b", "scripts": [ {"type": "Shoot", "maxRadius": 5, "targeted": true,
                                               "projectile": {"id": "Bolt", "damage": 3, "path": [{"type": "Line", "speed": 4}]}} ]} ] }}}})",
                                "imp.json")
                .has_value());
    REQUIRE(db.link().has_value());
    const auto* b = db.behavior(0x21);
    REQUIRE(b != nullptr);
    CHECK(b->states.size() == 3);
    CHECK(b->states[1].transitions.size() == 1);  // the transition to "nowhere" was dropped
    CHECK(std::holds_alternative<behavior::NoOp>(b->states[0].scripts[1].kind));
    CHECK(std::get<behavior::Shoot>(b->states[2].scripts[0].kind).projectile[0].object_type == 0x20);
    CHECK(db.warnings().size() >= 2);
    CHECK_FALSE(db.add_behaviors(R"({"format": 2, "behaviors": {}})", "future.json").has_value());
}
