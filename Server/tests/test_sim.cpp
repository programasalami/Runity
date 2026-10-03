#include <doctest/doctest.h>

#include <algorithm>

#include "sim_fixtures.hpp"
#include "waw/sim/world.hpp"

using namespace waw;
using namespace waw::sim;

namespace {

constexpr std::int8_t kEast = 127;
constexpr std::int8_t kWest = -127;

Vec2 walk(const TileMap& map, Vec2 start, std::int8_t dx, std::int8_t dy, int ms, int speed = 0) {
    movement::MoverState s;
    s.position = start;
    for (int t = 0; t < ms; t += 16) movement::step(map, s, movement::direction_from_input(dx, dy), 16.0f, speed);
    return s.position;
}

}  // namespace

TEST_CASE("move speed keeps the reference's integer division") {
    CHECK(movement::move_speed(0, 1.0f) == doctest::Approx(0.004f));
    CHECK(movement::move_speed(74, 1.0f) == doctest::Approx(0.004f));
    CHECK(movement::move_speed(75, 1.0f) == doctest::Approx(0.0096f));
    CHECK(movement::move_speed(150, 1.0f) == doctest::Approx(0.0152f));
    CHECK(movement::move_speed(75, 0.5f) == doctest::Approx(0.0048f));
}

TEST_CASE("input directions are normalised, never longer than one") {
    const auto d = movement::direction_from_input(127, 127);
    CHECK(d.x * d.x + d.y * d.y == doctest::Approx(1.0f));
    const auto e = movement::direction_from_input(127, 0);
    CHECK(e.x == doctest::Approx(1.0f));
    const auto z = movement::direction_from_input(0, 0);
    CHECK(z.x == 0.0f);
    CHECK(z.y == 0.0f);
    const auto worst = movement::direction_from_input(-128, -128);
    CHECK(worst.x * worst.x + worst.y * worst.y == doctest::Approx(1.0f));
}

TEST_CASE("open ground: 4 tiles per second at Speed below 75") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"................"}), db);
    const auto end = walk(map, {2.5f, 0.5f}, kEast, 0, 1008);  // 63 steps of 16 ms
    CHECK(end.x == doctest::Approx(2.5f + 0.004f * 1008).epsilon(0.001));
    CHECK(end.y == doctest::Approx(0.5f));
}

TEST_CASE("a wall stops the body at the half tile before it") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"..........#....."}), db);
    const auto end = walk(map, {2.5f, 0.5f}, kEast, 0, 5000);
    CHECK(end.x == doctest::Approx(9.5f));
    const auto back = walk(map, {13.5f, 0.5f}, kWest, 0, 5000);
    CHECK(back.x == doctest::Approx(11.5f));
}

TEST_CASE("water stops movement at the tile edge; a rock blocks its square") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"......~....o...."}), db);
    const auto end = walk(map, {2.5f, 0.5f}, kEast, 0, 5000);
    CHECK(end.x == doctest::Approx(5.99f));
    const auto rock = walk(map, {8.5f, 0.5f}, kEast, 0, 5000);
    CHECK(rock.x == doctest::Approx(10.99f));
}

TEST_CASE("the map edge and void tiles block") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"....", "....", "....", "  .."}), db);
    const auto north = walk(map, {1.5f, 1.5f}, 0, -127, 2000);
    CHECK(north.y == doctest::Approx(0.5f));  // the edge counts as FullOccupy: the body stays half a tile in
    const auto south = walk(map, {0.5f, 1.5f}, 0, 127, 2000);
    CHECK(south.y == doctest::Approx(2.5f));  // the void below
}

TEST_CASE("diagonal movement slides along a wall") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"..........", "..........", "##########"}), db);
    const auto end = walk(map, {2.5f, 0.5f}, kEast, 127, 1000);
    CHECK(end.y == doctest::Approx(1.5f));
    CHECK(end.x > 4.0f);
}

