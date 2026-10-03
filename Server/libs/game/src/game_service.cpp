#include "waw/game/game_service.hpp"

#include <algorithm>

namespace waw::game {

using protocol::FailureCode;
using session::SessionState;

protocol::EntityKind to_protocol(sim::EntityKind kind) noexcept {
    switch (kind) {
        case sim::EntityKind::Player: return protocol::EntityKind::Player;
        case sim::EntityKind::Enemy: return protocol::EntityKind::Enemy;
        case sim::EntityKind::Container: return protocol::EntityKind::Container;
        case sim::EntityKind::Portal: return protocol::EntityKind::Portal;
        case sim::EntityKind::StaticObject: return protocol::EntityKind::StaticObject;
        case sim::EntityKind::Npc: return protocol::EntityKind::Npc;
        case sim::EntityKind::Other: return protocol::EntityKind::Other;
    }
    return protocol::EntityKind::Other;
}

protocol::EntityFull to_full(const sim::Entity& e) {
    return protocol::EntityFull{.id = e.id.value(),
                                .object_type = e.object_type,
                                .kind = to_protocol(e.kind),
                                .position = {e.position.x, e.position.y},
                                .name = e.name,
                                .hp = e.hp,
                                .max_hp = e.max_hp,
                                .level = static_cast<std::uint16_t>(e.level),
                                .skin_type = e.skin,
                                .conditions = e.conditions,
                                .items = e.container ? e.container->items : std::vector<int>{}};
}

protocol::EntityDelta to_delta(const sim::Entity& e, std::uint32_t f) {
    protocol::EntityDelta d;
    d.id = e.id.value();
    if (f & sim::dirty::Position) d.position = protocol::Vec2{e.position.x, e.position.y};
    if (f & sim::dirty::Hp) d.hp = e.hp;
    if (f & sim::dirty::MaxHp) d.max_hp = e.max_hp;
    if (f & sim::dirty::Level) d.level = static_cast<std::uint16_t>(e.level);
    if (f & sim::dirty::Skin) d.skin_type = e.skin;
    if (f & sim::dirty::Conditions) d.conditions = e.conditions;
    if (f & sim::dirty::Name) d.name = e.name;
    if ((f & sim::dirty::Items) && e.container) d.items = e.container->items;
    return d;
}

protocol::PlayerStats to_stats(const sim::Entity& e) {
    const auto& p = *e.player;
    return protocol::PlayerStats{.hp = e.hp,
                                 .max_hp = e.max_hp,
                                 .mp = p.mp,
                                 .max_mp = p.stats.max_mp,
                                 .attack = p.stats.attack,
                                 .defense = p.stats.defense,
                                 .speed = p.stats.speed,
                                 .dexterity = p.stats.dexterity,
                                 .vitality = p.stats.vitality,
                                 .wisdom = p.stats.wisdom,
                                 .level = static_cast<std::uint16_t>(e.level),
                                 .xp = p.xp,
                                 .xp_goal = sim::rules::xp_to_next_level(e.level),
                                 .fame = p.fame};
}

GameService::GameService(const content::ContentDb& content, session::SessionManager& sessions, persistence::ICharacterRepository& characters,
                         core::Worker& worker, core::TaskQueue& completions, core::Logger log, GameRules rules)
    : content_(content),
      sessions_(sessions),
      characters_(characters),
      worker_(worker),
      completions_(completions),
      log_(std::move(log)),
      rules_(std::move(rules)),
      rng_(rules_.world_seed) {}

GameService::~GameService() { worker_.wait_idle(); }

sim::World* GameService::world_named(std::string_view name) noexcept {
    for (auto& w : worlds_) {
        if (w->config().name == name) return w.get();
    }
    return nullptr;
}

std::optional<std::pair<sim::World*, sim::EntityId>> GameService::presence(std::uint32_t session_id) const {
    auto it = presence_.find(session_id);
    if (it == presence_.end()) return std::nullopt;
    return std::pair{it->second.world, it->second.entity};
}

std::expected<sim::World*, std::string> GameService::make_world(const std::string& name, std::string display_name) {
    const auto* config = content_.world(name);
    if (config == nullptr) return std::unexpected("world '" + name + "' has no config");
    if (config->maps.empty()) return std::unexpected("world '" + name + "' has no map");
    // The original World.Load: one of the config's maps at random.
    std::uniform_int_distribution<std::size_t> pick(0, config->maps.size() - 1);
    const auto& map_name = config->maps[pick(rng_)];
    const auto* map = content_.map(map_name);
    if (map == nullptr) return std::unexpected("world map '" + map_name + "' is missing");
    sim::WorldRules world_rules;
    world_rules.terrain_spawner = name == rules_.realm_world;
    const auto seed = rules_.world_seed + worlds_.size();
    worlds_.push_back(std::make_unique<sim::World>(config->id, *config, *map, content_, seed, world_rules));
    auto* w = worlds_.back().get();
    if (!display_name.empty()) w->set_display_name(std::move(display_name));
    log_.info("world '{}' ({}) ready: {} ({}x{} tiles), {} entities", config->name, w->display_name(), map_name, map->width, map->height,
              w->entity_count());
    return w;
}

std::expected<void, std::string> GameService::init() {
    auto nexus = make_world(rules_.start_world, "");
    if (!nexus) return std::unexpected(nexus.error());
    if (content_.world(rules_.realm_world) != nullptr && rules_.realm_count > 0) {
        // The original Nexus: a Realm Portal (0x0704) on a free "Realm Portals" tile per realm, each to a new realm named after
        // a monster.
        auto tiles = (*nexus)->map().region("Realm Portals");
        const auto* portal = content_.object("Realm Portal");
        if (tiles.empty() || portal == nullptr) {
            log_.warn("the Nexus has no 'Realm Portals' tile or no 'Realm Portal' object: the Realm cannot be reached");
        } else {
            std::shuffle(tiles.begin(), tiles.end(), rng_);
            auto names = rules_.realm_names;
            std::shuffle(names.begin(), names.end(), rng_);
            for (int i = 0; i < rules_.realm_count && static_cast<std::size_t>(i) < tiles.size(); ++i) {
                const auto& t = tiles[static_cast<std::size_t>(i)];
                const auto id = (*nexus)->add_object(portal->type, {t.x + 0.5f, t.y + 0.5f});
                const std::string name = names.empty() ? "Realm" : names[static_cast<std::size_t>(i) % names.size()];
                (*nexus)->find(id)->name = name;
                portals_[{*nexus, id.value()}] = PortalTarget{nullptr, rules_.realm_world, name};
                realm_portals_.emplace_back(*nexus, id.value());
            }
        }
    }
    if (rules_.entry_world == rules_.realm_world && open_realm(0) == nullptr) {
        return std::unexpected("the entry world '" + rules_.entry_world + "' could not be opened");
    }
    return {};
}

sim::World* GameService::open_realm(std::size_t index) {
    if (index >= realm_portals_.size()) return nullptr;
    auto& target = portals_[realm_portals_[index]];
    if (target.world == nullptr) {
        auto made = make_world(target.config, target.display_name);
        if (!made) {
            log_.error("{}", made.error());
            return nullptr;
        }
        target.world = *made;
    }
    return target.world;
}

void GameService::on_game_message(session::Session& s, const protocol::ClientMessage& message) {
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, protocol::LoadCharacter> || std::is_same_v<T, protocol::CreateCharacter>) {
                if (s.state != SessionState::CharacterSelect || loading_.contains(s.id)) {
                    sessions_.fail(s, FailureCode::InvalidRequest, "A character is already being loaded.");
                    return;
                }
                if constexpr (std::is_same_v<T, protocol::LoadCharacter>) load_character(s, m.character_id);
                else create_character(s, m);
            } else if constexpr (std::is_same_v<T, protocol::MoveInput>) {
                if (s.state == SessionState::InWorld) on_move(s, m);
            } else if constexpr (std::is_same_v<T, protocol::Shoot>) {
                if (s.state == SessionState::InWorld) on_shoot(s, m);
            } else if constexpr (std::is_same_v<T, protocol::UsePortal>) {
                if (s.state == SessionState::InWorld) on_portal(s, m);
            } else if constexpr (std::is_same_v<T, protocol::Escape>) {
                if (s.state != SessionState::InWorld) return;
                auto it = presence_.find(s.id);
                if (it != presence_.end() && it->second.world != start_world()) transfer(s, *start_world());
            } else if constexpr (std::is_same_v<T, protocol::ChatSend>) {
                if (s.state == SessionState::InWorld) on_chat(s, m);
            } else if constexpr (std::is_same_v<T, protocol::InvSwap> || std::is_same_v<T, protocol::UseItem> ||
                                 std::is_same_v<T, protocol::InvDrop>) {
                if (s.state != SessionState::InWorld) return;
                auto it = presence_.find(s.id);
                if (it == presence_.end()) return;
                sim::World& w = *it->second.world;
                sim::World::ItemResult r;
                if constexpr (std::is_same_v<T, protocol::InvSwap>) {
                    r = w.swap_items(it->second.entity, sim::EntityId(m.from_entity), m.from_slot, sim::EntityId(m.to_entity), m.to_slot);
                } else if constexpr (std::is_same_v<T, protocol::UseItem>) {
                    r = w.use_item(it->second.entity, m.slot);
                } else {
                    r = w.drop_item(it->second.entity, m.slot);
                }
                if (r == sim::World::ItemResult::TooFar) say(s, protocol::ChatChannel::Error, "You are too far away from that.");
                // Refused moves need no answer: the next Inventory / bag update shows the true state (the client redraws from it).
                if (r != sim::World::ItemResult::Done) {
                    if (auto* e = w.find(it->second.entity); e != nullptr && e->player) e->player->inventory_dirty = true;
                }
            } else {
                log_.debug("session {}: {} ignored", s.id, protocol::message_name(static_cast<std::uint16_t>(T::kId)));
            }
        },
        message);
}

