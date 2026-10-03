# Persistence - Audit (migration phase: AUDIT)

What is saved, when, by whom, and what happens on failure, in the reference (REF-relative paths). Companion docs: `AccountServer.md` (RPC table), `PostgreSQL.md` (SQL), `Redis.md` (locks).

Ownership rule in the reference: **only the AccountServer touches PostgreSQL**; the GameServer persists by RPC (`IAccountServerRpc`, `Common/Messaging/Proxies.cs:43-95`) and holds authoritative live state in memory between saves.

---

## 1. What is saved, by whom, when

| Data | Live owner | Writer (GameServer side -> RPC -> DB class) | When | Write style |
|---|---|---|---|---|
| Character (level, xp, char fame, HP/MP, base stats, stat potions, inventory + backpack `ItemTypes/ItemDatas`, potions stacks, power-ups, cat timer, bounty state, run stats) | GameServer | `CharacterSaver.SaveUser` -> `SaveCharacter` -> `CharacterDb.SaveAsync` | world leave / disconnect / portal / escape (`GameInfo.Unload`, `Game/Network/GameInfo.cs:82-95`), every 60 s autosave (`Game/GameLogic.cs:21,81-84,92-107`), shutdown (`GameLogic.cs:122-130`), immediately after starter claim (`Session/ClaimStarter.cs:95`), cat petting (`Systems/Inventory/CatCare.cs:80`), bounty claim (`Systems/Rewards/BountyBoard.cs:110`) | targeted jsonb_set of `Characters[CharId]` |
| Death (dead character + fame payout to account) | GameServer | `PlayerDeath.Die` -> `RecordDeath` -> `DeathDb.RecordAsync` | at death (`Systems/Combat/PlayerDeath.cs:24-74,100-116`) | one TX, idempotent |
| Campsite chests (`VaultChests`) | GameServer (Campsite world) | `Campsite.SaveIfChanged` -> `SaveCampsiteChests` -> `CampsiteDb.SaveAsync` | every 3 s if contents changed (`Worlds/Logic/Campsite.cs:32,334-366`) | targeted jsonb_set |
| Storage row purchase (gold, VaultCount) | AccountServer | `StorageShop` -> `BuyStorage` -> `CampsiteDb.BuyRowAsync` | on purchase | conditional UPDATE |
| Gift Chest contents | GameServer (Campsite) | `GiftCheck` -> `SaveGiftChest` / `FillGiftChest` -> `GiftsDb` | check every 500 ms; save after a take; fill every 5 s when owner present and room (`Campsite.cs:33-34,170-199`); at shutdown (`GameLogic.cs:113-119`) | own table, fill = one TX |
| Gift queue (`gift_items`) | AccountServer | inbox claim / daily gift | HTTP | TX |
| Account currencies (gold `CurrentCredits`, fame) | **AccountServer** (DB is truth); GameServer holds a display copy | `RewardsDb` (inbox, daily, spin, bounty), `SkinDb`, `CampsiteDb.BuyRowAsync`, `DeathDb`, `BuyCharSlotAsync` | on each operation | targeted, except BuyCharSlot (WHOLE) |
| Skins | AccountServer | `/account/purchaseSkin`, RPC `UnlockSkin` -> `SkinDb` | on purchase/unlock | conditional |
| StarterPending | AccountServer | RPC `ClaimStarter` -> `StarterDb` | after items handed over | conditional |
| Role | AccountServer | `CreateCharacterAsync`, `/char/chooseRole` -> `RoleDb` | create / choose | conditional |
| Account (whole document) | mixed | `FlushAccount` (Hello ban-lapse, `Session/Hello.cs:86-87`), `CreateCharacter` (`Session/Create.cs:26` -> `DbClient.cs:401-403`), `/char/delete`, `/account/purchaseCharSlot` -> `DbWriter<Account>` | on those events | **WHOLE, asynchronous** |
| Bans / mutes / ranks | AccountServer | `AdminDb` (chat commands via RPC `Moderate`, `/dev/moderate`) | on command | INSERT + targeted |
| Anti-cheat flags | AccountServer | RPC `FlagSuspect` -> `AntiCheatDb` | kick-level score | INSERT |
| Login metadata | AccountServer | `VerifyAccount` (`last_login_at`, hash upgrade), `LoginTokens` | login | targeted |
| Inbox, daily rewards, bug board, patch notes | AccountServer | HTTP handlers | on request | TX / single statements |
| Not persisted at all | - | music playlist state, public/dev caches, rate-limiter counters, weather override (UNVERIFIED for weather; not traced), world state (enemies, loot bags on the ground) | - | - |