TEST_CASE("sinking ground slows down over time and recovers off it") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"ssssssssssssssssssssssss"}), db);
    movement::MoverState s;
    s.position = {0.5f, 0.5f};
    const auto dir = movement::direction_from_input(kEast, 0);
    movement::step(map, s, dir, 16.0f, 0);
    const float first = s.position.x - 0.5f;
    for (int i = 0; i < 100; ++i) movement::step(map, s, dir, 16.0f, 0);
    const float before = s.position.x;
    movement::step(map, s, dir, 16.0f, 0);
    const float later = s.position.x - before;
    CHECK(s.sink_level == doctest::Approx(movement::kMaxSinkLevel));
    CHECK(later < first);
    CHECK(later == doctest::Approx(0.004f * 0.1f * 16.0f));
}

TEST_CASE("static walls become tile objects, portals become entities") {
    const auto db = test::fixture_content();
    const auto map = TileMap::build(test::fixture_map(db, {"..#..P.."}), db);
    CHECK(map.at(2, 0).object == db.object("Wall")->type);
    CHECK((map.at(2, 0).flags & tile_flags::FullOccupy) != 0);
    CHECK((map.at(2, 0).flags & tile_flags::BlocksSight) != 0);
    CHECK(map.at(5, 0).object == 0);
    REQUIRE(map.entity_placements().size() == 1);
    CHECK(map.entity_placements()[0].position == Vec2{5.5f, 0.5f});
}

namespace {

struct WorldFixture {
    content::ContentDb db = test::fixture_content();
    content::MapData map_data;
    content::WorldConfig config = test::fixture_world_config();
    std::unique_ptr<World> world;

    explicit WorldFixture(std::vector<std::string> rows, WorldRules rules = {}) {
        map_data = test::fixture_map(db, rows);
        world = std::make_unique<World>(1, config, map_data, db, 42, rules);
    }

    EntityId add(std::uint32_t session, std::string name = "Bob") {
        return world->add_player({.session_id = session, .character_id = 1, .class_type = 0x030e, .name = std::move(name),
                                  .level = 1, .hp = 100, .max_hp = 100, .speed = 0});
    }

    std::vector<ViewUpdate> tick(float ms = 50.0f) {
        std::vector<ViewUpdate> out;
        world->tick(ms, out);
        return out;
    }

    static const ViewUpdate& for_session(const std::vector<ViewUpdate>& v, std::uint32_t session) {
        auto it = std::find_if(v.begin(), v.end(), [&](const ViewUpdate& u) { return u.session_id == session; });
        REQUIRE(it != v.end());
        return *it;
    }
};

std::vector<MoveCommand> east_steps(std::uint32_t first_seq, int count) {
    std::vector<MoveCommand> v;
    for (int i = 0; i < count; ++i) v.push_back({first_seq + static_cast<std::uint32_t>(i), 16, kEast, 0});
    return v;
}

}  // namespace

TEST_CASE("a new player spawns on a Spawn tile and receives its surroundings") {
    WorldFixture f({"........", "...S....", "........"});
    const auto id = f.add(1);
    const auto* e = f.world->find(id);
    REQUIRE(e != nullptr);
    CHECK(e->position == Vec2{3.5f, 1.5f});
    const auto updates = f.tick();
    const auto& u = WorldFixture::for_session(updates, 1);
    CHECK(u.tiles.size() == 24);
    REQUIRE(u.entered.size() == 1);
    CHECK(u.entered[0]->id == id);
    CHECK(u.left.empty());
    // Tiles are sent once.
    const auto again = f.tick();
    CHECK(WorldFixture::for_session(again, 1).tiles.empty());
    CHECK(WorldFixture::for_session(again, 1).entered.empty());
}

TEST_CASE("movement inputs are applied and acknowledged") {
    WorldFixture f({"S..............................."});
    const auto id = f.add(1);
    f.tick();
    const auto steps = east_steps(1, 3);
    CHECK(f.world->queue_input(id, steps) == World::InputResult::Accepted);
    const auto updates = f.tick();
    const auto& u = WorldFixture::for_session(updates, 1);
    CHECK(u.ack_input_seq == 3);
    REQUIRE(u.changed.size() == 1);
    CHECK((u.changed[0].fields & dirty::Position) != 0);
    CHECK(f.world->find(id)->position.x == doctest::Approx(0.5f + 0.004f * 48).epsilon(0.001));
}