void GameService::load_character(session::Session& s, std::int32_t character_id) {
    loading_.insert(s.id);
    const auto account_id = s.account->account_id;
    const auto session_id = s.id;
    worker_.submit([this, account_id, character_id, session_id] {
        auto result = characters_.load(account_id, character_id);
        post_completion([this, session_id, result = std::move(result)]() mutable { enter_world(session_id, std::move(result), false); });
    });
}

void GameService::create_character(session::Session& s, const protocol::CreateCharacter& m) {
    const auto* cls = content_.player_class(m.class_type);
    if (cls == nullptr) {
        sessions_.fail(s, FailureCode::CharacterCreateFailed, "That class does not exist.");
        return;
    }
    if (m.skin_type != 0) {
        // Skins (ownership, prices) are not migrated yet; only the class's own look can be chosen.
        sessions_.fail(s, FailureCode::CharacterCreateFailed, "That skin is not available.");
        return;
    }
    persistence::CharacterRecord initial;
    initial.account_id = s.account->account_id;
    initial.class_type = cls->type;
    initial.skin_type = 0;
    initial.level = 1;
    initial.hp = cls->stats[static_cast<std::size_t>(content::Stat::MaxHp)].start;
    initial.mp = cls->stats[static_cast<std::size_t>(content::Stat::MaxMp)].start;
    // The original DbClient.CreateCharacterAsync + newCharsConfig.xml: class start stats, the class's Equipment, level 1, one
    // health and one magic potion in the potion stacks.
    const auto stats = sim::rules::starting_stats(*cls);
    initial.stats.assign(stats.begin(), stats.end());
    initial.items = cls->equipment;
    initial.items.resize(static_cast<std::size_t>(rules_.player_slots), -1);
    initial.health_potions = 1;
    initial.magic_potions = 1;

    loading_.insert(s.id);
    const auto session_id = s.id;
    worker_.submit([this, initial = std::move(initial), session_id] {
        auto result = characters_.create(initial);
        post_completion([this, session_id, result = std::move(result)]() mutable { enter_world(session_id, std::move(result), true); });
    });
}

