# Combat, items and progression

Re-baselined on 2026-10-03 on the ORIGINAL Alloy (`zolmex/alloy-server` = ORIG; `GameServerOld/` = LEGACY, quoted only where the
modern server has no rule). The W&W rules (level 999, sqrt stat curve, W&W XP curve, 1000 XP per fame, 10 % defense floor, universal
gear slots, potions-drunk stats) are gone. Spec: `scratchpad/specs/content-and-rules.md` section 3. Code: `Server/libs/sim/src/rules.cpp`
(formulas), `world.cpp` (shooting, hits, damage, XP, loot, items), `behavior/engine.cpp` (monster attacks, AI.md).

## 1. Authority

The server decides everything: fire rate, damage rolls, every hit (player and enemy bullets, swept in 4 sub-steps, hit radius 0.5),
defense, conditions, HP, death, XP, levels, fame, loot, inventory changes. The client sends `Shoot{shotId, angle}` per attack and draws
its own bullets at once; other players get a `ProjectileVolley`. (The original client reported hits; INTENTIONALLY DIFFERENT, A2.)

## 2. Rules

| # | Rule | Implementation | Source |
|---|---|---|---|
| 1 | Movement speed | `0.004 + (speed/75 int) * 0.0056` tiles/ms x tile speed, sinking ground | ORIG client `Player.cs` (unchanged, shared vectors) |
| 2 | Stats | stored per character (`characters.stats`); level 1 = class start values; each level-up adds `rand(min..max)` of every `<LevelIncrease>`, clamped to the class max | ORIG stores stats per character but never levels up; LevelIncrease = classic RotMG data in Players.xml (owner decision) |
| 3 | Level cap | 20 | owner decision (ORIG has none, LEGACY 50 "for testing") |
| 4 | XP to next level | `(int)(50 + (L-1) * 100 * (1 + L/10f))`; XP is progress within the level | ORIG `PlayerExtensions.cs` |
| 5 | Kill XP | `(int)(ceil(MaxHP/10f) * XpMult)` to every player within `sqrt(2) * sight` (20) of the monster, damage not required, capped at 10 % of each player's next-level goal | LEGACY `CharacterEntity.HandleXpGain`, `Player.CalculateXPGain` (no quests yet, so no 50 % quest cap) |
| 6 | Fame | 1 per 500 XP gained (carried between kills) | LEGACY `XPPerFame` (which did not carry) |
| 7 | Damage | `round(roll * (0.5 + att/50))`, Weak = x0.5 total, Damaging x1.5; roll includes MaxDamage | LEGACY `0.5 + att/75*1.5` (same) |
| 8 | Defense | `max(dmg - def, (int)(dmg * 0.15))`; Armored doubles def; ArmorBroken / armor piercing ignore it; Invulnerable / Invincible / Stasis take nothing | LEGACY `EntityUtils` |
| 9 | Regeneration | HP `1 + 0.12 VIT`/s (not while Sick), MP `0.5 + 0.06 WIS`/s (not while Quiet) | kept from the port (owner decision; ORIG has none) |
| 10 | Fire rate | `1 / (0.0015 + dex/75 * 0.0065) / RateOfFire` ms, server bucket of 3 with 30 % slack | ORIG client formula |
| 11 | Paths | Line, Amplitude, Wavy, Boomerang (single segment) | ORIG ProjectilePaths subset; the realm behaviours use only Line and Amplitude, so the protocol did not change |
| 12 | Slots | 4 class slots (`SlotTypes`) + 8 inventory + 8 backpack; no universal gear | ORIG `EntityInventory` |

## 3. Items

- Equipment boosts: `<ActivateOnEquip stat="N" amount>`, stat numbers as the original client reads them: 0 MaxHP, 3 MaxMP, 20 ATT,
  21 DEF, 22 SPD, 26 VIT, 27 WIS, 28 DEX (others boost nothing; e.g. stat 8 on some robes shows "Invalid Stat!" in the original too).
- UseItem: Heal / Magic potions (`<Activate amount>`), stat potions (`<Activate stat amount>IncrementStat`, +amount up to the class
  max; a potion that cannot raise anything is kept and reported as `Maxed`); `<Consumable/>` items are used up. Abilities are not ported.
- InvDrop: the item goes into a bag at the player's feet (ORIG never moved the bag there), owned by the player if the item is
  `<Soulbound/>`, bag type from its BagType.
- Loot: AI.md section 6. Bag reach 1.5 tiles; bags last 60 s.
- New characters (ORIG `DbClient.CreateCharacterAsync` + `newCharsConfig.xml`): the class's `<Equipment>` (weapon, ability, Health
  Potion in slot 4), class start stats, level 1, one health and one magic potion in the potion stacks (stored; no protocol for them yet).

## 4. Condition effects

Bit N of an entity's `conditions` = the original `ConditionEffectIndex` N (replicated in EntityFull / EntityDelta). Timed effects expire
on the server. Set by monster `ConditionEffectBehavior` (on themselves) and by projectile / AOE `effects` (on players). Rules that read
them: damage (Invulnerable, Invincible, Stasis, Armored, ArmorBroken), player damage (Weak, Damaging), regeneration (Sick, Quiet),
monsters (Paralyzed: no movement, Stunned: no shooting). Player movement is NOT slowed / paralysed by conditions: the client predicts
movement and does not know them yet.

## 5. Death

Permadeath: `PlayerDied{killedBy, level, fame}`, the character is saved dead, the session ends. Not yet: gravestones, death fame to the
account, class bests.

## 6. Tests

`Server/tests/test_combat.cpp` (formulas, level-up growth, XP sharing and cap, fame, conditions, enemy fire, death, walls, fire rate,
regeneration), `test_items.cpp` (gear boosts, slot rules, potions, stat potions, drops, soulbound bags, bag types), `test_behavior.cpp`
(monster attacks, public and soulbound loot). E2E `AWizardHuntsRealmMonstersAndEarnsXp` (the Unity client's code hunts original realm
monsters for XP against the real server).
