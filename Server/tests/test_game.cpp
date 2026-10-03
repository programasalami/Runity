#include <doctest/doctest.h>

#include "real_fixtures.hpp"
#include "waw/game/game_service.hpp"

using namespace waw;
using namespace std::chrono_literals;
using protocol::FailureCode;

namespace {

const content::ContentDb& content_db() { return test::real(); }

class Link final : public session::IClientLink {
public:
    std::map<std::uint32_t, std::vector<protocol::ServerMessage>> inbox;  // by connection id
    std::set<std::uint32_t> closed;

    void send(net::ConnectionId id, std::vector<std::uint8_t> frame) override {
        protocol::FrameDecoder d;
        d.feed(frame);
        auto f = d.next();
        REQUIRE(f.has_value());
        auto m = protocol::decode_server_message(f->id, f->payload);
        REQUIRE(m.has_value());
        inbox[id.value()].push_back(std::move(*m));
    }
    void close(net::ConnectionId id, bool) override { closed.insert(id.value()); }

    template <class T>
    std::vector<T> all(std::uint32_t conn) const {
        std::vector<T> out;
        if (auto it = inbox.find(conn); it != inbox.end()) {
            for (const auto& m : it->second) {
                if (const auto* t = std::get_if<T>(&m)) out.push_back(*t);
            }
        }
        return out;
    }
    template <class T>
    std::optional<T> last(std::uint32_t conn) const {
        auto v = all<T>(conn);
        if (v.empty()) return std::nullopt;
        return v.back();
    }
};

constexpr std::uint16_t kWizard = 0x030e;

struct Game {
    core::MemoryLogSink sink;
    core::ManualClock clock;
    Link link;
    persistence::InMemoryAccountSessions accounts;
    persistence::InMemoryCharacterRepository characters;
    core::TaskQueue completions;
    core::Worker worker;
    std::unique_ptr<session::SessionManager> sessions;
    std::unique_ptr<game::GameService> game;

    Game() {
        session::SessionConfig config;
        config.build_version = "t";
        sessions = std::make_unique<session::SessionManager>(config, link, accounts, worker, completions, clock, core::Logger(sink, "s"));
        game = std::make_unique<game::GameService>(content_db(), *sessions, characters, worker, completions, core::Logger(sink, "g"));
        REQUIRE(game->init().has_value());
        sessions->set_game_handler(game.get());
    }
    ~Game() {
        sessions->set_game_handler(nullptr);
        game.reset();
        sessions.reset();
    }

    void pump() {
        worker.wait_idle();
        completions.run_pending();
    }
    void send(std::uint32_t conn, protocol::ClientMessage m) {
        sessions->on_net_event(net::MessageReceived{net::ConnectionId(conn), std::move(m)});
        pump();
    }
    void login(std::uint32_t conn, std::int64_t account, std::string name) {
        const std::string ticket = "ticket-" + std::to_string(conn) + "-" + std::to_string(account);
        accounts.add_ticket(ticket, {.account_id = account, .name = std::move(name), .rank = 0});
        sessions->on_net_event(net::Connected{net::ConnectionId(conn), "127.0.0.1"});
        send(conn, protocol::Hello{.protocol_version = protocol::kProtocolVersion, .build_version = "t", .token = ticket});
        REQUIRE(link.last<protocol::HelloAck>(conn).has_value());
    }
    void disconnect(std::uint32_t conn) {
        sessions->on_net_event(net::Disconnected{net::ConnectionId(conn), net::DisconnectReason::ClosedByPeer, ""});
        pump();
    }
    void tick() {
        game->tick(50.0f);
        pump();
    }
    FailureCode failure(std::uint32_t conn) const {
        auto f = link.last<protocol::Failure>(conn);
        REQUIRE(f.has_value());
        return f->code;
    }
};

}  // namespace

TEST_CASE("create a character, enter the Nexus and receive the world") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    auto info = g.link.last<protocol::WorldInfo>(1);
    REQUIRE(info.has_value());
    CHECK(info->name == "Nexus");
    CHECK(info->width == 142);  // the original Nexus.jm
    auto spawned = g.link.last<protocol::PlayerSpawned>(1);
    REQUIRE(spawned.has_value());
    CHECK(spawned->character_id == 1);
    CHECK(g.characters.count(100) == 1);

    g.tick();
    auto tiles = g.link.all<protocol::TileData>(1);
    REQUIRE_FALSE(tiles.empty());
    auto snap = g.link.last<protocol::Snapshot>(1);
    REQUIRE(snap.has_value());
    const auto self = std::find_if(snap->entered.begin(), snap->entered.end(), [&](const auto& e) { return e.id == spawned->entity_id; });
    REQUIRE(self != snap->entered.end());
    CHECK(self->kind == protocol::EntityKind::Player);
    CHECK(self->name == "Bob");
    CHECK(self->object_type == kWizard);
    CHECK(self->hp == 100);
}

