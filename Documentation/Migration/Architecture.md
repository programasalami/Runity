# Warriors & Wizards - Target Architecture

Written 2026-10-02 after the source audit. Every claim about the reference implementation is sourced in the subsystem documents
listed at the end; this file records the **decisions** and the reasons for them. REF = `Reference - This Is The Source Being Ported To
Unity/Warriors-and-Wizards-Testing`.

---

## 1. What the reference actually is (summary of the audit)

```
 Desktop client (C#/.NET 10, OpenTK, GL 3.3)        Browser client (same sources, WASM + WebGL2 shims)
   |  HTTP form POST :8080 (password on every call)      |  WSS -> nginx -> ww-bridge (python) -> TCP :2050
   |  TCP :2050 (custom binary, password in Hello)       |
   v                                                     v
 AccountServer (C#, HttpListener :8080) <---- JSON-RPC over TLS :8081 (pinned cert + shared secret) ---- GameServer (C#, TCP :2050)
   |  the ONLY process that touches the databases                                                         | 20 TPS simulation,
   v                                                                                                      | no DB access of its own
 PostgreSQL (accounts JSONB document + side tables)        Redis (account locks only, no TTL)
```

Findings that shape the design (details: Protocol.md, GameServer.md, Movement.md, Combat.md, Persistence.md, Redis.md):

| # | Finding | Source doc |
|---|---|---|
| F1 | The client is authoritative for its own position (sends absolute X,Y each NewTick); the server only checks plausibility (~2.25x max speed slack, destination-tile walls) and snaps back with Goto. First Move after entering a world is not checked. | Movement.md |
| F2 | Hits on enemies are reported by the client (`EnemyHit`) and accepted inside a 2-tile / 400 ms window, through walls, against any damageable target (including other players). Server-side bullets ignore walls. | Combat.md |
| F3 | Bullet ids are implicit counters that must stay in lock-step on both sides (wrap 2000). | Protocol.md U3 |
| F4 | No ticks, sequence numbers, timestamps, ping or idle timeout in the game protocol. | Protocol.md U4, U10 |
| F5 | Passwords travel in plain text in every HTTP call and every `Hello` (also on each world switch). Same-socket reconnect trusts the client's next world id. | Protocol.md U8, U9; AccountServer.md |
| F6 | Serializers are hand-written twice; 6 live client/server wire mismatches exist (stat enum divergence above 82, AccountList, colours, ShowEffect, non-fatal Failure, string stats 122-125). | Protocol.md section 10 |
| F7 | The GameServer reaches persistence only through RPC to the AccountServer; several paths still overwrite the whole account JSONB document through a write-behind queue that reports success before commit and drops failed batches. | Persistence.md, PostgreSQL.md |
| F8 | Redis holds only account locks: no TTL, owned per server (not per session), so one account can play twice on the same server. | Redis.md |
| F9 | Worlds tick in parallel (`Parallel.ForEach`) while sharing static mutable state (`RealmManager`, projectile owner tables, Campsite/GuildHall dictionaries, WeatherControl). | GameServer.md, ServerArchitecture.md |
| F10 | Content (Objects/Equip/Ground/Players/... XML, ~230 KB) lives in the CLIENT's `Content/Xmls` and is linked into the server build: the server and client already share one definition set. Maps are `.jm` (JSON + zlib big-endian int16 tiles). | WorldSystem.md, ClientArchitecture.md |
| F11 | Most of the client is presentation that only exists because of the custom GL engine and the Intel HD 4400 constraints (atlas builder, shader generator, UI library, web shims). | ClientArchitecture.md, UnityMigration.md |
| F12 | Only a small live bestiary (beach cubes) is active; the behaviour system is large but mostly dormant. Condition effects and abilities are largely unimplemented. | AI.md, Combat.md |

---

## 2. Target architecture (decided)

```
                                   UNITY CLIENT (C#, Unity 6 URP)
              WaW.Protocol (generated) . WaW.Net . WaW.Domain (no UnityEngine) . Presentation . UI
                     |  HTTPS JSON (login, characters, shop, mail, rewards)       |  TCP binary GAME PROTOCOL (token auth)
                     v                                                            v
        ACCOUNT / API SERVICE (C#, ASP.NET Core)                     C++ GAME SERVER (headless, C++23)
        accounts, credentials, session tokens,                      net . sessions . worlds . entities . movement
        character list/create/delete, purchases,                    combat . projectiles . AI . inventory . loot
        mail/inbox, daily rewards, guilds, public                   validation . replication . character persistence
        Portal API (/public/*), dev API (/dev/*)
                     |                     \                     /            |
                     v                      \                   /             v
                PostgreSQL  <----------------+----- Redis -----+--------> PostgreSQL
           (one database, table ownership           sessions/tokens (TTL), account locks (TTL + heartbeat),
            per service - see section 5)             world-server registry, rate-limit counters
```

