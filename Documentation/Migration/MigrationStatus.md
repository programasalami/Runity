# Migration Status

Status values: COMPLETE / IN PROGRESS / BLOCKED / NOT STARTED / NEEDS TESTING / INTENTIONALLY DIFFERENT.
COMPLETE means: compiles, dependencies work, behaviour implemented, integration points work, tested, differences documented.
A row is never COMPLETE because code exists; the Tests column names what proves it.

Last updated: 2026-10-03.

**Re-baselined on the original Alloy (2026-10-03).** The project now ports the ORIGINAL open-source Alloy game (`zolmex/alloy-server`,
`NotTheLegend/AlloyClient`) instead of the Warriors & Wizards fork: original content (134 XMLs, maps, world configs, sprite sheets),
original classes / items / monsters, transpiled original monster behaviours, the original's rules where it has them. The architecture
(C++ authoritative server, Unity client, account service, protocol) stays. W&W-only content and code were removed (a backup exists).

## How to verify everything (all automated, none of it opens a game window)

| Suite | Command | What it covers |
|---|---|---|
| C++ server (doctest) | `Server\build.cmd` | protocol codecs + golden bytes, framing, net (real sockets), sessions, Redis contract (local Redis), content loading (real Content/, loaded once for the whole run), tile map, movement (+ shared vectors), world, rules, behaviour engine, realm spawner, game service: 117 cases |
| Account/API service (xUnit) | `dotnet test AccountService` | rules, hashing, HTTP API end to end in-process, Redis store (local Redis); PostgreSQL tests skip until `WAW_PG_TEST_CONNINFO` is set |
| Unity client core under .NET (NUnit) | `dotnet test Tests/ClientCore` | the Unity sources of WaW.Protocol / Net / Domain / Client + their EditMode tests, outside the editor |
| Unity EditMode (Unity Test Runner) | Unity `-runTests -testPlatform EditMode` | the same tests inside Unity's compiler and runtime |
| Unity PlayMode | Unity `-runTests -testPlatform PlayMode` | the Game scene signs in and enters the Nexus against the real API + C++ server processes |
| End to end (xUnit) | `dotnet test Tests/EndToEnd` | real C++ server process + API service + real Redis; both a scripted protocol client and the Unity client's own GameSession |
| Protocol generator | `python Protocol/generator/gen.py --check` | generated C++/C# code is current (run by `Server\build.cmd`) |

## Phase 1 - Audit and design

| Item | Status | Notes |
|---|---|---|
| Source audit (all areas) | COMPLETE | 16 subsystem documents written from the code with file:line citations and discrepancy sections |
| Engineering audit review | COMPLETE | `Docs/EngineeringAudit.md` checked against the code; stale claims recorded per document (e.g. "no sleep" loop, F25/F26/F27, "no character save", OpenGL version) |
| Architecture | COMPLETE | Architecture.md, decisions A1-A9 |
| C++ server design | COMPLETE | CppMigration.md |
| Unity client design | COMPLETE | UnityMigration.md |
| Protocol boundary design | COMPLETE | Protocol.md section 12 (new protocol); contract in `Protocol/schema/protocol.toml` |
| Migration roadmap | COMPLETE | CppMigration.md section 11 + this table |

## Phase 2 - Implementation

