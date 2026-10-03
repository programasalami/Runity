#include <doctest/doctest.h>

#include "real_fixtures.hpp"

using namespace waw;
using namespace waw::test;

namespace {

std::vector<EntityId> bags(World& w) {
    std::vector<EntityId> out;
    w.for_each([&](Entity& e) {
        if (e.container) out.push_back(e.id);
    });
    return out;
}

}  // namespace

TEST_CASE("worn gear adds its bonuses; the class's slot rules apply") {
    Arena a({"S......"});
    const auto me = a.wizard({1.5f, 0.5f});
    auto* e = a.world->find(me);
    // The Wizard starts with an Energy Staff, a Fire Spray Spell and a Health Potion; no robe, no ring (Players.xml).
    const int vitality = e->player->stats.vitality;
    e->player->items[5] = item_type("Ring of Vitality");  // +3 vitality (stat 26)
    CHECK(a.world->swap_items(me, me, 5, me, 3) == World::ItemResult::Done);
    CHECK(e->player->items[3] == item_type("Ring of Vitality"));
    CHECK(e->player->stats.vitality == vitality + 3);
    e->player->items[6] = item_type("Ring of Health");  // +40 max HP
    CHECK(a.world->swap_items(me, me, 6, me, 3) == World::ItemResult::Done);
    CHECK(e->max_hp == 140);
    CHECK(e->player->stats.vitality == vitality);

    e->player->items[7] = item_type("Iron Mail");  // a warrior's armour: not a robe slot
    CHECK(a.world->swap_items(me, me, 7, me, 2) == World::ItemResult::NotAllowed);
    CHECK(a.world->swap_items(me, me, 7, me, 12) == World::ItemResult::NotAllowed);  // the backpack is locked without one
    CHECK(a.world->swap_items(me, me, 7, me, 11) == World::ItemResult::Done);         // any inventory slot takes anything
    CHECK(a.world->swap_items(me, me, 8, me, 9) == World::ItemResult::NoItem);
}

TEST_CASE("a health potion heals and is used up; a stat potion raises its stat up to the class maximum") {
    Arena a({"S......"});
    const auto me = a.wizard({1.5f, 0.5f});
    auto* e = a.world->find(me);
    e->hp = 5;
    CHECK(e->player->items[4] == item_type("Health Potion"));  // the starting potion
    CHECK(a.world->use_item(me, 4) == World::ItemResult::Done);
    CHECK(e->hp == 100);
    CHECK(e->player->items[4] == -1);
    CHECK(a.world->use_item(me, 4) == World::ItemResult::NoItem);
    CHECK(a.world->use_item(me, 0) == World::ItemResult::NotAllowed);  // a staff cannot be drunk

    e->player->items[5] = item_type("Potion of Life");
    CHECK(a.world->use_item(me, 5) == World::ItemResult::Done);
    CHECK(e->player->base[0] == 105);
    CHECK(e->max_hp == 105);
    e->player->base[0] = 670;  // maxed
    e->player->items[5] = item_type("Potion of Life");
    CHECK(a.world->use_item(me, 5) == World::ItemResult::Maxed);
    CHECK(e->player->items[5] == item_type("Potion of Life"));  // kept
    e->player->items[6] = item_type("Potion of Attack");
    CHECK(a.world->use_item(me, 6) == World::ItemResult::Done);
    CHECK(e->player->stats.attack == 13);
}

TEST_CASE("a dropped item lands at the player's feet; soulbound ones only its owner sees") {
    Arena a({"S.........", "..........", ".........."});
    const auto me = a.wizard({1.5f, 0.5f});
    const auto them = a.wizard({2.5f, 1.5f}, 1, 2, 2);
    const int staff = a.world->find(me)->player->items[0];
    CHECK(a.world->drop_item(me, 0) == World::ItemResult::Done);
    CHECK(a.world->find(me)->player->items[0] == -1);
    auto all = bags(*a.world);
    REQUIRE(all.size() == 1);
    const auto bag = all.front();
    CHECK(a.world->find(bag)->container->owner_account == 0);  // an Energy Staff is not soulbound
    CHECK(a.world->find(bag)->container->items[0] == staff);
    CHECK(a.world->find(bag)->position == a.world->find(me)->position);
    CHECK(a.world->swap_items(me, bag, 0, me, 0) == World::ItemResult::Done);
    a.run(50.0f);
    CHECK(bags(*a.world).empty());  // an emptied bag disappears

    // Soulbound: the original <Soulbound/> flag.
    int soulbound = -1;
    for (std::uint32_t t = 0x0a00; t < 0xFFFF && soulbound < 0; ++t) {
        const auto* d = real().item(static_cast<std::uint16_t>(t));
        if (d != nullptr && d->soulbound && d->slot_type == 10) soulbound = d->type;
    }
    REQUIRE(soulbound >= 0);
    a.world->find(me)->player->items[5] = soulbound;
    CHECK(a.world->drop_item(me, 5) == World::ItemResult::Done);
    all = bags(*a.world);
    REQUIRE(all.size() == 1);
    CHECK(a.world->find(all.front())->container->owner_account == 1);
    std::vector<ViewUpdate> views;
    a.world->tick(50.0f, views);
    for (const auto& v : views) {
        const bool sees = std::any_of(v.entered.begin(), v.entered.end(), [&](const Entity* x) { return x->id == all.front(); });
        CHECK(sees == (v.viewer == me));
    }
    CHECK(a.world->swap_items(them, all.front(), 0, them, 4) == World::ItemResult::NotAllowed);
}

TEST_CASE("bags are only within reach when standing next to them, and vanish after a minute") {
    Arena a({"S.................."});
    const auto me = a.wizard({1.5f, 0.5f});
    a.world->drop_bags({10.5f, 0.5f}, 0, {item_type("Health Potion")});
    const auto bag = bags(*a.world).front();
    CHECK(a.world->swap_items(me, bag, 0, me, 5) == World::ItemResult::TooFar);
    a.run(59000.0f);
    CHECK(bags(*a.world).size() == 1);
    a.run(1100.0f);
    CHECK(bags(*a.world).empty());
}

TEST_CASE("bags hold eight items each; the bag type is the highest BagType inside") {
    Arena a({"S..............................."});
    a.world->drop_bags({3.5f, 0.5f}, 1, std::vector<int>(10, item_type("Health Potion")));
    a.world->drop_bags({8.5f, 0.5f}, 0, {item_type("Health Potion"), item_type("Potion of Life")});
    int potion_bags = 0;
    a.world->for_each([&](Entity& e) {
        if (!e.container) return;
        if (e.container->items[0] == item_type("Health Potion") && e.container->owner_account == 1) {
            ++potion_bags;
            CHECK(e.object_type == real().loot_bag_type(0));
        } else {
            CHECK(e.object_type == real().loot_bag_type(5));  // Potion of Life: BagType 5, the purple bag
        }
    });
    CHECK(potion_bags == 2);
}