TEST_CASE("movement input is acknowledged and moves the player") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    const auto start = g.link.last<protocol::PlayerSpawned>(1)->position;
    g.tick();
    // Walk a little in whichever direction is open: try all four, one at a time.
    const std::array<std::pair<std::int8_t, std::int8_t>, 4> dirs{{{127, 0}, {-127, 0}, {0, 127}, {0, -127}}};
    std::uint32_t seq = 0;
    bool moved = false;
    for (const auto& [dx, dy] : dirs) {
        protocol::MoveInput input;
        for (int i = 0; i < 3; ++i) input.steps.push_back({.seq = ++seq, .dt_ms = 16, .dir_x = dx, .dir_y = dy});
        g.send(1, input);
        g.tick();
        auto snap = g.link.last<protocol::Snapshot>(1);
        REQUIRE(snap.has_value());
        CHECK(snap->ack_input_seq == seq);
        for (const auto& c : snap->changed) moved = moved || c.position.has_value();
        if (moved) break;
    }
    CHECK(moved);
    const auto* world = g.game->start_world();
    REQUIRE(world != nullptr);
    CHECK(world->player_count() == 1);
    (void)start;
}

TEST_CASE("invalid creation requests are refused") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = 0x0001, .skin_type = 0});
    CHECK(g.failure(1) == FailureCode::CharacterCreateFailed);
    g.login(3, 102, "Cy");
    g.send(3, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 7});
    CHECK(g.failure(3) == FailureCode::CharacterCreateFailed);
    CHECK(g.characters.count(100) + g.characters.count(102) == 0);
}

TEST_CASE("reconnecting loads the same character; unknown characters are refused") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    const auto id = g.link.last<protocol::PlayerSpawned>(1)->character_id;
    g.disconnect(1);
    CHECK(g.game->players_in_world() == 0);

    g.login(2, 100, "Bob");
    g.send(2, protocol::LoadCharacter{.character_id = id});
    auto spawned = g.link.last<protocol::PlayerSpawned>(2);
    REQUIRE(spawned.has_value());
    CHECK(spawned->character_id == id);

    g.login(3, 200, "Amy");
    g.send(3, protocol::LoadCharacter{.character_id = id});  // Bob's character id, Amy's account
    CHECK(g.failure(3) == FailureCode::CharacterNotFound);
}

TEST_CASE("the character slot limit is enforced") {
    Game g;
    for (std::uint32_t conn = 1; conn <= 2; ++conn) {
        g.login(conn, 100, "Bob");
        g.send(conn, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
        REQUIRE(g.link.last<protocol::PlayerSpawned>(conn).has_value());
        g.disconnect(conn);
    }
    g.login(3, 100, "Bob");
    g.send(3, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    CHECK(g.failure(3) == FailureCode::CharacterCreateFailed);
}

TEST_CASE("a character database outage refuses entry but keeps the server up") {
    Game g;
    g.characters.set_unavailable(true);
    g.login(1, 100, "Bob");
    g.send(1, protocol::LoadCharacter{.character_id = 1});
    CHECK(g.failure(1) == FailureCode::CharacterLoadFailed);
}

TEST_CASE("players see each other, chat reaches the world, and a leaver disappears") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    g.login(2, 200, "Amy");
    g.send(2, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    const auto amy = g.link.last<protocol::PlayerSpawned>(2)->entity_id;
    // The Nexus's spawn tiles can be farther apart than the sight radius: put Amy right next to Bob.
    {
        auto* world = g.game->start_world();
        const auto bob = g.link.last<protocol::PlayerSpawned>(1)->entity_id;
        auto* amy_entity = world->find(sim::EntityId(amy));
        const auto* bob_entity = world->find(sim::EntityId(bob));
        REQUIRE(amy_entity != nullptr);
        REQUIRE(bob_entity != nullptr);
        amy_entity->position = bob_entity->position;
        amy_entity->player->mover.position = bob_entity->position;
    }
    g.tick();

    bool bob_sees_amy = false;
    for (const auto& s : g.link.all<protocol::Snapshot>(1)) {
        for (const auto& e : s.entered) bob_sees_amy = bob_sees_amy || e.id == amy;
    }
    CHECK(bob_sees_amy);

    g.send(2, protocol::ChatSend{.text = "hello  "});
    auto heard = g.link.last<protocol::ChatMessage>(1);
    REQUIRE(heard.has_value());
    CHECK(heard->text == "hello");
    CHECK(heard->sender_name == "Amy");
    CHECK(heard->channel == protocol::ChatChannel::Say);

    g.send(2, protocol::ChatSend{.text = std::string(300, 'x')});
    CHECK(g.link.last<protocol::ChatMessage>(2)->channel == protocol::ChatChannel::Error);

    g.disconnect(2);
    g.tick();
    auto snap = g.link.last<protocol::Snapshot>(1);
    REQUIRE(snap.has_value());
    CHECK(std::find(snap->left.begin(), snap->left.end(), amy) != snap->left.end());
}

TEST_CASE("a second load while one is in flight is refused") {
    Game g;
    g.login(1, 100, "Bob");
    // Send two creates without letting the worker finish in between.
    g.sessions->on_net_event(net::MessageReceived{net::ConnectionId(1), protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0}});
    g.sessions->on_net_event(net::MessageReceived{net::ConnectionId(1), protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0}});
    g.pump();
    CHECK(g.failure(1) == FailureCode::InvalidRequest);
}

