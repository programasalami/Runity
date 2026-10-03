# PostgreSQL - Audit (migration phase: AUDIT)

Sources: `WaW-Server/Common/Database/*` (REF-relative), `Common/Database/Models/*`, `Common/Resources/Config/PostgresConfig.cs`, callers in `AccountServer` and `GameServer`. No database was opened or queried for this audit; everything below is from code. Row counts, actual data shapes in production and the PostgreSQL server version in use are **UNVERIFIED** (README says PostgreSQL 17, `README.md:36`).

---

## 1. Access stack

### Reference behavior / Current implementation
- Driver: Npgsql `NpgsqlDataSource` (pooled) + Dapper (`DbClient.cs:25-29,46`). Connection string built from `postgresConfig.xml` (Host, Port, Database, Username, Password - **file holds a secret**, git-ignored) with no SSL/pool options (`PostgresConfig.cs:30-31`).
- Only the AccountServer process opens connections; the GameServer goes through RPC (`WaW-Server/AGENTS.md:15-19`).
- Schema: `Common/Database/Schema.sql` is executed in full, synchronously, on every AccountServer start (`DbClient.cs:31-35`). It is idempotent (`CREATE ... IF NOT EXISTS`, `ADD COLUMN IF NOT EXISTS`) plus two data `UPDATE`s (`Schema.sql:98-99`). There is no migration table/version.
- Serialization: `System.Text.Json` default options for the `accounts.data` document (`DbClient.cs:75,142`) - property names are the C# PascalCase names unless overridden by `[JsonPropertyName]`.

### New architecture (RECOMMENDATION)
Keep PostgreSQL. Use a versioned migration tool (e.g. a plain numbered-SQL runner, DbUp/Flyway/Sqitch) owned by the Account/API service; run migrations as a deploy step, not on every start. Enable `sslmode` if the DB is ever off-host. If the C++ GameServer gets direct DB access (Option B in `AccountServer.md` section 12), use libpqxx with its own pooled connections and a restricted DB role.

### Implementation status
NOT STARTED.

---

## 2. Full schema (from `Common/Database/Schema.sql`)

No foreign keys exist anywhere. All timestamps `TIMESTAMPTZ`.

### accounts (`Schema.sql:9-15`)
| Column | Type | Constraints | Notes |
|---|---|---|---|
| id | SERIAL | PK | |
| name | TEXT | NOT NULL UNIQUE | display casing; lookups use `LOWER(name)=...` (no functional index -> seq scan) |
| guild_id | INT | NOT NULL DEFAULT 0 | duplicated from `data.GuildId` on every whole-account write |
| data | JSONB | NOT NULL | the whole `Account` model (section 3) |
Index: `idx_accounts_guild_id (guild_id)`.

### logins (`:17-25`)
| Column | Type | Constraints |
|---|---|---|
| id | SERIAL | PK |
| name | TEXT | NOT NULL UNIQUE (lowercase) |
| password_hash | TEXT | NOT NULL (`pbkdf2$<iter>$<b64>` or legacy base64 SHA1) |
| password_salt | TEXT | NOT NULL |
| ip_address | TEXT | NOT NULL (registration IP) |
| last_login_at | TIMESTAMPTZ | NOT NULL DEFAULT now() (set on GameServer entry, `DbClient.cs:329`) |
Index: `idx_logins_ip_address (ip_address)`. Link to `accounts` is **by name only** (`LOWER(accounts.name) = logins.name`), not by id.

### guilds (`:27-35`)
id SERIAL PK; name TEXT NOT NULL UNIQUE; level SMALLINT NOT NULL DEFAULT 0; current_fame BIGINT NOT NULL DEFAULT 0; total_fame BIGINT NOT NULL DEFAULT 0; guild_board TEXT; created_at TIMESTAMPTZ NOT NULL DEFAULT now(). No writer in live code (see `AccountServer.md` section 7).

### bans (`:37-46`)
id SERIAL PK; target_acc_id INT NOT NULL; moderator_acc_id INT NOT NULL; reason TEXT; created_at TIMESTAMPTZ NOT NULL DEFAULT now(); expires_at TIMESTAMPTZ; permanent BOOLEAN NOT NULL DEFAULT false. Index `idx_bans_target_acc_id`.

