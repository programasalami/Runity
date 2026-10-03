# Server architecture - as built, and recommended C++ layout

Audit date: 2026-10-02. Paths relative to REF (`Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing`).
Implementation status of every recommendation: NOT STARTED.

---

## 1. Projects, namespaces and ownership

### Reference behavior
Solution `WaW-Server/WarriorsAndWizards.Server.sln`:

| Project | Kind | Depends on | Role |
|---|---|---|---|
| `Shared/Common.Protocol` | library | - | `PacketId` (`PacketId.cs:6-88`), `WorldIds` (`WorldIds.cs:6-12`), `WorldPosData`, `LevelRules`, `InventoryLayout`, `Roles`, `Bounties`, `NexusCats`, `PowerUpList`, `ItemCategories`. Shared with the desktop client. |
| `WaW-Server/Common` | library | Common.Protocol; Dapper, Npgsql, StackExchange.Redis, StreamJsonRpc, Newtonsoft.Json, Ionic.Zlib (`Common.csproj`) | Database access (used only by AccountServer), RPC contracts (`Messaging/Proxies.cs`), resources (XML descriptors, world configs/maps, configs), network buffers (`Network/*`), structs, utilities (Logger, SparseSet, EntityId). |
| `WaW-Server/AccountServer` | exe | Common | HTTP :8080, RPC hub :8081, Postgres + Redis. Out of scope here. |
| `WaW-Server/GameServer` | exe | Common (`GameServer.csproj`) | Simulation + game TCP :2050. |
| `WaW-Server/Tests/Common.Tests`, `Tests/GameServer.Tests` | xunit | | see section 8 |
| `Shared/Tests/Common.Protocol.Tests` | xunit | | PacketId, WorldPosData, LevelRules, Roles, Bounties, ItemCategories |

GameServer namespaces / folders (`WaW-Server/GameServer/`):
- `GameServer` - `Program.cs` (entry, RPC connection, shutdown hooks).
- `GameServer.Game` - `GameLogic` (loop), `RealmManager` (world + user registry), `RealmTime`, `GameThreadSynchronizationContext`.
- `GameServer.Game.Network` - `SocketServer`, `ConnectionLedger`, `User`, `NetworkHandler`, `GameInfo`, `ClientRandom`; `.Messaging` - packet
  plumbing and a few packets (Move, Goto, Update, NewTick, RegionTriggers...).
- `GameServer.Game.Session` - login/world-entry packets.
- `GameServer.Game.Entities` - `Entity`, `EntityType`, `EntityManager`, `ManagerBase<T>`, `EntityView`, extensions.
- `GameServer.Game.Worlds` - `World`, `WorldMap`, `ChunkMap`, `SpatialQueryCache`, `ZoneSpawner`; `.Logic` - world subclasses.
- `GameServer.Game.Systems.*` - Behaviors, Chat (+Commands), Combat, Events, Inventory, Movement (empty folder), Persistence, Portals,
  Projectiles, Quests, Rewards, Sight, Stats, Weather.
- `GameServer.Messaging` - `GameServerRpcHandler` (RPC server side of AccountServer -> GameServer calls).
- `GameServer.Utilities` - `PositionUtils`, `Easing`.

Packets are not a separate layer: each incoming packet class contains its own gameplay handler (`IIncomingPacket.Handle(User)`,
`Game/Network/Messaging/IIncomingPacket.cs:9-12`), and the handler files live inside the gameplay systems (e.g. `Systems/Combat/EnemyHit.cs`).

### Current implementation
As above. Ownership is by convention, not by type: most coordinators are static classes.

### New architecture (RECOMMENDATION)
See section 7.

### Implementation status
NOT STARTED

### Differences / Reason for differences
See section 7.

### Tests
See section 8.

### Known issues
Packet decoding, validation and gameplay are fused in one class per packet; hard to test or reuse the protocol from Unity.

---

## 2. How worlds own managers (component storage)

### Reference behavior
`World` (`WaW-Server/GameServer/Game/Worlds/World.cs:26-104`) owns, by readonly fields constructed in its constructor:

