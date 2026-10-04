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