### Decision A1 - Keep a separate Account/API service (C#), do NOT merge it into the C++ GameServer
Reason (AccountServer.md): it serves 45 HTTP routes (registration, password hashing, launcher tokens, purchases, mail, daily rewards,
guild/board/news, the public Portal API, the developer dashboard API). None of this is simulation; all of it is request/response
CRUD with transactional SQL that already exists and is tested in C#. Porting it to C++ would add risk and no value; embedding it
in the game server would couple a crash-sensitive real-time process to web traffic. The Launcher, Installer, Portal and Developer
Dashboard all depend on these endpoints (SourceInventory.md), so keeping the service keeps them working.
Change: it becomes ASP.NET Core (Kestrel) behind TLS, issues **session tokens** (no password after login), and loses its role
as the GameServer's RPC hub (A3).

### Decision A2 - The C++ GameServer is authoritative for all simulation
Movement, projectiles, hits, damage, AI, loot, inventory and world entry are decided by the server. The client sends **intents**
(movement input with sequence numbers, shoot requests with an explicit client shot id, use item, swap, portal), never results.
The `EnemyHit` / `PlayerHit` / `SquareHit` / `OtherHit` packets and the implicit bullet counter are dropped (F2, F3).
Responsiveness (RotMG feel) is kept with **client-side prediction + server reconciliation** for the local player and cosmetic
local bullets (Movement.md, Combat.md recommendations). Known trade-off, documented in Combat.md: enemy bullets are tested against
the server's player position (which trails the client by ~RTT/2); a "favour the dodger" rewind can be added later behind a flag.

### Decision A3 - The GameServer talks to PostgreSQL and Redis directly through its own persistence layer
The reference routes every game write through JSON-RPC to the AccountServer carrying whole DB models (F7, Protocol.md U12). The
target diagram in the task puts persistence inside the GameServer, and the audit shows the RPC hop is the source of the
whole-document overwrite and lost-write bugs. The GameServer therefore gets `persistence/` (repository interfaces + libpq
implementation + a save queue) and writes **only the tables it owns** with intent-based statements (save character snapshot,
record death, move item to storage), never whole accounts. Cross-service actions that need the other side's tables are done with
single conditional SQL statements (e.g. gold spend: `UPDATE ... SET gold = gold - $1 WHERE id=$2 AND gold >= $1`), which is
safe from either service. The TLS JSON-RPC link is retired.

