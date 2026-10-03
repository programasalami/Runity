# World system

Re-baselined on 2026-10-03 on the ORIGINAL Alloy (`zolmex/alloy-server` `Common/Resources/World`, `GameServer/Game/Worlds`). The W&W
Nexus (72 wide), hand-made Realm with spawn zones, Campsite, Tutorial and Testing World are gone. Code: `Server/libs/content`
(loading), `Server/libs/sim/src/tile_map.cpp` + `world.cpp` (instances), `Server/libs/game/src/game_service.cpp` (which worlds exist).

## 1. Content

| What | Where | Loader |
|---|---|---|
| World configs (45, the original's JSON: Id, Name, DisplayName, Difficulty, MaxPlayers, Blocksight, LongLasting, Maps, Music ...) | `Content/Worlds/*.json` | `ContentDb::add_world`; a duplicate Name is a warning, the first wins (the set-piece configs repeat "Avatar") |
| `.jm` maps (54) | `Content/Maps/*.jm` | `add_map`: `{width, height, dict, data}`, data = base64 zlib of big-endian int16 per tile; unknown grounds / objects are warnings |
| `.wmap` realm maps | `Content/Maps/realm1-3-test.wmap` (2048 x 2048) | `add_wmap` (the original MapData.LoadWMap): version byte + zlib {i16 template count; per template u16 ground, u8-length object id, u8-length config, u8 terrain, u8 region, [v1 elevation]; i32 width, i32 height; i16 LE index per tile; [v2 elevation per tile]}; strict length check |

Regions are kept by name ("Spawn", "Realm Portals", ...; `.wmap` region bytes use the original TileRegion values). Terrain bytes are the
original `TerrainType` (None, Mountains, HighSand ... ShorePlains, BeachTowels). The realm maps place no monsters: their 52 object kinds
(trees, rocks, flowers, lilypads) are all static.

## 2. Instances

- A world instance picks one of its config's maps at random (the original `World.Load`).
- `TileMap::build` resolves each palette entry once, then stamps the tiles: static objects and walls become part of the tile (collision
  and sight flags, sent with the tile data - INTENTIONALLY DIFFERENT from the original, which made them entities); every other placed
  object becomes an entity. Walkable tiles are listed per terrain for the realm spawner. A realm costs ~48 MB of tiles.
- Entities live in a slot array with generation ids; a spatial grid of 16 x 16-tile cells is rebuilt once per tick (counting sort) for
  range queries (AI targets, bullets, replication).
- Monsters farther than 40 tiles from every player do not think (`WorldRules::active_radius`); the realm holds ~28 000 of them.

## 3. Worlds and transitions

| World | How it is created | Original |
|---|---|---|
| Nexus (`Nexus.jm`, 142 x 130) | at start; every character enters here | `Nexus.cs` |
| Realm (`realm1/2/3-test.wmap`) | one per Realm Portal (`GameRules::realm_count` = 1, the original `RealmCount`); the portal (0x0704) stands on a random "Realm Portals" tile and carries the realm's name (a monster name from `realmConfig.xml`, e.g. "Medusa"), which is also the realm's display name. The realm itself is created when its portal is first used (or at start when it is the entry world): building it with ~28 000 monsters is the most expensive thing the server does | `Nexus.AddRealmPortal`, `Realm.cs` |
| Vault, Guild Hall, dungeons, set pieces | configs and maps load; no instance logic yet (their portals say "That place is not open yet.") | the modern original has logic only for Nexus, Realm, Vault |

Escape returns to the Nexus. Portals need the player within 2 tiles (the original has no distance check). Transfers stay inside one
server process (no reconnect).

## 4. Replication

Tiles are sent once, only within the sight radius (20) of the player (a realm player receives ~1 250 tiles at first, not 4 million);
entities within the sight radius enter / change / leave per tick; owned loot bags only reach their owner.

## 5. Not ported yet

Realm events (Ghost Ship, Hermit God, Kage Kami set pieces) and realm closing (`realmConfig.xml` Close), Vault instances per account,
dungeons, guild halls, Nexus merchants.

## 6. Tests

`test_content.cpp` (Nexus regions, wmap size / terrains / static objects, broken wmaps), `test_realm.cpp` (spawner fill per terrain,
no monster in walls or water, repopulation, tiles and entities only near the player, far monsters idle), `test_sim.cpp` (tile map,
movement, replication), `test_game.cpp` (Nexus entry, realm portal -> 2048 x 2048 realm and back).
