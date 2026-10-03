# Entity system - audit and C++ migration notes

Audit date: 2026-10-02. Paths relative to REF (`Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing`).
`GS/` below = `WaW-Server/GameServer/`, `CM/` = `WaW-Server/Common/`. Implementation status of every recommendation: NOT STARTED.

---

## 1. Entity ids

### Reference behavior
- `EntityId` is a 32-bit int: **index = low 20 bits, generation = bits 20-31 (12 bits)** (`CM/Utilities/Collections/EntityId.cs:10-20`).
  `EntityId.Null = (0,0)` (`:8`). On the wire it is written as the raw `int` (`EntityId.Read`, `:22-23`; e.g. `Goto.cs:13`).
- Generation: `SparseSet.MoveNextGeneration(index)` = `gen % 2047 + 1`, so it cycles **1..2047** per index (`CM/Utilities/Collections/SparseSet.cs:9,104-114`).
  0 is never used (keeps `Null` unique); max value `2047 << 20` keeps the sign bit clear. The generation is kept on removal
  (`SparseSet.cs:71-73`), so a reused index gets a new generation and stale ids miss.
- Index allocation (`GS/Game/Entities/EntityManager.cs:20-24`): pop from a free-index stack, else `++_idxCounter` (first id index = 1).
  Index returned to the free list only on a real removal (`:30-33`).
- Ids are **per world**: each `World` has its own `EntityManager` (`GS/Game/Worlds/World.cs:82`), so the same id value exists in many worlds.
- Projectiles have their own id space with the same format (`GS/Game/Systems/Projectiles/ProjectileManager.cs:21-25`), plus a per-owner
  16-bit "local" bullet number 0..1999 that wraps (`GS/Game/Systems/Projectiles/EntityProjectiles.cs:14,42-48`) - this local number is what
  the client and server exchange (`PlayerHit.ProjectileId`, `EnemyHit.ProjectileId`).
- Wrap: generation wraps after 2047 reuses of an index. The index has no overflow check: more than 1,048,575 live-or-never-freed indices
  would bleed into the generation bits (practically unreachable with capacity 5000, but unguarded).
- `User.Id` (connection pool slot, `GS/Game/Network/User.cs:40,52`) and account id are separate from entity ids.

### Current implementation
As above.

### New architecture (RECOMMENDATION)
`struct EntityHandle { uint32_t index : 20; uint32_t generation : 12; }` per world (same wire format as reference if the client protocol
is kept; otherwise 32-bit index + 32-bit generation). Registry asserts on index exhaustion. Projectile ids remain a separate pool;
bullet numbers stay per-owner `uint16` with explicit wrap at a protocol constant.

### Implementation status
NOT STARTED

### Differences
Explicit overflow handling.

### Reason for differences
Reference silently corrupts on index overflow.

### Tests
`EntityIdTests` (`WaW-Server/Tests/Common.Tests/Utilities/Collections/EntityIdTests.cs`, 14), `EntitySlotTests` (1: removing twice frees once),
`BulletIdSyncTests` (6, incl. "bullet numbers wrap at 2000").

### Known issues
- `ProjectileManager.Remove` still pushes the index to the free list unconditionally (`ProjectileManager.cs:27-31`) - the exact
  double-free bug that was fixed in `EntityManager.Remove` (`EntityManager.cs:26-33`; CLAUDE.md:344-347). A projectile removed twice puts
  its index on the free list twice.
- `EntityManager.Count` returns the high-water index counter, not the live count (`EntityManager.cs:10`); `EntityCombat` sizes its damage
  record set from it (`GS/Game/Systems/Combat/EntityCombat.cs` constructor).

---

## 2. Entity types / classes and object descriptors

### Reference behavior
- `Entity` is a small struct: `Id`, `ObjectType` (ushort), `Type` (`EntityType`), `Desc => XmlLibrary.ObjectDescs[ObjectType]`
  (`GS/Game/Entities/Entity.cs:13-22`).
