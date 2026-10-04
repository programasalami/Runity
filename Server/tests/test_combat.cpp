#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include "real_fixtures.hpp"
#include "runity/sim/world.hpp"

using namespace runity;
using namespace runity::sim;
using namespace runity::test;

TEST_CASE("rules: the original's XP curve, kill XP with its cap, fame, defense floor and attack") {
    CHECK(rules::kMaxLevel == 20);
    CHECK(rules::xp_to_next_level(1) == 50);
    CHECK(rules::xp_to_next_level(2) == 170);    // (int)(50 + 1 * 100 * 1.2)
    CHECK(rules::xp_to_next_level(10) == 1850);  // 50 + 9 * 100 * 2
    CHECK(rules::xp_to_next_level(20) == 5750);
    CHECK(rules::xp_for_kill(5, 1.0f) == 1);     // a Pirate: ceil(5 / 10)
    CHECK(rules::xp_for_kill(200, 1.6f) == 32);  // a Hobbit Mage
    CHECK(rules::xp_for_kill(0, 1.0f) == 0);
    CHECK(rules::capped_xp(32, 1) == 5);         // at most 10 % of the next level's goal
    CHECK(rules::capped_xp(32, 5) == 32);
    CHECK(rules::kXpPerFame == 500);
    CHECK(rules::after_defense(100, 30, false) == 70);
    CHECK(rules::after_defense(100, 95, false) == 15);       // at least 15 %
    CHECK(rules::after_defense(100, 30, false, true) == 40); // Armored doubles defense
    CHECK(rules::after_defense(100, 95, true) == 100);       // armor piercing
    CHECK(rules::after_defense(0, 0, false) == 0);
    CHECK(rules::damage_with_attack(20, 12) == 15);          // 20 * (0.5 + 12 / 50) = 14.8
    CHECK(rules::damage_with_attack(20, 12, true) == 10);    // Weak: half
    CHECK(rules::damage_with_attack(20, 12, false, true) == 22);  // Damaging: x1.5
    CHECK(rules::attack_period_ms(15, 1.0f) == doctest::Approx(357.14f).epsilon(0.001));
}

TEST_CASE("level-ups grow stats by the class's LevelIncrease, never past its maximum") {
    const auto& wizard = *real().player_class(kWizard);
    std::mt19937_64 rng(5);
    auto stats = rules::starting_stats(wizard);
    CHECK(stats[0] == 100);
    const auto start = stats;
    rules::level_up(stats, wizard, rng);
    CHECK(stats[0] >= start[0] + 20);
    CHECK(stats[0] <= start[0] + 30);
    CHECK(stats[1] >= start[1] + 5);   // MP 5-15 for a wizard
    CHECK(stats[3] == start[3]);       // defense never grows (LevelIncrease max 0)
    for (int i = 0; i < 200; ++i) rules::level_up(stats, wizard, rng);
    for (std::size_t s = 0; s < content::kStatCount; ++s) CHECK(stats[s] <= wizard.stats[s].max);
    CHECK(stats[0] == 670);
}

TEST_CASE("the fire-rate bucket allows a burst of three, then the attack rate") {
    rules::FireRateBucket b;
    CHECK(b.try_attack(0, 300));
    CHECK(b.try_attack(1, 300));
    CHECK(b.try_attack(2, 300));
    CHECK_FALSE(b.try_attack(3, 300));
    CHECK(b.try_attack(300, 300));  // refilled 1.3 attacks
}

TEST_CASE("projectile paths") {
    const PathSpec line{content::PathKind::Line, 10.0f, 1000, 0, 1};
    auto p = path_offset(line, 500, 0, 0.0f);
    CHECK(p.x == doctest::Approx(5.0f));
    CHECK(p.y == doctest::Approx(0.0f));
    const PathSpec amp{content::PathKind::Amplitude, 10.0f, 1000, 0.5f, 2.0f};
    CHECK(path_offset(amp, 125, 0, 0.0f).y == doctest::Approx(0.5f));   // sin(2 * pi * 0.25) at the wave's peak
    CHECK(path_offset(amp, 125, 1, 0.0f).y == doctest::Approx(-0.5f));  // paired bullets mirror
    const PathSpec boom{content::PathKind::Boomerang, 10.0f, 1000, 0, 1};
    CHECK(path_offset(boom, 500, 0, 0.0f).x == doctest::Approx(5.0f));
    CHECK(path_offset(boom, 1000, 0, 0.0f).x == doctest::Approx(0.0f));
}

TEST_CASE("a wizard shoots a hobbit mage dead; every player nearby gets the capped XP, far ones none") {
    Arena a({"S......................................................", std::string(55, '.')}, WorldRules{.active_radius = 0.0f});
    const auto me = a.wizard({2.5f, 0.5f}, 1, 1, 1);
    const auto friend_near = a.wizard({3.5f, 1.5f}, 5, 2, 2);  // never shoots
    const auto far = a.wizard({52.5f, 1.5f}, 5, 3, 3);       // 50 tiles away: farther than sqrt(2) * sight
    const auto mage = a.world->add_object(type_of("Hobbit Mage"), {8.5f, 0.5f});
    REQUIRE(a.world->find(mage)->enemy.has_value());
    a.world->find(mage)->enemy->behavior = nullptr;  // no hobbit escort to steal the kill: this test is about XP sharing
    a.world->find(mage)->hp = 1;
    std::uint16_t shot = 0;
    for (int i = 0; i < 40 && a.world->find(mage) != nullptr; ++i) {
        a.world->find(me)->hp = a.world->find(me)->max_hp;  // the test is about XP, not survival
        (void)a.world->shoot(me, shot++, std::atan2(a.world->find(mage)->position.y - 0.5f, a.world->find(mage)->position.x - 2.5f));
        a.run(100.0f);
    }
    REQUIRE(a.world->find(mage) == nullptr);
    CHECK(a.world->find(me)->player->xp == 5);          // 32 XP capped at 10 % of 50
    CHECK(a.world->find(friend_near)->player->xp == 32);  // level 5: under the cap; no damage needed
    CHECK(a.world->find(far)->player->xp == 0);
}