| Manager | Element (struct) | Initial capacity | File |
|---|---|---|---|
| `Entities : EntityManager` | `Entity` | 5000 | `Game/Entities/EntityManager.cs` |
| `Projectiles : ProjectileManager` | `Projectile` | 5000 | `Systems/Projectiles/ProjectileManager.cs` |
| `EntityBehaviors` | `EntityBehavior` | 5000 | `Systems/Behaviors/EntityBehaviorManager.cs` |
| `EntityStats` | `EntityStats` | 5000 | `Systems/Stats/EntityStatsManager.cs` |
| `EntityProjectiles` | `EntityProjectiles` | 1000 | `Systems/Projectiles/EntityProjectilesManager.cs` |
| `EntityCombat` | `EntityCombat` | 1000 | `Systems/Combat/EntityCombatManager.cs` |
| `EntityEvents` | `EntityEvents` | 1000 | `Systems/Events/EntityEventsManager.cs` |
| `EntityInventories` | `EntityInventory` | 1000 | `Systems/Inventory/EntityInventoryManager.cs` |
| `PortalDatas` | `PortalData` | 1000 | `Systems/Portals/PortalDatasManager.cs` |
| `PlayerSights` | `PlayerSight` | 100 | `Systems/Sight/PlayerSightManager.cs` |
| `PlayerChat` | `PlayerChat` | 100 | `Systems/Chat/PlayerChatManager.cs` |

Plus `Map : WorldMap`, `Zones : ZoneSpawner?`, `Users : ImmutableDictionary<EntityId, User>`, timers, removal queue, `QuestEntities`.
`ManagerBase<T>` (`Game/Entities/ManagerBase.cs:17-42`) wraps a `SparseSet<T>` (`Common/Utilities/Collections/SparseSet.cs`) keyed by
`EntityId.Index` with a generation check; `Get` returns a `ref` into the dense array, or a ref to the dummy slot 0 when missing (callers
check `Id == EntityId.Null`). Every component struct stores a back-pointer to its `World` (e.g. `EntityStats._world`, `EntityStats.cs:45`).
Details in `EntitySystem.md`.

### Current implementation
Archetype is decided in `World.AddComponents` by `EntityType` (`World.cs:146-205`).

### New architecture (RECOMMENDATION)
Keep data-oriented component pools (they port well to C++): `World` owns `ComponentPool<T>` members by value; components hold no
back-pointer to `World`; systems are free functions or classes `void tick(WorldContext&, Dt)` that receive the pools they need. Entity
handle = 32-bit index + generation in a `EntityRegistry` owned by the world.

### Implementation status
NOT STARTED

### Differences
No back-pointers, systems explicit.

### Reason for differences
Back-pointers make component structs non-relocatable and tie them to world lifetime.

### Tests
`TestWorldFactoryTests` (5), `EntitySlotTests` (1).

### Known issues
See EntitySystem.md (events manager never ticked, projectile index double-free).

---

## 3. How packets reach systems

### Reference behavior
1. Socket thread: frame -> `PacketId` -> factory -> `pkt.Read(ref SpanReader)` -> `User.Network._pendingReceive` (`NetworkHandler.cs:111-163`).
2. Main thread `GameLogic.Update` -> `HandleIncomingPackets` -> `pkt.Handle(User)` (`NetworkHandler.cs:173-223`).
3. The handler reaches gameplay through `user.GameInfo` (`Account`, `World`, `PlayerId`, `Char`; `Game/Network/GameInfo.cs:19-49`) and
   then `world.<Manager>.Get(playerId)` with refs. Examples:
   - `Move` mutates `EntityStats.Pos` directly (`Move.cs:57-58`), then `RegionTriggers.OnMoved`.
   - `InvSwap` enqueues into `EntityInventoryManager._swapCommands`, applied in the next `World.Tick` (`InvSwap.cs:26-28`; `EntityInventoryManager.cs:318-322`).
   - `PlayerShoot`/`EnemyHit` validate and then `GameLogic.Enqueue` the effect (runs in the next drain, still on main thread) (`PlayerShoot.cs:60-84`; `EnemyHit.cs:28-49`).
   - `PlayerText` -> `Speak` -> broadcast or `CommandManager` (`PlayerExtensions.cs:120-157`).
   - Session handlers `await` `Program.AccountServerRpc` calls (`Hello.cs:53,76,87,137`; `Load.cs:32`; `Create.cs:26`).