- `EntityType : byte { GameObject, StaticObject, Portal, Merchant, Character, Enemy, Container, Player }` (`GS/Game/Entities/EntityType.cs:3-12`).
- `Entity.ResolveType(objType)` (`Entity.cs:44-79`) by XML `<Class>` first: ConnectedWall/CaveWall/Wall -> StaticObject; Portal/GuildHallPortal ->
  Portal; Character -> Enemy if `<Enemy/>` else Character; ClosedCampsiteChest -> GameObject; Container/StorageChest -> Container;
  Merchant/GuildMerchant -> Merchant. Otherwise `<Enemy/>` -> Enemy, `<Static/>` -> StaticObject, `<Player/>` -> Player, else GameObject.
- `EntityFlags { None, Spawned }` (`GS/Game/Entities/EntityFlags.cs`) stored in `EntityStats.Flags` (BitMask256); used by behaviours
  (`Reproduce.cs:45,57`, `Spawn.cs:109`, `DropPortalOnDeath.cs:31`).
- Descriptors (`CM/Resources/Xml/XmlLibrary.cs:71-147`): each `<Object type= id=>` becomes, by its child elements:
  `<Container>` -> `ContainerDesc`; `<Skin>` -> `SkinDesc`; `<Player>` -> `PlayerDesc`; `<Item>` -> `Item` (ItemDescs, Gemstones). Every
  `<Object>` that is not an `<Item>` and not a `<PetAbility>` is ALSO an `ObjectDesc` (`:112-136`). `<Ground>` -> `TileDesc` (`:139-145`).
  Duplicate type ids: first loaded wins silently (`TryAdd`). Items are **not** ObjectDescs, so an item type can never be an entity.
- `ObjectDesc` fields (`CM/Resources/Xml/Descriptors/ObjectDesc.cs:10-127`): Class, Group, DisplayId, Static, CaveWall, ConnectedWall,
  BlocksSight (tag, or any Wall/CaveWall/ConnectedWall class, `:81`), OccupySquare, FullOccupy, EnemyOccupySquare, ProtectFromGroundDamage,
  ProtectFromSink, Enemy, Player, God, Cube, Quest (+ `QuestDesc level/priority/min/max`), Hero, Level, Oryx, XpMult, MinLevel/MaxLevel,
  Size/MinSize/MaxSize, MaxHitPoints (default 100), Defense, DungeonName, RealmPortal, KeepInSight, SpawnProb, Spawn, Texture, Terrain,
  Projectiles (`ProjectileCollection`, plus runtime "custom" projectiles added by behaviours, `:160-181`).
- `TileDesc` (`CM/Resources/Xml/Descriptors/TileDesc.cs`): GroundId, GroundType, NoWalk, Damage, Speed (default 1), Sinking, Push (+DX/DY).
- XML data files (linked from the client, see Discrepancies) in `WaW-Client/WaWClient/Content/Xmls/`: Objects.xml (205 `<Object>`, 21 with
  `<Enemy`, 190 with `<Static`), StaticObjects.xml (14), Containers.xml (10), Equip.xml (95 items), Players.xml (12), Projectiles.xml (4),
  Ground.xml (78 `<Ground>`, 11 NoWalk), NPCs.xml (empty, 66 bytes). Counts are `grep -c` of opening tags.

### Current implementation
As above. The whole content database is static (`XmlLibrary`) and shared by all worlds.

### New architecture (RECOMMENDATION)
`content` module: `ObjectDef`, `ItemDef`, `TileDef`, `PlayerClassDef`, `ContainerDef`, `SkinDef` as plain structs in flat arrays indexed
by type id, loaded once into an immutable `ContentDb`. Entity archetype is an explicit field in the content data (computed at load by
the same `ResolveType` rules, then validated), not recomputed per spawn. Duplicate type ids are a load error. Runtime-added projectile
definitions move to per-world state.

