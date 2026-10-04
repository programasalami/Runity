# C++ GameServer - Migration Plan and Design

Responsibilities are translated, not classes. Related: GameServer.md, WorldSystem.md, Combat.md, AI.md.

## 1. Toolchain (verified on the dev PC, 2026-10-02)

| Item | Choice | Verified |
|---|---|---|
| Language | C++23 (`std::expected`, `std::print`, `std::span`) | MSVC 14.51 (VS 2026 Community 18.9) built a C++23 probe |
| Build | CMake >= 3.25 + Ninja (bundled with VS), presets `windows-debug/release`, `linux-*` later | yes |
| Packages | vcpkg manifest mode (bundled vcpkg 2026-05-27): `asio`, `pugixml`, `zlib`, `nlohmann-json`, `doctest` | internet reachable |
| PostgreSQL client | libpq from the installed PostgreSQL 17 (`find_package(PostgreSQL)`; on Linux `libpq-dev`) | headers + lib present |
| Redis client | own minimal RESP2 client on asio (~300 lines, unit-testable against a fake stream) - avoids hiredis | - |
| Tests | doctest; `ctest` runs unit tests always, DB/Redis integration tests only when `RUNITY_TEST_PG` / `RUNITY_TEST_REDIS` are set | - |
| Platforms | Windows (dev), Linux x64 (VPS). No platform API outside `net/` and `app/` | - |

Compiler settings: `/W4 /permissive- /Zc:__cplusplus /utf-8` (MSVC), `-Wall -Wextra -Wpedantic` (GCC/Clang); warnings as errors in CI.

## 2. Modules

| Library | Responsibility | Depends on | Must not depend on |
|---|---|---|---|
| `runity_core` | strong ids (`EntityId`, `WorldId`, `SessionId`, `AccountId`, `CharacterId`), `Vec2`, time (`SimTime`, `Duration` in ms), `Result<T>` = `std::expected<T, Error>`, logging sink interface, RNG (per-world, seeded) | std | anything else |
| `runity_protocol` | generated message structs + codecs, framing (`FrameDecoder`, `FrameEncoder`), `ByteReader`/`ByteWriter`, protocol version | core | net, sim |
| `runity_content` | `ContentDb` (objects, items, grounds, players/classes, projectiles, world configs), XML + `.jm` loaders; immutable after load | core, pugixml, zlib, nlohmann-json | sim, net |
| `runity_sim` | `World` (entity registry, component tables, tile map, systems), `WorldDirector` (instances, transfers), systems: movement, projectiles, combat, AI, spawning, sight/replication, inventory, loot, portals | core, content | protocol wire types, sockets, SQL |
| `runity_session` | `Session` state machine (Connected -> Authenticating -> CharacterSelect -> InWorld -> Transferring -> Closing), translation protocol <-> sim commands/events, rate limits, anti-cheat score | core, protocol, sim | sockets, SQL |
| `runity_net` | asio TCP acceptor, `Connection` (read/write buffers, size caps, per-address limit, idle timeout), thread-safe inbound/outbound queues | core, protocol, asio | sim |
| `runity_persistence` | repository interfaces (`IAccountRepository`, `ICharacterRepository`, `ISessionStore`, `ILockStore`), libpq + RESP implementations, `SaveQueue` (latest snapshot per character, retry with backoff, flush on shutdown), in-memory fakes for tests | core, libpq, asio | sim internals (works on snapshot DTOs) |
| `runity_server` (exe) | config, wiring, run loop, signal handling, shutdown order | all | - |

Sim never includes protocol or persistence headers: sessions translate wire messages to `sim::Command`s and sim `Event`s to wire
messages; persistence works on `CharacterSnapshot` value types produced by sim.

## 3. Ownership tree (RAII; destruction = reverse order)

```
main()
 `- Server (stack)
     |- Config                        value
     |- Logger                        unique_ptr<LogSink>
     |- ContentDb                     unique_ptr<const ContentDb>
     |- Persistence                   (PgPool, RedisClient, SaveQueue + worker jthread)
     |- WorldDirector                 map<WorldId, unique_ptr<World>>
     |- SessionManager                map<SessionId, unique_ptr<Session>>   (refs: WorldDirector&, Persistence&)
     `- NetServer                     asio io_context + jthread, Connections (shared_ptr owned by asio handlers only)
```
Shutdown (SIGINT/SIGTERM/Ctrl+C): stop accepting -> tell clients (`Failure{ServerShutdown}`) -> sessions leave worlds -> final
character snapshots enqueued -> SaveQueue drained with a deadline -> locks released -> net stopped. A failed final save is logged
loudly with the snapshot written to `failed-saves/<character>.json` (never silently dropped).

Non-owning access is by reference down the tree and by id across it (`EntityId`, `SessionId` resolved at use). No raw owning pointers,
no singletons, no mutable globals.

## 4. Threading

| Thread | Does | Shares via |
|---|---|---|
| net (asio) | accept, read, decode frames, write | `MpscQueue<Inbound>` (net -> sim), per-connection outbound queue (sim -> net, posted to the io_context) |
| sim (main) | fixed 50 ms tick (20 TPS, kept from the reference); drains inbound, ticks every world, flushes outbound, emits save intents | the two queues above + `SaveQueue` |
| persistence | executes saves/loads; completions posted back to the sim inbound queue as `PersistenceResult` | MPSC queue |

