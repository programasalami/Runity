# AI: monster behaviours, realm spawning and loot

Re-baselined on 2026-10-03 on the ORIGINAL open-source Alloy server (`zolmex/alloy-server`, modern `GameServer/`; `GameServerOld/` is
quoted as LEGACY only where the modern server has no code). The earlier Warriors & Wizards (W&W) bestiary (styles, zones, healing
object, cats) is gone. Spec: `scratchpad/specs/content-and-rules.md` sections 2 and 5.

## 1. Where things are

| What | Where |
|---|---|
| Behaviour data | `Content/Behaviors/<Area>.json`, one file per original `BehaviorLib.<Area>.cs` (Lowland, Midland, Highland, Mountain, Shore, GhostShip, Hermit, Sphinx, LotLL): 142 monsters |
| Transpiler (run once, kept for re-runs) | `Tools/behaviors/transpile_behaviorlib.py <alloy-server root>` |
| Descriptors + JSON loader | `Server/libs/content/include/waw/content/behavior.hpp`, `src/behavior_loader.cpp` |
| Engine | `Server/libs/sim/src/behavior/engine.cpp` (World members), state in `EnemyState` / `ScriptState` (`world.hpp`) |
| Realm spawner | `World::populate_terrain` / `spawn_terrain_group` (`Server/libs/sim/src/world.cpp`) |
| Tests | `Server/tests/test_behavior.cpp`, `test_realm.cpp`, `test_content.cpp` |

## 2. Data format (`"format": 1`)

```json
{ "format": 1, "source": "BehaviorLib.Shore.cs",
  "behaviors": { "Pirate": {
      "loot": [ { "public": true, "items": [ { "type": "Item", "id": "Health Potion", "threshold": 0.03, "chance": 0.9 } ] } ],
      "root": { "scripts": [ { "type": "Shoot", "maxRadius": 3, "targeted": true, "cooldownMs": 2500,
                               "projectile": { "id": "Blade", "damage": 4, "lifetimeMs": 600, "path": [ { "type": "Line", "speed": 4 } ] } },
                             { "type": "Follow", "distFromTarget": 1, "speed": 5.46 }, { "type": "Wander", "speed": 2.94 } ] } } } }
```

- A state is `{name?, scripts[], transitions[], states[]}`; the root has no name. A transition is `{type, to: name | [names], mode?}` plus
  its parameters. Composites `Timed {period, scripts}`, `Duration {duration, script}`, `Sequence {scripts}` nest scripts.
- Names and defaults are the original constructors'. Only arguments written in the C# are emitted. Renames: every cooldown spelling
  (`cooldownMS`, `coolDown`, `coolDownMS`) is `cooldownMs`; offsets (`coolDownOffset`, `cooldownOffsetMS`, `cooldownOffset`) are
  `cooldownOffsetMs`; `durationMS` -> `durationMs`; `followTimeMS` -> `followTimeMs`. Transition types drop the `Transition` suffix.
- Inline projectile: `{id, damage | minDamage+maxDamage, lifetimeMs, size, multiHit, passesCover, armorPiercing, effects: [{effect,
  durationMs}], path: [segment]}`; `projectileIndex: N` fires the monster's own XML `<Projectile id="N">` instead.
- Loot: `{public, items: [{type: Item, id, threshold, chance} | {type: Tier, tier, itemType: Weapon|Armor|Ability|Ring, threshold, chance}]}`.

## 3. Engine semantics (as the original)

- `EntityBehavior.Load`: the monster starts in the root's first child, recursively (`GetDeepState`); entering a state runs its
  transitions' and scripts' Start top-down.
- `State.Tick`, every tick, root first: the state's transitions in order (the first that fires wins, and fires once per entry of its
  state = the original `PastTransitions`), then EVERY script of that state, then the child. A root `Wander` and a child `Follow` both move
  the monster in the same tick.
- `TransitionTo`: exits the current state and every ancestor that is not an ancestor of the target (their scripts' End runs: condition
  effects without `persist` are removed), then enters the target's deep state. `Order` / `OrderOnDeath` use the same path.
- Per-monster, per-script runtime state (`ScriptState`, the original `StateResourceController`) is reset on entry.
- Death scripts (`TransformOnDeath`, `DropPortalOnDeath`, `OrderOnDeath`) arm when their state is first entered and run when the
  monster is killed; `Suicide` and `Transform` remove the monster without a death (no loot, no XP), like `LeaveWorld`.
