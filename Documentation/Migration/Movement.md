# Movement (reference audit)

Scope: how things move in the reference game - the local player (client-integrated), the server's check of that movement, enemy and NPC
movement (server-simulated), projectile motion (both sides), how the client shows other entities, teleports, and latency handling.
All paths are relative to REF = `Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing/`.
Line numbers were read from the code on 2026-10-02. "UNVERIFIED" marks anything not confirmed in code. Nothing was run.
`Archive/` holds retired code and is not described here.

Units used everywhere below: positions are in **tiles** (float, tile (x, y) covers [x, x+1) x [y, y+1)); time is in **milliseconds**
unless stated. Server tick = 50 ms (`WaW-Server/Common/Resources/Config/Data/gameServerConfig.xml:8` `<TPS>20</TPS>`;
`WaW-Server/Common/Resources/Config/GameServerConfig.cs:21-22` `MsPT = 1000 / TPS`).

---

## 1. Local player movement (client integration)

### Reference behavior
The client owns its own position. Every rendered frame it integrates velocity x frame time, resolves collision against tiles, and keeps the
result. The server never moves the local player except by an explicit `Goto` (snap-back or the `/goto` command).

### Current implementation
- Entry: `WaW-Client/WaWClient/Game/Objects/Player.cs:260-268` `Update` -> for the local player `HandleRelativeMovement(time, dt)`
  (`Player.cs:207-258`); `dt` is the frame's `GameTime.ElapsedMs` (variable, frame-rate dependent - NOT a fixed step).
- Input: `WaW-Client/WaWClient/Game/Components/UserInput.cs:114-123` `SetPlayerMovement` passes `(rotate, right-left, down-up)` to
  `Player.SetRelativeMovement` (`Player.cs:678-689`). Confused (if it were ever set) swaps/negates the axes.
- Direction: `moveVectorAngle = atan2(rel.Y, rel.X)` (`Player.cs:212`); world direction = `CameraAngle + moveVectorAngle`
  (`Player.cs:233-234`). Because only the angle of the input vector is used, **diagonals are not faster** (W+D gives the same speed as W).
- Velocity: `MovementVector = moveSpeed * (cos(a), sin(a))` (tiles/ms) (`Player.cs:233-234`).
- Integration: `WalkTo(Position + dt * MovementVector)` (`Player.cs:255`) -> `ModifyMove` (collision, below) -> `Entity.MoveTo`
  (`WaW-Client/WaWClient/Game/Objects/Entity.cs:252-273`; refuses a target with no tile, i.e. off the map).
- Slide tiles (`GroundProperties.SlideAmount > 0`, `Player.cs:216-231,237-239`): velocity is blended (`MovementVector *= SlideAmount`,
  plus `-(SlideAmount-1) * input`), and keeps coasting with no input while `|v| > 0.00012`. Push tiles are a TODO (`Player.cs:245-249`).

#### Speed formula (exact)
`Player.GetMoveSpeed` (`Player.cs:691-709`), constants `Player.cs:22-25`:
```
MinMoveSpeed = 0.004   tiles/ms  (4 tiles/s)
MaxMoveSpeed = 0.0096  tiles/ms  (9.6 tiles/s)
FocusedSpeed = 15
if Slowed:                       return 0.004 * MovementMultiplier
speed      = Focused ? 15 : Speed          // Speed = the player's Speed stat (int, base + gear + power-up bonus)
moveSpeed  = 0.004 + (speed / 75) * (0.0096 - 0.004)
if Speedy or NinjaSpeedy:        moveSpeed *= 1.5
return moveSpeed * MovementMultiplier
```
**`speed / 75` is INTEGER division** (`speed` is `int`, `75` is an int literal, `Player.cs:696-697`). The result is a step function, not a
curve: Speed 0-74 -> 0.004 tiles/ms (4.0 t/s); 75-149 -> 0.0096 (9.6 t/s); 150-224 -> 0.0152 (15.2 t/s). The class maximums in Players.xml
are 50 (Wizard `Speed max="50"`, Warrior `Speed max="50"`; `WaW-Client/WaWClient/Content/Xmls/Players.xml`), so **every character moves at
4 tiles/s regardless of its Speed stat** unless gear/power-ups lift it to 75+ (e.g. the speed power-up `<Boost stat="22" amount="30"/>`,
`Equip.xml:1903`, or the `+50` speed `ActivateOnEquip` at `Equip.xml:580`), at which point it jumps to 9.6 t/s. The server comment
(`PlausibilityRules.cs:9`) and the RotMG original treat it as a continuous `speed/75f`. See Known issues / Discrepancies.

