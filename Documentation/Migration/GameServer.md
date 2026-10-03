# GameServer

## Current state (2026-10-03, re-baselined on the original Alloy)

The C++ server (`Server/`) now runs the ORIGINAL Alloy content and rules (`zolmex/alloy-server`); the W&W content and rules are gone.

- Startup (`Server/app/main.cpp`): config -> `ContentDb::load(Content/)` (the original's 134 XMLs, 54 `.jm` + 3 realm `.wmap` maps,
  45 world configs, 9 behaviour files) -> one info line with the counts and the number of content warnings (each warning is logged at
  debug level; the original XML has dangling names and duplicate world names, first definition wins) -> Redis sessions -> network ->
  `GameService::init` (Nexus with its Realm Portal; the realm opens on first use, or at once when it is the configured entry world).
  A debug build loads the content in ~6 s and builds a realm with ~28 000 monsters in ~1-2 s (176 MB in total).
- Worlds: WorldSystem.md. Rules: Combat.md. Monster AI, realm spawner and loot: AI.md.
- Characters (`persistence::CharacterRecord`, table `characters`): class, level, XP within the level, fame, HP / MP, the character's own
  base stats (`stats`, grown on level-up), 20 item slots, the two potion stacks, backpack flag. Creation (no role): any of the 14
  classes, skin 0, class Equipment, start stats, potion stacks 1 / 1; at most 2 living characters per account (the original MaxChars).
- Client-visible behaviour that changed without a protocol change: WorldInfo.displayName is the instance's name (a realm is called
  e.g. "Medusa"); monster taunts arrive as `ChatMessage` (Say) whose sender is the monster's entity id and object id; entity
  `conditions` bits are now set (original ConditionEffectIndex numbering).

Everything below is the 2026-10-02 audit of the W&W GameServer that the first port followed. It is kept for the parts that still
describe the architecture (loop, networking, sessions); where it talks about W&W content or rules (roles, Campsite, Tutorial, cats,
zones, starter rewards) it is historical.

---

## W&W audit (historical)

Audit date: 2026-10-02. Scope: `WaW-Server/GameServer` and the parts of `WaW-Server/Common` it uses.
All paths are relative to REF = `Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing`.
`file:N` = line N in the reference source. "UNVERIFIED" = not confirmed in code during this audit.
Implementation status of every new-architecture item is NOT STARTED (audit phase only).

Related documents: `ServerArchitecture.md` (module structure, statics, C++ layout), `EntitySystem.md`, `WorldSystem.md`.

---

## 0. One-paragraph summary

The GameServer is a .NET 10 console process (`WaW-Server/GameServer/GameServer.csproj`) that accepts raw TCP on port 2050, keeps a
pool of 1000 pre-allocated `User` objects, and runs one main loop (`WaW-Server/GameServer/Game/GameLogic.cs:33-90`) at 20 TPS
(50 ms). Between ticks the main thread sleeps 1 ms at a time and on every iteration drains a global action queue, applies pending
entity removals of every world, and handles every user's incoming packets and flushes their send buffers
(`GameLogic.cs:214-244`). Every 50 ms it ticks all worlds **in parallel** with `Parallel.ForEach` (`GameLogic.cs:246-260`).
Accounts, characters and all persistence live in the AccountServer, reached through StreamJsonRpc over TLS (`WaW-Server/Common/Messaging/IpcClient.cs:12-24`).
Almost all coordination state is static (see `ServerArchitecture.md` section "Global state inventory").

---

## 1. Startup sequence

### Reference behavior
`WaW-Server/GameServer/Program.cs:24-64`, in order:
1. Read the assembly informational version, set console title, set `CultureInfo.InvariantCulture` on the main thread (`Program.cs:25-29`).
2. Install `AppDomain.UnhandledException` -> `_log.Fatal` (`Program.cs:31`, `:112-114`).
3. `OwnerSettings.SeedAll()` - copies missing owner setting files into the settings folder (`Program.cs:34-36`; `WaW-Server/Common/Resources/Config/OwnerSettings.cs:198-213`).
4. `GameServerConfig.Config` (lazy, cached by `ConfigLoader<T>`) (`Program.cs:38`).
5. Inside an `EasyTimer` log scope (`Program.cs:39`):
   1. `EnumUtils.Load()` (`:40`)
   2. `XmlLibrary.Load(config.XmlsDir)` - all `*.xml` under `Resources/Xml/Data/` (`:41`; `WaW-Server/Common/Resources/Xml/XmlLibrary.cs:45-69`)
   3. `MerchantsLibrary.Load(config.MerchantsDir)` (`:42`)
   4. `WorldLibrary.Load(config.WorldsDir)` - all world `*.json` configs and their maps, parsed with `Parallel.ForEach` (`:43`; `WaW-Server/Common/Resources/World/WorldLibrary.cs:25-50`)
   5. `BehaviorLibrary.Load()` (`:44`)
   6. `CommandManager.Load()` - reflection over `Command` subclasses (`:45`; `WaW-Server/GameServer/Game/Systems/Chat/Commands/CommandManager.cs:15-35`)
   7. `IpcClient.ConnectAsync(new GameServerRpcHandler(), Timeout(5 s))`, then `AccountServerRpc.GameServerConnected(Guid)` (`:47-50`). **If this fails the exception escapes `Main` and the process dies** (no try/catch).
   8. Fire-and-forget `MaintainAccountServerConnectionAsync` (reconnect loop, 5 s backoff) (`:52`, `:84-110`).
   9. `RealmManager.Init()` - creates the Nexus world (which creates the Realm worlds through its portals) (`:54`; `WaW-Server/GameServer/Game/RealmManager.cs:31-35`; `WaW-Server/GameServer/Game/Worlds/Logic/Nexus.cs:20-33`).
   10. `SocketServer.Start(config.Port, config.MaxPlayers)` (`:57`).
6. `InstallShutdownHandlers()` (`:60`, `:68-79`).
7. `GameLogic.Run(config.MsPT)` - blocks until stop (`:61`).
8. `await GameLogic.ShutdownAsync()`; `Logger.Flush(2000)` (`:62-63`).

Note: the socket accepts connections (step 5.10) before the game loop starts (step 7). Packets that arrive in between sit in each
user's `_pendingReceive` queue until the first `Update()` (`WaW-Server/GameServer/Game/Network/NetworkHandler.cs:154`).