### Implementation status
NOT STARTED

### Differences
Load-time validation, immutability.

### Reason for differences
Reference mutates shared descriptors from parallel world ticks (`ObjectDesc.cs:160-181`, `Shoot.cs:260`).

### Tests
`TypeIdTests` (2: type id clashes), `TestWorldFactory` registers synthetic descriptors in the 60000+ range.

### Known issues
- Silent duplicate-id handling.
- `Entity.Desc` does a dictionary lookup on every access.

---

## 3. Components / managers

### Reference behavior
Storage: one `ManagerBase<T>` per component type per world, each a `SparseSet<T>` (dense array of structs + sparse index array + per-index
generation array; dense slot 0 is a permanent "null" element; swap-remove) (`GS/Game/Entities/ManagerBase.cs:17-42`; `SparseSet.cs:6-146`).
`Get(id)` returns `ref T` (null slot if missing/stale). Enumeration walks the dense array from index 1 (`SparseEnumerator.cs:14-16`).

Components attached by `World.AddComponents` (`GS/Game/Worlds/World.cs:146-205`):

| EntityType | Stats | Events | Behavior | Projectiles | Combat | Inventory | PortalData | PlayerSight | PlayerChat |
|---|---|---|---|---|---|---|---|---|---|
| GameObject, StaticObject, Merchant | x | | | | | | | | |
| Portal | x | | | | | | x | | |
| Character, Enemy | x | x | x | x | x | | | | |
| Container | x | | | | | x (8 slots) | | | |
| Player | x | x | | x | x | x (`InventoryLayout.PlayerSlots`) | | x | x |

Component structs: `EntityStats` (`GS/Game/Systems/Stats/EntityStats.cs`), `EntityEvents` (OnDeath / OnDamageReceived event buses,
`GS/Game/Systems/Events/EntityEvents.cs`), `EntityBehavior` (state machine, `Systems/Behaviors/EntityBehavior.cs`), `EntityProjectiles`
(bullet number table + current target list, `Systems/Projectiles/EntityProjectiles.cs`), `EntityCombat` (damage accumulator + per-attacker
damage records, `Systems/Combat/EntityCombat.cs`), `EntityInventory` (`Systems/Inventory/EntityInventory.cs`), `PortalData`
(`Systems/Portals/PortalData.cs`), `PlayerSight` (visible entity set, visible/discovered tile bitsets, `Systems/Sight/PlayerSight.cs`),
`PlayerChat` (cooldown, `Systems/Chat/PlayerChat.cs`). `EntityView` (`GS/Game/Entities/EntityView.cs:16-55`) is a ref struct bundling refs
to all components of one id.

### Current implementation
As above. Every component keeps a `World` back-reference.

### New architecture (RECOMMENDATION)
Keep sparse-set pools (or a simple archetype table). Components = PODs without back-pointers; system functions receive the world. Make
the component set per archetype data-driven from `ContentDb`. Replace event buses holding delegates in components with per-world
event queues processed by systems in a defined order.

### Implementation status
NOT STARTED

### Differences
No delegates stored in component structs; events as queued records.

### Reason for differences
Reference's `EventBus<T>` is a `List` of delegates copied by value with the struct (`EventBus.cs`); fragile under struct copying.

### Tests
`TestWorldFactoryTests` (5), `EntityCombatTests` (2), `DefenseAndGearTests` (8), behaviour tests.

### Known issues
- **`EntityEventsManager.Tick` is never called** (`World.Tick`, `World.cs:301-323`, omits it), so `OnDamageReceived` never publishes;
  `DamageTakenTransition` (`Systems/Behaviors/Transitions/DamageTakenTransition.cs:24`) can never fire.
- `OnDeath` is published from `EntityEvents.Dispose` (`EntityEvents.cs` Dispose), i.e. on ANY removal of an entity with events (despawn,
  transform, world-leave of a player), not only on death. Loot/portal-on-death behaviours therefore trigger on any `LeaveWorld` of an enemy.