namespace {

sim::EntityId realm_portal(sim::World& nexus) {
    sim::EntityId found;
    nexus.for_each([&](sim::Entity& e) {
        if (e.kind == sim::EntityKind::Portal && e.object_type == test::type_of("Realm Portal")) found = e.id;
    });
    return found;
}

void stand_next_to(sim::World& world, sim::EntityId who, sim::EntityId what) {
    auto* e = world.find(who);
    const auto target = world.find(what)->position;
    e->position = {target.x, target.y + 1.0f};
    e->player->mover.position = e->position;
}

}  // namespace

TEST_CASE("the realm portal takes a player to the realm and escape brings them back") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    auto* nexus = g.game->start_world();
    const auto portal = realm_portal(*nexus);
    REQUIRE(portal.value() != 0);
    const auto me = g.game->presence(1)->second;

    g.send(1, protocol::UsePortal{.entity_id = portal.value()});
    CHECK(g.link.last<protocol::ChatMessage>(1)->text == "You are too far away from the portal.");

    stand_next_to(*nexus, me, portal);
    g.send(1, protocol::UsePortal{.entity_id = portal.value()});
    auto info = g.link.last<protocol::WorldInfo>(1);
    REQUIRE(info.has_value());
    CHECK(info->name == "Realm");
    CHECK(info->display_name == nexus->find(portal)->name);  // a realm is named after a monster, like its portal
    CHECK(g.game->presence(1)->first == g.game->world_named("Realm"));
    CHECK(g.game->world_named("Realm")->map().width() == 2048);
    CHECK(nexus->find(me) == nullptr);

    g.send(1, protocol::Escape{});
    CHECK(g.link.last<protocol::WorldInfo>(1)->name == "Nexus");
    CHECK(g.game->presence(1)->first == nexus);
}

TEST_CASE("the player's own stats and items are sent; shots reach other players as volleys") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    g.login(2, 200, "Amy");
    g.send(2, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    auto* nexus = g.game->start_world();
    const auto bob = g.game->presence(1)->second;
    const auto amy = g.game->presence(2)->second;
    nexus->find(amy)->position = nexus->find(bob)->position;
    nexus->find(amy)->player->mover.position = nexus->find(bob)->position;
    g.tick();
    auto stats = g.link.last<protocol::PlayerStats>(1);
    REQUIRE(stats.has_value());
    CHECK(stats->max_hp == 100);  // the Wizard's start; its starting gear boosts nothing
    CHECK(stats->level == 1);
    CHECK(stats->xp_goal == 50);
    auto inv = g.link.last<protocol::Inventory>(1);
    REQUIRE(inv.has_value());
    CHECK(inv->items.size() == 20);
    CHECK(inv->items[0] == 0xa97);  // Energy Staff
    CHECK(inv->items[4] == 0xa22);  // Health Potion

    g.send(1, protocol::Shoot{.shot_id = 3, .client_time_ms = 0, .angle = 0.0f});
    g.tick();
    CHECK(g.link.all<protocol::ProjectileVolley>(1).empty());  // the shooter drew its own
    auto seen = g.link.last<protocol::ProjectileVolley>(2);
    REQUIRE(seen.has_value());
    CHECK(seen->owner_id == bob.value());
    CHECK(seen->first_bullet_id == 3 * test::real().item(0xa97)->num_projectiles);  // shot id x projectiles (the Energy Staff fires 2)
    CHECK_FALSE(seen->owner_is_enemy);
}

TEST_CASE("a dead character is told, saved as dead, and cannot be loaded again") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    const auto id = g.link.last<protocol::PlayerSpawned>(1)->character_id;
    auto [world, me] = *g.game->presence(1);
    world->damage(*world->find(me), 100000, true, sim::EntityId(), "Test Doom");
    g.tick();
    auto died = g.link.last<protocol::PlayerDied>(1);
    REQUIRE(died.has_value());
    CHECK(died->killed_by == "Test Doom");
    CHECK(g.link.closed.contains(1));
    g.disconnect(1);
    g.pump();
    CHECK(g.characters.load(100, id).error() == persistence::CharacterError::Dead);
}

TEST_CASE("leaving saves the character's progress") {
    Game g;
    g.login(1, 100, "Bob");
    g.send(1, protocol::CreateCharacter{.class_type = kWizard, .skin_type = 0});
    const auto id = g.link.last<protocol::PlayerSpawned>(1)->character_id;
    auto [world, me] = *g.game->presence(1);
    world->award_xp(*world->find(me), 50 + 13);
    const auto grown = world->find(me)->player->base;
    g.disconnect(1);
    g.pump();
    auto saved = g.characters.load(100, id);
    REQUIRE(saved.has_value());
    CHECK(saved->level == 2);
    CHECK(saved->xp == 13);
    REQUIRE(saved->stats.size() == 8);
    CHECK(saved->stats[0] == grown[0]);  // the stats grown on level-up are the character's own
    CHECK(saved->stats[0] >= 120);
}