### mutes (`:48-56`)
id SERIAL PK; target_acc_id INT NOT NULL; moderator_acc_id INT NOT NULL; reason TEXT; created_at TIMESTAMPTZ NOT NULL DEFAULT now(); expires_at TIMESTAMPTZ (NULL = permanent). Index `idx_mutes_target_acc_id`.

### bug_posts (`:59-68`)
id SERIAL PK; account_id INT NOT NULL; author TEXT NOT NULL; message TEXT NOT NULL; status TEXT NOT NULL DEFAULT 'new'; created_at TIMESTAMPTZ NOT NULL DEFAULT now(). Indexes `idx_bug_posts_created_at (created_at DESC)`, `idx_bug_posts_account_id (account_id, created_at DESC)`.

### inbox_messages (`:71-83`, `:127`)
id SERIAL PK; acc_id INT NOT NULL; sender TEXT NOT NULL; subject TEXT NOT NULL; body TEXT NOT NULL; gold INT NOT NULL DEFAULT 0; fame INT NOT NULL DEFAULT 0; created_at TIMESTAMPTZ NOT NULL DEFAULT now(); is_read BOOLEAN NOT NULL DEFAULT false; claimed BOOLEAN NOT NULL DEFAULT false; items INT[] NOT NULL DEFAULT '{}' (added by ALTER). Index `idx_inbox_acc_id (acc_id, created_at DESC)`.

### daily_rewards (`:88-99`)
acc_id INT PK; last_gift_day DATE (legacy, read-only); gift_streak INT NOT NULL DEFAULT 0; last_spin_day DATE (legacy); last_gift_at TIMESTAMPTZ (ALTER); last_spin_at TIMESTAMPTZ (ALTER). Start-up data fix: rows claimed "today" under the old day-based model get `last_*_at = now()` (`:98-99`).

### login_tokens (`:103-109`)
token_hash TEXT PK (SHA-256 hex); login_id INT NOT NULL; created_at TIMESTAMPTZ NOT NULL DEFAULT now(); last_used_at TIMESTAMPTZ NOT NULL DEFAULT now(). Index `idx_login_tokens_login_id (login_id, last_used_at DESC)`.

### anticheat_flags (`:113-121`)
id SERIAL PK; account_id INT NOT NULL; account_name TEXT NOT NULL; points INT NOT NULL; summary TEXT NOT NULL; created_at TIMESTAMPTZ NOT NULL DEFAULT now(). Index `idx_anticheat_flags_account (account_id, created_at DESC)`.

### gift_items (`:128-135`)
id SERIAL PK; acc_id INT NOT NULL; item_type INT NOT NULL; source TEXT NOT NULL DEFAULT ''; created_at TIMESTAMPTZ NOT NULL DEFAULT now(). Index `idx_gift_items_acc (acc_id, id)`. The queue of item gifts waiting for room in the Gift Chest.

### gift_chests (`:136-140`)
acc_id INT PK; items INT[] NOT NULL (8 slots, -1 = empty); updated_at TIMESTAMPTZ NOT NULL DEFAULT now().

### patch_notes (`:145-154`)
id SERIAL PK; date TEXT NOT NULL; version TEXT NOT NULL DEFAULT ''; title TEXT NOT NULL; author TEXT NOT NULL; lines TEXT NOT NULL ('\n'-joined); posted_by TEXT NOT NULL DEFAULT ''; created_at TIMESTAMPTZ NOT NULL DEFAULT now(). No index.

### Inline DDL elsewhere
None found outside `Schema.sql` (grep for `CREATE TABLE|ALTER TABLE|CREATE INDEX` across all `.cs/.sql/.py/.ps1/.sh` files of REF, excluding bin/obj/dist/Archive, matched nothing else). `IgnoredAccounts` / `LockedAccounts` have no writer anywhere (no `.Add` found); `CampsiteDb.LoadAsync` has no caller.

### Relationships (logical, unenforced)
- `logins.name` = `LOWER(accounts.name)` (1:1).
- `login_tokens.login_id` -> `logins.id`.
- `bans.target_acc_id`, `bans.moderator_acc_id`, `mutes.*_acc_id`, `bug_posts.account_id`, `inbox_messages.acc_id`, `daily_rewards.acc_id`, `anticheat_flags.account_id`, `gift_items.acc_id`, `gift_chests.acc_id` -> `accounts.id`.
- `accounts.guild_id` -> `guilds.id` (0 = none).
- Characters, campsite chests, skins, stats: embedded in `accounts.data`.