- `EntityManager.Tick` and `PlayerChatManager.Tick` are empty.

---

## 4. Lifecycle

### Reference behavior
1. **Create**: `var en = new Entity(objType)` - resolves `Type`; no id yet (`Entity.cs:19-22`).
2. **Register / AddEntity**: `World.EnterWorld(ref en)` -> `Entities.Add` assigns id (`EntityManager.cs:20-24`) and copies the struct into the
   dense array -> `AddComponents` creates and adds every component for the type; for Character/Enemy the behaviour is added then
   `Load()`ed (enters its root state) (`World.cs:140-205`). Players use `EnterPlayer`, which also adds to `World.Users` (`World.cs:130-134`).
   Quest objects are added to `World.QuestEntities` (`:174-175`).
3. **Spawn / place**: `Init(world, pos)` -> `EntityStats.Move` (first move sets `SpawnPos` and `Tile`, `EntityStats.cs:142-146`); static
   objects claim `tile.ObjectId` (`GS/Game/Entities/Extensions/EntityExtensions.cs:23-32`). Spawn sources: map load (`World.SpawnFromMap`,
   `World.cs:115-126`), `ZoneSpawner.TrySpawn` (`GS/Game/Worlds/ZoneSpawner.cs:80-108`), map-enemy respawn timer (`EntityCombat.Death`),
   behaviours (Spawn, Reproduce, TossObject, Transform, LootDrop...), set pieces (`WorldMap.SpawnSetPiece`), portals (`Nexus.AddRealmPortal`,
   `DungeonPortals.Open`, `Dungeon.BossDefeated`), `/spawn`, players (`GameInfo.Load`, `GS/Game/Network/GameInfo.cs:63-80`:
   `EnterPlayer` -> `InitPlayer` (stats + inventory from the Character record) -> `MoveToSpawn` (random `Spawn` region tile)).
   Note: behaviours' root state runs at `EnterWorld`, BEFORE the entity is moved to its spawn position (CLAUDE.md:431 documents this).
4. **Tick**: entities have no per-entity tick; systems tick component pools in `World.Tick` order (`World.cs:301-323`, see GameServer.md
   section 3).
5. **Remove / LeaveWorld**: `World.LeaveWorld(id)` only enqueues (`World.cs:207-209`). `World.Update()` (main thread, every loop iteration)
   calls `RemoveEntity`: map origin, quest set, `EntityEvents` FIRST (so OnDeath handlers still see the other components), then Entities,
   Behaviors, Combat, Stats, Projectiles, Inventories, PortalDatas, PlayerSights, PlayerChat, Users (`World.cs:211-225`). Each manager's
   `Remove` calls `Dispose` on the removed struct (`ManagerBase.cs:31-34`).
6. Player-specific exit: `GameInfo.Unload` (play time, `CharacterSaver.SaveUser`, `World.LeaveWorld`) on world switch or disconnect
   (`GameInfo.cs:82-96`); death goes through `PlayerDeath.Die` (save, Death packet, disconnect after 1.5 s) (`Systems/Combat/PlayerDeath.cs`).

### Current implementation
As above.

### New architecture (RECOMMENDATION)
`EntityRegistry::create(archetype, spawn_params)` creates and places atomically (position known before any behaviour runs).
`destroy(handle, reason)` enqueues; removals applied at a fixed point at the end of the tick; `reason` (Died, Despawned, LeftWorld,
Transformed) is passed to death/loot systems so they react only to `Died`.

### Implementation status
NOT STARTED

### Differences
Create+place atomic; removal reason explicit.

### Reason for differences
Reference runs behaviour Start before placement and fires OnDeath on every removal.