sim::PlayerSpawn GameService::spawn_from(const session::Session& s, const persistence::CharacterRecord& c) const {
    sim::PlayerSpawn spawn;
    spawn.session_id = s.id;
    spawn.character_id = c.character_id;
    spawn.class_type = c.class_type;
    spawn.skin_type = c.skin_type;
    spawn.name = s.account->name;
    spawn.level = c.level;
    spawn.hp = c.hp;
    spawn.mp = c.mp;
    spawn.xp = c.xp;
    spawn.fame = c.fame;
    spawn.items = c.items;
    spawn.account_id = s.account->account_id;
    spawn.has_backpack = c.has_backpack;
    for (std::size_t i = 0; i < spawn.stats.size() && i < c.stats.size(); ++i) spawn.stats[i] = c.stats[i];
    return spawn;
}

void GameService::enter_world(std::uint32_t session_id, std::expected<persistence::CharacterRecord, persistence::CharacterError> result,
                              bool created) {
    loading_.erase(session_id);
    session::Session* s = sessions_.find(session_id);
    if (s == nullptr || s->state != SessionState::CharacterSelect) return;  // left while loading
    if (!result) {
        switch (result.error()) {
            case persistence::CharacterError::NotFound:
                sessions_.fail(*s, FailureCode::CharacterNotFound, "That character does not exist.");
                break;
            case persistence::CharacterError::Dead:
                sessions_.fail(*s, FailureCode::CharacterNotFound, "That character has died.");
                break;
            case persistence::CharacterError::SlotsFull:
                sessions_.fail(*s, FailureCode::CharacterCreateFailed, "You have no free character slot.");
                break;
            default:
                log_.warn("session {}: character {} failed: {}", session_id, created ? "create" : "load",
                          persistence::to_string(result.error()));
                sessions_.fail(*s, created ? FailureCode::CharacterCreateFailed : FailureCode::CharacterLoadFailed,
                               "Your character could not be loaded. Please try again.");
                break;
        }
        return;
    }
    if (content_.player_class(result->class_type) == nullptr) {
        log_.error("character {}/{} has unknown class {}", result->account_id, result->character_id, result->class_type);
        sessions_.fail(*s, FailureCode::CharacterLoadFailed, "Your character's class no longer exists.");
        return;
    }
    Presence& presence = presence_[session_id];
    presence.character = std::move(*result);
    sim::World* entry = rules_.entry_world.empty() ? nullptr : world_named(rules_.entry_world);
    place(*s, presence, entry != nullptr ? *entry : *start_world(), spawn_from(*s, presence.character));
    s->state = SessionState::InWorld;
    log_.info("session {} entered {} as character {} ({})", session_id, presence.world->config().name, presence.character.character_id,
              content_.player_class(presence.character.class_type)->id);
}