`MovementMultiplier` (tile speed) is set in `Player.OnMove` (`Player.cs:735-767`), which is called **once per received NewTick**
(`WaW-Client/WaWClient/Networking/Packets/Incoming/NewTick.cs:49`), not per frame:
- normal ground: `MovementMultiplier = tile.GroundProperties.Speed` (Ground.xml `<Speed>`, default 1)
- sinking ground: `SinkLevel = min(SinkLevel + 1, 18)`; `MovementMultiplier = 0.1 + (1 - SinkLevel/18) * (tileSpeed - 0.1)`.
So tile speed takes effect with up to one server tick (50 ms) + network delay of lag after stepping onto a tile.

Condition effects (`Slowed`, `Speedy`, `NinjaSpeedy`, `Confused`, `Paralyzed`...) are read from the `Condition1` stat bucket
(`Entity.cs:346-348`), which **the server never sends** (`Entity.cs:346` comment "TODO: implement same thing server side"; no
`StatType.Condition*` write exists in `WaW-Server/GameServer`). In practice these branches are dead.

#### Collision (exact)
`Player.ModifyMove` (`Player.cs:508-542`): if the frame's displacement is < `MoveThreshold = 0.4` tiles on both axes, one `ModifyStep`;
otherwise it sub-steps in increments of `0.4 / max(|dx|, |dy|)` of the move, each through `ModifyStep`.
`Player.ModifyStep` (`Player.cs:548-620`) - kept "as close to the original as possible" (RotMG's algorithm): it detects crossing a
half-tile border on X and/or Y; if no border is crossed, or the target is valid, the move is accepted. Otherwise it clamps to just before the
next half-tile border (`-0.01` when moving up into the next integer tile), and for a two-axis crossing prefers the axis with the larger
remaining distance, trying `(x, borderY)` then `(borderX, y)` (or the reverse), falling back to `(borderX, borderY)`. On slide tiles a blocked
axis also bounces (`MovementVector *= -0.5`, one component negated).
`Player.IsValidPosition` (`Player.cs:622-676`):
- the target tile must exist and be walkable unless it is the current tile: `MapTile.IsWalkable` = `!NoWalk && (no OccupiedObject ||
  !OccupiedObject.OccupySquare)` (`WaW-Client/WaWClient/Game/MapTile.cs:126-128`). `OccupiedObject` is only set for **static** objects
  (`Entity.cs:262-268`).
- a 0.5-tile body margin against FullOccupy: depending on whether the fractional X/Y is < 0.5 or > 0.5 it checks the neighbouring
  1-3 tiles with `IsFullOccupy` (`Player.cs:717-733`: no tile, tile type 255 (void), or a FullOccupy object).

### New architecture (RECOMMENDATION)
- Keep client-side prediction of the local player (essential for the RotMG feel), but make the movement simulation a **shared, deterministic
  module**: the same C++ code (or a C# port kept bit-compatible by tests) runs on the Unity client and the C++ server, at a **fixed step**
  (e.g. 1/60 s or the server tick subdivided), with `float` math in a defined order.
- Send **inputs, not positions**: per client step `(sequence, dt-step count, move direction (quantised angle or 8-way + camera angle),
  focus flag)`. The server re-simulates the same steps against its own tile map and condition state; the client keeps an input history
  and reconciles on each server acknowledgement (`last processed input seq + authoritative position`), replaying unacknowledged inputs.
  This removes the speed/teleport trust problem entirely instead of bounding it.
- If position-reporting is kept for the first iteration (cheaper), at minimum: timestamp every Move with a client sequence number, check the
  full swept path (not only the destination tile), cap the first Move after a spawn, and use the player's real speed (not the global
  maximum) in the allowance.
- Fix the speed formula deliberately: decide whether the step function is wanted (it is almost certainly an accident) and put the chosen
  formula in the shared module (RotMG: continuous `0.004 + speed/75 * 0.0056`).
- Apply tile speed / sinking per simulation step from the tile under the player at that step, on both sides.

### Implementation status
NOT STARTED

### Differences
None yet (nothing implemented). Planned differences: input-based authority instead of position trust; fixed-step instead of per-frame
integration; tile speed per step instead of per NewTick.

### Reason for differences
Server authority (anti-speed/teleport/noclip) and determinism (prediction must replay identically on both sides).

### Tests
- No client test covers `GetMoveSpeed`, `ModifyMove`, `ModifyStep` or `IsValidPosition` (searched `WaW-Client/Tests/WaWClient.Tests`;
  `Game/FixedStepperTests.cs` covers only the trail fixed stepper).
- Server: `WaW-Server/Tests/GameServer.Tests/Combat/PlausibilityRulesTests.cs` (`MovementDistanceAgainstElapsedTime`,
  `MovementAllowanceIsCappedForVeryOldMoves`), `Combat/CheatScoreTests.cs` (`PlayersCannotEnterWaterButMayStayInTheirTile`).

### Known issues
1. Integer division in `GetMoveSpeed` (`Player.cs:697`): Speed stat has no effect below 75 (all classes cap at 50).
2. Movement is integrated per frame with a variable `dt` - results differ slightly by frame rate (collision sub-stepping helps but the
   last sub-step is still frame-size dependent).
3. Tile speed (`MovementMultiplier`) is updated only per NewTick (`NewTick.cs:49`).
4. Condition effects that alter movement are never sent by the server, so they never apply.

---

## 2. What the client sends

### Reference behavior / Current implementation
- **Packet**: `Move` (id 4, `Shared/Common.Protocol/PacketId.cs:11`), payload = `WorldPosData` = two `float`s X, Y
  (`WaW-Client/WaWClient/Networking/Packets/Outgoing/Move.cs`; server reader `WaW-Server/GameServer/Game/Network/Messaging/Move.cs:67-69`).
  **No timestamp, no tick id, no sequence number, no position history** (RotMG sent a time and a record of recent positions; this does not).
- **Cadence**: one `Move` per **received NewTick** (`NewTick.cs:40-50`), with the player's current position. The server sends one NewTick per
  world tick to each player (`WaW-Server/GameServer/Game/Systems/Sight/PlayerSightManager.cs:49-64,282-284`), i.e. every 50 ms. So movement
  reports are at 20 Hz but clocked by the server's packets arriving (jitter on arrival = jitter on reports).
- The server's NewTick also carries the local player's own position; the client ignores it for the local player (`OnTickPosition(...,
  isPlayer: true)` does not glide, `Entity.cs:289`; the local player's `Update` only runs `HandleRelativeMovement`, `Player.cs:261-262`).

### New architecture (RECOMMENDATION)
Input packets at a fixed client rate (e.g. every 2-3 fixed steps, carrying the inputs of each step and a sequence number), plus the server
sending `ackSeq + authoritative position + velocity` to the owner in its snapshot. Redundantly include the last N inputs in each packet so a
lost UDP datagram does not lose input (if UDP is adopted; UNVERIFIED which transport the C++ server will use).

### Implementation status
NOT STARTED

### Differences / Reason
Planned: inputs + sequence numbers instead of bare positions, decoupled from server-packet arrival - to allow reconciliation and to stop a
modified client from choosing its position.

### Tests
`WaW-Client/Tests/WaWClient.Tests/Networking/SendBufferTests.cs` and `SpanReaderWriterTests.cs` cover serialisation generally, not Move.

### Known issues
- Move cadence depends on NewTick delivery; a stalled downstream also stalls upstream position reports.

---

## 3. Server validation of player movement

### Reference behavior
The server accepts the client's reported position unless it is "impossible"; an impossible move is refused, the player is snapped back with
`Goto`, and anti-cheat points are added. Enforced since 2026-09-24 (comment `PlausibilityRules.cs:6-7`).

### Current implementation (exact rules, `WaW-Server/GameServer/Game/Network/Messaging/Move.cs:19-60`)
Runs on the game thread (packets are drained in `GameLogic.Update`, `WaW-Server/GameServer/Game/GameLogic.cs:214-244`).
`now = GameLogic.WorldTime.TotalElapsedMs` - the **last tick's** time, not the packet's arrival time (`Move.cs:24`).
1. Ignore unless `GameState.Playing` (`Move.cs:20`). (A dead player - `GameInfo.Dead` - is not checked; they can move during the 1.5 s before
   disconnect.)
2. Pending snap-back: if `GotoSentMs != 0` and `now - GotoSentMs < GotoAckTimeoutMs (3000)` the Move is **dropped silently**; after 3 s the
   pending Goto is forgotten (`Move.cs:27-31`, constant `WaW-Server/GameServer/Game/Systems/Combat/PlausibilityRules.cs:25`).
3. Distance check, **only if `LastMoveAtMs > 0`** (`Move.cs:39-49`):
   ```
   distance = |Pos - serverPos|                                   (Euclidean, tiles)
   allowed  = MaxDistance(now - LastMoveAtMs)
   MaxDistance(ms) = clamp(ms, 0, 2000) * 0.0096 * 1.5 * 1.5 + 1.0     (PlausibilityRules.cs:12-21)
                   = clamp(ms,0,2000) * 0.0216 + 1.0 tiles
   ```
   `0.0096` = fastest client speed (MaxMoveSpeed), `1.5` Speedy, `1.5` slack, `+1.0` flat tile of slack, elapsed capped at 2000 ms.
   At a normal 50 ms interval the allowance is 2.08 tiles, i.e. ~10x the real 4-tiles/s speed.
   If `distance > allowed`: snap back + `AntiCheat.Report(MovedTooFar, overBy = distance / allowed)`.
4. Wall check `MovementRules.CanEnter(map, serverPos, Pos)` (`PlausibilityRules.cs:29-41`):
   - same integer tile as the server position -> allowed;
   - destination tile null (off map) -> refused;
   - `tile.Desc.NoWalk` -> refused; `tile.FullOccupy` -> refused; `tile.OccupySquare` -> refused (flags of the static object on the tile).
   Only the **destination tile** is checked: nothing between the two points, no 0.5-tile body margin, no void-tile (type 255) rule.
   Refused -> snap back + `AntiCheat.Report(WalkedThroughWall)`.
5. Accepted: `LastMoveAtMs = now`; `player.Move(world, X, Y)` (`EntityExtensions.cs:18-21` -> `EntityStats.Move`, which for a player has
   no checks, `WaW-Server/GameServer/Game/Systems/Stats/EntityStats.cs:126-147`); region triggers (`RegionTriggers.OnMoved`).

**Snap-back** (`Move.cs:62-65`): `GotoSentMs = now`; send `Goto(playerId, serverPos)` (`WaW-Server/GameServer/Game/Network/Messaging/Goto.cs:9-16`;
wire: int object id + two floats). Client (`WaW-Client/WaWClient/Networking/Packets/Incoming/Goto.cs:23-34`) moves the local player there,
resets its TickPosition/PositionAtTick and replies `GotoAck` (int Time, always 0 - `Outgoing/GotoAck.cs`). Server `GotoAck`
(`Goto.cs:18-31`): `GotoSentMs = 0`, `LastMoveAtMs = now`. A GotoAck is accepted at any time, unsolicited too (harmless: it only shrinks the
next allowance).

**Anti-cheat scoring** (`WaW-Server/GameServer/Game/Systems/Combat/CheatScore.cs`, `AntiCheat.cs`): weights MovedTooFar
`1 + clamp((overBy-1)*2, 0, 8)`, WalkedThroughWall 3, FiredTooFast 0.5 (`CheatScore.cs:23-28`); drain 0.2 points/s
(`CheatScore.cs:13`); >= 20 logged once ("suspicious"), >= 60 kicked + `FlagSuspect` RPC (`CheatScore.cs:14-15`, `AntiCheat.cs:16-46`).
No automatic ban.

Reset of the state: `GameInfo.Load` sets `LastMoveAtMs = 0`, `GotoSentMs = 0`, `FireRate = default`
(`WaW-Server/GameServer/Game/Network/GameInfo.cs:63-79`); `Reset` also clears `Cheat` (`GameInfo.cs:98-112`).

### New architecture (RECOMMENDATION)
With input-based movement (section 1) the server's own simulation replaces all of this: illegal positions cannot be expressed. Keep a
rate check on input packets (too many simulated steps per real second = speed hack; clamp, do not trust) and keep the CheatScore idea for
that. If position reports stay for a first milestone: swept tile check along the segment, the player's actual speed x elapsed (with
measured jitter allowance), the first Move after spawn bounded, and receive-time (not tick-time) elapsed.

### Implementation status
NOT STARTED

### Differences / Reason
Planned: server simulation instead of plausibility bounds - closes the gaps listed below.

### Tests
- `WaW-Server/Tests/GameServer.Tests/Combat/PlausibilityRulesTests.cs`: `MovementDistanceAgainstElapsedTime` (theory),
  `MovementAllowanceIsCappedForVeryOldMoves`.
- `WaW-Server/Tests/GameServer.Tests/Combat/CheatScoreTests.cs`: lag spike vs speed hack scoring, drain, `CanEnter` on water.
- No test drives the `Move` packet handler end to end (snap-back / GotoAck flow).

### Known issues
1. **First Move is unbounded**: `LastMoveAtMs == 0` after every `GameInfo.Load` (`GameInfo.cs:65`) skips the distance check
   (`Move.cs:39`), so the first Move in each world may place the player on any enterable tile.
2. Destination-only wall check: a ~2-tile hop per 50 ms can cross a 1-tile wall line diagonally or straight if the far side is free.
3. The allowance is ~5x larger than real speed (built for the 9.6 t/s maximum x Speedy x slack), so a 2-4x speed hack passes unscored.
4. `now` is the last tick time, so two Moves handled in the same tick get `elapsed = 0` -> allowance 1.0 tile (can snap back an honest player
   after a burst of delayed packets; mitigated by the flat tile).
5. While a Goto is pending, all Moves are dropped for up to 3 s; with packet loss the player is frozen server-side.
6. `PlayerExtensions.MoveToSpawn` places the player at the integer tile corner (`spawnTile.X, spawnTile.Y`, `Entities/Extensions/PlayerExtensions.cs:115-118`),
   not the tile centre.

---

## 4. Enemy / NPC movement (server)

### Reference behavior
Monsters and NPCs move only on the server, driven by behaviour scripts (see `AI.md`). Clients receive positions in NewTick and glide to them.

### Current implementation
- All server movement goes through `EntityStats.Move(x, y)` (`WaW-Server/GameServer/Game/Systems/Stats/EntityStats.cs:126-147`):
  1. **Zoned monster** (`Zone != null` and spawned): a step outside the zone's rectangles or onto a non-passable tile is refused **whole**
     (no slide) (`EntityStats.cs:127-128`; `SpawnZone.Contains`, `WaW-Server/Common/Resources/World/SpawnZone.cs:16-24`).
  2. **Any `EntityType.Enemy`**: a step onto a blocked tile is refused, but a diagonal step slides on the free axis (try `(newX, oldY)`,
     then `(oldX, newY)`) (`EntityStats.cs:132-139`). `CanStep` = same integer tile, or `Map.IsPassable` (`EntityStats.cs:150-151`).
  3. Characters that are not enemies (cats, the Healing Object) and players: no check here (cats check passability themselves, below).
- `WorldMap.IsPassable(x, y)` (`WaW-Server/GameServer/Game/Worlds/WorldMap.cs:67-79`): in bounds; not `NoWalk` ground; tile object type 0 ->
  passable; else `!FullOccupy && !EnemyOccupySquare && !OccupySquare`.
- Speed modifiers `EntityStats.GetSpeed` (`EntityStats.cs:83-105`): Player: Slowed -> **1** (returns the constant 1 tile/s, not a
  multiplier); Speedy x1.5; x tile speed. Character: Slowed -> 1; Speedy x1.5. **`EntityType.Enemy` falls through and gets the raw
  speed** (enemies are resolved to `EntityType.Enemy`, `WaW-Server/GameServer/Game/Entities/Entity.cs:55-58,69-70`), so neither conditions
  nor tile speed affect monsters. Only `Follow` (and `EntityStats.MoveTowards`) call `GetSpeed`; `Wander` and `Move` do not.
- Behaviour movement formulas (all `dt = time.ElapsedMsDelta`, the real elapsed ms of the tick):
  - `Follow` (`WaW-Server/GameServer/Game/Systems/Behaviors/Actions/Follow.cs:65-80`): step = `GetSpeed(speed) * dt/1000` tiles straight
    at the target, stopping when `distSqr < distFromTarget^2`. No pathfinding; the wall slide in `EntityStats.Move` is the only obstacle
    handling.
  - `Wander` (`Actions/Wander.cs:47-95`): picks an angle (constrained by circle-circle intersection to stay within `distanceFromSpawn`
    of where the state was entered), then each tick moves `dt/1000 / (distance/speed) * distance` = `speed * dt/1000` tiles along it for
    `distance/speed` seconds, then waits `cooldownMs`.
  - `Move` (`Actions/Move.cs:49-81`): position = `startPos + move * (elapsedSinceStart / moveTime)` (absolute interpolation; unused).
  - `CatLife` (`Actions/CatLife.cs:135-266`): own speed/ease model (`CatMind.Ease`, tested), checks `IsPassable` at the target and at a
    `Margin` around it before stepping.
- Live monster speeds come from `RealmBestiary.g.cs:8-10`: Green Cube 2.8 t/s, Red Cube 2.38, Blue Cube 2.52 (fight state `Follow` at full
  speed + `Wander` at half speed, both running every tick - their steps add; see `AI.md`).
- Zone placement: `ZoneSpawner` (`WaW-Server/GameServer/Game/Worlds/ZoneSpawner.cs`), Realm zones in
  `WaW-Server/Common/Resources/World/Data/Config/Realm.json` (eight beach zones of 8x8 rectangles).

### New architecture (RECOMMENDATION)
Server-only simulation as today, but at a fixed tick with a real spatial step check (swept circle vs tile grid) shared with the player
collision module so monsters and players collide with the same rules. Add simple pathfinding (grid A* or flow field toward the target,
recomputed at low frequency) if chase behaviour should go around walls. Send per-entity velocity (or let the client derive it from
snapshots) to drive interpolation/animation.

### Implementation status
NOT STARTED

### Differences / Reason
Planned: conditions and tile speed applied uniformly to monsters (today ignored for `EntityType.Enemy`); swept collision; optional
pathfinding.

### Tests
`WaW-Server/Tests/GameServer.Tests/Worlds/ZoneSpawnerTests.cs` (`TheRealmFillsItsZonesAndMonstersStayInside`, `AZoneOnlyHoldsWhatItsRectanglesCover`),
`Behaviors/CatMindTests.cs`, `Behaviors/NexusCatsWalkTests.cs`, `Behaviors/SwirlTests.cs` (unused action).

### Known issues
1. `GetSpeed` ignores Slowed/Speedy/tile speed for `EntityType.Enemy` (`EntityStats.cs:83-105`).
2. `Slowed` returns a constant 1 tile/s instead of scaling.
3. Follow + Wander run concurrently in the fight state; the effective chase speed is the vector sum (UNVERIFIED in play; follows from
   `State.Tick` ticking every script, `WaW-Server/GameServer/Game/Systems/Behaviors/State.cs:99-100`).
4. No pathfinding: a chasing monster slides along walls and can stick in concave corners.

---

## 5. Projectile movement (paths and formulas)

### Reference behavior
Projectiles are not simulated per tick as entities: both sides evaluate a closed-form **path function** `PositionAt(elapsedMs, bulletId,
angle)` from the shot's start position, angle and start time. Same formulas on client (`WaW-Client/WaWClient/Game/Objects/ProjectilePaths/`)
and server (`WaW-Server/Common/Projectiles/ProjectilePaths/`).

### Current implementation (server formulas; client copies verified for Line / Amplitude / Wavy)
`Speed` is in **tiles per second** at path level. XML `<Speed>` is divided by 10 on load (server `ProjectileDesc.cs:25`, client
`Assets/XmlStructs/ProjectileProperties.cs` `Speed = RealSpeed / 10`). Behaviour-defined enemy shots pass tiles/s directly (e.g.
`new LinePath(6f)` = 6 t/s).
- `LinePath` (`LinePath.cs:16-29`): `d = t * Speed/1000`; `p = d * (cos a, sin a)`.
- `AmplitudePath` (`AmplitudePath.cs:23-42`): line + perpendicular `amplitude * sin(phase + t/Lifetime * frequency * 2pi)` where
  `phase = 0` for even bullet ids, `pi` for odd ids (so paired bullets mirror). Client identical (`Game/Objects/ProjectilePaths/AmplitudePath.cs:35-42`).
- `WavyPath` (`WavyPath.cs:16-33`): angle wobble `theta = a + pi/64 * sin(phase + 6pi * t/1000)`, `d = t * Speed/1000`.
- `BoomerangPath` (`BoomerangPath.cs:16-29`): `t' = t > L/2 ? L - t : t`, then line.
- `AcceleratePath`: `speed *= t/L`, `d = t * speed/1000` (`AcceleratePath.cs:16-32`). `DeceleratePath`: `speed *= 2 - t/(L+10)`.
- `ChangeSpeedPath`: piecewise speed increments every `Cooldown` ms after `CooldownOffset` (`ChangeSpeedPath.cs:28-52`).
- `CirclePath`: `angle = a + rps * t/1000 * 2pi`, `p = radius * (cos, sin)` (`CirclePath.cs:22-38`; note `Clone()` divides speed by 50 -
  a latent bug if cloned, `CirclePath.cs:46`).
- `CombinedPath`: average of active segments (`CombinedPath.cs:22-48`).
- `ProjectilePath` (`ProjectilePath.cs:35-52`): a list of segments played in sequence; position = sum of previous segments' end offsets +
  current segment at its local time; returns `(0,0)` past the end.
- Optional `PathSegmentModifier.Boomerang` folds time like BoomerangPath (`ProjectilePathSegment.cs:37-41`).
- Parsing: `ProjectilePathSegment.ParsePath(ProjectileDesc)` (legacy tags: Amplitude/Frequency -> Amplitude, Wavy, Boomerang, else Line)
  and `ParsePath(XElement)` for `<Path>` elements (Line, Wavy, Boomerang, Circle, Amplitude, Accelerate, Decelerate, ChangeSpeed)
  (`ProjectilePathSegment.cs:69-128`).
- Wire format (EnemyShoot): `int segmentCount`, then per segment `byte type` + `float speed, int lifetimeMs, float fixedAngle(rad), int
  timeOffset, int mods` + type extras (`ProjectilePath.cs:54-60`, `ProjectilePathSegment.cs:57-63`).
- Server: a `Projectile` stores `StartPos, Angle (rad), StartTime = tick time, LifetimeMs, LocalId` (bullet number used as projId for
  phase) and is evaluated at `time - StartTime` (`WaW-Server/GameServer/Game/Systems/Projectiles/Projectile.cs:60-86`). **No wall
  collision on the server.**
- Client: `Projectile.Update` per rendered frame: `_elapsed += frameMs`; `newPos = start + Path.PositionAt(_elapsed)`; sweep hit test from
  the previous to the new position; then `MoveTo(newPos)` which kills the bullet on entering a void tile or a tile whose occupying object
  blocks (`EnemyOccupySquare`, or `OccupySquare` unless `PassesCover`) (`WaW-Client/WaWClient/Game/Objects/Projectile.cs:150-190,226-244`).
- Client bullets start at `elapsed = 0` when the packet is **received** (no fast-forward for latency) (`Incoming/EnemyShoot.cs:52-75`;
  player's own bullets at the moment of firing, `Player.cs:490-505`).

### New architecture (RECOMMENDATION)
Keep closed-form paths - they are ideal for authority (the server can evaluate any bullet at any time without per-tick state). Put the path
library in the shared deterministic module. Add wall collision to the server evaluation (precompute, per bullet, the time it first enters a
blocking tile by stepping the path at a fixed sub-step, then treat that as its end time) so both sides agree where a bullet dies. Stamp each
shot with the server tick so the client can fast-forward enemy bullets by its estimated latency if desired (RotMG does not; optional).

### Implementation status
NOT STARTED

### Differences / Reason
Planned: server-side wall termination (today only the client stops bullets at walls); shared path code instead of two hand-synced copies.

### Tests
`WaW-Client/Tests/WaWClient.Tests/Combat/HitRadiusTests.cs` (sweep and bullet-id wrap), `WaW-Server/Tests/GameServer.Tests/Combat/HitValidationTests.cs`
(uses line paths), `Combat/BulletIdSyncTests.cs` (`EveryStoredBulletKnowsItsOwnNumber` - wave phase depends on it). No test compares
client and server path outputs for the same inputs.

### Known issues
1. Server bullets ignore walls; client bullets stop at walls (see `Combat.md`).
2. Player bullets on the server use `ParsePath(projDesc)` (`PlayerShoot.cs:74`), which ignores `<Path>` elements, while the client uses
   `projDesc.Path` which honours them. No current weapon (slot 0) uses `<Path>` (only the `BulletNova` spells do, which are not implemented),
   so this is latent.
3. `CirclePath.Clone()` divides speed by 50 (`CirclePath.cs:46`).

---

## 6. Other entities on the client (interpolation)

### Current implementation
- NewTick gives each visible entity's server position once per tick; `Entity.OnTickPosition` (`WaW-Client/WaWClient/Game/Objects/Entity.cs:277-305`)
  stores it as `TickPosition` and, for non-local entities, computes a glide velocity
  `MovementVector = (TickPosition - Position) / interval` where `interval = clamp(now - lastArrival, 30, 200)` ms (50 ms on the first update).
  A jump > 3 tiles (`> 9` squared) snaps instead of gliding.
- `Entity.Update` with `Settings.MovementInterpolation` (default true, `WaW-Client/WaWClient/Core/Settings.cs:204`) moves toward
  `TickPosition` at that velocity, never past it; after 120 ms with no update the entity stops (`Entity.cs:177-203`).
  This is **"glide to latest"**: no buffered interpolation delay, no extrapolation. Visual position lags the server by roughly one update
  interval plus network latency, and stutters when update arrival is uneven.
- The non-interpolation branch (`Entity.cs:204-220`) depends on `LastTickId`/`Map.LastTickId`, but NewTick always passes `tickTime = 0`,
  `tickId = 0` (`Incoming/NewTick.cs:63`) - effectively broken / unused.
- Other players are moved the same way (their own client reports their position; the server relays it).

### New architecture (RECOMMENDATION)
Snapshot interpolation with a small playout buffer (render remote entities ~2 server ticks in the past, interpolate between the two
bracketing snapshots, extrapolate briefly on loss). Include a server tick number in every snapshot. For hit detection fairness see the
lag-compensation discussion in `Combat.md`.

### Implementation status
NOT STARTED

### Tests
None for `OnTickPosition` / interpolation.

### Known issues
Unbuffered glide = jitter-sensitive; tick id never transmitted.

---

## 7. Teleport, portals, Goto

### Current implementation
- `Teleport` packet (id 22) exists in the table and as a client class (`WaW-Client/WaWClient/Networking/Packets/Outgoing/Teleport.cs`) but
  **nothing sends it and the server has no handler** (grep). `MapInfo.AllowPlayerTeleport` is still sent (`WaW-Server/GameServer/Game/Session/MapInfo.cs:14,27`).
- Server-initiated position changes: anti-cheat snap-back (section 3); moderator `/goto <player>` moves the moderator server-side, sets
  `GotoSentMs` and `LastMoveAtMs` and sends `Goto` (`WaW-Server/GameServer/Game/Systems/Chat/Commands/ModCommands.cs:53-93`).
- Changing worlds (portals, Nexus) is a reconnect into a new world instance (out of scope here; see the session/world docs); on entry the
  player spawns at a random `Spawn` region tile (`PlayerExtensions.cs:115-118`).

### New architecture (RECOMMENDATION)
A single server-authoritative "teleport" message carrying a new position + a reconciliation sequence reset, used by snap-backs, abilities and
admin moves alike.

### Implementation status
NOT STARTED

---

## 8. Latency handling summary

| Concern | Reference handling |
|---|---|
| Local player | Fully client-predicted; server only bounds it (section 3). No reconciliation other than Goto snap-back. |
| Remote entities | Glide-to-latest (section 6). No interpolation buffer, no tick ids. |
| Enemy bullets on client | Start when the EnemyShoot packet arrives (no fast-forward). |
| Hit reports | Server accepts if the bullet was within 2.0 tiles of the target's server position at any time in the last 400 ms (`Combat.md`). |
| Server time base | `GameLogic.WorldTime.TotalElapsedMs`, advanced once per tick (`GameLogic.cs:60-67`); packet handlers see the last tick's time. |

---

## Discrepancies

1. `PlausibilityRules.cs:9` says the client formula is `MinMoveSpeed + speed/75 * (Max - Min)`; the client code computes `speed / 75` with
   **integer** division (`Player.cs:696-697`), a step function.
2. `PlausibilityRules.cs:9-11` says the allowance covers "Slide tiles and pushes"; push tiles are not implemented on the client
   (`Player.cs:245-249` TODO).
3. `PlausibilityRules.cs:27-28` says `CanEnter` is "the client's own rule (Player.IsValidPosition + MapTile.IsWalkable)"; the client rule
   additionally has a 0.5-tile FullOccupy margin and treats void tiles (type 255) as blocked (`Player.cs:622-676,717-733`); the server only checks
   the destination tile's flags (`PlausibilityRules.cs:29-41`).
4. `Docs/EngineeringAudit.md:246` (F25) says the server trusts position "no speed, wall or teleport check"; code enforces speed and
   destination-tile checks since 2026-09-24 (`Move.cs:37-54`). Teleport (first move) is still unchecked (`Move.cs:39`).
5. `CLAUDE.md:601-603` says impossible moves are snapped back and "Moves ignored until GotoAck, 3 s timeout" - matches code; it does not
   mention that the first move after load is unchecked.
6. `EntityStats.GetSpeed` treats `EntityType.Character` and `Player` but not `EntityType.Enemy` (`EntityStats.cs:83-105`); `CLAUDE.md:813`
   ("MONSTERS never step onto a blocked tile") matches `Move`, but nothing documents that monster speed ignores conditions.
7. The task brief places the content XML in `WaW-Server/Common/Resources/Xml`; the XML files actually live in
   `WaW-Client/WaWClient/Content/Xmls/` and are linked into the server build (`WaW-Server/Common/Common.csproj:143-160`).

---

## Implementation (2026-10-02)

Status: COMPLETE (MigrationStatus.md row 13).

- Shared rules: C++ `Server/libs/sim/src/movement.cpp`, C# `WaW/Assets/Scripts/Domain/World/MovementRules.cs` (line-for-line port).
  `Protocol/vectors/movement_cases.json` + generated `movement.json` pin both bit for bit; the C++ suite regenerates them with
  `WAW_WRITE_VECTORS=1`, the C# suite (in .NET AND in Unity) must match every bit.
- Float rule: every C# intermediate is cast with `(float)`. Unity's Mono kept an intermediate at double precision and missed the vector by
  one bit on sinking ground (case "mud") until the casts were added; .NET and C++ were already identical. GCC/Clang builds use
  `-ffp-contract=off` (MSVC does not contract by default).
- Input: 16 ms client steps `{seq, dtMs, dirX, dirY}` (direction quantized to -127..127, normalised on both sides, camera-relative input
  rotated to world space by `InputMapping`). The server applies steps within a real-time budget (+250 ms slack); unapplied steps wait.
- Reconciliation: each Snapshot carries `ackInputSeq`; the client rebuilds its prediction from the server's position by replaying
  unacknowledged steps. Measured: 0 corrections over 6 s of scripted walking against the real server (`UnityClientFlowTests`).
- Remote entities: interpolated 2 ticks (100 ms) in the past from per-tick samples (`PositionBuffer`, `ServerClock`); never extrapolated.
- Differences: input authority; sink level per step time instead of per server tick; negative coordinates are outside the map (the
  reference allowed a little walking past the top / left edge); sub-steps of long moves start from the previous sub-step.