### Tests
`WorldTimerAndSpawnTests` (3), `ZoneSpawnerTests` (4), `EntitySlotTests` (1), `PlayerDeathTests` (2), `RealmWorldTests` (9).

### Known issues
- `WorldMap.SpawnSetPiece` sets `tile.ObjectId = entity.Id` BEFORE `EnterWorld` assigns the id (`GS/Game/Worlds/WorldMap.cs:110-117`), so the
  tile gets `EntityId.Null`; and its bounds checks use `>` instead of `>=` (`:82`, `:100`), so `this[x,y]` can return null at the map edge.
- `MoveToSpawn` moves to the integer tile corner of a random Spawn tile (`PlayerExtensions.cs:115-118`), unlike map objects (+0.5); throws if
  the map has no Spawn region.
- `EntityCombat.Tick` calls `Death` on every tick while HP <= 0 until the removal is applied (`EntityCombat.cs` Tick); removal is applied
  before the next tick, so at most once in practice.

---

## 5. Stats storage and dirty tracking

### Reference behavior
- `EntityStats` (`GS/Game/Systems/Stats/EntityStats.cs:17-232`): `Pos`, `PrevPos`, `SpawnPos`, `Tile`, `Flags`, `Stats` (rented
  `StatValue[STAT_COUNT]`), `StatUpdates` (rented `StatData[STAT_COUNT]`), `PublicMask`, `PrivateMask` (BitMask256), `StatUpdateCount`,
  `PositionUpdate`, `ConditionEffects`, zone + zone tag, potions, regen carries. `STAT_COUNT = StatType.StatTypeCount` = 150
  (`CM/Enumerables.cs:82-...`, last real entry `BountyState = 149`).
- `Set(stat, int|float|string, isPrivate)` -> `SetInternal` (`EntityStats.cs:165-188`): no-op if equal; else store, mark
  `_statUpdatesMask`, set `PublicMask` bit unless private, always set `PrivateMask` bit. **Masks are never cleared**: they mean "this stat
  has ever been set (publicly / at all)".
- `Move` sets `Pos` and `PositionUpdate = true`; zoned monsters and enemies refuse/slide on blocked tiles (`EntityStats.cs:126-147`).
- `EntityStats.Tick` (last system of `World.Tick`, `EntityStats.cs:190-207`): `PrevPos = Pos`, refresh `Tile`, copy every dirty stat into
  `StatUpdates[0..StatUpdateCount)`, clear dirty mask, **clear `PositionUpdate`**, tick condition effects, regenerate HP/MP for players.

### Current implementation
Consequence of the order (sight runs before stats in `World.Tick`): NewTick in tick N carries stat changes snapshotted at the end of
tick N-1 but `PositionUpdate` set by moves made during tick N (and by packet handlers between N-1 and N). Stat changes are therefore
sent one tick late; positions are current.

### New architecture (RECOMMENDATION)
Stats as a fixed array per entity plus a dirty bitset; replication system runs LAST in the tick and consumes and clears dirty bits in the
same pass. Visibility class per stat (public/owner-only) comes from a static stat schema table, not from the first `Set` call.

### Implementation status
NOT STARTED

### Differences
Same-tick replication; schema-defined privacy.

### Reason for differences
Avoid the one-tick stat lag and the "privacy decided by first writer" behaviour.

### Tests
`StatValueTests` (19), `ConditionEffectSetTests` (19), `BitMask256Tests` (18), `StatRulesTests` (9), `DefenseAndGearTests` (8).

### Known issues
- A stat set once publicly is public forever; private-ness depends on every call site passing `isPrivate` correctly.
- `EntityStats.Tick` sets `Tile = Map[(int)Pos.X,(int)Pos.Y]`, null if the entity is off-map; `GetSpeed` then dereferences `Tile.Desc`
  for players (`EntityStats.cs:91,192`).

---

## 6. Visibility and sync to clients (Update / NewTick)