void GameService::place(session::Session& s, Presence& presence, sim::World& world, const sim::PlayerSpawn& spawn) {
    presence.world = &world;
    presence.entity = world.add_player(spawn);
    const auto& config = world.config();
    sessions_.send(s, protocol::WorldInfo{.world_id = world.id(),
                                          .name = config.name,
                                          .display_name = world.display_name(),
                                          .width = world.map().width(),
                                          .height = world.map().height(),
                                          .seed = 0,
                                          .music = config.music,
                                          .allow_player_teleport = false});
    const auto* e = world.find(presence.entity);
    sessions_.send(s, protocol::PlayerSpawned{.entity_id = presence.entity.value(),
                                              .character_id = presence.character.character_id,
                                              .position = {e->position.x, e->position.y},
                                              .speed = e->player->speed});
}

void GameService::transfer(session::Session& s, sim::World& target) {
    auto it = presence_.find(s.id);
    if (it == presence_.end()) return;
    Presence& p = it->second;
    const auto* entity = p.world->find(p.entity);
    if (entity == nullptr || entity->player->dead) return;
    save(p, entity, false);  // keeps the record current; the transfer itself carries the live state
    sim::PlayerSpawn spawn = spawn_from(s, p.character);
    p.world->remove(p.entity);
    place(s, p, target, spawn);
    log_.info("session {} moved to {}", s.id, target.config().name);
}

void GameService::on_move(session::Session& s, const protocol::MoveInput& m) {
    auto it = presence_.find(s.id);
    if (it == presence_.end()) return;
    std::vector<sim::MoveCommand> steps;
    steps.reserve(m.steps.size());
    for (const auto& step : m.steps) steps.push_back({step.seq, step.dt_ms, step.dir_x, step.dir_y});
    switch (it->second.world->queue_input(it->second.entity, steps)) {
        case sim::World::InputResult::Accepted:
            break;
        case sim::World::InputResult::Flooding:
            sessions_.fail(s, FailureCode::Kicked, "Too many movement inputs.");
            break;
        case sim::World::InputResult::OutOfOrder:
            sessions_.fail(s, FailureCode::InvalidRequest, "Movement inputs out of order.");
            break;
    }
}

void GameService::on_shoot(session::Session& s, const protocol::Shoot& m) {
    auto it = presence_.find(s.id);
    if (it == presence_.end()) return;
    const auto result = it->second.world->shoot(it->second.entity, m.shot_id, m.angle);
    if (result == sim::World::ShootResult::TooFast) log_.debug("session {}: shot refused (fire rate)", s.id);
}