### Current implementation
As above. The process GUID (`Program.Guid`, `Program.cs:20`) identifies this game server to the AccountServer (account locks).

### New architecture (RECOMMENDATION)
`main()` builds an explicit object graph, each step returning a value or an error, no lazy statics:
`Config cfg = load_config(paths)` -> `ContentDb content = load_content(cfg)` (XML + world configs + maps, immutable after load) ->
`AccountServiceClient acct(cfg.rpc)` (non-fatal: retries in background, game refuses logins until connected) ->
`Simulation sim(content, cfg)` (creates Nexus) -> `NetServer net(cfg.port, cfg.max_players)` (listening only after the simulation is
ready) -> `run_loop(sim, net, acct)`. Shutdown in reverse construction order through RAII destructors plus an explicit
`sim.save_all()` before destruction.

### Implementation status
NOT STARTED

### Differences
Listening starts after the simulation exists; the account-server connection does not crash startup.

### Reason for differences
Reference crashes if the AccountServer is down at start (`Program.cs:47`) and opens the port before the loop runs.

### Tests
None in the reference cover `Program.Main`. `OwnerSettingsTests` (`WaW-Server/Tests/Common.Tests/OwnerSettingsTests.cs`, 7 tests) covers the settings folder.

### Known issues
- AccountServer down at startup = process crash (`Program.cs:47`).
- Port opened before the loop runs (`Program.cs:57` vs `:61`).

---

## 2. Configuration

### Reference behavior
- `gameServerConfig.xml` (`WaW-Server/Common/Resources/Config/Data/gameServerConfig.xml:1-15`): XmlsDir `Resources/Xml/Data/`,
  MerchantsDir `Resources/Xml/Merchants/`, WorldsDir `Resources/World/Data/`, Port 2050, Address 127.0.0.1, ServerName "US East",
  **TPS 20**, MaxPlayers 1000, Version `0.3.16`, AdminOnly (commented out), BehaviorsDir `../../../GameServer/Game/Entities/Behaviors/Library`,
  RealmCount 1, MaxClientsPerIP 10.
- `GameServerConfig` (`WaW-Server/Common/Resources/Config/GameServerConfig.cs:13-29`): `MsPT = 1000 / TPS` (`:22`) = 50 ms.
  `AdminOnly = e.HasElement("AdminOnly")` (`:26`).
- Owner override file `serverSettings.xml` (`GameServerConfig.cs:31-52`): ServerName, MaxPlayers, RealmCount, MaxClientsPerIP,
  AdminOnly (OR-ed). Applied once at construction; GameServerConfig itself is not an owner setting, so it is never re-read (restart needed).
- `ConfigLoader<T>` (`WaW-Server/Common/Resources/Config/ConfigLoader.cs:85-128`): one static cached instance per config type; files that are
  owner settings (`OwnerSettings.IsSetting`) are re-checked at most every 2 s and hot-reloaded when their mtime changes (`:95`, `:109-125`).
- `OwnerSettings` (`WaW-Server/Common/Resources/Config/OwnerSettings.cs:150-176`): folder = `WAW_SETTINGS_DIR`, else `/opt/WaW/settings` on Linux,
  else none. Entries: daily-rewards.txt, new-accounts.xml, new-characters.xml, game.xml, server.xml.
- Other configs the game server reads: `gameConfig.xml` (StarGoals `20,150,400,800,2000`, `WaW-Server/Common/Resources/Config/Data/gameConfig.xml`),
  `realmConfig.xml` (realm names; its `<Close>` and `<Event>` entries are parsed by `RealmConfig.cs:28-33` but **never used** by game code - only
  `Names` is read, `RealmManager.cs:69`), `newAccountsConfig.xml` (storage price, `StorageShop.cs:33`), `rpcClientConfig.xml` (RPC host/port/cert).
- World configs are JSON (see `WorldSystem.md`).
- Environment variables: `WAW_SETTINGS_DIR`, `WAW_DEBUG_LOG` (debug log lines, `WaW-Server/Common/Utilities/Logger.cs:30,35`),
  `WAW_COMBAT_TRACE` (combat trace, per CLAUDE.md:541; implementation in `Systems/Combat/CombatTrace.cs`, not read in detail - UNVERIFIED).

### Current implementation
Static, lazily loaded singletons (`GameServerConfig.Config`, `GameConfig.Config`, ...) read from anywhere, any thread.

### New architecture (RECOMMENDATION)
One `ServerConfig` value struct parsed at startup (format can stay XML or move to JSON/TOML), validated (TPS > 0, ports, limits),
passed by const reference to modules that need it. Hot-reloadable "owner settings" become an explicit `SettingsWatcher` that posts
a new immutable `LiveSettings` snapshot into the simulation's command queue. Drop `BehaviorsDir` (no runtime C# compilation).

### Implementation status
NOT STARTED

### Differences
No hidden statics; reload is an explicit event applied at a tick boundary.

### Reason for differences
Reference reads configs lazily from all threads (`ConfigLoader.Load` is called from the game thread, RPC threads, and parallel world ticks).

### Tests
`OwnerSettingsTests` (Common.Tests, 7). No GameServerConfig parsing test found.

### Known issues
- `BehaviorsDir` points to `GameServer/Game/Entities/Behaviors/Library` which does not exist (behaviours live in `GameServer/Game/Systems/Behaviors/Library`); hot reload is compiled out by default (`BehaviorLibrary.cs:106-107`, `GameServer.csproj` BehaviorHotReload=false).
- `TPS` must divide 1000; `MsPT = 1000 / TPS` truncates (`GameServerConfig.cs:22`).
- `realmConfig.xml` `<Close>`/`<Event>` are dead config.

---

## 3. Main loop, ticks and threading

### Reference behavior
`GameLogic.Run(int mspt)` (`WaW-Server/GameServer/Game/GameLogic.cs:33-90`):
- `TPS = 1000 / mspt` (`:34`) = **20 TPS, 50 ms per tick**.
- Installs `GameThreadSynchronizationContext` on the main thread (`:35-36`); raises its priority to AboveNormal (`:37`); starts the
  `CharacterSaver` worker (`:38`); calls `timeBeginPeriod(1)` on Windows (`:42-43`).