"Vault" is the stored name of the Campsite storage (`Models/Account.cs:30-32,49-50`; `CLAUDE.md:674-679`).

---

## 2. Character save flow (reference)

1. Game thread: `CharacterSaver.SaveUser(user)` (`Game/Systems/Persistence/CharacterSaver.cs:78-92`) - only when `GameState.Playing` with world, char and account. Copies the live inventory into the `Character` (`inv.Save`) and stats (`CharacterSnapshot.CaptureStats`, `:15-36`; base stats = shown minus gear bonuses).
2. `Enqueue` (`:66-75`) deep-clones the Character via JSON round-trip and stores it in `_latest[(accountId, charId)]` (only the newest snapshot per character is kept); the key is pushed to an unbounded channel if new.
3. Background worker (`:102-132`) takes the latest snapshot and calls RPC `SaveCharacter`. On exception: `Failed++`, logs, puts the snapshot back (unless a newer one arrived) and re-queues the key after 5 s. **Retries forever** while the process lives.
4. AccountServer: `CharacterDb.SaveAsync` (`Common/Database/CharacterDb.cs:72-98`): `Clean` (clamps level/fame/xp/potions, HP 0 -> MaxHP, MP clamp, invalid role -> null; `:19-69`), refuses dead characters, then targeted `jsonb_set` that (a) only applies if the array element exists, (b) never overwrites a stored dead character, (c) keeps a stored role. Returns false / logs a warning when nothing was written - the RPC returns normally, so **the GameServer treats "0 rows" as success** (`AccServerRpcHandler.cs:105-107` discards the bool).

Load flow: `Session/Load.cs:24-64` -> RPC `GetCharacter` -> `DbClient.GetCharacterAsync` (`DbClient.cs:449-459`) reads the account row, returns `Characters[charId]`; dead -> "Character is dead."; null -> "Failed to load character #N" and disconnect.

---

## 3. Death flow (reference)

`PlayerDeath.Die` (`Game/Systems/Combat/PlayerDeath.cs:24-74`): mark `info.Dead`; snapshot inventory + stats into the record; compute fame (`FameRules.Calculate`); set `IsDead` + `DeathInfo`; update the session's Account copy; `Record()` -> background task calling RPC `RecordDeath` up to **6 attempts, 5 s apart** (`:21-22,100-116`); send `Death` packet; disconnect 1.5 s later. The disconnect triggers `GameInfo.Unload` -> `SaveUser`, but `CharacterDb.SaveAsync` refuses `IsDead` characters, so only DeathDb writes the dead state (`CLAUDE.md:598-599` - verified at `CharacterDb.cs:76-79`).
`DeathDb.RecordAsync` (`Common/Database/DeathDb.cs:37-76`): TX, row lock, idempotent (already dead -> true), writes dead char + CurrentFame/TotalFame/BestCharFame/ClassStats.

Gaps: if all 6 attempts fail (AccountServer down > ~30 s) or the GameServer process dies before the task succeeds, **the death is never recorded**: the character stays alive in the DB with the state of its last save (inventory included) and no fame is paid. No durable outbox.

---

## 4. Campsite (vault) and Gift Chest

- **Campsite storage**: chests loaded from the Account object fetched at `Hello` (`Campsite.SetupChests`, `Campsite.cs:80-113`); saved every 3 s if changed. `SaveIfChanged` sets `_lastSaved = now` **before** sending (`:344-366`); if the RPC is null or fails, the change is not retried until the next change. Not saved at shutdown (`GameLogic.ShutdownAsync` saves only Gift Chests + characters, `GameLogic.cs:112-130`). The timer callback returns early once the world is `Deleted` (`:336-337`), so changes made within the last 3 s before world removal are not saved (when Campsite worlds are deleted: UNVERIFIED).
- **Moving items between chest and character** is two independent writes (chest via `SaveCampsiteChests`, character via `SaveCharacter`) at different times -> a crash or shutdown between them can **duplicate** an item (character saved with it, chest not yet saved) or **lose** it (chest saved without it, character not yet saved).
- **Gift Chest** (`GiftChestRules.cs:7-15`, `GiftsDb.cs`, `Campsite.cs:116-313`): designed for no duplication: item in exactly one place (message -> queue -> chest), each move one TX, chest is take-only and locked during a fill; failed saves are retried by the next check; a fill with a lost answer reloads from DB; saved first at shutdown. Residual risk: an item taken from the gift chest into the inventory follows the same two-write pattern as the campsite (gift chest saved first at shutdown on purpose, `GameLogic.cs:113`).