| # | Subsystem | Status | Tests | Notes |
|---|---|---|---|---|
| 1 | Protocol schema + generator + golden vectors | COMPLETE | C++ `test_protocol.cpp` (10 cases incl. 20 000 random payloads); C# `ProtocolTests` (61 in .NET and in Unity) | one TOML contract, generated C++ + C#, identical bytes on both sides (A5). Pre-release: stays version 1 until the first release |
| 2 | C++ server skeleton (config, logging, fixed tick, shutdown) | COMPLETE | `test_core`, `test_config`, `test_server_app`; E2E `ShutdownTellsPlayersAndReleasesLocks` | 20 TPS, sleeps between ticks (no spin); SIGINT/SIGTERM via asio; Ctrl+C by hand: NEEDS TESTING (automated shutdown uses --run-for-ms) |
| 3 | Unity project skeleton | COMPLETE | Unity EditMode 82/82 in batch mode; the same 82 under .NET (`Tests/ClientCore`) | assemblies WaW.Protocol / Net / Domain / Client (no UnityEngine) + Presentation + EditorTools; scene `Game` built by `WaW > Build Game Scene` |
| 4 | Network transport | COMPLETE | `test_net.cpp` (8 cases, real sockets); C# `NetTests`; E2E garbage/limits | asio server (per-address cap, idle timeout, frame cap, send-queue cap, flush-then-close); C# reader/writer threads, main-thread Poll |
| 5 | Connection / session management | COMPLETE | `test_session.cpp` (13 cases); E2E ticket reuse / account in use / version | Hello -> join ticket -> Redis lock (TTL + heartbeat); timeouts; Ping/Pong; kick on lost lock; fail-closed when Redis is down |
| 6 | Database migrations | NEEDS TESTING | `RulesTests.MigrationsAreNumberedAndOrdered`; PostgreSQL tests written, skipped | BLOCKED on a database: the project owner runs `Database/setup/create_database.sql` once (creating the role needs the PostgreSQL superuser password, which this session must not use) |
| 7 | Account/API service | IN PROGRESS | 36 tests (HTTP API, rules, Redis); 4 PostgreSQL tests skipped | register, login (bearer token), logout, account + characters, join ticket, character delete. Not yet: purchases, inbox, daily rewards, guilds, board, news, music, /public (Portal), /dev (dashboard), launcher remember-tokens |
| 8 | Redis integration | COMPLETE | C++ `redis account sessions follow the key contract`; C# `RedisSessionStoreTests`; E2E | keys `waw:session|join|lock:account` shared by both services (Redis.md "New key contract") |
| 9 | Content loading | COMPLETE | `test_content.cpp` (real Content/), C# `TheRealContentLoads` | the original's Definitions XML (2250 grounds, 3599 items, 14 classes), .jm and .wmap maps, 45 world configs, behaviour JSON; like the original, the first definition wins and duplicates / dangling names are warnings (counted in the startup log) |
| 10 | Entity system | IN PROGRESS | `test_sim.cpp` | ids with generations (no reuse while live), players + map entities; components for combat/AI come with those systems |
| 11 | World system | IN PROGRESS | `test_sim.cpp`, `test_realm.cpp`, `test_game.cpp` | the original Nexus (142 wide) + Realm Portal, realms from a random 2048 x 2048 `.wmap` named after a monster, spatial grid, tiles sent only near players, monsters think only near players (WorldSystem.md). Not yet: realm events / closing, Vault, guild halls, dungeons |
| 12 | Player system | IN PROGRESS | `test_game.cpp`, `test_combat.cpp`, E2E | all 14 original classes, original starting gear; stats stored per character and grown by LevelIncrease (cap 20), original XP curve, LEGACY kill XP sharing + cap, 500 XP per fame, regeneration, permadeath (Combat.md). Not yet: skins, quests, gravestones, account fame |
| 13 | Movement (authoritative + prediction) | COMPLETE | C++ movement tests + `movement.json` vectors; C# `MovementTests` (bit-for-bit with C++); E2E `LoginEnterAndWalkWithoutPredictionErrors` (0 corrections against the real server) | INTENTIONALLY DIFFERENT authority model (A2): inputs, not positions; time-budget speed-hack guard. Reference speed formula kept incl. its integer division (open decision 1). Slide tiles: no content uses them, not implemented |
| 14 | Entity synchronization (snapshots) | COMPLETE | `test_sim`, `test_game`, C# `ClientWorldTests`, E2E | entered / left / changed with presence masks; per-player tile streaming; client interpolation buffer + server clock |
| 15 | Combat / projectiles | COMPLETE | `test_combat.cpp` (formulas, paths, XP sharing, conditions, enemy fire, death, walls, fire rate, level-up, regen); C# `CombatTests`; E2E `AWizardHuntsRealmMonstersAndEarnsXp` (the Unity client's code hunts original realm monsters for XP) | INTENTIONALLY DIFFERENT: the server decides every hit; one Shoot per attack; walls stop server bullets; damage roll includes MaxDamage. Defense floor 15 %, Armored / ArmorBroken / Invulnerable. Abilities not yet |
| 16 | AI | IN PROGRESS | `test_behavior.cpp` (transpiled realm monsters: states, rings, spawns, protect, rage, taunts, death scripts, invulnerable traps, every behaviour runs), `test_realm.cpp` (terrain spawner) | data-driven state machines transpiled from the original BehaviorLib (Lowland, Midland, Highland, Mountain, Shore + 4 event files, 142 monsters), engine with the original tick order, 29 primitives and 10 transitions; LEGACY terrain spawner (1.5 % per terrain, 25 s repopulation) (AI.md). Not yet: realm events (set pieces), dungeon behaviours |
| 17 | Items / inventory / equipment / loot | IN PROGRESS | `test_items.cpp` (gear bonuses + slot rules, potions, stat potions, drops, soulbound bags, bag types, 8 per bag), `test_behavior.cpp` (public Pirate potions, soulbound tier loot); E2E `DropAnItemIntoABagAndTakeItBack` | the original LootDrop / ItemLoot (per damage record, threshold + chance, public vs per-account bags, the original bag objects) fed by the original's commented loot tables; TierLoot = a random item of that tier and class. Not yet: abilities, potion-stack use, backpack purchase |
| 18 | Character persistence | IN PROGRESS | `test_game` (in-memory repository) | repository interface + in-memory implementation; per-character base stats (`stats`) and potion stacks in the record and in migration 0001 (no `role`, no `stat_potions`). The PostgreSQL implementation waits for the database (row 6). The server warns at start that characters are not saved |
| 19 | World transitions | IN PROGRESS | `test_game.cpp` portal test | Nexus <-> Realm through the Realm Portal (F) and Escape (R), inside one server process (INTENTIONALLY DIFFERENT: no reconnect). Not yet: Vault, guild halls, dungeons (their portals refuse) |
| 20 | Chat | IN PROGRESS | `test_game`, E2E | world chat, 256-byte limit. Not yet: commands, whispers, guild chat, mutes |
| 21 | Unity entity + world presentation | NEEDS TESTING | PlayMode `SignsInEntersTheNexusAndDrawsIt`: passed (1257 tiles, 6 entities, screenshot `TestResults/playmode-world.png`), then failed once because the API's first answer took 10.8 s on a memory-starved PC (client timeout was 10 s; now 30 s + a warm-up in the test). Re-run pending; a person has not played it yet | Tilemap ground + objects, pooled entity views, camera follow + free turn (Q/E, Z resets), placeholder art via ArtCatalog |
| 22 | UI | IN PROGRESS | PlayMode (sign-in path) | uGUI built in code: sign in / register, characters (play / create class + role), HUD (status, chat, leave, disconnect overlay). Not yet: inventory, options, every reference window |
| 23 | Animation / VFX / audio | NOT STARTED | - | waits for the separately sourced assets |
| 24 | Launcher adaptation | NOT STARTED | - | the client already reads `WAW_LAUNCH_TOKEN`; the launcher's API calls must move to /api/v1 |
| 25 | Portal integration | NOT STARTED | - | needs the API's /public routes |
| 26 | Deployment | NOT STARTED | - | |
| 27 | End-to-end tests | IN PROGRESS | `Tests/EndToEnd` (8 tests) | Connect, Authenticate, Select/Create character, Enter world, Spawn player, Spawn entity (map entities), Move, Receive state, Chat, Disconnect, Reconnect, Reload character: covered. Attack, Interact, Save to PostgreSQL: not yet migrated |
| 28 | Profiling / optimization | NOT STARTED | - | correctness first |