void GameService::on_portal(session::Session& s, const protocol::UsePortal& m) {
    auto it = presence_.find(s.id);
    if (it == presence_.end()) return;
    sim::World& world = *it->second.world;
    const auto* me = world.find(it->second.entity);
    const auto* portal = world.find(sim::EntityId(m.entity_id));
    if (me == nullptr || portal == nullptr || portal->kind != sim::EntityKind::Portal) return;
    if (sim::distance_squared(me->position, portal->position) > rules_.portal_use_distance * rules_.portal_use_distance) {
        say(s, protocol::ChatChannel::Error, "You are too far away from the portal.");
        return;
    }
    auto target = portals_.find({&world, m.entity_id});
    if (target == portals_.end()) {
        say(s, protocol::ChatChannel::Error, "That place is not open yet.");
        return;
    }
    if (target->second.world == nullptr) {
        auto made = make_world(target->second.config, target->second.display_name);
        if (!made) {
            log_.error("{}", made.error());
            say(s, protocol::ChatChannel::Error, "That place is not open yet.");
            return;
        }
        target->second.world = *made;
    }
    transfer(s, *target->second.world);
}

void GameService::say(session::Session& s, protocol::ChatChannel channel, std::string text) {
    sessions_.send(s, protocol::ChatMessage{.channel = channel, .sender_id = 0, .sender_name = "", .text = std::move(text)});
}

void GameService::on_chat(session::Session& s, const protocol::ChatSend& m) {
    auto it = presence_.find(s.id);
    if (it == presence_.end()) return;
    std::string text = m.text;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) return;
    if (text.size() > rules_.max_chat_bytes) {
        say(s, protocol::ChatChannel::Error, "Your message is too long.");
        return;
    }
    if (text.front() == '/') {
        say(s, protocol::ChatChannel::Error, "Commands are not available yet.");
        return;
    }
    const protocol::ChatMessage out{.channel = protocol::ChatChannel::Say,
                                    .sender_id = it->second.entity.value(),
                                    .sender_name = s.account->name,
                                    .text = std::move(text)};
    for (const auto& [session_id, presence] : presence_) {
        if (presence.world != it->second.world) continue;
        if (auto* other = sessions_.find(session_id)) sessions_.send(*other, out);
    }
}

void GameService::save(Presence& p, const sim::Entity* entity, bool dead) {
    auto& c = p.character;
    if (entity != nullptr && entity->player) {
        const auto& ps = *entity->player;
        c.level = entity->level;
        c.hp = entity->hp;
        c.mp = ps.mp;
        c.xp = ps.xp;
        c.fame = ps.fame;
        c.items = ps.items;
        c.stats.assign(ps.base.begin(), ps.base.end());
    }
    c.is_dead = c.is_dead || dead;
    c.save_version += 1;
    worker_.submit([this, snapshot = c] {
        if (auto r = characters_.save(snapshot); !r) {
            log_.error("saving character {}/{} failed: {}", snapshot.account_id, snapshot.character_id, persistence::to_string(r.error()));
        }
    });
}

void GameService::on_session_closed(const session::Session& s) {
    loading_.erase(s.id);
    auto it = presence_.find(s.id);
    if (it == presence_.end()) return;
    const auto* entity = it->second.world->find(it->second.entity);
    if (!it->second.character.is_dead) save(it->second, entity, false);
    it->second.world->remove(it->second.entity);
    presence_.erase(it);
}

void GameService::tick(float dt_ms) {
    for (auto& world : worlds_) {
        views_.clear();
        world->tick(dt_ms, views_);
        // Volleys and floating texts first: a killing blow's damage number must arrive before the Snapshot that removes the
        // victim, or the client has nothing left to draw it over. Deaths last (they end sessions).
        send_events(*world);
        for (const auto& v : views_) send_view(v);
        send_deaths(*world);
    }
}