### Current implementation
Direct, synchronous mutation of world state from handlers running between ticks.

### New architecture (RECOMMENDATION)
`net` decodes to plain `InboundCommand` variants (no behaviour). `SessionManager` routes in-world commands to the world's
`CommandQueue`. Each system declares which commands it consumes; the world applies them at tick start in a fixed order (session ->
movement -> inventory -> combat input -> chat). Validation is a pure function per command (testable without sockets).

### Implementation status
NOT STARTED

### Differences
Commands are data; application is ordered and inside the tick.

### Reason for differences
Removes between-tick mutation and makes replays/tests possible.

### Tests
`BulletIdSyncTests` (6) drive `PlayerShoot`/`EnemyHit` handlers through a test world.

### Known issues
Handlers can run up to ~50 times between two ticks; a world's state seen by its tick may include half-applied input from several drains.

---

## 4. Cross-thread boundaries

| Boundary | Mechanism in reference | Safe? |
|---|---|---|
| Socket receive -> main | per-user `ConcurrentQueue<IIncomingPacket>` (`NetworkHandler.cs:30`) | yes |
| Any thread -> main | `GameLogic.Enqueue` / `ConcurrentQueue<Action>` (`GameLogic.cs:17,210-212`); `GameThreadSynchronizationContext.Post` (`GameThreadSynchronizationContext.cs:14-16`) | yes |
| Main -> world ticks | sequential: `Update()` and `TickWorlds` never overlap (both on the loop, `Parallel.ForEach` blocks) | yes |
| World tick <-> world tick | `Parallel.ForEach` (`GameLogic.cs:251`); no locks around shared statics | **no** (see section 5) |
| World registry | `ImmutableInterlocked.TryAdd/TryRemove` on `RealmManager.Worlds/Users` (`RealmManager.cs:43,48,52,59`) | yes for the dictionary itself |
| Entity removal | `World._removeEntities` ConcurrentQueue, applied in `World.Update` on main (`World.cs:75,207-209,258-261`) | yes |
| Accept thread -> main | `RealmManager.UserConnected` sends packets from the accept thread (`RealmManager.cs:51-56`) | **no** (unsynchronized send buffer) |
| RPC threads -> game state | `GameServerRpcHandler.GetUserInfo`, `GetStatus` read `RealmManager.Users`, `GameInfo.Data` (which reads `World.EntityStats`) directly (`GameServerRpcHandler.cs:24-27,231-244`; `GameInfo.cs:49`); `ApplyModeration` correctly enqueues (`:247-280`); `SetWeather` writes `WeatherControl` statics then enqueues (`WeatherControl.cs:191-209`) | **partly no** |
| Game -> persistence | `CharacterSaver.Enqueue` clones the `Character` via JSON on the game thread, worker sends RPC (`CharacterSaver.cs:130-139,166-196`) | yes |
| Logging | bounded channel to a writer thread (`Logger.cs:39-42`) | yes |

---

## 5. Global / static mutable state inventory

Every significant static mutable singleton found (GameServer + the Common parts it uses):