## Retired (INTENTIONALLY DIFFERENT, not ported)

WebClient shim build (A8), OpenGL renderer + HD 4400 workarounds, UiLib, ContentBuilder/ContentReader atlases, ShaderSourceGen,
JSON-RPC link between servers (A3), client-reported hit packets and implicit bullet ids (A2), dead protocol ids (Protocol.md 9.3 item 10),
Roslyn behaviour hot-reload, promote.ps1, password-per-request authentication.

## Intentional differences recorded so far

| Area | Reference | New | Why |
|---|---|---|---|
| Authentication | username + password on every HTTP call and in every Hello | bearer token for HTTP; single-use join ticket (60 s) in Hello | credentials never reach the game server (AccountServer.md) |
| Account lock | Redis key without TTL, owner = server | owner = server/session, 30 s TTL refreshed every 10 s, owner-checked release | a crashed server's locks expire; the same account cannot play twice on one server |
| Movement authority | client sends positions, server checks plausibility | client sends inputs, server simulates, client predicts + reconciles | Movement.md |
| Sink level | raised once per server tick | raised per input step by step time (same 50 ms rate) | deterministic on both sides |
| Map edge | the client could walk slightly past the top/left edge (C# int truncation of negatives) | negative coordinates are outside the map | bug fix |
| Static map objects | every placed object was an entity | static objects + walls travel in tile data | they never change; less traffic |
| Snapshots | NewTick every tick + Update on visibility change, stat id/value lists | one Snapshot per tick with typed presence-masked deltas | Protocol.md section 12 |
| Level-up | the original never levels up (the modern server gives no XP at all) | cap 20, stats grow by `<LevelIncrease>` rolls, stored per character | owner decision: classic RotMG data already in Players.xml |
| XP / fame | none in the modern server | LEGACY: ceil(MaxHP/10) x XpMult to nearby players, capped at 10 % of the goal; 1 fame per 500 XP (carried) | the modern server is unfinished |
| Regeneration | none in the modern server (LEGACY: VIT / WIS per second) | kept: HP 1 + 0.12 VIT /s, MP 0.5 + 0.06 WIS /s | owner decision |
| Loot | one active LootDrop (Pirate); TierLoot a stub | the commented original loot tables as soulbound loot; TierLoot defined | AI.md section 4 |
| Monster AI | C# code; monsters ignore walls; radius / spawn-count bugs | JSON transpiled from that code; monsters collide; bugs fixed | AI.md section 4 |
| Monster activity | every monster ticks | only within 40 tiles of a player | ~28 000 monsters per realm |
| Realm creation | at Nexus start | on first portal use | start-up time, memory |
| Second login | passed the lock check on the same server | refused with AccountInUse | Redis.md |

## Open decisions for the project owner

1. Movement speed: the original's `Speed/75` integer division makes the Speed stat do nothing below 75 (all classes move at 4 tiles/s).
   Kept for fidelity on both sides (one constant each in `movement.cpp` / `MovementRules.cs`). Keep or fix?
2. Old player data: DECIDED 2026-10-03 - start fresh, no import.
3. Enemy-bullet fairness: plain server-authoritative hits first (decided), "favour the dodger" rewind later?
4. Local database: run `Database/setup/create_database.sql` (needs the PostgreSQL superuser) so PostgreSQL persistence can be built and tested.
5. DECIDED 2026-10-03 - rules where the original has none: level cap 20, LevelIncrease growth, LEGACY XP / fame, 15 % defense floor,
   the port's regeneration curve.