### Decision A4 - Redis = transient state only
Keys (all with TTL): `session:<token>` (account id, issued by the API service, read by the GameServer at Hello), `lock:account:<id>`
(owner = server id + session id, TTL refreshed by heartbeat; released on clean leave; a crashed server's locks expire),
`transfer:<ticket>` (single-use world-transfer ticket, bound to target world), `ratelimit:*` counters (login, registration). Redis
unavailable => no new logins (fail closed), existing sessions continue and saves continue (they go to Postgres). PostgreSQL remains
the only store of record.

### Decision A5 - One protocol schema, generated codecs for C# and C++ (clean wire break)
The new Unity client is a new client; there is no requirement to talk to old clients (the reference keeps its own server for them),
so the wire format is a clean break. Messages, enums (message ids, stat ids, condition effects, failure codes, projectile path
types) and shared structs live in `Protocol/schema/*.toml`; `Protocol/generator/gen.py` (Python 3.12, stdlib only) emits
`Server/libs/protocol/generated/*.hpp` and `WaW/Assets/Scripts/Protocol/Generated/*.cs`. Golden byte vectors generated from the
schema are decoded by BOTH test suites, so the two codecs cannot drift (fixes F6). Framing: `[u32 LE payload length][u16 LE
message id][payload]`; little-endian fixed-width primitives, UTF-8 strings with u16 length (kept from the reference: sound and
simple, Protocol.md 9.1). Details and every message: Protocol.md section 12 (new protocol).

### Decision A6 - Content stays data, shared by server and client, art referenced by key
The reference XML definitions (Objects, Equip, Ground, Players, Projectiles, Containers, StaticObjects) are the gameplay source of
truth and are already shared (F10). They are copied into `Content/Definitions/` (repo root) and loaded by the C++ server (pugixml)
and, for presentation and prediction, by the Unity client (imported at edit time into a read-only catalog). Gameplay values never
live only in Unity assets. Art is referenced by **key** (`<Texture><File>sheet</File><Index>n</Index>` becomes an opaque art key);
Unity maps keys to sprites/models/sounds through replaceable `PresentationCatalog` ScriptableObjects with a visible placeholder
fallback, so all art can be swapped without touching gameplay. Maps stay `.jm` (the existing map editor writes it; Tools verdict
KEEP) in `Content/Maps/`, with world configs `Content/Worlds/*.json`.

### Decision A7 - Unity is presentation + prediction; domain logic is plain C#
Assemblies: `WaW.Protocol` (generated, no UnityEngine), `WaW.Net` (transport, framing, session state machine; no UnityEngine),
`WaW.Domain` (client world model, entity table, interpolation buffers, prediction/reconciliation, content catalog; no UnityEngine),
`WaW.Presentation` (MonoBehaviours, view pooling keyed by entity id, camera, input, audio, VFX), `WaW.UI` (UI Toolkit), and tests
(EditMode for the plain assemblies, PlayMode for presentation). No GameObject is a server entity; a view is created from client
state and destroyed when the entity leaves. See UnityMigration.md.

### Decision A8 - Retire the WebClient shim build; WebGL later via WebSocket
The WebClient exists only to run the OpenTK client in a browser (shims, source patching, shader porting). It has no role after the
Unity port. A Unity WebGL build is possible later; it needs a WebSocket transport behind `WaW.Net`'s `ITransport` and either native
WebSocket support in the C++ server or a gateway that forwards the real client address. Not in the first milestones.

### Decision A9 - Launcher: keep and adapt; Portal: keep; Tools: per SourceInventory.md verdicts
The Launcher is engine-independent (downloads by manifest, hands `WAW_LAUNCH_USER` / `WAW_LAUNCH_TOKEN` to the game); the Unity client
reads the same two variables. The Portal is a static site on the API service's `/public/*` endpoints; it does not couple to the
client. The map editor (Tools/Editor) is kept because maps stay `.jm`. Deployment: one VPS, systemd units for the API service and the
game server, nginx with TLS in front of the API, port 8080 no longer public, CI-built artifacts (SourceInventory.md, Deployment).

---

## 3. Repository layout (new)

```
Runity/
  Documentation/Migration/     the 19 migration documents (this folder)
  Protocol/
    schema/                    *.toml - the protocol contract
    generator/gen.py           emits C++ and C# codecs + golden vectors
    vectors/                   golden byte vectors (generated, checked in)
  Content/
    Definitions/               object/item/ground/player/projectile XML (copied from REF, then owned here)
    Maps/                      .jm maps
    Worlds/                    world configs (.json)
  Server/                      C++ game server (CMake)
    libs/core  libs/protocol  libs/content  libs/sim  libs/net  libs/session  libs/persistence
    app/                       main, config, wiring
    tests/                     unit + integration tests (doctest)
  AccountService/              C# ASP.NET Core Account/API service (+ tests)
  Database/migrations/         numbered SQL migrations (shared schema, ownership per table)
  WaW/                         Unity client project
  Reference - .../             read-only reference implementation
```

## 4. Process / port map (development)

| Process | Listens | Talks to |
|---|---|---|
| Unity client | - | API service HTTPS/HTTP :5080 (dev), GameServer TCP :2050 |
| Account/API service | :5080 | PostgreSQL :5432, Redis :6379 |
| C++ GameServer | :2050 (game), :2051 (admin/health, localhost only) | PostgreSQL, Redis |
| PostgreSQL 17 | :5432 | - (database `waw`, separate from the reference's `alloy`) |
| Redis (Memurai) | :6379 | - (key prefix `waw:` to stay clear of the reference) |

## 5. Table ownership

| Owner | Tables (see PostgreSQL.md for the new schema) |
|---|---|
| Account/API service | accounts, credentials, login_tokens, account_currency writes from purchases/rewards, inbox_messages, daily_rewards, guilds, bans, mutes, bug_posts, patch_notes, skins owned |
| GameServer | characters, character_items (inventory/equipment), character_stats, deaths, storage (campsite chests), gift_chests take, anticheat_flags |
| Both (conditional single statements only) | account currency (gold/fame) spend/grant, `logins.last_login_at` |

## 6. Threading model of the GameServer (summary; CppMigration.md has the detail)

One network thread (asio) decodes frames into per-session inbound queues; one simulation thread runs a fixed 20 Hz tick (world ticks
may later be spread over a job pool because worlds share no mutable state); one persistence worker drains the save queue. Systems never
touch sockets or SQL; they emit outbound messages and persistence intents.

## 7. Priorities and non-goals

Correctness -> architecture -> behavioural fidelity -> testability -> maintainability -> performance -> content.
Non-goals for the first milestones: old-client wire compatibility, migrating live VPS data (an importer from the old JSONB `accounts`
document can be written later; PostgreSQL.md describes the mapping), WebGL, the in-client Portal, the launcher's Unity packaging.

## 8. Documents

SourceInventory, DependencyMap (inventory + runtime map) - Protocol (old protocol + new design) - ClientArchitecture, UnityMigration
(client) - ServerArchitecture, GameServer, CppMigration (server) - AccountServer, PostgreSQL, Redis, Persistence (services + data) -
EntitySystem, WorldSystem, Movement, Combat, AI (simulation) - MigrationStatus (status of everything).
