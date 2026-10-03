# Redis - Audit (migration phase: AUDIT)

Paths relative to REF. Redis was not connected to or inspected; everything is from code.

## 0. Inventory - every Redis touch point in the codebase

Search: `redis|IDatabase|ConnectionMultiplexer|memurai` (case-insensitive) over all `.cs/.csproj/.xml/.ps1/.sh/.json` files, and a filename-level search over every file, excluding bin/obj/dist/Archive/node_modules. Results:

| File | What |
|---|---|
| `WaW-Server/Common/Common.csproj:30` | `StackExchange.Redis` 2.8.31 package |
| `WaW-Server/Common/Resources/Config/RedisConfig.cs:10-25` | config class: Host, Port -> connection string `"{Host}:{Port}"` (no password, no TLS, no db index) |
| `WaW-Server/Common/Resources/Config/Data/redisConfig.xml` | `localhost:6379` (no secret in it); machine-owned on the VPS, never overwritten by deploy (`deploy.ps1:241`, `VPS_SETUP.md:144`) |
| `WaW-Server/Common/Database/DbClient.cs:37` | `AccountLockManager.Init(RedisConfig.Config.ConnectionString)` (AccountServer start) |
| `WaW-Server/Common/Database/DbClient.cs:311-325` | the only Redis use in a request path (lock acquire inside `VerifyAccount`) |
| `WaW-Server/Common/Database/AccountLockManager.cs` (whole file) | **the only class that issues Redis commands** |
| `AccountServer/Messaging/AccServerRpcHandler.cs:26`, `AccountServer/Program.cs:150-158` | callers of release / stale sweep |
| `deploy.ps1:716,860`, `VPS_SETUP.md:15-22,66` | `redis-server` systemd service; the game service starts `After=... redis-server.service` |