TEST_CASE("a speed hack cannot spend more input time than real time allows") {
    WorldRules rules;
    rules.movement_slack_ms = 100.0f;
    WorldFixture f({"S..............................................................."}, rules);
    const auto id = f.add(1);
    const auto steps = east_steps(1, 125);  // 2 seconds of input sent at once
    REQUIRE(f.world->queue_input(id, steps) == World::InputResult::Accepted);
    f.tick(50.0f);
    // At most tick (50) + slack (100) ms of movement was applied: 9 steps of 16 ms.
    const float moved = f.world->find(id)->position.x - 0.5f;
    CHECK(moved <= 0.004f * 160.0f + 0.001f);
    CHECK(moved > 0.0f);
    // The rest arrives over real time, in order.
    for (int i = 0; i < 40; ++i) f.tick(50.0f);
    CHECK(f.world->find(id)->position.x == doctest::Approx(0.5f + 0.004f * 2000).epsilon(0.001));
}

TEST_CASE("input floods and replays are reported") {
    WorldFixture f({"S..."});
    const auto id = f.add(1);
    CHECK(f.world->queue_input(id, east_steps(1, 300)) == World::InputResult::Flooding);
    WorldFixture g({"S..."});
    const auto id2 = g.add(1);
    REQUIRE(g.world->queue_input(id2, east_steps(5, 2)) == World::InputResult::Accepted);
    CHECK(g.world->queue_input(id2, east_steps(6, 1)) == World::InputResult::OutOfOrder);
}

TEST_CASE("players see each other enter, move and leave") {
    WorldFixture f({"S...S..................................................."});
    const auto a = f.add(1, "Amy");
    f.tick();
    const auto b = f.add(2, "Bob");
    auto u = f.tick();
    const auto& seen_by_a = WorldFixture::for_session(u, 1);
    REQUIRE(seen_by_a.entered.size() == 1);
    CHECK(seen_by_a.entered[0]->id == b);
    CHECK(seen_by_a.entered[0]->name == "Bob");
    CHECK(WorldFixture::for_session(u, 2).entered.size() == 2);

    REQUIRE(f.world->queue_input(b, east_steps(1, 3)) == World::InputResult::Accepted);
    u = f.tick();
    const auto& moved = WorldFixture::for_session(u, 1);
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].entity->id == b);

    f.world->remove(b);
    u = f.tick();
    const auto& gone = WorldFixture::for_session(u, 1);
    REQUIRE(gone.left.size() == 1);
    CHECK(gone.left[0] == b);
    CHECK(f.world->find(a) != nullptr);
}

TEST_CASE("entities beyond the sight radius are not sent") {
    WorldRules rules;
    rules.sight_radius = 5.0f;
    WorldFixture f({"S............S"}, rules);
    // Two spawn tiles 13 apart; the RNG decides which each player gets, so place them explicitly by moving one.
    const auto a = f.add(1);
    const auto b = f.add(2);
    f.world->find(a)->player->mover.position = {0.5f, 0.5f};
    f.world->find(a)->position = {0.5f, 0.5f};
    f.world->find(b)->player->mover.position = {13.5f, 0.5f};
    f.world->find(b)->position = {13.5f, 0.5f};
    const auto u = f.tick();
    CHECK(WorldFixture::for_session(u, 1).entered.size() == 1);
    CHECK(WorldFixture::for_session(u, 2).entered.size() == 1);
}

TEST_CASE("a removed entity's id is not reused by the next entity") {
    WorldFixture f({"S..."});
    const auto a = f.add(1);
    f.world->remove(a);
    const auto b = f.add(2);
    CHECK(a != b);
    CHECK(f.world->find(a) == nullptr);
    CHECK(f.world->find(b) != nullptr);
}

TEST_CASE("map portals exist as entities when the world starts") {
    WorldFixture f({"S..P"});
    CHECK(f.world->entity_count() == 1);
    f.add(1);
    const auto u = f.tick();
    CHECK(WorldFixture::for_session(u, 1).entered.size() == 2);
}