---

## 5. Inventory saves
Inventory is part of the character snapshot (`ItemTypes`, `ItemDatas`), saved only by the character save triggers above (no per-change save). Loot picked up, trades (if any), drops and equipment swaps between autosaves are lost on a GameServer crash (up to 60 s).

---

## 6. Failure scenarios - what the reference does today

| Scenario | Reference behaviour (file:line) |
|---|---|
| **PostgreSQL unavailable at AccountServer start** | `DbClient.Load` opens a connection and runs Schema.sql without try/catch (`DbClient.cs:27-35`) inside `Main` (`AccountServer/Program.cs:59`) -> process exits (runtime UNVERIFIED). |
| **PostgreSQL unavailable while running** | HTTP: exception -> `<Error>Internal error.</Error>` (`AccountServer/Program.cs:219-223`). RPC: exception propagates to GameServer. `SaveCharacter` retried every 5 s forever (`CharacterSaver.cs:117-125`); `RecordDeath` 6 tries then dropped (`PlayerDeath.cs:102-114`); campsite save logged and **not retried** (`Campsite.cs:359-366`); gift chest save retried on next check (`Campsite.cs:235-249`); logins fail (Hello faults -> disconnect, `NetworkHandler.cs:179-181`); `DbWriter<Account>` batches (create/delete/buy slot/ban-lapse) **silently dropped** with a console line (`DbWriter.cs:67-70`) after the caller was told success. |
| **Redis unavailable** | Start: AccountServer exits (`AccountLockManager.cs:42`). Running: game logins fail (lock script throws); HTTP and saves unaffected (no Redis in those paths); lock release on GameServer disconnect fails and is logged (`IpcServer.cs:78-83`). See `Redis.md` section 3. |
| **Save fails** | Character: retried in memory every 5 s; lost if the GameServer process ends first (shutdown waits max 8 s, `GameLogic.cs:139-148`). "0 rows updated" is treated as success (no retry, only a Warn on the AccountServer, `CharacterDb.cs:95-96`). |
| **Load fails** | `GetCharacter` null/exception -> failure packet + disconnect (`Load.cs:32-38`); `Hello` verify exception -> handler fault -> disconnect. No retry, no partial state. |
| **Session lock fails** | `VerifyAccount` returns `AccountInUse` (Acc = null) -> client gets `Failure.DEFAULT` with the description (`Hello.cs:58-61`; the ACCOUNT_IN_USE branch `:63-66` is unreachable). Dead-owner takeover if the owning GameServer is disconnected (`DbClient.cs:315-325`). |
| **Disconnect during save** | Player disconnect: `Unload` snapshots and queues the save before the entity leaves the world (`GameInfo.cs:82-95`), so the disconnect itself is safe. AccountServer RPC link drop during an in-flight `SaveCharacter`: exception -> retry; the write is idempotent (full replace of one element) so a duplicate apply is harmless. GameServer reconnects every 5 s (`GameServer/Program.cs:84-109`); calls made meanwhile throw (`Program.AccountServerRpc` still points at the dead proxy until reconnected). |
| **Server shutdown (graceful)** | GameServer: Ctrl+C/SIGTERM/ProcessExit -> `RequestStop` -> loop ends -> `ShutdownAsync`: gift chests (3 s cap), save + "server is restarting" to every user, wait up to 8 s for the save queue, log if not drained (`GameServer/Program.cs:68-79`, `GameLogic.cs:110-153`); ProcessExit waits 6 s. Campsite chests NOT flushed; pending death records not awaited. AccountServer: stop listener, flush `DbWriter`s, dispose data source (`AccountServer/Program.cs:101-136`); ProcessExit waits 4 s. If the AccountServer stops before the GameServer, the GameServer's final saves fail and are lost when it exits. |
| **Crash (GameServer)** | Lost: up to 60 s of character progress (since last autosave/unload), pending save/death queues, campsite changes < 3 s old. Redis locks of that server are released when its RPC TCP connection drops (`IpcServer.cs:71-84`); otherwise taken over at next login. Item duplication possible via chest/character split writes (section 4). |
| **Crash (AccountServer)** | Lost: queued `DbWriter<Account>` items (character creations / deletions / slot purchases already acknowledged). In-memory limiters, music state, caches reset. GameServers' saves fail and retry until it is back; deaths older than ~30 s are dropped. Locks stay in Redis; sweep after restart releases those of GameServers that do not reconnect within 10 s. |
| **Reconnect (player)** | World switch = `ReconnectTo` -> `Unload(true)` (save queued) -> client reconnects with the same `User` in `Reconnecting` state, which skips `VerifyAccount` and `GetCharacter` and reuses the in-memory account/char (`Hello.cs:52`, `Load.cs:31`). Full re-login after disconnect: same GameServer -> lock re-entrant, passes; the character is re-read from DB, so if the last save had not committed yet, the player sees older state (no "wait for pending save" before load). |
| **Reconnect (GameServer <-> AccountServer)** | Retry every 5 s, `GameServerConnected` re-registers; locks not re-asserted (see `Redis.md` R3). |

