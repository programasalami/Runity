// The behaviour engine running transpiled original realm monsters (Content/Behaviors/*.json).

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include "real_fixtures.hpp"

using namespace runity;
using namespace runity::sim;
using namespace runity::test;

namespace {

std::string state_of(World& w, EntityId id) {
    const auto* e = w.find(id);
    REQUIRE(e != nullptr);
    REQUIRE(e->enemy);
    REQUIRE(e->enemy->behavior != nullptr);
    return e->enemy->behavior->states[static_cast<std::size_t>(e->enemy->current_state)].name;
}

std::vector<std::string> field(int rows, int width) {
    return std::vector<std::string>(static_cast<std::size_t>(rows), std::string(static_cast<std::size_t>(width), '.'));
}

}  // namespace

TEST_CASE("a Hobbit Mage idles until a player comes within 12 tiles, then fires its 15-bullet rings in turn") {
    Arena a(field(3, 40));
    const auto mage = a.world->add_object(type_of("Hobbit Mage"), {5.5f, 1.5f});
    a.run(500.0f);
    CHECK(state_of(*a.world, mage) == "idle");
    CHECK(a.world->take_volleys().empty());

    const Vec2 mage_at = a.world->find(mage)->position;
    (void)a.wizard({mage_at.x + 8.0f, mage_at.y});  // within EntityWithinTransition("ring1", radius: 12)
    a.run(100.0f);
    CHECK(state_of(*a.world, mage) == "ring1");
    a.run(450.0f);
    CHECK(state_of(*a.world, mage) == "ring2");
    std::vector<VolleyEvent> rings;
    for (const auto& v : a.world->take_volleys()) {
        if (v.enemy && v.owner == mage) rings.push_back(v);
    }
    REQUIRE_FALSE(rings.empty());
    CHECK(rings[0].count == 15);
    CHECK(rings[0].angle_step == doctest::Approx(24.0f * std::numbers::pi_v<float> / 180.0f));
    CHECK(rings[0].angle == doctest::Approx(0.0f));  // fixed angle 0: not aimed
    CHECK(rings[0].projectile_type == type_of("Dark Blue Magic"));
    CHECK(rings[0].path.speed == doctest::Approx(5.0f));
    CHECK(rings[0].path.lifetime_ms == 340);
}

TEST_CASE("a Hobbit Mage calls its archers and rogues, at most as many as maxSpawnsPerReset, and they protect it") {
    Arena a(field(30, 40));
    const auto mage = a.world->add_object(type_of("Hobbit Mage"), {20.5f, 15.5f});
    a.run(60000.0f);
    const auto archers = a.all(type_of("Hobbit Archer"));
    const auto rogues = a.all(type_of("Hobbit Rogue"));
    CHECK(archers.size() == 4);  // Spawn("Hobbit Archer", maxSpawnsPerReset: 4, cooldownMs: 12000)
    CHECK(rogues.size() == 3);
    for (const auto* r : rogues) {
        CHECK(r->enemy->parent == mage);
        // Protect(7.38, "Hobbit Mage", 15, 9, 2.5): never far from the mage.
        CHECK(std::sqrt(distance_squared(r->position, a.world->find(mage)->position)) < 12.0f);
    }
}

TEST_CASE("a Sumo Master wakes up when hurt, calls Lil Sumos, and enrages below half health") {
    Arena a(field(20, 30));
    const auto sumo = a.world->add_object(type_of("Sumo Master"), {15.5f, 10.5f});
    a.run(1500.0f);
    const auto asleep = state_of(*a.world, sumo);
    CHECK((asleep == "sleeping1" || asleep == "sleeping2"));
    auto* s = a.world->find(sumo);
    a.world->damage(*s, 10, true, EntityId(), "test");
    a.run(100.0f);
    CHECK(state_of(*a.world, sumo) == "hurt");
    a.run(1000.0f);
    CHECK(state_of(*a.world, sumo) == "awake");
    CHECK(a.all(type_of("Lil Sumo")).size() >= 4);  // Spawn("Lil Sumo", cooldownMs: 200) for one second
    a.world->damage(*a.world->find(sumo), a.world->find(sumo)->max_hp / 2, true, EntityId(), "test");
    a.run(100.0f);
    const auto rage = state_of(*a.world, sumo);
    CHECK((rage == "shoot" || rage == "rest"));  // "rage" enters its first child
}

TEST_CASE("Taunt speaks to the players around; Wander keeps a monster near home") {
    Arena a(field(20, 30));
    const auto me = a.wizard({2.5f, 2.5f});
    const auto sumo = a.world->add_object(type_of("Sumo Master"), {8.5f, 8.5f});
    a.world->damage(*a.world->find(sumo), 200, true, EntityId(), "test");  // below half: straight to "rage" after waking
    a.run(3000.0f);
    bool taunted = false;
    for (const auto& t : a.world->take_taunts()) {
        taunted = taunted || (t.text == "Engaging Super-Mode!!!" && t.speaker == "Sumo Master" &&
                              std::find(t.to.begin(), t.to.end(), me) != t.to.end());
    }
    CHECK(taunted);

    Arena b(field(30, 30));
    const auto snake = b.world->add_object(type_of("Snake"), {15.5f, 15.5f});
    float farthest = 0.0f;
    bool moved = false;
    for (int i = 0; i < 200; ++i) {
        b.run(50.0f);
        const auto p = b.world->find(snake)->position;
        moved = moved || !(p == Vec2{15.5f, 15.5f});
        farthest = std::max(farthest, std::sqrt(distance_squared(p, {15.5f, 15.5f})));
    }
    CHECK(moved);
    CHECK(farthest < 10.0f);  // Wander(2.94): distanceFromSpawn 5 around where its state began
}