- Loop while `!_stopRequested` (`:45`):
  1. `Update()` (`:46`, body `:214-244`) - runs **every loop iteration**, i.e. roughly every 1 ms:
     - `DrainPendingActions()` - the global `ConcurrentQueue<Action>` (`:17`, `:156-168`), each action try/caught.
     - `world.Update()` for every world, sequentially (`:223-230`) - applies queued entity removals (`WaW-Server/GameServer/Game/Worlds/World.cs:258-261`).
     - for every connected user, sequentially: `Network.HandleIncomingPackets()` then `Network.SendSocketData()` (`:235-243`).
  2. If more than 2 ms remain until the tick: `Thread.Sleep(1)` and loop (`:50-54`). If 0-2 ms remain: `Thread.SpinWait(50)` and loop (`:55-58`).
  3. Tick due: update `WorldTime` (`RealmTime` struct: `TickCountDecimal`, `TickCount`, `TotalElapsedMs`, `ElapsedMsDelta`;
     `WaW-Server/GameServer/Game/RealmTime.cs:3-8`) from the stopwatch (`:60-67`). `ElapsedMsDelta` is the real elapsed time, so the
     simulation step is variable, not fixed.
  4. Late-tick accounting: `ElapsedMsDelta >= 1.5*mspt` counts as late; `>= 5*mspt` also logs `LAGGED` (`:69-73`).
  5. `TickWorlds(WorldTime)` = `Parallel.ForEach(RealmManager.Worlds.Values, world => world.Tick(ref localCopy))` (`:246-260`). Each
     world is ticked on a thread-pool thread with a private copy of `RealmTime`; exceptions are caught per world.
  6. `Stats.Record(...)` (`:79`), then autosave every 60 s (`AutosaveIntervalMs = 60_000`, `:21`, `:81-84`, `:92-107`).
- After the loop: removes the sync context (`:89`).