### Reference behavior
`PlayerSightManager.Tick` (`GS/Game/Systems/Sight/PlayerSightManager.cs:49-64`), for each player in the world:
1. **Tiles** (`GetNewTiles`, `:82-111`): sight radius `SIGHT_RADIUS = 20` tiles (`:37`). World config `Blocksight` 0 = all tiles in the
   radius circle; 1 = line of sight via an iterative 8-octant recursive-shadowcast-style scan where tiles with `BlocksSight` stop sight
   (`:113-173`). Every tile not yet in the player's `DiscoveredTiles` bitset (or force-updated by a set piece) is sent once (`:175-181`).
2. **Entities** (`ProcessEntities`, `:183-256`): drop entities no longer visible (`ObjectDropData`), then for every entity in the radius
   (chunk query, `WorldMap.GetEntitiesWithin`) that is visible (`IsVisible`, `:286-299`: in LOS worlds the entity's current tile must be in
   `VisibleTiles`; entities with an inventory are visible only if `OwnedBy` the viewer's account - loot bags, chests): new ones go into
   `Update.newObjs` with the full stat array masked by `PublicMask` (`PrivateMask` for the player's own object); known ones with changes go
   into the NewTick status list with `StatUpdates` (masked) and position.
3. **Quest target** beyond the radius is always included (`AddQuest`, `:261-280`).
4. Sends `Update(tiles, newObjs, drops)` if any of the three is non-empty (`:76-79`; packet `GS/Game/Network/Messaging/Update.cs`), and
   **always** sends `NewTick(statuses)` (`:282-284`; `NewTick.cs`), i.e. 20 NewTick packets/s per player even when empty.
Per-tick caches share one `ObjectData`/`ObjectStatusData` per entity across viewers (`:46-47`, `:214-230`, `:234-249`).
Spatial query: chunks of 16x16 tiles rebuilt every tick (`GS/Game/Worlds/ChunkMap.cs:26-40`), 3x3 chunk neighbourhood scanned
(`WorldMap.cs:140-183`) - a 48x48-tile window that extends at least 16 and at most 31 tiles from the player on each axis depending on
where the player stands inside its chunk, so entities 17-20 tiles away can be missed even though `SIGHT_RADIUS` is 20 (derived from the
code; UNVERIFIED at runtime). Results are cached per (2-tile cell, radius) per tick
(`SpatialQueryCache.cs:8,40-65`), so two queries from positions in the same 2x2 cell return the same set.
Wire encoding: `ObjectStatusData.Write`/`WriteForNewTick` (`CM/Structs/ObjectStatusData.cs:27-70`): id, position, byte count, then
`(statType, value)` pairs filtered by the privacy mask.

### Current implementation
Server-driven interest management, no delta compression beyond "changed stats only", no sequence numbers or acks (client `UpdateAck` is
never sent on the wire - client `Outgoing/UpdateAck.cs:4` uses `PacketId.Unknown`). The client replies to every NewTick with its own
`Move` (client `Networking/Packets/Incoming/NewTick.cs` Handle), so movement uploads are paced by server ticks.

### New architecture (RECOMMENDATION)
`replication` system per world, last in the tick: interest set per player from a uniform grid (cell size >= radius, or query by exact
radius), entity enter/leave/update messages, per-connection sequence numbers, skip empty ticks, optional quantized positions. Keep
radius 20 and LOS semantics from content config. Tile streaming: send discovered tile chunks, not per-tile records.

### Implementation status
NOT STARTED

### Differences
Exact-radius interest, empty ticks skipped, sequenced snapshots.

### Reason for differences
Possible missed entities near the radius edge; 20 Hz empty packets; no way to detect loss/reorder (TCP masks it today).

### Tests
None for `PlayerSightManager` output. `RealmQuestTests` (5) cover quest selection; `HealingObjectTests` (2) indirectly.

### Known issues
- 3x3 chunk scan vs radius 20 (see above).
- `SpatialQueryCache` quantizes the query origin to 2-tile cells, so results can include/exclude entities up to ~1.4 tiles off the true
  radius boundary, and different callers in the same cell share the array.
- `IsVisible` and `ProcessUpdate` index `_world.Users[...]` and throw if a sight component exists for a non-user.
- All current world configs have `Blocksight: 0` (WorldSystem.md), so the LOS path is unused by shipped content.

---

## 7. Client representation (brief)

### Reference behavior
- The desktop client keeps `Map.Entities` keyed by the int object id (client `WaW-Client/WaWClient/Networking/Packets/Incoming/NewTick.cs`
  `ProcessObjectStats`); `Update` creates objects / sets tiles / removes drops (client `Incoming/Update.cs`), `NewTick` applies stats and
  calls `Entity.OnTickPosition` (interpolation) (client `Game/Objects/Entity.cs:277`).
- Client object classes: `Entity` (`Game/Objects/Entity.cs:23`, `ObjectId` field `:28`), `Player` (`Game/Objects/Player.cs`), `Projectile`
  (`Game/Objects/Projectile.cs`, pooled via `ObjectPools.Projectiles`, `Game/Objects/ObjectPools.cs:4-6`), projectile paths mirrored from
  the server (`Game/Objects/ProjectilePaths/*` vs `CM/Projectiles/ProjectilePaths`). Total ~1638 lines in `Game/Objects`.
- The client also simulates its own projectiles from `EnemyShoot`/`ServerPlayerShoot` and reports hits.
- Client sight radius constant is 22 (client `Game/SightMap.cs:19`, "the server's SIGHT_RADIUS is 20").

### Current implementation / New architecture (RECOMMENDATION)
Unity client: `EntityView` MonoBehaviours (or ECS) keyed by `EntityHandle` value, fed by a replication decoder; content definitions
loaded from the same exported data as the server. No gameplay authority in the client beyond input and cosmetic prediction.

### Implementation status
NOT STARTED

### Differences / Reason for differences
Out of scope for the server; listed so both sides agree on ids and stat schema.

### Tests
Client tests exist under `WaW-Client/Tests` (not audited here).

### Known issues
Bullet numbering must stay in lock-step between client and server (`PlayerShoot.cs:29-34`).

---

## 8. Discrepancies (docs vs code)

| Doc says | Code does |
|---|---|
| Task brief: object descriptors in `WaW-Server/Common/Resources/Xml` | Descriptor classes live there (`CM/Resources/Xml/Descriptors/*`); data `Resources/Xml/Data/` is empty in source and is linked from `WaW-Client/WaWClient/Content/Xmls/*.xml` at build (`CM/Common.csproj`, `<Content Include="..\..\WaW-Client\WaWClient\Content\Xmls\...">`). |
| CLAUDE.md:344-347 the double-free index bug is fixed ("EntityManager.Remove frees an index ONLY when ...") | True for `EntityManager` (`EntityManager.cs:30-33`), still present in `ProjectileManager.Remove` (`ProjectileManager.cs:27-31`). |
| CLAUDE.md:807 "the server's algorithm, `Shadowcast`" | No `Shadowcast` symbol; inline `Scan` in `PlayerSightManager.cs:113-173`. |
| CLAUDE.md:604 "Every map tile has a non-zero ObjectType ('none' too): judge tiles by flags, never `ObjectType == 0`" | `WorldMap.IsPassable` still returns early on `tile.ObjectType == 0` (`WorldMap.cs:75-76`); `.jm` tiles without objects get ObjectType 255 (`CM/Resources/World/MapData.cs:210`), tiles with an unknown object id get 0. Result is the same as checking flags, but the doc's rule is not followed. |
| `SparseSet.MaxGeneration = 2047` while `EntityId` reserves 12 bits (4095) for generation | Intentional (comment `SparseSet.cs:111-112`: keep sign bit clear); not documented elsewhere. |