TEST_CASE("death scripts: a Big Green Slime splits into four Little Green Slimes; Order and parent links work") {
    Arena a(field(20, 30));
    const auto slime = a.world->add_object(type_of("Big Green Slime"), {10.5f, 10.5f});
    a.world->damage(*a.world->find(slime), 100000, true, EntityId(), "test");
    a.run(50.0f);
    CHECK(a.world->find(slime) == nullptr);
    CHECK(a.all(type_of("Little Green Slime")).size() == 4);  // four TransformOnDeath("Little Green Slime")

    const auto bunny = a.world->add_object(type_of("Easily Enraged Bunny"), {20.5f, 5.5f});
    a.world->damage(*a.world->find(bunny), 100000, true, EntityId(), "test");
    a.run(50.0f);
    CHECK(a.all(type_of("Enraged Bunny")).size() == 1);
}

TEST_CASE("an invulnerable trap cannot be hurt, poisons with Weak, and removes itself (Suicide)") {
    Arena a(field(10, 30));
    const auto me = a.wizard({13.5f, 5.5f});  // east of it: its Web Threads fly at a fixed angle of 0
    const auto trap = a.world->add_object(type_of("Left Horizontal Trap"), {11.5f, 5.5f});
    REQUIRE(a.world->find(trap)->has(content::Condition::Invulnerable));
    const int hp = a.world->find(trap)->hp;
    a.world->damage(*a.world->find(trap), 1000, true, me, "Wiz1");
    CHECK(a.world->find(trap)->hp == hp);
    bool weakened = false;
    for (int i = 0; i < 30 && !weakened; ++i) {
        a.world->find(me)->hp = a.world->find(me)->max_hp;
        a.run(100.0f);
        weakened = a.world->find(me)->has(content::Condition::Weak);
    }
    CHECK(weakened);  // its Web Thread shots carry (Weak, 6000)
    a.run(6500.0f);
    CHECK(a.world->find(trap) == nullptr);  // Suicide(6000)
}

TEST_CASE("public loot: a Pirate drops Health Potions in shared bags, rolled once per damage record") {
    Arena a(field(3, 40));
    const auto me = a.wizard({1.5f, 1.5f});
    int potions = 0;
    int bags = 0;
    for (int i = 0; i < 40; ++i) {
        const auto pirate = a.world->add_object(type_of("Pirate"), {20.5f, 1.5f});
        a.world->damage(*a.world->find(pirate), 1000, true, me, "Wiz1");
        a.run(50.0f);
    }
    a.world->for_each([&](Entity& e) {
        if (!e.container) return;
        ++bags;
        CHECK(e.container->owner_account == 0);  // LootDrop(true): public
        CHECK(e.object_type == real().loot_bag_type(0));
        for (const int it : e.container->items) potions += it == 0xa22 ? 1 : 0;
    });
    CHECK(bags > 20);       // 90 % of 40 kills
    CHECK(potions == bags);  // one damage record: one potion a bag
}

TEST_CASE("soulbound loot from the original's loot tables: per account, only for players who did enough damage") {
    Arena a(field(3, 40));
    const auto me = a.wizard({1.5f, 1.5f}, 1, 11, 1);
    const auto other = a.wizard({1.5f, 2.5f}, 1, 22, 2);
    for (int i = 0; i < 60; ++i) {
        const auto mage = a.world->add_object(type_of("Hobbit Mage"), {20.5f, 1.5f});
        a.world->damage(*a.world->find(mage), 1, true, other, "Wiz2");  // 1 of 200 HP: under the 1 % threshold
        a.world->damage(*a.world->find(mage), 100000, true, me, "Wiz1");
        a.run(50.0f);
    }
    int mine = 0;
    int theirs = 0;
    a.world->for_each([&](Entity& e) {
        if (!e.container) return;
        mine += e.container->owner_account == 11 ? 1 : 0;
        theirs += e.container->owner_account == 22 ? 1 : 0;
        for (const int it : e.container->items) {
            if (it < 0) continue;
            const auto* d = real().item(static_cast<std::uint16_t>(it));
            REQUIRE(d != nullptr);
            CHECK((d->tier <= 2 || d->id == "Health Potion" || d->id == "Magic Potion"));  // TierLoot 1-2 + potions
        }
    });
    CHECK(mine > 10);
    CHECK(theirs == 0);
}

TEST_CASE("every transpiled behaviour runs next to a player without breaking the world") {
    Arena a(field(40, 60));
    const auto me = a.wizard({30.5f, 20.5f});
    int placed = 0;
    for (std::uint32_t t = 1; t < 0xFFFF; ++t) {
        const auto* b = real().behavior(static_cast<std::uint16_t>(t));
        if (b == nullptr) continue;
        (void)a.world->add_object(b->object_type, {5.5f + static_cast<float>(placed % 50), 5.5f + static_cast<float>(placed / 50 % 30)});
        ++placed;
    }
    CHECK(placed >= 140);
    for (int i = 0; i < 100; ++i) {
        if (auto* p = a.world->find(me)) p->hp = p->max_hp;
        a.run(50.0f);
    }
    CHECK(a.world->entity_count() > 0);
}