The reference ticks worlds in parallel over shared statics (F9 in Architecture.md); the new server ticks them sequentially first.
Worlds share no mutable state, so a job pool can be added later without changing systems.
Sleep: the sim thread sleeps until the next tick deadline (no busy spin; the reference's late-tick spin is unnecessary at 20 TPS).

## 5. Entity model

`EntityId` = 32-bit (index 20 bits | generation 12 bits), unique per world, never reused while the generation matches (fixes the
reference's slot reuse bug class). Storage: `World` owns dense component tables keyed by entity index
(`Transform`, `Kinematics`, `Health/Stats`, `Inventory`, `AiState`, `ProjectileEmitter`, `Replication`), plus an `EntityKind`
(Player, Enemy, Projectile-is-not-an-entity, Container, Portal, StaticObject, Npc). Projectiles are a separate per-world pool (as in the
reference) with explicit ids `(ownerEntity, shotId)`.
Replication: per player, an `InterestSet` from the sight radius (and line of sight in LoS worlds); per tick the server sends
`Snapshot{serverTick, ackInputSeq, entered[], left[], changed[]}` with typed field deltas (dirty bitmask per entity) - see
`Protocol/schema/protocol.toml`.

## 6. Movement (authoritative)

Client sends `MoveInput{seq, dtMs, dirX, dirY}` (unit or zero vector, quantized to i8) for every client simulation step (fixed 1/60 s
steps, batched up to 4 per message). The server applies inputs in order using the SAME movement function as the client
(`MovementRules`: speed formula, tile speed multipliers, collision against walls/NoWalk/OccupySquare, per-axis slide), with a time
budget: the sum of applied `dtMs` cannot exceed real elapsed time + 250 ms slack (speed-hack guard replacing the reference's distance
slack). The snapshot carries `ackInputSeq` + authoritative position; the client replays unacknowledged inputs (reconciliation).
Reference speed formula is kept, with its integer-division bug documented as an intentional fidelity decision (every class
currently moves at 4 tiles/s because Speed < 75); the fix is a one-line content/rules change and is tracked as an open decision.

## 7. Combat (authoritative)

`Shoot{shotId (u16, client-chosen, echoed), clientTimeMs, angle}`; the server checks fire rate (reference formula + leaky bucket),
weapon, ammo/mana, spawns projectiles from the server position, simulates paths (ProjectilePaths ported with shared test vectors from
the C# formulas), tests collisions against enemies, players (PvE only: player bullets never hit players) and walls on the server, and
applies damage (`round(roll * (0.5 + ATT/50))`, then `max(dmg - def, ceil(0.1 * dmg))` with Armored/ArmorBroken/Invulnerable rules).
The client spawns a cosmetic copy immediately keyed by `shotId` and lets the server's `ProjectileHit`/damage events drive results.

## 8. Persistence

Save points: on world leave, every 60 s while dirty, on death (death record + character, one
transaction), on item-moving actions that cross containers (one transaction, so no duplication), on shutdown. Load failure => the
client gets `Failure{CharacterLoadFailed}` and no partial character is created. Postgres down => logins refused, in-world play
continues, saves queue with backoff, an alarm log line every 10 s; on shutdown the queue is dumped to disk.

## 9. Config

`Server/config/gameserver.json` (ports, tick rate, content paths, limits, `characterStore` + `pgConnInfo`) + the `RUNITY_REDIS_URL`
environment variable. `pgConnInfo` holds the local development login (runity / runitypass); a server others can reach uses its own
password there. The reference's XML configs are not reused.

## 10. What is intentionally dropped

Roslyn behaviour hot-reload, async-on-game-thread SynchronizationContext, pooled `User` identity reuse, reflection registries,
dead packets (trade, guild, Aoe, Damage, PlaySound, Buy, Teleport, Reskin, old Ping/Pong, UpdateAck, EditAccountList), client
reported hits, implicit bullet counters, password in Hello, JSON-RPC to the account server, `.wmap` loader (no map uses it).

## 11. Implementation order (derived from the dependency graph)

1. Protocol schema + generator + golden vectors (C++ and C# tests) - nothing else can talk without it.
2. Server skeleton: core, config, logging, fixed-tick loop, clean shutdown; runs headless and exits on Ctrl+C.
3. Net: asio listener, framing, connection limits, idle/Hello timeout, Ping/Pong.
4. Database migrations (`Database/migrations`) + Account/API service minimal slice (register, login -> token in Redis).
5. Session: Hello{token} -> Redis session lookup -> account lock -> character list/load/create.
6. Content: XML definitions + `.jm` maps + world configs.
7. Sim: world, entity registry, tile map, spawn player, sight/replication, Snapshot.
8. Movement (shared rules with the client, reconciliation).
9. Projectiles + combat + damage + death.
10. AI (behaviour state machines for the live bestiary first), spawning zones.
11. Items, inventory, equipment, loot bags.
12. Character persistence (save queue), reconnect + world transfer tickets.
13. Chat + commands, portals and the remaining worlds.

Status of each step: MigrationStatus.md.