---

## 7. New architecture (RECOMMENDATION)

1. **Ownership**: the C++ GameServer is authoritative for live session data (character snapshot, inventory, campsite + gift chest while loaded, bounty progress). The Account/API service (C#) owns accounts, credentials, currencies, purchases, mail, moderation. PostgreSQL is the single source of truth; Redis holds only leases/tickets/limits.
2. **Save path**: GameServer -> internal API (gRPC or HTTP/JSON over mTLS) or direct SQL (see `AccountServer.md` section 12, Option A/B). Every save carries `(accountId, charId, sessionId, saveSeq)`; the DB rejects a save with `saveSeq` lower than the stored one (monotonic version column) so late/out-of-order saves can never roll state back.
3. **Atomic cross-container moves**: write character + campsite chest (+ gift chest) in **one transaction** per save ("session save" = character row + chests rows), instead of separate RPCs at different times. This removes the duplication/loss window.
4. **Durable outbox for deaths and payouts**: death, bounty and other one-shot events get an idempotency key and are persisted locally (or retried until acknowledged) and applied server-side exactly once (`INSERT ... ON CONFLICT DO NOTHING` on an events table, then apply). Never "give back" a claim on an unknown outcome - re-query instead.
5. **Autosave**: keep 60 s (or 30 s) plus save-on-leave; additionally save immediately after high-value events (rare drop, trade, purchase).
6. **Shutdown**: drain order = stop accepting -> save all sessions (chests + characters in one TX each) -> await acknowledgements with a deadline -> release leases. The API must stay up until GameServers finish (deploy order / health checks).
7. **Crash safety**: Redis lease TTL (30 s) frees accounts automatically; a re-login waits until the previous session's lease is gone **and** its last save is committed (lease released only after final save ack).
8. **Failure policy**:
   - PostgreSQL down: API returns 503; GameServer keeps sessions running, buffers latest snapshot per character in memory (and optionally a local append-only journal file for crash recovery), refuses new logins, alerts.
   - Redis down: refuse new logins (cannot guarantee single session), keep existing sessions and saves running.
   - Save fails: retry with backoff, surface "0 rows" as an error (not success), alert after N failures; on persistent failure, kick the player with a message rather than letting progress accumulate unsaved.
   - Load fails: return a clear error to the client; never create a default character.
9. **No whole-document writes** anywhere; no fire-and-forget batch writer.

Implementation status: NOT STARTED.

### Differences / Reason for differences
- Single-transaction session saves and save sequence numbers replace independent RPCs and "newest snapshot wins in memory" - required to eliminate the dupe/loss windows identified above.
- Outbox/idempotency for deaths and payouts replaces the 6-try in-memory retry and the bounty "give back" logic.
- Lease-based locks replace release-on-RPC-disconnect (see `Redis.md`).

---

## 8. Tests (reference)
- `GameServer.Tests/Persistence/CharacterSnapshotTests.cs` (snapshot capture/clone), `GameServer.Tests/Combat/PlayerDeathTests.cs`, `GameServer.Tests/Worlds/CampsiteSwapTests.cs`, `CampsiteWorldTests.cs`, `CampsiteRulesTests.cs`, `GiftChestTests.cs` (in-memory `IGiftChestStore`), `Session/StarterRewardTests.cs`, `GameThreadSyncContextTests.cs` (shutdown drain, per `Docs/EngineeringAudit.md:725`).
- `Common.Tests/CharacterDbTests.cs`, `CampsiteDbTests.cs`, `GiftChestRulesTests.cs`, `FameRulesTests.cs` (incl. `DeathDb.Apply`).
- Not covered by any automated test: RPC transport, any SQL statement, Redis locks, crash/restart scenarios, DbWriter.
- Manual procedures: `Docs/EngineeringAudit.md:704-737` (save on exit / relog), `Tools/E2E/*.py` (live servers; not run).

---

## 9. Known issues (summary)
1. WHOLE-account writes (`Hello` ban-lapse `FlushAccount`, `CreateCharacter`, `/char/delete`, `/account/purchaseCharSlot`) can roll back concurrent targeted writes; they go through `DbWriter`, acknowledged before commit, dropped on failure.
2. A character created via a failed DbWriter batch is played but never saved (`CharacterDb` finds no array slot, reports nothing to the GameServer).
3. Death records can be lost (6 in-memory retries).
4. Campsite chest saves are not retried, not flushed at shutdown, skipped after world deletion; chest/character writes are not atomic (item dupe/loss window).
5. Bounty payout can double-pay (non-idempotent RPC + give-back on error; claim flag persisted later by character save).
6. Same account can play twice on the same GameServer (lock is per server) -> two sessions saving the same character (last writer wins; dupe vector).
7. Deleting a character while it is online is reverted by the next save (`CharacterDb.Clean` copies `IsDeleted`).
8. Up to 60 s of progress lost on GameServer crash; up to 8 s shutdown budget may not drain saves if the AccountServer is slow/down.

---

## 10. Discrepancies

| # | Doc says | Code does |
|---|---|---|
| S1 | `Docs/EngineeringAudit.md:102-103,275-277` (F37): "There is no character save anywhere in the game server" | **No longer true.** `CharacterSaver` + RPC `SaveCharacter` + `CharacterDb.SaveAsync` exist (`CharacterSaver.cs:48-133`, `Proxies.cs:72`, `CharacterDb.cs:72-98`); the audit's own change log (`:662-701`) records the fix, but sections 1.4 / F37 were not updated |
| S2 | `Docs/EngineeringAudit.md:102`: GameServer persists via "`SaveVaultChests`" | RPC is now `SaveCampsiteChests` (`Proxies.cs:63`); stored JSON key is still `VaultChests` |
| S3 | `Docs/EngineeringAudit.md:947` / `WaW-Server/AGENTS.md:42-44`: GameServer never writes the whole account except `Create` (F56) | `Hello.cs:86-87` also sends `FlushAccount(acc)` (whole document) when a ban has expired |
| S4 | `Docs/EngineeringAudit.md:105` "Shutdown: none" / F34 | Graceful shutdown exists in both servers (`GameServer/Program.cs:68-79`, `GameLogic.cs:110-153`, `AccountServer/Program.cs:101-136`); campsite chests and pending death records are not part of it |
| S5 | `WaW-Server/AGENTS.md:35-36` "ShutdownAsync saves every character, disconnects everyone, drains the save queue" | True, with an 8 s cap; it also saves gift chests first, but not campsite chests (`GameLogic.cs:110-153`) |
| S6 | `CLAUDE.md:528-531`: whole-account saves "from Hello / Create / buy slot" could bring gift items back (hence own table) | Accurate; note `/char/delete` (`DbClient.cs:414-435`) is a fourth whole-account writer not listed there |
| S7 | `Common/Messaging/Proxies.cs:71` "called by the game server on disconnect, world switch, autosave and shutdown" | Accurate, plus starter claim, cat petting and bounty claim (`ClaimStarter.cs:95`, `CatCare.cs:80`, `BountyBoard.cs:110`) |
| S8 | `CharacterDb.cs:13` / audit C4 "equipment, potions, HP were lost on disconnect" (historical) | Fixed; but up to 60 s is still lost on crash |