TEST_CASE("a pirate fights back and its blades hurt; a player killed by monsters dies once") {
    Arena a({"S..............................", "..............................."});
    const auto me = a.wizard({2.5f, 0.5f});
    (void)a.world->add_object(type_of("Pirate"), {3.5f, 1.5f});
    a.run(3000.0f);
    const auto* wizard = a.world->find(me);
    REQUIRE(wizard != nullptr);
    CHECK(wizard->hp < wizard->max_hp);
    bool enemy_volley = false;
    for (const auto& v : a.world->take_volleys()) enemy_volley = enemy_volley || (v.enemy && v.projectile_type == type_of("Blade"));
    CHECK(enemy_volley);

    a.world->find(me)->hp = 1;
    (void)a.world->add_object(type_of("Pirate"), {2.5f, 1.5f});
    (void)a.world->add_object(type_of("Pirate"), {1.5f, 1.5f});
    a.run(5000.0f);
    CHECK(a.world->find(me) == nullptr);
    const auto deaths = a.world->take_deaths();
    REQUIRE(deaths.size() == 1);
    CHECK(deaths[0].killed_by == "Pirate");
}

TEST_CASE("walls stop bullets") {
    Arena a({"S...#.#.#......", "......###......"});
    const auto me = a.wizard({1.5f, 0.5f});
    const auto egg = a.world->add_object(type_of("Hobbit Mage"), {7.5f, 0.5f});
    a.world->find(egg)->enemy->behavior = nullptr;  // stands still
    std::uint16_t shot = 0;
    for (int i = 0; i < 10; ++i) {
        (void)a.world->shoot(me, shot++, 0.0f);
        a.run(400.0f);
    }
    REQUIRE(a.world->find(egg) != nullptr);
    CHECK(a.world->find(egg)->hp == a.world->find(egg)->max_hp);
}

TEST_CASE("shooting needs a weapon and respects the fire rate") {
    Arena a({"S...."});
    PlayerSpawn s;
    s.session_id = 2;
    s.class_type = kWizard;
    s.name = "Bare";
    s.position = Vec2{1.5f, 0.5f};
    const auto bare = a.world->add_player(s);
    CHECK(a.world->shoot(bare, 0, 0.0f) == World::ShootResult::NoWeapon);
    const auto me = a.wizard({2.5f, 0.5f});
    CHECK(a.world->shoot(me, 0, 0.0f) == World::ShootResult::Fired);
    CHECK(a.world->shoot(me, 1, 0.0f) == World::ShootResult::Fired);
    CHECK(a.world->shoot(me, 2, 0.0f) == World::ShootResult::Fired);
    CHECK(a.world->shoot(me, 3, 0.0f) == World::ShootResult::TooFast);
}

TEST_CASE("enough XP levels up, grows the stats, heals fully, earns fame; the cap is level 20") {
    Arena a({"S...."});
    const auto me = a.wizard({1.5f, 0.5f});
    auto* e = a.world->find(me);
    e->hp = 10;
    const auto base_before = e->player->base;
    a.world->award_xp(*e, 50 + 7);
    CHECK(e->level == 2);
    CHECK(e->player->xp == 7);  // progress within the new level
    CHECK(e->player->base[0] >= base_before[0] + 20);
    CHECK(e->max_hp == e->player->base[0]);  // no HP gear at the start
    CHECK(e->hp == e->max_hp);
    CHECK(e->player->fame == 0);
    a.world->award_xp(*e, 1000);
    CHECK(e->player->fame == 2);  // 1057 XP in total: 1 fame per 500
    for (int i = 0; i < 100; ++i) a.world->award_xp(*e, 100000);
    CHECK(e->level == 20);
    CHECK(e->max_hp <= 670);
}

TEST_CASE("players regenerate over time") {
    Arena a({"S...."});
    const auto me = a.wizard({1.5f, 0.5f});
    auto* e = a.world->find(me);
    e->hp = 50;
    a.run(2000.0f);
    // Wizard vitality 12 at level 1: 1 + 0.12 * 12 = 2.44 HP a second.
    CHECK(a.world->find(me)->hp == 54);
}

TEST_CASE("condition effects: Armored halves what gets through, Invulnerable takes nothing, timers expire") {
    Arena a({"S......"});
    const auto mage = a.world->add_object(type_of("Hobbit Mage"), {4.5f, 0.5f});
    auto* m = a.world->find(mage);
    m->enemy->behavior = nullptr;
    const int hp = m->hp;
    a.world->damage(*m, 20, false, EntityId(), "test");  // defense 2
    CHECK(m->hp == hp - 18);
    a.world->apply_condition(*m, content::Condition::Armored, 1000);
    a.world->damage(*m, 20, false, EntityId(), "test");
    CHECK(m->hp == hp - 18 - 16);
    a.world->apply_condition(*m, content::Condition::Invulnerable, -1);
    a.world->damage(*m, 50, true, EntityId(), "test");
    CHECK(m->hp == hp - 34);
    CHECK(m->has(content::Condition::Armored));
    a.run(1100.0f);
    CHECK_FALSE(a.world->find(mage)->has(content::Condition::Armored));
    CHECK(a.world->find(mage)->has(content::Condition::Invulnerable));
}