| Static | Where | Written by | Notes / risk |
|---|---|---|---|
| `GameLogic.WorldTime`, `TPS`, `_pendingActions`, `_stopRequested`, `ShutdownCompleted`, `SyncContext`, `Stats.*` | `Game/GameLogic.cs:14-25,172-181` | main thread | `WorldTime` read from all world workers and RPC threads; tests overwrite it (`BulletIdSyncTests` Dispose). |
| `RealmManager.Worlds`, `Users`, `Accounts` (unused), `ActiveRealms` (never written), `_nextWorldId` | `Game/RealmManager.cs:24-29` | main, accept thread, world ticks (portal world creation) | Immutable dict + interlocked. |
| `Program.AccountServerRpc`, `Program.Guid` | `Program.cs:20-22` | startup + reconnect loop | Read from everywhere; swapped on reconnect without synchronization. |
| `SocketServer._userFactory`, `Ledger`, `_socket` | `Game/Network/SocketServer.cs:17-22` | startup; accept/disconnect | Ledger is locked internally. |
| `User._nextClientId` | `Game/Network/User.cs:40` | constructor | pool slot ids. |
| `NetworkHandler._packetFactory` | `NetworkHandler.cs:24-28` | type init | immutable after init. |
| `CommandManager._commands` | `Systems/Chat/Commands/CommandManager.cs:12` | startup | immutable after load. |
| `PortalData._worldTypes` | `Systems/Portals/PortalData.cs:15-26` | static ctor | world class lookup by config name (reflection). |
| `Campsite._campsites`, `Campsite.GiftStore` | `Worlds/Logic/Campsite.cs:37,40` | handlers (main), portal instance creation | plain `Dictionary`, no lock. |
| `GuildHall._halls`, `GuildHall.OpenForTesting` | `Worlds/Logic/GuildHall.cs:12,28` | handlers/region triggers | plain `Dictionary`, no lock; halls never close. |
| `Tutorial._worlds` | `Worlds/Logic/Tutorial.cs:14-15` | locked | ok. |
| `Realm._openNames` | `Worlds/Logic/Realm.cs:14` | locked; never cleared | names accumulate. |
| `ZoneSpawner._nextTag` | `Worlds/ZoneSpawner.cs:33` | Interlocked | ok. |
| `Shoot.CustomProjectileOwners` | `Systems/Behaviors/Actions/Shoot.cs:34,260` | behaviour code (world ticks, in parallel) | `HashSet` mutated without lock, iterated on accept thread (`RealmManager.cs:94`). **Race.** |
| `ObjectDesc.Projectiles` custom dict + `_nextProjId` | `Common/Resources/Xml/Descriptors/ObjectDesc.cs:130-181` | `AddOrGet` from behaviours | shared descriptor mutated at runtime; `_nextProjId++` not atomic. **Race.** |
| `BehaviorLibrary.ClassicBehaviors`, `_behaviorFileCache`, `_lastAssembly` | `Systems/Behaviors/BehaviorLibrary.cs:54-58` | startup, reload | Concurrent dictionaries. |
| `PowerUps.Definitions`, `PowerUps.Boosts` | `Systems/Inventory/PowerUps.cs:38-39` | handlers + `PowerUps.Tick` in world ticks | ConcurrentDictionary + `lock(list)`. |
| `StorageShop._buying`, `SkinUnlock._busy` | `Session/StorageShop.cs:28`; `Systems/Inventory/SkinUnlock.cs:16` | locked | in-flight guards. |
| `CharacterSaver._latest`, `_keys`, `_worker`, `_inFlight`, `Saved`, `Failed` | `Systems/Persistence/CharacterSaver.cs:116-123` | game thread + worker | `Saved++`/`Failed++` non-atomic (stats only). |
| `WeatherControl.Weather/Time/SetBy/SetAt` | `Systems/Weather/WeatherControl.cs:185-188` | command (main) or RPC thread | unsynchronized strings (benign race). |
| `XmlLibrary.*` (ObjectDescs, ItemDescs, TileDescs, PlayerDescs, ContainerDescs, SkinDescs, TerrainEnemies, Gemstones) | `Common/Resources/Xml/XmlLibrary.cs:19-40` | startup; tests add entries | read-only after load except as above. |
| `WorldLibrary.WorldConfigs`, `MapDatas` | `Common/Resources/World/WorldLibrary.cs:18-19` | startup; tests | read-only after load. |
| `ConfigLoader<T>._instance` etc. (one per config type) | `Common/Resources/Config/ConfigLoader.cs:85-91` | lazy, hot reload | locked. |
| `OwnerSettings._dirOverride` | `Common/Resources/Config/OwnerSettings.cs:158-165` | tests | |
| `Logger` statics (queue, files, problems ring) | `Common/Utilities/Logger.cs:32-47,152` | all threads | thread-safe. |
| `MerchantsLibrary`, `EnumUtils` | `Common/Resources/Xml/MerchantsLibrary.cs`, `Common/Utilities/EnumUtils.cs` | startup | not inspected in detail (UNVERIFIED). |

Hot spots: `RealmManager`, `GameLogic.WorldTime`, `Program.AccountServerRpc`, `XmlLibrary` (mutable descriptor projectile tables),
`Shoot.CustomProjectileOwners`, the per-world-type instance dictionaries (`Campsite`, `GuildHall`, `Tutorial`), `WeatherControl`.

---

## 6. RPC to the AccountServer