No other component uses Redis: no rate limits, no caches, no pub/sub, no sessions, no queues, no leaderboards. The GameServer has no Redis client at all (it reaches the locks only through the `VerifyAccount` RPC). Portal, Forums (NodeBB), WebClient bridge, Launcher, Developer Dashboard: no Redis reference found (NodeBB's own datastore is out of scope; UNVERIFIED whether NodeBB is configured on the same Redis instance - `deploy.ps1` lists a `ww-forums` service, its config was not inspected).

**Conclusion: `Docs/EngineeringAudit.md:121` "Redis (Memurai locally) only holds account locks" is still TRUE.**

---

## 1. Operations (complete list)

All in `Common/Database/AccountLockManager.cs`. Process: AccountServer only. Database index: default 0.

| # | Operation | Redis command(s) | Key | Value | TTL | Line | Called from |
|---|---|---|---|---|---|---|---|
| 1 | Connect | `ConnectionMultiplexer.Connect(options)`; `SyncTimeout`/`AsyncTimeout` forced to 15 000 ms unless set in the connection string | - | - | - | 36-44 | `DbClient.Load` (`DbClient.cs:37`) |
| 2 | TryAcquire | `EVAL` Lua: `GET k; if nil or == ARGV[1] then SET k ARGV[1]; return 1 else return 0` | `account_lock:{accountId}` | GameServer GUID string (`Guid.ToString()`, "D" format) | **none** (plain SET) | 24-30, 46-55 | `DbClient.VerifyAccount` when `gameServerGuid != Guid.Empty` (only RPC `VerifyAccount` from GameServer `Hello.cs:53`) |
| 3 | Track lock per server | `SADD` (separate round trip, only if #2 returned 1) | `gameserver_locks:{serverGuid}` | member = accountId (int) | none | 51-52 | same |
| 4 | GetOwner | `GET` | `account_lock:{accountId}` | GUID | - | 57-60 | `DbClient.cs:316` when acquire failed |
| 5 | Release one | `DEL` | `account_lock:{accountId}` | - | - | 62-64 | `DbClient.cs:322` (dead-owner takeover only). **The set entry in `gameserver_locks:{oldGuid}` is NOT removed** |
| 6 | Release all of a server | `SMEMBERS` then one `DEL` per account, then `DEL` of the set (not atomic, not pipelined) | `gameserver_locks:{guid}`, `account_lock:*` | - | - | 68-74 | `AccServerRpcHandler.Close()` (`:24-27`) when the GameServer's RPC connection ends (`IpcServer.cs:77-84`); and #7 |
| 7 | Stale sweep | `SCAN` (via `server.KeysAsync(pattern: "gameserver_locks:*")`) on the **first endpoint only**, then #6 for every GUID not in the live set | `gameserver_locks:*` | - | - | 79-90 | `AccountServer/Program.cs:150-158`, 10 s after start |

Key formats: `account_lock:{int}` and `gameserver_locks:{Guid}` (`AccountLockManager.cs:17-18`). Value formats: GUID text; set members are integer account ids stored as Redis strings.

---

## 2. Account lock semantics (exact)

- **Purpose**: one live game session per account across GameServer instances; replaces the old LiteDB `Account.LockOwner` field (`AccountLockManager.cs:9-12`).
- **Owner** = a GameServer process GUID (`GameServer/Program.cs:20`, `Guid.NewGuid()` per process start). The lock is per *server*, not per *connection/session*.
- **Acquire**: at GameServer `Hello` (non-reconnect) via RPC `VerifyAccount(username, password, Program.Guid)` (`Hello.cs:52-53`). HTTP endpoints pass `Guid.Empty` and never touch Redis (`DbClient.cs:306-311`).
- **Re-entrant per server**: the same GUID re-acquires successfully (`AcquireScript` `current == ARGV[1]`). Consequence: **two simultaneous sessions of the same account on the same GameServer are allowed** (no in-process duplicate-login check was found in `GameServer` - searched for account-id comparisons in session code). This is the open audit item F40/M8 (`Docs/EngineeringAudit.md:280-281,470-471`).
- **Contention**: a different GUID holds it -> `GetOwner`; if that owner GUID is not in `IpcServer.Clients` (not connected to this AccountServer), the lock is deleted and re-acquired ("dead-owner takeover", `DbClient.cs:315-325`); else `AccountInUse`.
- **Release**: never per player disconnect, logout, death or world change (no caller of `ReleaseAsync` besides takeover). Released only:
  1. when that GameServer's RPC connection to the AccountServer ends (clean exit, crash, network drop) - all its locks (`IpcServer.cs:71-84`);
  2. by the start-up sweep of the AccountServer for GUIDs that have not reconnected within 10 s;
  3. lazily, per account, by dead-owner takeover.
- **AccountServer restart**: Redis keeps the keys. GameServers reconnect every 5 s (`GameServer/Program.cs:84-109`) with the same GUID and call `GameServerConnected`; they do not re-acquire anything. If a GameServer has not reconnected within the 10 s sweep window, all of its locks are deleted although its players are still online -> a second login elsewhere would be accepted. With a single GameServer this has no visible effect.
- **No TTL / heartbeat**: a lock lives until one of the releases above. A GameServer whose RPC link is alive but whose game loop is hung keeps all its locks.
- **Non-atomic bookkeeping**: acquire (EVAL) and SADD are two calls (`:47-52`); a failure between them leaves an `account_lock` key that no sweep can find (it is only reachable through takeover, which requires the owner GUID to be disconnected). Takeover deletes `account_lock` but leaves the account id in the old server's set; a later sweep of that old set would `DEL` the *new* owner's lock (`ReleaseAllForServerAsync` deletes `account_lock:{id}` unconditionally, without checking the value) - edge case, UNVERIFIED in practice.
- **Sweep scope**: `GetEndPoints()[0]` only (`:81`) - fine for a single Redis node, wrong for a cluster/replica set.

---

## 3. Failure behaviour (what the reference does today)

| Situation | Behaviour | Evidence |
|---|---|---|
| Redis down at AccountServer start | `ConnectionMultiplexer.Connect` throws (default `abortConnect=true`) inside `DbClient.Load` -> AccountServer exits | `DbClient.cs:37`, `AccountLockManager.cs:42`; runtime UNVERIFIED |
| Redis down while running, HTTP routes | Unaffected (Guid.Empty path never uses Redis) | `DbClient.cs:311` |
| Redis down while running, game login | `ScriptEvaluateAsync` throws (after up to 15 s timeout) -> RPC `VerifyAccount` fails -> GameServer `Hello` handler faults -> user disconnected with "Internal error handling packet" | `AccountLockManager.cs:34,47`, `GameServer/Game/Network/NetworkHandler.cs:179-181` |
| Redis down during GameServer disconnect | `Close()` throws, logged "Releasing locks ... failed"; locks remain until sweep/takeover | `IpcServer.cs:78-83` |
| Redis restarted without persistence | All locks vanish -> duplicate logins possible until each account logs in again (fail-open). Redis persistence settings on the VPS: UNVERIFIED | - |
| Slow Redis | 15 s timeouts chosen deliberately after a 2-core laptop stall (`AccountLockManager.cs:32-34`) | |

---

## 4. Tests
None. No test references `AccountLockManager` or Redis. The behaviour was checked manually per `Docs/EngineeringAudit.md:728-737` (Redis showed `account_lock:10` held after a GameServer exit - fixed by the `finally` in `IpcServer`).

---

## 5. New architecture (RECOMMENDATION)

Keep Redis, but give it a precise, small role:

1. **Session lock with lease** (replaces account_lock):
   - Key `session:account:{accountId}` = `{gameServerId}:{sessionId}` set with `SET key value NX PX 30000`; the owning C++ GameServer renews every 10 s (`PEXPIRE` via a compare-value Lua script) for every live session, and deletes it (compare-and-delete Lua) on logout/disconnect after the final save is acknowledged.
   - Crash => lock expires within 30 s without any sweep. No `gameserver_locks:*` sets needed (or keep a set only for observability).
   - The lock should be per **session** (sessionId) so a second login on the same server is detected; policy on conflict: kick the old session (RotMG style) after its save completes, or refuse - product decision.
   - Who talks to Redis: either the Account/API service issues the lock when it redeems a join ticket, or the C++ GameServer acquires it directly (hiredis/redis-plus-plus). Recommendation: the **GameServer owns acquire/renew/release** (it knows when the session really ends); the API only reads it (to refuse character delete / purchases that conflict with a live session).
   - Save ordering: release the lock only after the final character save is durably committed, so a fast re-login on another server never loads stale data.
2. **Join tickets**: `ticket:{random}` -> `{accountId, charId, serverId}` with `PX 60000`, `GETDEL` on redemption (one-time).
3. **Shared rate limits** (replaces in-memory `AttemptLimiter` when more than one API instance runs): sliding-window counters `rl:{scope}:{key}` with TTL = window.
4. Optional: presence (`presence:{accountId}` -> world, TTL renewed) so `/public/player` and `/dev/player` stop fanning out RPC to every GameServer; short-lived caches for leaderboards.
5. **Never** store durable game data in Redis (characters, balances, chests) - PostgreSQL stays the source of truth. Configure Redis with a password (`requirepass`/ACL user) bound to localhost, AOF optional (all keys are reconstructible/expiring).
6. Failure policy: if Redis is unavailable, **refuse new game logins** (fail closed) but keep existing sessions playing and saving; HTTP routes that do not need locks keep working.

Implementation status: NOT STARTED.

### Differences / Reason for differences
- TTL + heartbeat instead of release-on-RPC-disconnect: the C++ GameServer will not have a persistent StreamJsonRpc channel whose drop implies "all sessions gone"; leases are the standard, crash-safe pattern.
- Per-session instead of per-server ownership: fixes the same-server duplicate-login gap (F40).

---

## 6. Known issues (reference)
1. Same account can be logged in twice on the same GameServer.
2. No TTL; hung-but-connected GameServer holds locks forever.
3. Non-atomic acquire + SADD; takeover leaves stale set members; release does not compare owner.
4. `ReleaseStaleLocksAsync` assumes one Redis endpoint.
5. No auth/TLS options in `RedisConfig`.
6. AccountServer cannot start without Redis even though only game login needs it.

---

## 7. Discrepancies

| # | Doc says | Code does |
|---|---|---|
| R1 | `Docs/EngineeringAudit.md:121` "Redis only holds account locks" | **Still true** (verified: only `AccountLockManager` issues commands) |
| R2 | `Docs/EngineeringAudit.md:100-101` locks "released only for stale servers, never per disconnect" | Still true per *player* disconnect; since the 2026-09-21 fix, all of a server's locks are also released when its RPC connection ends (`IpcServer.cs:71-84`). `WaW-Server/AGENTS.md:49-50` describes this correctly |
| R3 | `AccountServer/Program.cs:146-149` "reconnect and reassert their locks" | GameServers do not reassert locks on reconnect (`GameServer/Program.cs:95-102` only calls `GameServerConnected`) |
| R4 | `AccountLockManager.cs:20-23` "two simultaneous logins for the same account can't both win" | True only across different GameServer GUIDs; two logins on the same server both "win" (re-entrant script) |
| R5 | `Docs/EngineeringAudit.md:267-268` (F34) "Redis locks stay until the next start's stale-lock sweep" | Fixed: released in `IpcServer` `finally` when the GameServer connection ends |

---

## New key contract (implemented, 2026-10-02)

Shared by the Account/API service (`AccountService/src/WaW.AccountService/Sessions/RedisSessionStore.cs`) and the C++ GameServer
(`Server/libs/persistence/src/account_sessions.cpp`). `{p}` = the configured prefix (`waw:` by default; tests use unique prefixes).
Tokens and tickets are 32 random bytes, base64url; only their lower-case hex SHA-256 is ever used as a key (test vector "abc" in both suites).

| Key | Value | TTL | Written by | Read / removed by |
|---|---|---|---|---|
| `{p}session:{sha256(token)}` | `{"accountId":N,"name":"...","rank":N}` | `Service:SessionHours` (12 h) | API on login | API on every bearer request; deleted on logout |
| `{p}join:{sha256(ticket)}` | same JSON | `Service:JoinTicketSeconds` (60 s) | API `POST /api/v1/game/join` | GameServer `GETDEL` at Hello (single use) |
| `{p}lock:account:{accountId}` | `{serverId}/{sessionId}` | 30 s, refreshed every 10 s | GameServer `SET NX PX` after a valid ticket | GameServer heartbeat / release via owner-checked Lua scripts; API checks existence before deleting a character |

Failure behaviour: Redis down -> no new logins (`ServiceUnavailable`, fail closed); players already in game keep playing (a failed heartbeat
is logged and retried); a lock lost to another owner kicks the session. A crashed server's locks expire within 30 s.
Implementation status: COMPLETE (MigrationStatus.md row 8).