`World.Tick` order (`World.cs:301-323`): close-when-empty check -> timers -> `ZoneSpawner.Tick` -> `Projectiles` -> `Map` (chunk rebuild,
query cache invalidate) -> `PortalDatas` (1 Hz) -> `EntityInventories` (queued swaps) -> `EntityCombat` (apply damage, deaths) ->
`EntityProjectiles` -> `EntityBehaviors` (AI) -> `Quests.Tick` (1 Hz) -> `PlayerSights` (builds and SENDS Update/NewTick) ->
`PowerUps.Tick` -> `EntityStats` (snapshots dirty stats for the NEXT tick's NewTick, condition effects, regen) -> clear chat text cache.

Threads in the process:
| Thread | What runs there | Source |
|---|---|---|
| Main ("game thread") | `Update()` (packets, removals, queued actions), the loop, `ShutdownAsync` | `GameLogic.cs:33-90` |
| Thread pool (Parallel.ForEach workers) | `World.Tick` of every world, concurrently; the main thread participates | `GameLogic.cs:251` |
| IOCP/socket callback threads | accept (`SocketServer.ProcessAccept`), receive + packet parsing (`NetworkHandler.HandleReceive`), send completion | `SocketServer.cs:49-88`, `NetworkHandler.cs:111-163`, `:81-99` |
| Thread pool (async) | RPC calls to AccountServer, their continuations unless captured by the game sync context, `CharacterSaver` worker, AntiCheat flag task, Campsite saves | `CharacterSaver.cs:166-196`, `AntiCheat.cs:35-43` |
| StreamJsonRpc threads | incoming RPC from AccountServer (`GameServerRpcHandler`) | `WaW-Server/GameServer/Messaging/GameServerRpcHandler.cs` |
| "Logger" background thread | console + file writes | `WaW-Server/Common/Utilities/Logger.cs:59-60,169-190` |

The action queue (`GameLogic.Enqueue`, `:210-212`) is the official hand-over from other threads. `GameThreadSynchronizationContext.Post`
(`WaW-Server/GameServer/Game/GameThreadSynchronizationContext.cs:14-16`) routes every `await` continuation of a packet handler back
through it.

### Current implementation
As described. Important consequences:
- Packet handlers (movement, shooting, chat, portals...) run on the main thread **between ticks**, up to ~50 times per tick, and mutate
  world state immediately (e.g. `Move` writes the player position at `WaW-Server/GameServer/Game/Network/Messaging/Move.cs:57-58`).
- World ticks run concurrently with each other. Anything a tick touches outside its own world (statics, `RealmManager`, other worlds,
  other users' send buffers, shared descriptors) is a cross-thread access. See `ServerArchitecture.md` "Cross-thread boundaries".
- Some handlers re-enqueue their real work (`EnemyHit.cs:28`, `PlayerShoot.cs:60,72`) so it runs in the next drain, still between ticks.

### New architecture (RECOMMENDATION)
- Fixed-step simulation at 20 Hz (configurable), constant `dt = 50 ms` passed to systems (reference uses measured delta; keep a
  catch-up cap of e.g. 3 steps).
- Networking thread(s) only parse frames into `InboundCommand` values and push them into a lock-free per-connection or per-world
  SPSC queue. The simulation thread drains commands **at the start of a tick** (not between ticks), so all world mutation happens inside
  the tick.
- Worlds are independent shards: each `World` is ticked by a worker in a fixed job pool; a world never touches another world. Cross-world
  operations (portal transfer, global chat, admin commands) are messages to the `WorldDirector`, applied at the next tick boundary.
- Outbound: each world writes snapshots into per-connection outbound queues owned by the connection; the network thread serializes and
  sends.

### Implementation status
NOT STARTED

### Differences
Commands applied at tick start instead of "whenever the loop spins"; fixed dt; no shared mutable state between world workers.

### Reason for differences
Determinism, testability, and removing the data races listed in `ServerArchitecture.md`.

### Tests
`GameThreadSyncContextTests` (`WaW-Server/Tests/GameServer.Tests/GameThreadSyncContextTests.cs`, 5 tests: Post queues, Send inline,
awaited handler resumes through the queue, shutdown completes with context installed, autosave interval ~1 min).
`WorldTimerAndSpawnTests` (3: throwing timed action is dropped, behaviour root state on spawn, spawn count).

### Known issues
- Variable timestep (`GameLogic.cs:60`) - behaviours, regen, projectiles depend on real elapsed ms.
- Parallel world ticks share static state without locks (see ServerArchitecture.md).
- A thrown exception inside `World.Tick` aborts the rest of that world's tick (`GameLogic.cs:253-258`): e.g. `EntityStats.Tick` is skipped, so dirty flags persist into the next tick.
- `EntityEventsManager` is never ticked (absent from `World.cs:301-323`), so `OnDamageReceived` never fires (`WaW-Server/GameServer/Game/Systems/Events/EntityEvents.cs` Tick; subscriber `Behaviors/Transitions/DamageTakenTransition.cs:24`).

---

## 4. Networking and connection lifecycle

### Reference behavior
- **Listen**: IPv4 any address, port from config (2050), backlog 3000 (`WaW-Server/GameServer/Game/Network/SocketServer.cs:29-31`).
  Accept loop with `SocketAsyncEventArgs` (`:36-88`).
- **Per-address cap**: `ConnectionLedger(MaxClientsPerIP)` (`SocketServer.cs:26`; `ConnectionLedger.cs:40-53`): refuse when an address
  already has `MaxPerAddress` (10) sockets open; **127.0.0.1 / ::1 / ::ffff:127.0.0.1 are never capped** (`ConnectionLedger.cs:29,45`)
  because browser players arrive through a local websocket bridge. Refused sockets are closed; first refusal and every 100th is logged
  as `[FLOOD]` (`SocketServer.cs:60-68`; `ConnectionLedger.cs:74-81`).
- **User pool**: `ConcurrentFactory<User>(MaxPlayers)` pre-allocates 1000 `User` objects (`SocketServer.cs:27`;
  `WaW-Server/Common/Utilities/ConcurrentFactory.cs:15-20`). Each `User` owns a `NetworkHandler` with a 128 KiB (0x20000) rented receive
  buffer (`NetworkHandler.cs:40-41`). `User.Id` is assigned once in the constructor from a static counter (`User.cs:40,52`) - it is a
  **pool slot id** reused across connections, not an account id. Pool empty -> socket refused "server is full" (`SocketServer.cs:70-79`).
- **Connect**: `user.Setup(ip, socket)` (TCP NoDelay, `NetworkHandler.cs:59-63`), `RealmManager.UserConnected` (adds to `RealmManager.Users`,
  sends every `ServerProjectileProps`, starts receiving) (`RealmManager.cs:51-56`, `:93-104`). Runs on the accept callback thread.
- **Framing** (`WaW-Server/Common/Network/SocketReceiveState.cs:46-79`): `[int32 length incl. 5-byte header][byte packetId][payload]`.
  Length < 5 or > buffer size -> `InvalidDataException` -> disconnect (`NetworkHandler.cs:137-143`). No encryption, no compression on the
  game socket. Byte order: `SpanReader` supports both; which one is used here is UNVERIFIED (default of `SpanReader` constructor not checked).
- **Receive** (socket thread): parse every complete frame, construct the packet through a static id->factory table built by reflection
  over `IIncomingPacket` types with `[Packet(id)]` (`NetworkHandler.cs:24-28`; `Messaging/Packet.cs:20-34`), call `Read`, and enqueue into the
  user's `ConcurrentQueue<IIncomingPacket>` (`NetworkHandler.cs:148-159`). Unknown ids are silently ignored (`:151`).
- **Handle** (main thread, `Update`): `HandleIncomingPackets` (`NetworkHandler.cs:173-223`) - one packet at a time; if a handler returns an
  incomplete `Task` (an RPC await) no further packet of that user is processed until it completes (`:174-184`, `:212-215`), preserving
  order. Budget: warn at 200 packets in one drain, disconnect above 2000 (`:170-171`, `:193-200`). Any exception or faulted task ->
  disconnect "Internal error handling packet" (`:202-221`).
- **Send**: `SendPacket` serializes immediately into the user's write buffer (`User.cs:107-109`; `NetworkHandler.cs:65-68`); `SendSocketData`
  swaps buffers and issues one `SendAsync` per loop iteration (`NetworkHandler.cs:70-79`). `SocketSendState` has no lock
  (`WaW-Server/Common/Network/SocketSendState.cs:45-73`).
- **States**: `ConnectionState { Disconnected, Connected, Reconnecting, Ready }` (`User.cs:22-27`) and `GameState { Idle, Loading, Playing }`
  (`GameInfo.cs:13-17`).
- **Disconnect**: `User.Disconnect` sets state and enqueues `FinishDisconnect` (`User.cs:133-150`): `Unload(false)` (save + LeaveWorld),
  remove from `RealmManager.Users`, socket shutdown/close, ledger decrement, `user.Reset()`, push back to pool (`SocketServer.cs:94-108`).
  `SendFailure` flushes the Failure packet before disconnecting (`User.cs:111-120`).

Connection lifecycle (login): TCP accept -> client `Hello` -> `MapInfo` (+ `WeatherState`) -> client `Load` (or `Create`) -> server
`CreateSuccess`, `AccountList` x2, optional `StarterOffer`/`StorageInfo` -> per-tick `Update`/`NewTick`.
World switch: server `Reconnect(worldId)` on the same socket -> client sends a new `Hello` with that GameId on the same connection
(client `WaW-Client/WaWClient/Networking/Packets/Incoming/Reconnect.cs:33-45`) -> `MapInfo` -> `Load`.

Incoming packets handled by the server (`[Packet(...)]`): Hello, Load, Create, Escape, Move, GotoAck, PlayerShoot, EnemyHit, PlayerHit,
PlayerText, UsePortal, InvSwap, InvDrop, UseItem, UsePowerUp, CatAction, ChangeSkin, BuyStorage, ClaimStarter, ClaimBounty.
Outgoing: Failure, CreateSuccess, AccountList, MapInfo, Reconnect, Update, NewTick, Goto, Text, Notification, ShowEffect, Death,
EnemyShoot, ServerPlayerShoot, ServerProjectileProps, InvResult, StarterOffer, StarterResult, StorageInfo, WeatherState, QuestObjId.
Packet ids: `Shared/Common.Protocol/PacketId.cs:6-88` (shared with clients).

### Current implementation
As described. The client also sends `UpdateAck` with `PacketId.Unknown`, which the client itself drops (`WaW-Client/WaWClient/Networking/Packets/Outgoing/UpdateAck.cs:4`).

### New architecture (RECOMMENDATION)
`net` module: `Listener` (accept + per-address limiter + global connection cap) -> `Connection` (RAII socket, bounded inbound frame
queue, outbound byte queue, explicit state machine) -> `Codec` (frame + packet structs generated from one shared schema used by the
Unity client). Session/auth state lives in a `Session` object owned by `SessionManager`, separate from `Connection` and from the
in-world `PlayerEntity`. Pooling is an allocator detail, never visible as identity: connection ids are monotonically increasing 64-bit
values, never reused. Keep the 2000-packets-per-tick flood guard as a per-connection token bucket. Put TLS (or at least an auth token)
on the game connection: the reference sends username+password in every `Hello` in clear text (`Hello.cs:36-43`, client `Reconnect.cs:41-42`).

### Implementation status
NOT STARTED

### Differences
Ids never reused; no password per world switch; send path owned by the network thread.

### Reason for differences
Reused `User` objects require "check `State == Disconnected` after every await" discipline (WaW-Server/AGENTS.md:33-34) and are a
source of stale-reference bugs; plaintext credentials on every reconnect.

### Tests
`ConnectionLedgerTests` (6: per-address cap and refusal counts, disconnect frees one slot, loopback never capped, full server returns slot,
warn first then every 100th, bad input). `BulletIdSyncTests` exercise PlayerShoot/EnemyHit handlers. No socket-level tests.

### Known issues
- Cross-thread writes to a user's unsynchronized send buffer are possible: packet handlers (main thread), world ticks (pool threads),
  and code reached from RPC threads all call `SendPacket`. Within one tick a user is normally written only by its own world, but e.g.
  `PlayerDeath`, `ChatManager.Announce`/`RealmManager.BroadcastAll` (`RealmManager.cs:88-91`) have no thread guard.
- `RealmManager.UserConnected` sends packets from the accept thread (`RealmManager.cs:53`) while the main thread may already flush.
- Credentials re-sent in clear on every world change.

---

## 5. Sessions (Hello / Load / Create / Escape / Reconnect)

### Reference behavior
- `Hello` (`WaW-Server/GameServer/Game/Session/Hello.cs:45-153`): exact version string match with config `Version` (`:46-49`). If not
  reconnecting: `VerifyAccount(username, password, guid)` RPC (await) (`:52-67`), `AccountInUse` refused. Ban check via `GetActiveBans`;
  expired bans are cleared with `FlushAccount(acc)` (`:74-88`). `AdminOnly` gate (`:90-93`). World resolution through
  `EntryWorlds.Resolve(GameId, acc)` (`:98`; `EntryWorlds.cs:13-34`): `-5` Campsite (per account), `-6` GuildHall, `-7` Tutorial, any other id
  must exist in `RealmManager.Worlds`, not be Deleted, and pass `AllowsEntry(acc)`. `-2` (test world) is refused "Not available" (`:104-129`).
  Then `SetGameInfo` (seed, world), `GetMuteState` RPC, send `MapInfo` and `WeatherState` (`:135-152`).
- `Load` (`Load.cs:24-64`): bans, `GetCharacter` RPC unless reconnecting (then the in-memory `GameInfo.Char` is reused), dead character
  refused, deleted world refused, `CeremonyGate.RefuseRoleless` / `Reroute` (`CeremonyGate.cs:19-41`), then `User.Load` (enqueued) creates the
  player entity (`User.cs:78-98`; `GameInfo.cs:63-80`).
- `Create` (`Create.cs:25-50`): `CreateCharacter` RPC with the session's in-memory `Account`, adopts the returned account, may reroute a
  new account's first character to its Tutorial.
- `Escape` (`Escape.cs:13-26`): to the Nexus via `ReconnectTo(Worlds[-1])`.
- `ReconnectTo(world)` (`User.cs:122-131`): state Reconnecting, `Unload(true)` (save + LeaveWorld), send `Reconnect(world.Id)`.

### Current implementation
As described; handlers `await` RPC calls and resume on the game thread.

### New architecture (RECOMMENDATION)
`SessionManager` state machine per connection: `Connected -> Authenticated(token) -> SelectingCharacter -> InWorld(worldId) ->
Transferring(targetWorldId) -> InWorld`. A transfer target chosen by the server is stored in the session and is the only world the
next "join" may enter (the reference trusts the GameId the client sends back). Authentication with a short-lived token issued by the
account service, not the password.

### Implementation status
NOT STARTED

### Differences
Server-remembered transfer target; token auth.

### Reason for differences
`Hello` while `Reconnecting` accepts any `GameId` that passes `AllowsEntry` (`Hello.cs:52`, `:98`), so a modified client can jump to any
open world it may enter (e.g. any Realm or Dungeon instance with room) without using a portal.

### Tests
`RoleSelectionTests` (7: roles, tutorial per account, walkable path from spawn), `GuildHallWorldTests` (7, includes "personal worlds only
let their owners in"), `StarterRewardTests` (4). No direct Hello/Load handler test.

### Known issues
- See "Differences". Also `Hello.cs:87` still calls `FlushAccount` with the game server's in-memory account (stale-overwrite risk that
  `WaW-Server/AGENTS.md:43-44` warns about, but for `Create`).
- `Create.cs:26` still sends the whole in-memory `Account` to `CreateCharacter` (EngineeringAudit F56; see Discrepancies).

---

## 6. Subsystem table

| Subsystem | Files (REF/WaW-Server/GameServer/...) | Authoritative? | Ticked / run where |
|---|---|---|---|
| Game loop, action queue, STATS | `Game/GameLogic.cs` | n/a | main thread |
| Socket accept, per-address cap, user pool | `Game/Network/SocketServer.cs`, `ConnectionLedger.cs`, `User.cs` | n/a | accept callback thread |
| Packet receive / dispatch / send | `Game/Network/NetworkHandler.cs`, `Messaging/*` | n/a | receive: socket thread; handle+send: main `Update()` |
| Sessions (Hello/Load/Create/Escape) | `Game/Session/*` | yes (entry rules) | main `Update()` + RPC awaits |
| World registry | `Game/RealmManager.cs` | yes | any thread (immutable dictionaries, interlocked) |
| World instance + timers + removals | `Game/Worlds/World.cs` | yes | `Tick`: Parallel.ForEach; `Update`: main |
| World types (Nexus, Realm, Campsite, GuildHall, Tutorial, Dungeon, TestingWorld) | `Game/Worlds/Logic/*` | yes | inside `World.Tick` |
| Map, chunks, spatial cache | `Game/Worlds/WorldMap.cs`, `ChunkMap.cs`, `SpatialQueryCache.cs` | yes | `World.Tick` -> `Map.Tick` (rebuild every tick) |
| Zone spawning | `Game/Worlds/ZoneSpawner.cs` | yes | `World.Tick`, 1 Hz |
| Map spawn + respawn of map enemies | `World.SpawnFromMap`, `EntityCombat.Death` | yes | world load; timed action |
| Entities (ids, components) | `Game/Entities/*` | yes | n/a (`EntityManager.Tick` empty) |
| Stats + dirty tracking, regen, conditions | `Game/Systems/Stats/*` | yes | `World.Tick` (last) |
| Movement | `Game/Network/Messaging/Move.cs`, `Systems/Combat/PlausibilityRules.cs` | **partial** (client position, server bounds check) | main `Update()` |
| Anti-cheat score | `Systems/Combat/AntiCheat.cs`, `CheatScore.cs` | yes (scoring/kick) | main `Update()` |
| Projectiles | `Systems/Projectiles/*`, `Combat/CombatExtensions.cs` | yes (spawn, path, damage); player->enemy hits client-reported + validated | `World.Tick` |
| Combat / damage / death | `Systems/Combat/EntityCombat*.cs`, `PlayerDeath.cs`, `Progression*.cs` | yes | `World.Tick` |
| Hit validation | `Systems/Combat/HitValidation.cs`, `EnemyHit.cs`, `PlayerHit.cs` | partial (validates client reports) | main (re-enqueued) |
| Behaviours / AI | `Systems/Behaviors/*` | yes | `World.Tick` |
| Events (OnDeath, OnDamageReceived) | `Systems/Events/*` | yes | OnDeath on component removal; **EntityEventsManager never ticked** |
| Inventory / items / loot | `Systems/Inventory/*`, `Behaviors/Loot/*` | yes (swaps validated: ownership, 3-tile distance, slot rules) | swaps queued, applied in `World.Tick` |
| Portals | `Systems/Portals/*` | yes, but `UsePortal` has no distance check | `World.Tick` 1 Hz; use: main |
| Region triggers | `Game/Network/Messaging/RegionTriggers.cs` | yes (server map) | after each accepted Move |
| Sight / visibility / Update+NewTick | `Systems/Sight/*` | yes | `World.Tick` |
| Quests (quest arrow) | `Systems/Quests/Quests.cs` | yes | `World.Tick`, 1 Hz |
| Chat | `Systems/Chat/*` | yes (routing, mute, rate limit) | main `Update()` |
| Commands | `Systems/Chat/Commands/*` | yes (rank-gated) | main, fire-and-forget async |
| Weather / time override | `Systems/Weather/WeatherControl.cs` | override only; day/night is client clock | command / RPC thread |
| Persistence (character saves) | `Systems/Persistence/CharacterSaver.cs` | snapshot from server state | worker task; autosave 60 s |
| Campsite storage + Gift Chest | `Worlds/Logic/Campsite.cs`, `CampsiteRules.cs`, `GiftChestStore.cs`, `Session/StorageShop.cs` | yes | timed actions in the Campsite world |
| Power-ups, bounties, skins, cats, starter rewards | `Systems/Inventory/PowerUps.cs`, `Systems/Rewards/BountyBoard.cs`, `Session/SkinChange.cs`, `Inventory/CatCare.cs`, `Session/ClaimStarter.cs` | yes (server counts/pays; not audited in depth) | handlers + `PowerUps.Tick` |
| Trading | none (packet ids 55-63 exist in `Shared/Common.Protocol/PacketId.cs:62-70`, no handler) | **NOT IMPLEMENTED** | - |
| RPC to AccountServer | `Program.cs`, `Messaging/GameServerRpcHandler.cs`, `Common/Messaging/*` | AccountServer owns accounts | thread pool |
| Logging | `Common/Utilities/Logger.cs` | n/a | Logger thread |

---

## 7. Authoritative vs client-trusted state (explicit answer)

**Server-authoritative** (the client cannot change it except through validated requests):
- Which worlds exist, which world a player is in, world entry permission (`World.AllowsEntry`, `EntryWorlds.cs:13-34`).
- Entity creation, ids, removal; all enemy/NPC positions and AI (`EntityBehaviorManager`).
- All stats (HP, MP, level, XP, fame, attack, defense...) - set only by server code (`EntityStats.Set`), regen server-side (`EntityStats.cs:210-225`).
- Damage amounts: player bullet damage is rolled on the server from the server-side inventory weapon and attack stat
  (`PlayerShoot.cs:40-43`, `:69`); defense applied server-side (`EntityCombat.DamageWithText`); death and loot server-side.
- Enemy bullet collision with players: the server tests positions itself (`Projectile.cs:60-86`, targets from `CombatExtensions.cs:14-16`).
- Inventory contents and swaps (validated, `EntityInventoryManager.cs:94-260`), item use, portals' targets, quests, zone spawns.
- Region triggers use the server's own map (`RegionTriggers.cs:18-44`).
- Persistence: what is saved is the server snapshot (`CharacterSaver.cs:142-156`).

**Client-trusted (wholly or partially)**:
- **Player position**: the client sends absolute positions (`Move.cs:17,67-69`). The server only checks (a) distance against a generous
  max speed `elapsed * 0.0096 * 1.5 * 1.5 + 1 tile`, capped at 2 s (`PlausibilityRules.cs:12-21`), and (b) that the **destination tile**
  is enterable (`PlausibilityRules.cs:29-41`). It does not check the path, so a move that crosses a thin wall in one step within the speed
  allowance is accepted. On violation: snap back with `Goto` and anti-cheat points (`Move.cs:39-54`).
- **Aim angle and fire timing** (`PlayerShoot.Angle`): angle trusted; rate limited by a leaky bucket (`FireRateBucket`, `PlausibilityRules.cs:47-84`).
- **Player-bullet hits on enemies**: reported by the client (`EnemyHit`), accepted if the server's copy of the bullet was within
  0.5 + 1.5 tiles of the target's server position at any 50 ms sample in the last 400 ms (`HitValidation.cs:11-34`). Player bullets
  have no server-side collision (their `TargetIds` are only filled for enemy shots, `CombatExtensions.cs:14-16`).
- **Being hit** (`PlayerHit`): client self-report is accepted after plausibility; the server also detects enemy bullet hits itself, so
  a client that never reports still takes damage.
- **Portal use**: any portal object id in the current world, no distance check (`UsePortal.cs:23-35`).
- **World choice during a transfer**: `Hello.GameId` while `Reconnecting` (see section 5).
- **Bullet numbering**: client-chosen sequence mirrored by the server (`PlayerShoot.cs:29-34`, `EntityProjectiles.cs:42-52`).
- **Day/night and weather**: computed by each client from UTC; the server only sends an override (`WeatherControl.cs:173-178` comment, `:211`).
- Projectile wall collision: none on the server (`Projectile.Tick` has no tile check, `Projectile.cs:60-86`) - walls only stop bullets
  client-side (client behaviour UNVERIFIED in this audit).

---

## 8. Chat and commands

### Reference behavior
- `PlayerText` packet -> `Speak` (`Systems/Chat/PlayerText.cs:15-19`; `Entities/Extensions/PlayerExtensions.cs:120-149`):
  `PlayerChat.ValidateSpeak` - non-blank, max 256 chars, 500 ms cooldown, admins exempt from the cooldown (`Systems/Chat/PlayerChat.cs:13-49`).
  Text starting with `/` -> `CommandManager`. Muted players refused (`GameInfo.IsMuted`, `GameInfo.cs:47`). Otherwise the text is sent to
  **every user in the same world** (not distance-limited) as a `Text` packet (`PlayerExtensions.cs:138-148`).
- `ChatManager.Announce` broadcasts to all playing users on this server (`ChatManager.cs:12-24`); cross-server announce is a TODO (`:15`).
  RPC `GlobalAnnouncement` only logs (`GameServerRpcHandler.cs:15-18`).
- Commands: reflection-registered (`CommandManager.cs:15-35`), case-insensitive name lookup, permission = `Ranks.Of(account) >= level`
  (`:45-53`; `WaW-Server/Common/Database/Ranks.cs:13-31`). Executed fire-and-forget (`_ = cmd.ExecuteAsync`, `:50`).

| Command | Required rank (`CommandPermissionLevel`) | File |
|---|---|---|
| /commands | Player (0) | `Systems/Chat/Commands/PlayerCommands.cs:9` |
| /online | Player (0) | `PlayerCommands.cs:17` |
| /rank | Player (0) | `PlayerCommands.cs:28` |
| /find | Moderator (80) | `ModCommands.cs:28` |
| /goto | Moderator (80) | `ModCommands.cs:53` |
| /kick | Moderator (80) | `ModCommands.cs:95` |
| /ban | Moderator (80) | `ModCommands.cs:120` |
| /unban | Moderator (80) | `ModCommands.cs:144` |
| /mute | Moderator (80) | `ModCommands.cs:160` |
| /unmute | Moderator (80) | `ModCommands.cs:184` |
| /reloadbehaviors | Developer (90) | `DevCommands.cs:8` |
| /weather | Developer (90) | `DevCommands.cs:18` |
| /time | Developer (90) | `DevCommands.cs:31` |
| /give | Owner (100) | `OwnerCommands.cs:14` |
| /setrank | Owner (100) | `OwnerCommands.cs:55` |
| /mail | Owner (100) | `OwnerCommands.cs:80` |
| /god | Owner (100) | `CreativeCommands.cs:11` |
| /spawn | Owner (100) | `CreativeCommands.cs:25` |
| /bag | Owner (100) | `BagCommand.cs:11` |

No command declares aliases. Enum also has Donor1-4 (10-40), Creative (50), Admin (100) levels that no command uses (`Command.cs:7-18`).

### Current implementation
As above.

### New architecture (RECOMMENDATION)
`chat` module: `ChatRouter` (channels: world, nearby, whisper, guild, announce) + `CommandRegistry` built from a static table, each
command a pure function `(CommandContext&, args) -> CommandResult` that only emits simulation commands; async work (DB moderation)
goes through the account-service client with a callback that re-enters via the command queue. Rate limiting per session, not per
entity component.

### Implementation status
NOT STARTED

### Differences
No fire-and-forget tasks; explicit channels.

### Reason for differences
Exceptions in `ExecuteAsync` are unobserved (no `TaskScheduler.UnobservedTaskException` handler anywhere in `WaW-Server`).

### Tests
`CommandsTests` (22: rank gating, every command declares a rank, CommandArgs parsing for ban/mute/give/mail/setrank, item search),
`SpawnRulesTests` (4), `RanksTests` (Common.Tests, 6), `ModerationRulesTests` (Common.Tests, 7).

### Known issues
- `/weather` usage text omits `cloudy`, which is accepted (`DevCommands.cs:22` vs `WeatherControl.cs:182`).
- Chat rate limit state lives in the per-world `PlayerChat` component and resets on every world change.

---

## 9. Logging, STATS line, error handling, shutdown, crash behaviour

### Reference behavior
- **Logger** (`WaW-Server/Common/Utilities/Logger.cs`): levels Info/Debug/Warn/Error/Fatal (`:14-20`); formatting on caller thread, writes on a
  background thread through a bounded channel of 20 000 entries with DropOldest (`:29`, `:39-42`); files under
  `<cwd>/logs/<ProcessName>/<level>/log.txt` (`:32-33`, `:206`); Debug only if `WAW_DEBUG_LOG` set (`:35`, `:75-78`); Warn+ also kept in a
  40-entry ring for the Developer Dashboard (`:151-167`); `Fatal` flushes (`:88-91`).
- **STATS** (`GameLogic.cs:172-205`): every 10 s one Info line
  `[STATS] tps= tick work avg/max | tick interval avg/max late= | users= worlds= queued= | sockets= accepted= refused=`;
  snapshot `Stats.Last` exposed to RPC `GetStatus` (`GameServerRpcHandler.cs:231-244`).
- **Other tagged logs**: `[FLOOD]` (SocketServer), `[PLAUSIBILITY]` (packet budget), `[ANTICHEAT]` (`AntiCheat.cs:25,32`), `[RPC]`.
- **Anti-cheat score** (`CheatScore.cs:12-50`): weights MovedTooFar `1 + clamp((overBy-1)*2, 0, 8)`, WalkedThroughWall 3, FiredTooFast 0.5;
  drain 0.2 points/s; >= 20 Suspicious (logged once per session); >= 60 Kick: disconnect + `FlagSuspect` RPC (no automatic ban)
  (`AntiCheat.cs:16-46`). Score lives in `GameInfo.Cheat` and survives world switches, reset on disconnect (`GameInfo.cs:44,110`).
- **Error handling**: try/catch per queued action (`GameLogic.cs:160-165`), per world Update/Tick (`:224-229`, `:253-258`), per user I/O
  (`:236-242`); a faulted packet handler disconnects that user (`NetworkHandler.cs:178-183`, `:206-220`); a throwing timed action is
  dropped (`World.cs:230-244`); malformed frames disconnect; unknown packet ids ignored.
- **Shutdown** (`Program.cs:68-79`, `GameLogic.cs:27-31`, `:111-153`): Ctrl+C, SIGTERM (`PosixSignalRegistration`), ProcessExit (waits up to
  6 s) -> `RequestStop` -> loop exits -> `ShutdownAsync`: save all Campsite Gift Chests (3 s timeout), `SaveUser` + "server is restarting"
  Failure for every user, drain actions, flush sockets, wait up to 8 s for `CharacterSaver.Pending == 0`, log result, set
  `ShutdownCompleted`.
- **Crash behaviour**: unhandled exception -> `Fatal` log only (`Program.cs:112-114`), then the runtime terminates the process; no
  in-process restart. On the VPS the systemd unit uses `Restart=on-failure` (`VPS_SETUP.md:86-88`). Characters not yet autosaved
  (up to 60 s of progress) are lost; queued `CharacterSaver` items are lost.

### Current implementation
As above.

### New architecture (RECOMMENDATION)
Structured logging (spdlog or equivalent) with an async sink and bounded queue, per-module loggers passed in, no static logger
construction in headers. Metrics struct (tps, tick time p50/p99, queue depths, connections) sampled per tick and logged every 10 s and
exposed on an admin endpoint. Crash: install a terminate handler that writes a minidump and the last N log lines; persistence through a
write-ahead save queue so a crash loses at most the in-flight batch. Shutdown = `request_stop()` -> stop accepting -> finish current
tick -> `save_all()` -> drain outbound -> destroy in reverse order.

### Implementation status
NOT STARTED

### Differences
Bounded crash loss; explicit stop sequence without static flags.

### Reason for differences
Reference relies on statics (`_stopRequested`, `ShutdownCompleted`) and loses unsaved progress on crash.

### Tests
`GameThreadSyncContextTests` (shutdown completes), `CheatScoreTests` (5), `PlausibilityRulesTests` (7), `HitValidationTests` (11),
`CharacterSnapshotTests` (2), `PlayerDeathTests` (2).

### Known issues
- Unobserved task exceptions vanish (fire-and-forget commands, RPC helpers).
- Log directory is relative to the current working directory, not the binary (`Logger.cs:32`).

---

## 10. Discrepancies (docs vs code)

| # | Doc says | Code does |
|---|---|---|
| 1 | `WaW-Server/AGENTS.md:27` "One thread ... sleep 1 ms at a time until the next tick (never spin)" | Spins the last <= 2 ms (`GameLogic.cs:55-58`) and ticks worlds on thread-pool threads via `Parallel.ForEach` (`GameLogic.cs:251`). |
| 2 | `WaW-Server/AGENTS.md:29` "Everything that touches a world runs on this thread" | `World.Tick` runs on Parallel.ForEach workers (`GameLogic.cs:251-259`). |
| 3 | `WaW-Server/AGENTS.md:57-58` plausibility "log-only for now" | Enforced: snap-back + points (`Move.cs:39-54`), shots dropped (`PlayerShoot.cs:51-56`), kick at 60 points (`AntiCheat.cs:28-45`). CLAUDE.md:601-604 agrees with the code. |
| 4 | `Docs/EngineeringAudit.md:89-90,186` "no sleep: the loop busy-spins a core" | Fixed later: sleeps 1 ms (`GameLogic.cs:51-53`). The audit's own change log (`EngineeringAudit.md:643`) records the fix. |
| 5 | `Docs/EngineeringAudit.md:753` server plausibility "all LOG-ONLY" | Enforced (see #3). |
| 6 | `Docs/EngineeringAudit.md:955` `MaxClientsPerIP` (2000) | `gameServerConfig.xml:14` and `serverSettings.xml` = 10. |
| 7 | `WaW-Server/AGENTS.md:44` "`Create.Handle` still does [FlushAccount]" | `Create.cs` no longer calls `FlushAccount`; it still passes the full in-memory `Account` to `CreateCharacter` (`Create.cs:26`). `Hello.cs:87` does call `FlushAccount`. `EngineeringAudit.md:978` lists F56 as removed while `:960` and `:1038` list it as open. |
| 8 | `gameServerConfig.xml:12` BehaviorsDir `.../GameServer/Game/Entities/Behaviors/Library` | Behaviours are in `GameServer/Game/Systems/Behaviors/Library/`; the Entities folder has no Behaviors directory. |
| 9 | Task brief: XML descriptors in `WaW-Server/Common/Resources/Xml` | `Resources/Xml/Data/` is empty in source; the XMLs are linked at build time from `WaW-Client/WaWClient/Content/Xmls/*.xml` (`WaW-Server/Common/Common.csproj` `<Content Include="..\..\WaW-Client\WaWClient\Content\Xmls\...">`). |
| 10 | CLAUDE.md:807 "the server's algorithm, `Shadowcast`" | No type or method named Shadowcast exists in `WaW-Server`; the algorithm is inline in `PlayerSightManager.Scan` (`PlayerSightManager.cs:113-173`). |
| 11 | `realmConfig.xml` `<Close>` realm-closing rules | No code reads `RealmConfig.Close` or `Events`; realms never close. |