### Reference behavior
- Transport: TCP + `SslStream` (TLS, certificate validation helper `RpcCertificateHelper`), StreamJsonRpc (`Common/Messaging/IpcClient.cs:12-24`).
  Host/port from `rpcClientConfig.xml`.
- GameServer -> AccountServer (`IAccountServerRpc`, `Common/Messaging/Proxies.cs:43-94`): GameServerConnected, GetUserInfo, VerifyAccount,
  GetActiveBans, FlushAccount, GetCharacter, CreateCharacter, FindAccount, Moderate, GetMuteState, SendMail, SaveCampsiteChests,
  LoadGiftChest, SaveGiftChest, FillGiftChest, SaveCharacter, RecordDeath, FlagSuspect, ClaimStarter, BuyStorage, UnlockSkin, ClaimBounty.
- AccountServer -> GameServer (`IGameServerRpc`, `Proxies.cs:12-25`; implemented in `GameServer/Messaging/GameServerRpcHandler.cs`):
  GlobalAnnouncement (logs only), GetGameServer, GetUserInfo, GetStatus, ApplyModeration, GetWeather, SetWeather.
- Reconnect loop with 5 s backoff (`Program.cs:84-110`); account locks are keyed by the game server GUID.

### Current implementation
Gameplay code calls `Program.AccountServerRpc` directly from handlers, commands, the saver worker, AntiCheat, Campsite.

### New architecture (RECOMMENDATION)
`persistence` module behind an interface `IAccountService` (async, returns futures/callbacks posted back into the simulation command
queue). Gameplay never sees the transport. Use a schema'd protocol (protobuf/flatbuffers or JSON-RPC kept for compatibility with the C#
AccountServer if it is kept). All RPC-originated actions enter the simulation as commands.

### Implementation status
NOT STARTED

### Differences / Reason
Decouples simulation from persistence; removes direct reads of game state from RPC threads.

### Tests
`DeveloperDashboardTests` (Common.Tests, 12) cover dashboard rules; no RPC transport test.

### Known issues
- `GetUserInfo` dereferences `c.GameInfo.Account.Id` for every pooled user, including connected-but-not-logged-in ones whose `Account`
  is null (`GameServerRpcHandler.cs:24-27`) -> NullReferenceException (UNVERIFIED at runtime).
- `GlobalAnnouncement` does not broadcast.

---

## 7. RECOMMENDATION - C++ server module layout

Principles applied: RAII, explicit ownership, no globals/singletons, networking separate from simulation, persistence separate from
gameplay, data-driven content, headless, no Unity dependency.

### 7.1 Modules (static libraries + one executable)