---

## 3. The `accounts.data` JSONB document

Serialized from `Common/Database/Models/Account.cs` with System.Text.Json defaults (PascalCase keys). Missing keys deserialize to defaults (false/0/null), which several features rely on (e.g. old accounts without `StarterPending` = not new).

### Account (`Models/Account.cs:22-50`)
| JSON key | C# type | Meaning / writer |
|---|---|---|
| Id | int | duplicate of `accounts.id` (written on whole-account writes; targeted writes never touch it) |
| Name | string | display name |
| Rank | int | 0/80/90/100 (`AdminDb.SetRankAsync` jsonb_set) |
| GuildName | string | unused writer |
| IsAdmin | bool | legacy Owner flag (`AdminDb.cs:107-109`) |
| IsBanned | bool | `AdminDb.SetFlagAsync` (`AdminDb.cs:157-158`); cleared by `Hello` whole-account write when bans expired |
| IsMuted | bool | **never read for muting** (mutes come from the `mutes` table, `AdminDb.ActiveMuteEndAsync`); stale field |
| MaxChars | int | slots; `BuyCharSlotAsync` |
| VaultCount | int | (C# `CampsiteCount`) storage rows; `CampsiteDb.BuyRowAsync` |
| NextCharId | int | next character index |
| CreatedAt | DateTime | since 2026-09-22 (older accounts default) |
| Stats | AccountStats | see below |
| Gifts | AccountGifts {ItemTypes int[], ItemDatas byte[]} | legacy, no live writer found |
| GuildId | int | mirrored to column |
| GuildRank | int | |
| TotalGuildFame | int | unused |
| LastSeenAt | DateTime | never written (`PublicDb.cs:173` comment); Portal uses `logins.last_login_at` |
| StarterPending | bool | set at registration, cleared by `StarterDb` |
| OwnedSkins | int[] | `SkinDb` |
| IgnoredAccounts | int[] | sent to client on load (`GameServer/Game/Network/User.cs:92`); no writer found |
| LockedAccounts | int[] | same (`User.cs:89`); no writer found |
| Characters | Character[] | index == CharId |
| VaultChests | CampsiteChest[] | (C# `CampsiteChests`) `CampsiteDb.SaveAsync` |

### AccountStats (`Models/AccountStats.cs`)
BestCharFame int; CurrentFame int (spendable account fame); TotalFame int (lifetime); CurrentCredits int (**gold**); TotalCredits int; CurrentGuildFame int; TotalGuildFame int; ClassStats `[{ObjectType ushort, BestLevel int, BestFame int}]`.

### Character (`Models/Character.cs:6-49`)
CharId int; ObjectType ushort (class); Level int; CurrentFame int (character fame); XpPoints int; SkinType ushort; TextureOne/TextureTwo ushort; PetType ushort; HealthPotions int; MagicPotions int; PowerUps string (`"type:count,..."`); CatPetUnixMs long; BountyState string (`"day:progress:claimed"`); IsDead bool; IsDeleted bool; HasBackpack bool; CreatedAt DateTime; ItemTypes int[] (inventory slots, -1 empty, length `InventoryLayout.PlayerSlots`); ItemDatas byte[] (serialized as **base64 string** by System.Text.Json); Stats CharacterStats; CombatStats; DungeonStats; ExplorationStats; KillStats; Death DeathInfo (null while alive); Role string (permanent once set); RoleRequired bool.

- CharacterStats: Hp, Mp, MaxHp, MaxMp, Attack, Defense, Speed, Dexterity, Vitality, Wisdom (base values without gear), Potions int[] (stat potions drunk per stat).
- CombatStats: Shots long, ShotsHit long, LevelUpAssists, PotionsDrank, AbilitiesUsed, DamageTaken long, DamageDealt long.
- DungeonStats: Completions `{string: int}`.
- ExplorationStats: TilesUncovered, QuestsCompleted, Escapes, NearDeathEscapes, MinutesActive, Teleports.
- KillStats: MonsterKills, MonsterAssists, GodKills, GodAssists, OryxKills, OryxAssists, CubeKills, CubeAssists, BlueBags, CyanBags, WhiteBags.
- DeathInfo: KilledBy string, DiedAt DateTime, BaseFame, TotalFame, Bonuses `[{Name, Description, Amount}]`.

### CampsiteChest (`Models/CampsiteChest.cs`)
ChestId int; ItemTypes int[8]; ItemDatas byte[] (always written empty, `Campsite.cs` `CurrentChests`).

---

## 4. Every query, grouped by repository class

Write style legend: **WHOLE** = replaces the entire `data` document; **TARGETED** = `jsonb_set` on specific paths in one statement; **TX** = explicit transaction.

### DbClient (`Common/Database/DbClient.cs`)
| Method | SQL | Style |
|---|---|---|
| `Load` 27-44 | run Schema.sql | DDL |
| `UpsertAccountAsync` 64-76 | `INSERT accounts(name,guild_id,data='{}') RETURNING id` if new; `UPDATE accounts SET name,guild_id,data=@Data WHERE id` | **WHOLE** |
| `UpsertLoginAsync` 78-91 | INSERT/UPDATE logins (all columns) | whole row |
| `UpsertGuildAsync` / `UpsertBanAsync` / `UpsertMuteAsync` 93-136 | INSERT/UPDATE | whole row - **never invoked** (no enqueues found) |
| `GetAccountByIdAsync` 140-143 | `SELECT data FROM accounts WHERE id` | read |
| `GetAccountByNameAsync` 145-156 | `SELECT data FROM accounts WHERE LOWER(name)=@Name` | read |
| `RememberAsync` / `ForgetAsync` 169-190 | `SELECT id FROM logins WHERE name` + LoginTokens | |
| `RegisterAsync` 192-261 | EXISTS logins by name; COUNT logins by ip; UpsertAccount; UpsertLogin; inbox INSERT + prune | same connection, **no TX** |
| `VerifyAccount` 265-333 | SELECT login; optional `UPDATE logins SET password_hash`; SELECT account; Redis lock; `UPDATE logins SET last_login_at=now()` | |
| `CreateCharacterAsync` 336-412 | in-memory append -> `FlushAsync(acc)` | **WHOLE via DbWriter** (async) |
| `DeleteCharacterAsync` 414-435 | SELECT data by id -> `IsDeleted=true` -> `FlushAsync` | **WHOLE via DbWriter** |
| `BuyCharSlotAsync` 437-447 | in-memory fame check/decrement -> `FlushAsync` | **WHOLE via DbWriter** |
| `GetCharacterAsync` 449-459 | SELECT data by id | read |
| `GetActiveBans` 461-468 | `SELECT ... FROM bans WHERE target_acc_id` (all bans, not only active) | read |

### LoginTokens (`LoginTokens.cs`)
`CreateAsync` 25-33: INSERT + DELETE (idle > 30 days OR beyond newest 10). `ValidateAsync` 35-43: EXISTS (hash, login_id, unexpired) + throttled `UPDATE last_used_at`. `RevokeAsync` 45-46: DELETE.

### CharacterDb (`CharacterDb.cs:72-98`)
`UPDATE accounts SET data = jsonb_set(data, ARRAY['Characters', idx], CASE WHEN stored Role IS NOT NULL THEN @Json || {Role: stored, RoleRequired:false} ELSE @Json END) WHERE id AND Characters is array AND length > idx AND stored IsDead = false`. **TARGETED**, refuses dead input (`:78-79`), never resurrects a dead stored character, keeps a stored role. Returns rows > 0.

### DeathDb (`DeathDb.cs:37-76`)
**TX**: `SELECT data ... FOR UPDATE`; if already dead -> commit, true (idempotent); else compute payout (`Apply`, pure) and one UPDATE with 5 nested `jsonb_set`s: the dead character, `Stats.CurrentFame`, `Stats.TotalFame`, `Stats.BestCharFame`, `Stats.ClassStats`.

### CampsiteDb (`CampsiteDb.cs`)
`SaveAsync` 25-29: `jsonb_set(data,'{VaultChests}', @Json)` TARGETED (max 64 chests x 8 slots, `Clean` 17-23). `BuyRowAsync` 40-62: conditional UPDATE (gold >= cost AND VaultCount = expected AND < max) RETURNING; on miss a SELECT to explain why. `LoadAsync` 64-68: `SELECT data->'VaultChests'` (no live caller found; the GameServer gets chests from the Account object).

### GiftsDb (`GiftsDb.cs`)
`QueueAsync` 16-22 (inside caller's TX): `INSERT gift_items ... FROM unnest(@Items) WITH ORDINALITY`. `LoadChestAsync` 24-28: SELECT items. `SaveChestAsync` 30-33: upsert `gift_chests` (`ON CONFLICT DO UPDATE`). `FillAsync` 37-53: **TX** `DELETE FROM gift_items WHERE id IN (SELECT ... ORDER BY id LIMIT free FOR UPDATE SKIP LOCKED) RETURNING`, place into empty slots, upsert chest, COUNT remaining. `WaitingAsync` 55-58: COUNT.

### RewardsDb (`RewardsDb.cs`)
`BalanceAsync` 31-36 (read jsonb paths); `ListAsync` 42-48; `AddMailAsync` 51-68 (INSERT + prune > 100 claimed/empty); `MarkReadAsync` 75-78; `ClaimAsync` 82-98 **TX** (conditional `UPDATE ... SET claimed=true ... AND NOT claimed RETURNING`, AddReward, QueueAsync); `DeleteAsync` 101-108; `StatusAsync` 112-119; `ClaimGiftAsync` 121-141 **TX** (`INSERT daily_rewards ON CONFLICT DO NOTHING`, `SELECT ... FOR UPDATE`, UPDATE, AddReward, optional mail); `SpinAsync` 144-160 **TX**; `GrantAsync` 193-199 **TX**; `AddRewardAsync` 201-208: one UPDATE with 4 nested jsonb_set adding to CurrentCredits/TotalCredits/CurrentFame/TotalFame (TARGETED, atomic increment).

### SkinDb (`SkinDb.cs`)
`BuyAsync` 24-51: conditional UPDATE charge + append to `OwnedSkins` WHERE balance >= price AND NOT owned RETURNING; else explain SELECT. `UnlockAsync` 55-61: conditional append. The balance path is chosen from server XML (`SkinRules.BalancePath`) and interpolated into SQL text (`:32-36`) - safe only because the value comes from server data, not input.

### RoleDb (`RoleDb.cs:14-37`), StarterDb (`StarterDb.cs:13-22`)
Conditional TARGETED jsonb_set (role only if null and alive; StarterPending only if true).

### AdminDb (`AdminDb.cs`)
`FindAsync`, `BanAsync` (INSERT bans + `jsonb_set IsBanned=true`), `UnbanAsync` (UPDATE bans ending active ones + flag false), `MuteAsync` (INSERT mutes), `UnmuteAsync` (UPDATE mutes), `ActiveMuteEndAsync` (SELECT), `SetRankAsync` (jsonb_set Rank + IsAdmin), `MailAsync` (AddMail). All without TX; ban INSERT and flag UPDATE are two statements (`:31-35`). `SetFlagAsync` interpolates the property name into SQL (`:157-158`) - only called with the constant "IsBanned".

### AntiCheatDb (`AntiCheatDb.cs:9-14`), NewsDb (`NewsDb.cs`), BugBoardDb (`BugBoardDb.cs`)
Simple INSERT/SELECT/UPDATE/DELETE on their own tables. `BugBoardDb.NowUnixAsync` opens a connection just to read `now()`.

### PublicDb (`PublicDb.cs`) and DevDb (`DevDb.cs`)
Read-only: `LoadAccountByNameAsync` (`lower(name)` + logins last login); `SearchNamesAsync` (ILIKE with a sanitized pattern); `LeaderboardAsync`: fame = order by `(data->'Stats'->>'TotalFame')::bigint` over all accounts; chars/level = `jsonb_array_elements(a.data->'Characters')` over **all accounts** then sort; guilds = `guilds` + correlated COUNT; `LoadGuildAsync`. DevDb: search with LEFT JOIN logins on `lower(name)`, player detail with bans/mutes/flags joins, `count(*)` accounts. None of these have supporting indexes (full scans; acceptable at current scale, UNVERIFIED size).

### ModelUtils (`Common/Utilities/ModelUtils.cs:108-126`)
`Guild.ToXml` runs a **synchronous** query `SELECT data FROM accounts WHERE guild_id` - only reachable from commented-out routes.

---

## 5. Write batching (`DbWriter<T>`, `Common/Database/DbWriter.cs`)

- One static unbounded `Channel<T>` + one long-running worker per type (`:17,22-30`). `Init` for Account, Login, Guild, MuteRecord, BanRecord (`DbClient.cs:39-43`); **only `Account` is ever written through it** (CreateCharacter, DeleteCharacter, BuyCharSlot, RPC FlushAccount).
- Worker drains up to 500 items, de-duplicates by reference (same object instance), writes all in one transaction (`:43-66`).
- `WriteAsync` returns when the item is **enqueued**, not committed (`:74-80`). Callers report success before persistence.
- Failure: the whole batch is dropped with a `Console.WriteLine` (`:67-70`) - no retry, no logger, no error propagated to the caller. One bad row rolls back every other account in the batch.
- Shutdown: `StopAsync` completes the channel and awaits the worker (`:82-86`), called from `DbClient.Dispose` on graceful shutdown (`AccountServer/Program.cs:125`). A crash loses everything queued.

RECOMMENDATION: remove `DbWriter` entirely. Every write in the new service should be an awaited, targeted statement (or a small explicit transaction) whose failure is returned to the caller. If batching is needed for GameServer autosaves, batch on the GameServer side with per-item results.

---

## 6. Transactions, concurrency and failure handling

- Good patterns (keep): conditional single-statement updates (`SkinDb`, `CampsiteDb.BuyRowAsync`, `StarterDb`, `RoleDb`, `RewardsDb.ClaimAsync`), `SELECT ... FOR UPDATE` serialization (`DeathDb`, daily rewards), `FOR UPDATE SKIP LOCKED` queue consumption (`GiftsDb.FillAsync`), idempotent death payout.
- Risky patterns: WHOLE-document writes (lost updates against all targeted writers), read-modify-write in memory (`BuyCharSlotAsync`), multi-statement operations without a transaction (`RegisterAsync`, `AdminDb.BanAsync`).
- Error handling: exceptions from Npgsql bubble up; HTTP handlers turn them into `<Error>Internal error.</Error>` (`AccountServer/Program.cs:219-223`); RPC calls propagate the exception to the GameServer (StreamJsonRpc remote exception), whose callers log and (sometimes) retry - see `Persistence.md`. No retry policy, no circuit breaker, no statement timeout configured.
- Connection pool limits: Npgsql defaults (max 100) - UNVERIFIED that nothing overrides them.

---

## 7. Migration approach (RECOMMENDATION)

1. **Phase 0 - compatibility:** the new C# Account/API service reads/writes the existing schema unchanged (same jsonb keys, including the legacy `VaultChests`/`VaultCount` names - test `CampsiteDbTests.SavedAccountsKeepTheirChestsUnderTheOldNames`). Port the targeted SQL verbatim; drop all WHOLE writes by turning create/delete/buy-slot/ban-lapse into targeted statements. This alone removes the lost-update class without a data migration.
2. **Phase 1 - baseline migration:** capture the current `Schema.sql` as migration `0001_baseline` (idempotent), add a `schema_version` table, stop running DDL at service start.
3. **Phase 2 - normalize hot data** (one migration per step, with backfill from jsonb and dual-read during transition):
   - `account_balances(account_id PK, gold, gold_total, fame, fame_total, best_char_fame)` or a `currency_ledger` (append-only, idempotency key UNIQUE) + balance columns; all reward/purchase code becomes `UPDATE ... SET gold = gold + $1 WHERE ... AND gold + $1 >= 0`.
   - `characters(account_id, char_id, PK(account_id,char_id), class, level, xp, char_fame, skin, is_dead, is_deleted, role, created_at, died_at, stats jsonb, death jsonb, ...)` - the C++ GameServer then saves one row by key, not an array element. Keep low-churn nested stat blocks as jsonb columns.
   - `character_items(account_id, char_id, slot, item_type, item_data)` or keep `item_types int[]` on `characters` (simpler; arrays are fine for fixed 8-20 slots). Recommendation: **int[] columns** on `characters` and `campsite_chests(account_id, chest_id, items int[])`, mirroring `gift_chests`.
   - `account_skins(account_id, skin_type PK pair)`, `class_stats(account_id, class, best_level, best_fame)`.
   - Add real foreign keys + `ON DELETE` rules, and a functional unique index `lower(name)` on accounts (and link `logins` by `account_id`, not name).
4. **Phase 3 - drop** the migrated jsonb keys after a verification job compares both representations.

Keep compatible or normalize? **Normalize characters, balances, skins and chests out of JSONB** (they are written frequently by different writers - the root cause of every lost-update issue found). Keep JSONB for genuinely document-shaped, rarely-queried blobs (combat/kill/exploration stats, death bonuses).

Implementation status: NOT STARTED.

---

## 8. Tests (reference)
- Pure-logic only: `CharacterDbTests.cs` (Clean), `CampsiteDbTests.cs` (Clean + JSON key names), `FameRulesTests.cs` (includes `DeathDb.Apply`, lines 93,103), `GiftChestRulesTests.cs`, `RewardRulesTests.cs`, `LoginTokensTests.cs`, `PasswordHasherTests.cs`, `PublicProfileTests.cs`, `DeveloperDashboardTests.cs`, `CharacterSlotsTests.cs` (all in `WaW-Server/Tests/Common.Tests`).
- **No test touches a real PostgreSQL** (no test file references Npgsql/`DbClient.Load`). Every SQL statement above is untested by automation. RECOMMENDATION: integration tests against a disposable PostgreSQL (Testcontainers) for every repository method, especially the conditional updates and the death/claim idempotency.

---

## 9. Known issues
1. WHOLE-account writes from stale copies (GameServer `Create`, `Hello` ban-lapse) and in-memory read-modify-write (`BuyCharSlot`, `DeleteCharacter`) can roll back gold, fame, skins, chests, roles and saved characters.
2. `DbWriter` acknowledges before commit and silently drops failed batches.
3. `LOWER(name)` lookups without a functional index; `name UNIQUE` is case-sensitive, so `Bob` and `bob` could both exist in `accounts` if inserted outside `RegisterAsync` (logins uniqueness is on the lowercased name, which prevents it on the normal path).
4. No foreign keys; orphan rows possible (e.g. a failed registration).
5. Leaderboards scan and unnest every account document.
6. Schema DDL + data UPDATE executed on every start.
7. `inbox_messages` pruning keeps at most 100 claimed/empty messages; unclaimed gifts are never pruned (unbounded by design).

---

## 10. Discrepancies

| # | Doc says | Code does |
|---|---|---|
| P1 | `Docs/EngineeringAudit.md:118-120` lists tables `accounts, logins, guilds, bans, mutes, bug_posts, inbox_messages, daily_rewards` | Schema now also has `login_tokens`, `anticheat_flags`, `gift_items`, `gift_chests`, `patch_notes` (`Schema.sql:103-154`) |
| P2 | `Schema.sql:1-7` header: "every write is still a whole-object replace" | Most writes are now targeted `jsonb_set`; only DbWriter<Account> paths are whole-object |
| P3 | `Docs/EngineeringAudit.md:947` "the schema is unchanged" (at that step) | Later steps added five tables and columns (see P1) - consistent with history, stale as a description |
| P4 | `Common/Database/DeathDb.cs:19` "pure; tested in DeathDbTests" | No `DeathDbTests` file; `DeathDb.Apply` is tested in `Common.Tests/FameRulesTests.cs:93,103` |
| P5 | Test name `CharacterDbTests.ADeadCharacterIsStoredAtFullHealthBecauseDeathIsNotBuiltYet` (`CharacterDbTests.cs:37`) | Death is implemented (`DeathDb`); `CharacterDb.SaveAsync` refuses dead characters (`CharacterDb.cs:78-79`) |
| P6 | `PublicDb.cs:14-15` "no ... gold" exposed | True for `/public/*`; `DevDb.Player` exposes gold (Developer-only, by design) |
| P7 | `Common/Database/Models/Character.cs:42-43` Role "Set ONCE, by RoleDb ... in the Role Selection ceremony" | Role is normally set by `DbClient.CreateCharacterAsync` (`DbClient.cs:360`); ceremony removed 2026-09-27 per `ChooseRole.cs:10-12` |
| P8 | `WaW-Server/README.md:39` alloy.db (LiteDB) | PostgreSQL |