- Target types: ClosestPlayer, FixedAngle, RandomPlayerPerBehavior / PerCycle, FarthestPlayer, Entity (by object id).
- Shoot: fan centred on the target (`angle - (count/2 - 0.5) * shootAngle`), `angleOffset`, `rotateAngle` accumulated per shot,
  predictive aim = target + 10 ticks of its last movement; a targeted shot with nobody in `maxRadius` fails without cooldown.

Primitives run: Wander, Follow, StayAwayFrom, Charge, Orbit, Protect, MoveLine, ReturnToSpawn, BackAndForth, Buzz, Swirl, Shoot, AOE,
Spawn (incl. groups, density), Reproduce, TossObject, Order, OrderOnDeath, HealSelf, HealGroup, ConditionEffectBehavior, Taunt, Suicide,
Transform, TransformOnDeath, DropPortalOnDeath, Timed, Duration, Sequence. Transitions: Timed, EntityWithin, EntityNotWithin,
EntitiesWithin, EntitiesNotWithin, HpLess, EntityHpLess, DamageTaken, NotMoving, OnParentDeath (Random / Random7Bag / Sequential).
No-ops (presentation only): Flash, SetAltTexture, ChangeSize. Loaded but never fire: PlayerTextTransition.

Not run (load warnings): `SpawnSetpieceOnDeath` (Ghost Ship, Hermit God: realm events are not ported), the `Hermit portal maker`
behaviour (no such object). Transpiler report: every realm behaviour converted; 18 commented loot tables referenced
`LootTemplates.MountainDrop` / `BasicDrop`, which do not exist in the original, and were skipped (the explicit items next to them kept).

## 4. Intentional differences

| Original | Here | Why |
|---|---|---|
| Monsters move through walls and water (`EntityStats.Move` has no check) | they collide with non-walkable tiles, sliding along the free axis | monsters left the map / sat in walls |
| Several queries pass a radius where a squared radius is expected (Protect, Orbit acquire ranges) | the named radius is used | intended behaviour |
| Spawn allows `maxSpawnsPerReset + 1`; `Random.Next(min, max)` never rolls `max` | stops at `maxSpawnsPerReset`; counts inclusive | intended behaviour |
| HealGroup compares its group with object names (never heals) | heals the objects of that `<Group>` | intended behaviour |
| MoveLine counts its distance but never stops | stops after `distance` when one is given | intended behaviour |
| Swirl mixes seconds and milliseconds | circles the point where its state began | the original is broken |
| Transition random bags are shared by every monster of a type | per monster | static state bug |
| Taunt is enemy chat to the whole world | a `ChatMessage` (Say, sender = the monster's entity id and object id) to players within sight | no protocol change; a 2048x2048 realm |
| Every monster ticks | only monsters within 40 tiles of a player think (`WorldRules::active_radius`) | ~30 000 monsters in a realm |
| `ConditionEffectBehavior` is a TODO | Invulnerable / Invincible / Stasis block damage, Armored doubles defense, ArmorBroken ignores it, Paralyzed stops movement, Stunned stops shooting | spec 3 #8 |
| Only Pirate drops loot; TierLoot is a stub | the commented `CharacterLoot` tables are soulbound loot (old format: chance, then threshold; default threshold 1 %); TierLoot = a random item of that `<Tier>` whose slot type is in the class (Weapon 1,2,3,8,17,24; Armor 6,7,14; Ring 9; Ability the rest) | the owner wants realm monsters to drop original items |

## 5. Realm spawner (LEGACY `Oryx.cs` Populate / Repopulate)

Per terrain (Mountains ... ShorePlains), at most 1.5 % of its walkable tiles hold monsters. Every object with `<Terrain>` is rolled
against `<SpawnProb>` (default 1) in turn until the count is reached; a success places `normal(<Mean>, <StdDev>)` clamped to
`[<Min>, <Max>]` monsters (1 without `<Spawn>`) within 5 tiles of a random tile of that terrain. Every 25 s the counts are refilled. Realm
events (Ghost Ship, Hermit God, Kage Kami set pieces) and realm closing are NOT ported yet. realm1-test.wmap holds ~28 000 monsters.

## 6. Loot (the original `LootDrop` / `ItemLoot`)

On death every damage record (one per attacker) rolls every entry: it must have dealt `threshold` x MaxHP, then `chance`. A public
table pools all rolls into shared bags; otherwise each account gets its own soulbound bags. Bags hold 8 items; the bag object is the
original's for the highest BagType inside (0 0x500, 1 0x506, 2 0x508, 3 0x509, 4 0x510, 5 0x507); each bag lands at the monster +
rand(0..1.5) and disappears after 60 s or when emptied.