| Module | Owns | Depends on | Notes |
|---|---|---|---|
| `core` | types (`EntityHandle`, `Vec2`, `TileCoord`), `Result<T>`, fixed-point/time types, logging interface, assertions | - | header-light, no I/O. |
| `protocol` | packet structs, `PacketId`, codec (frame + encode/decode), protocol version | core | generated from one schema shared with the Unity client (C#). Replaces `Shared/Common.Protocol` + per-packet classes. |
| `content` | `ContentDb` (object/item/tile/player descriptors, world configs, maps, behaviour definitions), loaders (.xml, .json, .jm, .wmap) | core | immutable after load; passed by `const ContentDb&`. Runtime-mutable bits (custom projectile tables) move into per-world state. |
| `sim` | `WorldDirector` (all worlds, instance policy, transfers), `World` (registry + component pools + map + systems), systems (movement validation, combat, projectiles, AI, spawning, sight/replication, inventory, portals, quests, chat routing) | core, content | no sockets, no DB, no threads of its own; deterministic given inputs and seed. |
| `session` | `SessionManager`, `Session` (auth state, character, current world, transfer target, anti-cheat score, rate limits) | core, protocol, sim (command types) | maps connections <-> players. |
| `net` | `Listener`, `Connection`, per-address limiter, flood guard | core, protocol | owns sockets and threads (asio or platform IO). |
| `persistence` | `IAccountService` + implementation (RPC client to AccountServer or direct DB), `SaveQueue` (latest-snapshot-per-character) | core, protocol/schema | no gameplay logic. |
| `admin` | metrics, status endpoint, moderation command intake | core | |
| `server` (exe) | `main`, config, wiring, run loop, shutdown | all | the only place objects are created. |

### 7.2 Ownership

```
Server (stack object in main)
 |- Config
 |- ContentDb                (unique_ptr, immutable)
 |- AccountService            (unique_ptr<IAccountService>)
 |- SaveQueue                 (owned; worker thread joined in destructor)
 |- WorldDirector             (owns vector<unique_ptr<World>>, keyed by WorldId)
 |    `- World               (owns EntityRegistry, ComponentPool<T>..., WorldMap, ZoneSpawner, timers, CommandQueue)
 |- SessionManager            (owns Session objects; references WorldDirector)
 `- NetServer                 (owns Listener, Connections; pushes decoded commands to SessionManager inbox)
```
Destruction order = reverse: net first (stop input), then sessions (save/leave), worlds, save queue (drain), account service.
Non-owning references are plain references/pointers with lifetimes guaranteed by this tree; cross-tree references use ids
(`WorldId`, `SessionId`, `EntityHandle`) resolved at use time.

### 7.3 Interfaces (sketch)
- `class IAccountService { virtual void verify(Token, Callback<VerifyResult>); virtual void load_character(...); virtual void save_character(CharacterSnapshot); ... };`
- `struct InboundCommand { SessionId; std::variant<Move, Shoot, HitReport, UsePortal, InvSwap, Chat, ...> }`
- `class World { void enqueue(InboundCommand); void tick(Dt); void collect_outbound(OutboundSink&); WorldId id() const; }`
- `class WorldDirector { World* find(WorldId); World& instance_for(WorldKind, AccountId/GuildId); void request_transfer(SessionId, WorldId); void tick_all(Dt, JobPool&); }`
- `class OutboundSink { void send(SessionId, const Packet&); }` - implemented by session/net layer; worlds never hold sockets.

### 7.4 Threading model
- 1 network I/O thread (or a small pool) - decode only, push into lock-free MPSC queues; encode/send outbound buffers.
- 1 simulation coordinator thread at fixed 20 Hz; worlds are independent and can be ticked by N worker threads (`JobPool`) with **no
  shared mutable state**; per-world RNG; per-world outbound buffers merged after the tick.
- 1 persistence worker (save queue + RPC client).
- Cross-world effects (transfers, global chat, moderation, weather override) are messages to `WorldDirector`, applied between ticks.

### 7.5 What to drop
- Runtime C# behaviour compilation and `/reloadbehaviors` (Roslyn, `BehaviorLibrary.Reload`); behaviours become data (or compiled C++).
- `SynchronizationContext` / async-await-on-game-thread pattern -> explicit callbacks into the command queue.
- Pooled `User` identity reuse (`ConcurrentFactory<User>`), "check State after every await".
- Reflection registries (`PacketLib.LoadIncoming`, `CommandManager.Load`, `PortalData._worldTypes`) -> static tables.
- Dead code / data: `realmConfig.xml` Close/Events, `WorldConfig.LongLasting/Setpiece`, `EntityManager.Tick`, `PlayerChatManager.Tick`,
  test world `TEST_ID` path in Hello, retired packet ids, `ClientRandom` (constructed in `User.SetGameInfo`, `User.cs:72-73`, but never read anywhere in
  GameServer - grep; the seed is only sent to the client in `MapInfo`), `RealmManager.Accounts` (never read or written outside its
  declaration - grep) and `RealmManager.ActiveRealms` (read at `RealmManager.cs:68`, never written - grep).
- Password in every Hello; trust in client `GameId` during transfer.
- `.wmap` loader if no map uses it (all current maps are `.jm`).

### Implementation status
NOT STARTED

### Differences
Everything above differs structurally from the reference; gameplay rules (numbers in `PlausibilityRules`, `CheatScore`, `HitValidation`,
sight radius 20, tick 50 ms) should be ported unchanged unless a separate decision changes them.

### Reason for differences
The project rules for the port (RAII, no singletons, separated networking/persistence, headless, data-driven) and the races listed in section 5.

### Tests
None exist for the new layout. Pure-rule tests in the reference (section 8) are the best porting oracle.

### Known issues
Unity client must speak the same protocol; the protocol module must be generated for both C# and C++.

---

## 8. Reference tests (what exists)

GameServer.Tests (`WaW-Server/Tests/GameServer.Tests`, counts = number of `[Fact]`/`[Theory]` attributes):
- Behaviors: CatMindTests 9, NexusCatsWalkTests 2, RingAttackTests 3, SwirlTests 3.
- Combat: BulletIdSyncTests 6, CheatScoreTests 5, DefenseAndGearTests 8, EntityCombatTests 2, HitValidationTests 11, PlausibilityRulesTests 7,
  PlayerDeathTests 2, ProgressionTests 9.
- Commands: CommandsTests 22, SpawnRulesTests 4.
- Fixtures: TestWorldFactory (builds a `World` with registered fake XML/world data, no files), TestWorldFactoryTests 5.
- GameLogicQueue (xunit collection to serialize tests touching the shared queue), GameThreadSyncContextTests 5.
- Items: BackpackTests 7, BugsAndTodoTests 8, PlayerSlotTests 2, SkinTests 7, StacksPowerUpsCatsTests 10, TypeIdTests 2, WeaponTierTests 8.
- Network: ConnectionLedgerTests 6.
- Persistence: CharacterSnapshotTests 2.
- Session: StarterRewardTests 4.
- Weather: WeatherControlTests 1.
- Worlds: CampsiteRulesTests 7, CampsiteSwapTests 12, CampsiteWorldTests 10, EntitySlotTests 1, GiftChestTests 13, GiveItemTests 2,
  GuildHallWorldTests 7, HealingObjectTests 2, InitiationWeaponTests 1, RealmQuestTests 5, RealmWorldTests 9, RoleSelectionTests 7,
  WorldTimerAndSpawnTests 3, ZoneSpawnerTests 4.

Common.Tests: AttemptLimiter 5, BugBoardRules 13, CampsiteDb 3, CharacterDb 7, CharacterSlots 4, DailyRewardsFile 6, DeveloperDashboard 12,
FameRules 8, GiftChestRules 6, LoginTokens 4, ModerationRules 7, MusicDirector 27, MusicLibrary 5, OwnerSettings 7, PasswordHasher 5,
PatchNotes 6, PublicProfile 6, Ranks 6, RewardRules 18, StatRules 9, Structs/ConditionEffectSet 19, Structs/StatValue 19,
Utilities/BitMask256 18, Utilities/Collections/EntityId 14, Utilities/Collections/PooledCollections 5.

Not covered by any test: the socket layer, framing, `GameLogic.Run` timing, `Parallel.ForEach` safety, `PlayerSightManager` output
(Update/NewTick contents), map loaders (`MapData`) beyond "the config and its maps load" (GuildHallWorldTests) and Realm/Nexus map checks
(RealmWorldTests), `Hello`/`Load` handlers end-to-end, RPC.

Tests were not run during this audit (instruction: do not run the game or test rigs).

---

## 9. Discrepancies (docs vs code)

| Doc says | Code does |
|---|---|
| `WaW-Server/AGENTS.md:27-29` single game thread; everything touching a world on it | `Parallel.ForEach` world ticks on pool threads (`GameLogic.cs:246-260`). |
| `WaW-Server/AGENTS.md:55-56` "Gameplay handlers return a completed task; only session handlers await" | Commands are awaited fire-and-forget inside `PlayerText` handling (`CommandManager.cs:50`); `SkinUnlock`, `StorageShop`, Campsite start untracked tasks. Packet handlers themselves are `async Task` without awaits (compiler-completed). Mostly consistent. |
| Task brief lists `Shadowcast` as a component | No such symbol; inline scan in `PlayerSightManager.cs:113-173`. |
| Task brief lists descriptors in `WaW-Server/Common/Resources/Xml` | Descriptor *classes* are there; the XML *data* is linked from `WaW-Client/WaWClient/Content/Xmls` (`Common.csproj`). |
| `EngineeringAudit.md:90` "Parallel.ForEach over worlds (2-3 worlds)" | Still true in code; additionally every Campsite, Tutorial, GuildHall, portal template world and Dungeon is in the same parallel set. |
| `GameServer/Game/Systems/Movement/` folder (implied subsystem) | Folder is empty; movement validation is in `Systems/Combat/PlausibilityRules.cs` and `Network/Messaging/Move.cs`. |