void GameService::send_events(sim::World& world) {
    const float sight = world.rules().sight_radius;
    for (const auto& v : world.take_volleys()) {
        const protocol::ProjectileVolley msg{.owner_id = v.owner.value(),
                                             .owner_is_enemy = v.enemy,
                                             .first_bullet_id = v.first_bullet,
                                             .position = {v.position.x, v.position.y},
                                             .angle = v.angle,
                                             .angle_step = v.angle_step,
                                             .count = static_cast<std::uint8_t>(std::clamp(v.count, 0, 255)),
                                             .spec = {.projectile_type = v.projectile_type,
                                                      .path = static_cast<protocol::PathKind>(v.path.kind),
                                                      .speed = v.path.speed,
                                                      .lifetime_ms = static_cast<std::uint16_t>(std::clamp(v.path.lifetime_ms, 0, 65535)),
                                                      .amplitude = v.path.amplitude,
                                                      .frequency = v.path.frequency,
                                                      .size = static_cast<std::uint8_t>(std::clamp(v.size, 0, 255))}};
        for (auto& [session_id, presence] : presence_) {
            if (presence.world != &world) continue;
            if (!v.enemy && presence.entity == v.owner) continue;  // the shooter drew its own bullets already
            const auto* viewer = world.find(presence.entity);
            if (viewer == nullptr || sim::distance_squared(viewer->position, v.position) > sight * sight) continue;
            if (auto* s = sessions_.find(session_id)) sessions_.send(*s, msg);
        }
    }
    for (const auto& t : world.take_taunts()) {
        // The original Taunt: enemy chat. Sent as a Say line from the monster (its entity id and object id as the sender).
        const protocol::ChatMessage msg{.channel = protocol::ChatChannel::Say, .sender_id = t.entity.value(), .sender_name = t.speaker, .text = t.text};
        for (const auto target : t.to) {
            const auto* e = world.find(target);
            if (e == nullptr || !e->player) continue;
            if (auto* session = sessions_.find(e->player->session_id)) sessions_.send(*session, msg);
        }
    }
    for (const auto& n : world.take_notifications()) {
        for (const auto target : n.to) {
            const auto* e = world.find(target);
            if (e == nullptr || !e->player) continue;
            if (auto* s = sessions_.find(e->player->session_id)) {
                sessions_.send(*s, protocol::Notification{.entity_id = n.entity.value(), .text = n.text, .color = n.color, .is_damage = n.damage});
            }
        }
    }
}

void GameService::send_deaths(sim::World& world) {
    for (const auto& d : world.take_deaths()) {
        auto it = presence_.find(d.session_id);
        if (it == presence_.end()) continue;
        it->second.character.level = d.level;
        it->second.character.fame = d.fame;
        save(it->second, nullptr, true);
        log_.info("session {}: character {} was killed by {}", d.session_id, it->second.character.character_id, d.killed_by);
        if (auto* s = sessions_.find(d.session_id)) {
            sessions_.send(*s, protocol::PlayerDied{.killed_by = d.killed_by, .level = static_cast<std::uint16_t>(d.level), .fame = d.fame});
            sessions_.end(*s);
        }
        presence_.erase(it);  // the world already removed the entity
    }
}

void GameService::send_view(const sim::ViewUpdate& v) {
    session::Session* s = sessions_.find(v.session_id);
    if (s == nullptr || s->state != SessionState::InWorld) return;

    for (std::size_t start = 0; start < v.tiles.size(); start += rules_.max_tiles_per_message) {
        protocol::TileData data;
        const std::size_t end = std::min(v.tiles.size(), start + rules_.max_tiles_per_message);
        data.tiles.reserve(end - start);
        for (std::size_t i = start; i < end; ++i) {
            const auto& t = v.tiles[i];
            data.tiles.push_back({.x = t.x, .y = t.y, .ground_type = t.ground, .object_type = t.object});
        }
        sessions_.send(*s, data);
    }
    if (v.inventory != nullptr) sessions_.send(*s, protocol::Inventory{.items = *v.inventory});
    if (v.stats != nullptr) {
        auto it = presence_.find(v.session_id);
        if (it != presence_.end()) {
            if (const auto* e = it->second.world->find(it->second.entity)) sessions_.send(*s, to_stats(*e));
        }
    }

    protocol::Snapshot snap;
    snap.server_tick = v.server_tick;
    snap.ack_input_seq = v.ack_input_seq;
    snap.entered.reserve(v.entered.size());
    for (const auto* e : v.entered) snap.entered.push_back(to_full(*e));
    snap.left.reserve(v.left.size());
    for (const auto id : v.left) snap.left.push_back(id.value());
    snap.changed.reserve(v.changed.size());
    for (const auto& c : v.changed) snap.changed.push_back(to_delta(*c.entity, c.fields));
    sessions_.send(*s, snap);
}

}  // namespace waw::game
