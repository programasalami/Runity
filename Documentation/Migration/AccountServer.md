# AccountServer - Audit (migration phase: AUDIT)

Scope: `WaW-Server/AccountServer` (all), the parts of `WaW-Server/Common` it runs (`Database/*`, `Messaging/*`, `Resources/Config/*`), and the client callers (`WaW-Client/WaWClient/AppEngine`, `Launcher`, `DeveloperDashboard`, `Portal`).
All paths are relative to REF = `Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing`. Line numbers are from the files as read on 2026-10-02.
Related docs: `PostgreSQL.md`, `Redis.md`, `Persistence.md` in this folder.

Legend: **UNVERIFIED** = not confirmed by reading code (or depends on runtime/deploy state that cannot be checked without running anything).

---

## 1. Startup and process model

### Reference behavior
One .NET 10 console process (`AccountServer.csproj` targets `net10.0`, depends on `Common`, `Newtonsoft.Json`, `MinVer`). It is both:
1. the public HTTP API (port 8080) used by the game client, browser client (through nginx `/api`), launcher, Developer Dashboard and the Portal website, and
2. the private RPC hub (port 8081, TLS + shared secret) that every GameServer connects to. The GameServer never opens PostgreSQL itself; every database read/write it needs goes through this hub.

### Current implementation (`WaW-Server/AccountServer/Program.cs`)
| Step | Line | What |
|---|---|---|
| ThreadPool | 35 | `ThreadPool.SetMinThreads(1000, 1000)` (see Discrepancies: the audit says this was removed) |
| Owner settings | 46-48 | `OwnerSettings.SeedAll()` copies missing owner-tunable files into the settings folder (`WAW_SETTINGS_DIR` or `/opt/WaW/settings` on Linux) |
| Load | 54-56 | `EnumUtils.Load()`, `RequestHandler.Load()` (reflection: every non-abstract `RequestHandler` subclass, keyed by `Path`, `Systems/RequestHandler.cs:33-42`), `XmlLibrary.Load(config.XmlsDir)` (game XMLs - needed for skin prices, class start stats, item names) |
| RPC hub | 58 | `_ = IpcServer.StartAsync<AccServerRpcHandler>()` (fire-and-forget; an exception here is unobserved) |
| Database | 59 | `DbClient.Load()` - opens the Npgsql data source, runs `Database/Schema.sql` synchronously on every start, connects Redis, starts the five `DbWriter<T>` workers (`Common/Database/DbClient.cs:27-44`) |
| Locks | 61 | `ReleaseLocks()` -> after 10 s, release Redis locks of GameServers that did not reconnect (`Program.cs:150-158`) |
| HTTP | 63-64 | `HttpListener` prefix `http://{Address}:{Port}/` |
| Accept loop | 67-94 | `SemaphoreSlim(MaxConcurrentRequests)`; every request is dispatched to `Task.Run` and waits for a slot inside the task |
| Shutdown | 101-136 | Ctrl+C / SIGTERM / ProcessExit -> stop listener -> `DbClient.Dispose()` flushes the `DbWriter` queues -> `Logger.Flush` |

If PostgreSQL or Redis is unreachable at start, `DbClient.Load()` throws inside `Main` (no catch) and the process exits (Npgsql `OpenConnection`, StackExchange.Redis `Connect` with default `abortConnect=true`). UNVERIFIED at runtime (not run, per instructions).

### Config files (`Common/Resources/Config/Data/`)
| File | Class | Keys | Notes |
|---|---|---|---|
| `appEngineConfig.xml` | `AppEngineConfig.cs` | XmlsDir, WorldsDir, Port (8080), Address (127.0.0.1 in source), MaxConcurrentRequests (64; default 200 if missing), DownloadUrl | `deploy` rewrites the address for the VPS (`CLAUDE.md:65-71`) |
| `postgresConfig.xml` | `PostgresConfig.cs` | Host, Port, Database, Username, Password -> plain `Host=..;Password=..` connection string (`PostgresConfig.cs:30-31`) | **Holds a real secret.** git-ignored; `.example.xml` has placeholders. Not copied here. |
| `redisConfig.xml` | `RedisConfig.cs` | Host, Port -> `"{Host}:{Port}"` | No password/TLS support in the config class (`RedisConfig.cs:13-24`) |
| `rpcServerConfig.xml` | `RpcServerConfig.cs` | ListenAddress, ListenPort (8081), CertificatePfxPath, CertificatePassword, SharedSecret | **Holds secrets.** git-ignored. The local copy listens on `0.0.0.0`; the example says `127.0.0.1` |
| `rpcClientConfig.xml` | `RpcClientConfig.cs` | ServerHost, ServerPort, TrustedCertPath, SharedSecret | **Holds a secret.** GameServer side |
| `newAccountsConfig.xml` | `NewAccountsConfig.cs` | Fame, Credits, MaxChars, CampsiteCount (fallback VaultCount), CharSlotCost, CampsiteSlotCost (fallback VaultSlotCost) | Owner setting `new-accounts.xml`, hot-reloaded. Source default: 100000 fame / 100000 gold (test values) |
| `newCharsConfig.xml` | `NewCharsConfig.cs` | Experience, Level, Fame, Tex1, Tex2, HealthPotions, MagicPotions, HasBackpack | Owner setting `new-characters.xml` |
| `gameServerConfig.xml` / `serverSettings.xml` | `GameServerConfig.cs` | Version, ServerName, Address, Port, MaxPlayers, AdminOnly | AccountServer reads it for `/app/version`, `/char/list` server list, `/public/*` version |
| `gameConfig.xml` | `GameConfig.cs` | StarGoals | Owner setting `game.xml` |
| `newsConfig.xml`, `musicConfig.xml` | `NewsConfig.cs`, `MusicConfig.cs` | news items in `/char/list`; music library | |

`ConfigLoader<T>` (`ConfigLoader.cs:22-56`) caches each config; owner settings (`OwnerSettings.All`, `OwnerSettings.cs:22-28`: daily-rewards.txt, new-accounts.xml, new-characters.xml, game.xml, server.xml) are re-checked every 2 s and hot-reloaded; a broken file keeps the last good values. Non-owner configs are read once.

### New architecture (RECOMMENDATION)
- Keep a separate **Account/API service** (see section 12). Replace `HttpListener` + reflection routing with ASP.NET Core minimal APIs/controllers (Kestrel), real middleware for rate limiting (`Microsoft.AspNetCore.RateLimiting`), forwarded-headers, CORS for `/public/*`, health checks.
- Configuration via `appsettings.json` + environment variables / secret store; never secrets in XML committed next to code. Keep the owner-settings hot-reload idea via `IOptionsMonitor`.
- Schema migrations run by a migration tool (see `PostgreSQL.md`), not `Schema.sql` on every start.
- Start-up must tolerate PostgreSQL/Redis being briefly unavailable (retry with backoff, report unhealthy) instead of crashing.

### Implementation status
NOT STARTED.

### Differences / Reason for differences
Framework swap (HttpListener -> ASP.NET Core) for standard middleware, TLS termination options, testability. Behaviour of every route should be preserved 1:1 during the first port (wire compatibility with existing clients is not required for Unity but the semantics are).

### Tests
None for `Program.cs`. `OwnerSettingsTests.cs` (Common.Tests) covers OwnerSettings seeding/resolution.

### Known issues
- `IpcServer.StartAsync` is fire-and-forget (`Program.cs:58`): a bind failure on 8081 is never surfaced; the HTTP side keeps running without any GameServer able to connect.
- Schema DDL + a data `UPDATE` (`Schema.sql:98-99`) run on every start.

---

## 2. HTTP layer

### Reference behavior / Current implementation
- Plain HTTP (`Program.cs:63`). On the VPS, nginx terminates TLS for the browser client (`/api/...`) and the Portal (`/api/public/...`) and forwards to 8080 (`Program.cs:244-246`, `VPS_SETUP.md:149-152`). **The desktop game client talks plain `http://<VPS address>:8080` directly** (`WaW-Client/WaWClient/Core/Settings.cs:35-41`); port 8080 is opened in the firewall (`VPS_SETUP.md:4,54`). Credentials therefore travel unencrypted from desktop clients.
- Routing: exact match on `Url.LocalPath` (`Program.cs:161-172`). Unknown path -> connection closed with no body (logged at Warn).
- Body: declared `Content-Length` > 64 KiB -> 413; body read as UTF-8 up to 64 KiB, else 413 (`Program.cs:176-199`).
- Parameters: form body parsed with `HttpUtility.ParseQueryString`; URL query string merged in for keys not already present (`Program.cs:201-209`). So every route accepts both POST form and GET query (passwords in URLs end up in proxy logs if a client ever uses GET - UNVERIFIED whether any does; the game client always POSTs, `AppEngineClient.cs:40`).
- Response: handler string; `Content-Type` = `application/json; charset=utf-8` if the body starts with `{`/`[`, otherwise the literal `text/*` (`Program.cs:228-229`). HTTP status is always 200 (except 413); errors are signalled in the body (`<Error>msg</Error>` for XML routes, `{"error":"..."}` for JSON routes).
- CORS: `Access-Control-Allow-Origin: *` only for `/public/*` (`Program.cs:232-233`).
- Client IP: `X-Forwarded-For` first entry is trusted only when the TCP peer is loopback or one of this host's addresses (`Program.cs:247-260`).
- Unhandled handler exception -> `<Error>Internal error.</Error>` (`Program.cs:219-223`).
- Concurrency: at most `MaxConcurrentRequests` (64) handlers at once; extra requests queue unbounded in `Task.Run` (`Program.cs:85-93`).

### Known issues
- No HTTPS for desktop clients; no HSTS; `text/*` is not a valid MIME type.
- Unknown routes get no HTTP response body/status (just `Close()`).
- `Dns.GetHostAddresses` is called per request that carries `X-Forwarded-For` (`Program.cs:258`).

---

## 3. Route table (every `RequestHandler` found)

45 live routes (3 guild routes are commented out). Auth column: **creds** = `username` + `password` form fields, where `password` may be a real password or a launcher token `waw-token:...` (both go through `DbClient.VerifyAccount(..., Guid.Empty)`, which never touches Redis). No route issues or accepts a session cookie/bearer token - credentials are re-verified (PBKDF2, 100k rounds) on every authenticated call.

Rate-limit legend: **A** = per-account-name failed-login limiter, 10 failures / 10 min (`DbClient.cs:23`, applies inside every `VerifyAccount`); **IPv** = per-IP failed-verify limiter 20 / 10 min (`LoginGuards.cs:10`); **IPr** = per-IP successful-registration limiter 5 / hour (`LoginGuards.cs:13`); **PUB** = 240 req/min/IP (`PublicHandlers.cs:17`); **DEV** = 600 req/min/IP (`DevHandlers.cs:26`). All limiters are in-process memory (`AttemptLimiter.cs`) - reset on restart, not shared across instances.

| # | Method | Path | Params | Auth | Response | DB / RPC calls | Rate limits | Handler |
|---|---|---|---|---|---|---|---|---|
| 1 | POST/GET | `/account/verify` | username, password | creds | `<Account>` XML (`ModelUtils.cs:13-27`: AccountId, Rank, Name, Guild, Admin, StarterPending, Stats) or `<Error>` | `logins` SELECT, maybe hash UPDATE, `accounts` SELECT | A, IPv | `Systems/Account/Verify.cs:17-30` |
| 2 | POST | `/account/register` | newUsername, newPassword | none | `<Success/>` / `<Error>` | `logins` EXISTS, COUNT by ip, INSERT accounts (placeholder + UPDATE), INSERT logins, INSERT inbox welcome mail (500 gold); all on one connection, **no transaction** | IPr; max 10 logins per IP in DB (loopback exempt) `DbClient.cs:19,210-216` | `Register.cs:16-24`, `DbClient.RegisterAsync:192-261` |
| 3 | POST | `/account/remember` | username, password (real password only) | creds (password) | `<Remember><Name/><Token/></Remember>` | VerifyAccount + `login_tokens` INSERT + prune | IPv, A | `Remember.cs:16-27`, `DbClient.cs:169-180` |
| 4 | POST | `/account/forget` | username, token | none (token is the proof) | always `<Success/>` | `login_tokens` DELETE | none | `Forget.cs:12-15`, `DbClient.cs:183-190` |
| 5 | POST | `/account/purchaseCharSlot` | username, password | creds | `<Success/>` / `<Error>` | VerifyAccount, then in-memory `CurrentFame -= cost; MaxChars++` and **whole-account write via `DbWriter<Account>`** (async, not awaited to commit) | A | `PurchaseCharSlot.cs:17-29`, `DbClient.BuyCharSlotAsync:437-447` |
| 6 | POST | `/account/purchaseSkin` | username, password, skinType | creds | `<Success><Gold/><Fame/></Success>` / `<Error>` | `SkinDb.BuyAsync` - one conditional UPDATE (price from server XML) | A | `PurchaseSkin.cs:17-28`, `SkinDb.cs:24-51` |
| 7 | POST | `/char/list` | username, password | creds, **falls back to Guest** | `<Chars nextCharId maxNumChars charSlotCost>` + OwnedSkins + `<Account>` + living chars + news + `<Servers>` (`ModelUtils.cs:29-49`) | VerifyAccount | A | `Systems/Char/List.cs:16-21` (class is misnamed `ListMembers`) |
| 8 | POST | `/char/delete` | username, password, charId | creds | `<Success/>` / `<Error>` | re-read account, set `IsDeleted`, **whole-account write via DbWriter** | A | `Char/Delete.cs:16-31`, `DbClient.cs:414-435` |
| 9 | POST | `/char/chooseRole` | username, password, charId, role | creds | `<Success/>` / `<Error>` | `RoleDb.SetAsync` targeted jsonb_set, only if no role yet | A | `ChooseRole.cs:16-26`, `RoleDb.cs:14-37` |
| 10 | POST | `/char/fame` | (accountId, charId commented out) | none | always `<Error>No death info available</Error>` | none | none | `Char/Fame.cs:14-22` (stub) |
| 11 | POST | `/fame/list` | timespan | none | always `<Error>Invalid legends timespan</Error>` | none | none | `Legends/FameList.cs:13-18` (stub) |
| 12 | GET/POST | `/app/version` | - | none | `<Version downloadUrl="..">x.y.z</Version>` | none (config) | none | `App/Version.cs:18-24` |
| 13 | GET | `/crossdomain.xml` | - | none | file text | file read | none | `Crossdomain/Crossdomain.cs:18-23` (see Discrepancies: path mismatch) |
| 14 | POST | `/news/feed` | - | none | `<NewsFeed>text</NewsFeed>` from `Resources/News/News.txt` | none | none | `News/NewsScrollFeed.cs:13-14` |
| 15 | POST | `/board/list` | optional creds | optional | `<Board canModerate><Post id author status created>msg</Post>...` (50 newest) | `bug_posts` SELECT | A (if creds given) | `Board/BoardHandlers.cs:30-49` |
| 16 | POST | `/board/post` | creds, message | creds | `<Success/>`/`<Error>` | rate check (10/hour/account, `BugBoardRules.cs:15`) + INSERT | A, 10 posts/h | `BoardHandlers.cs:51-76` |
| 17 | POST | `/board/delete` | creds, id | Moderator+ (rank >= 80) | `<Success/>`/`<Error>` | DELETE | A | `BoardHandlers.cs:78-96` |
| 18 | POST | `/board/status` | creds, id, status (new/confirmed/fixed) | Moderator+ | `<Success/>`/`<Error>` | UPDATE | A | `BoardHandlers.cs:98-121` |
| 19 | POST | `/music/now` | - | none | `<Music serial changedBy positionMs current next><Track .../>` | none (in-memory `MusicDirector`) | none | `Music/MusicHandlers.cs:64-97` |
| 20 | POST | `/music/skip` | creds, dir=next/prev | creds | `<Success/>`/`<Error>` | in-memory | 10 s per account, 2 s global (`MusicDirector.cs:162-163`) | `MusicHandlers.cs:100-127` |
| 21 | POST | `/music/set` | creds, track | creds | `<Success/>`/`<Error>` | in-memory | same | `MusicHandlers.cs:130-150` |
| 22 | POST | `/inbox/list` | creds | creds | `<Inbox><Message id sender subject created read claimed gold fame items>body` (50 newest) | `inbox_messages` SELECT | A | `Rewards/RewardsHandlers.cs:32-58` |
| 23 | POST | `/inbox/read` | creds, id | creds | `<Success/>` | UPDATE is_read | A | `RewardsHandlers.cs:60-71` |
| 24 | POST | `/inbox/claim` | creds, id | creds | `<Claimed gold fame items balanceGold balanceFame/>` | one transaction: mark claimed + add gold/fame (jsonb) + queue items into `gift_items` | A | `RewardsHandlers.cs:73-91`, `RewardsDb.ClaimAsync:82-98` |
| 25 | POST | `/inbox/delete` | creds, id | creds | `<Success/>`/`<Error>` | DELETE only if nothing left to claim | A | `RewardsHandlers.cs:93-104` |
| 26 | POST | `/daily/status` | creds | creds | `<Daily giftReady giftDay streak spinReady unread secondsToGift secondsToSpin secondsToReset><Gift day gold fame items/>x7<Prize gold fame/>...` | `daily_rewards`, `inbox_messages` COUNT | A | `RewardsHandlers.cs:106-136` |
| 27 | POST | `/daily/claim` | creds | creds | `<Gift day streak gold fame items balanceGold balanceFame/>` | one transaction, `SELECT ... FOR UPDATE` on `daily_rewards`, add reward, optional inbox mail with items | A, 24 h cooldown | `RewardsHandlers.cs:138-156`, `RewardsDb.cs:121-141` |
| 28 | POST | `/daily/spin` | creds | creds | `<Spin index gold fame balanceGold balanceFame/>` | one transaction, server-side `RandomNumberGenerator` roll | A, 24 h cooldown | `RewardsHandlers.cs:158-176`, `RewardsDb.cs:144-160` |
| 29 | GET/POST | `/public/player` | name | none | JSON `Profile` (`PublicDb.cs:24-40`) incl. online/world via RPC `GetUserInfo` to every GameServer | `accounts` + `logins` SELECT, cached 60 s | PUB | `Public/PublicHandlers.cs:54-67` |
| 30 | GET/POST | `/public/search` | q (<=16 chars) | none | JSON string array (10) | ILIKE search, cached 60 s | PUB | `PublicHandlers.cs:69-77` |
| 31 | GET/POST | `/public/leaderboard` | kind = fame/chars/level/guilds | none | JSON rows (100) | see `PostgreSQL.md` (PublicDb) | PUB | `PublicHandlers.cs:79-88` |
| 32 | GET/POST | `/public/guild` | name | none | JSON `PublicGuild` | `guilds` + member accounts | PUB | `PublicHandlers.cs:90-101` |
| 33 | GET/POST | `/public/releases` | - | none | JSON `{version, releases[]}` (patch notes: DB + shipped file) | `patch_notes` SELECT (15 s cache in `NewsFeed`) | PUB | `PublicHandlers.cs:106-113` |
| 34 | GET/POST | `/public/online` | - | none | JSON `{online, version, starGoals}` | RPC `GetGameServer` to every GameServer, cached 60 s | PUB | `PublicHandlers.cs:115-121` |
| 35 | POST | `/dev/whoami` | creds (token) | Developer+ (>= 90) | JSON | VerifyAccount | DEV + IPv | `Dev/DevHandlers.cs:73-81` |
| 36 | POST | `/dev/status` | creds | Developer+ | JSON: AccountServer uptime/memory/recent problems + every GameServer `GetStatus` | RPC, `accounts` COUNT | DEV | `DevHandlers.cs:84-122` |
| 37 | POST | `/dev/players` | creds, q | Developer+ | JSON rows (25) | `accounts` LEFT JOIN `logins` | DEV | `DevHandlers.cs:125-134` |
| 38 | POST | `/dev/player` | creds, name | Developer+ | JSON full player (chars, bans, mutes, flags) | several SELECTs + RPC presence | DEV | `DevHandlers.cs:137-150` |
| 39 | POST | `/dev/moderate` | creds, action (kick/ban/unban/mute/unmute/setrank), target, reason, minutes, rank | Developer+, plus `AdminDb` rank checks (setrank = Owner) | JSON | `AdminDb.*` + RPC `ApplyModeration` | DEV | `DevHandlers.cs:155-207` |
| 40 | POST | `/dev/mail` | creds, target, subject, body, gold, fame | **Owner** | JSON | `AdminDb.MailAsync` -> inbox INSERT | DEV | `DevHandlers.cs:210-230` |
| 41 | POST | `/dev/anticheat` | creds, limit | Developer+ | JSON flags | `anticheat_flags` SELECT | DEV | `DevHandlers.cs:233-241` |
| 42 | POST | `/dev/news/list` | creds | Developer+ | JSON | `patch_notes` SELECT | DEV | `DevHandlers.cs:244-256` |
| 43 | POST | `/dev/news/post` | creds, version, title, author, text | Developer+ | JSON `{ok,id,message}` | INSERT `patch_notes` | DEV | `DevHandlers.cs:259-278` |
| 44 | POST | `/dev/news/delete` | creds, id | Developer+ | JSON | DELETE | DEV | `DevHandlers.cs:280-291` |
| 45 | POST | `/dev/weather` | creds, weather, time | Developer+ | JSON | RPC `GetWeather`/`SetWeather` to every GameServer | DEV | `DevHandlers.cs:295-325` |
| - | - | `/guild/getBoard`, `/guild/listMembers`, `/guild/setBoard` | - | - | not registered (whole files commented out) | - | - | `Systems/Guild/*.cs` |

### Callers (who calls what)
- Game client (`WaW-Client/WaWClient/AppEngine/AppRequests.cs`): `/account/verify`(65), `/account/register`(96), `/account/purchaseCharSlot`(117), `/account/purchaseSkin`(136), `/char/delete`(146), `/char/chooseRole`(150), `/char/list`(156), `/board/*`(183-197), `/inbox/*`(256-268), `/daily/*`(271-281), `/news/feed`(314), `/music/*`(332-344); `PortalRequests.cs:21-42` `/public/online|player|search|leaderboard|guild`; `VersionCheck.cs:41` `/app/version`. Every call is an HTTP POST form with a 10 s timeout and up to 3 retries (`AppEngineClient.cs:22-60`, `Settings.cs:43`). The client stores username + password (or launcher token) in memory (`LoginData`) and sends them with every request.
- Launcher: `/account/remember`, `/account/forget`, `/account/verify`, `/account/register` (grep of `Launcher/`).
- Developer Dashboard: `/account/remember` + all `/dev/*` (grep of `DeveloperDashboard/`).
- Portal website / patch-notes page: `/public/*` through nginx `/api/public/...`.
- Unused by any found caller: `/char/fame`, `/fame/list`, `/crossdomain.xml`.

---

## 4. Authentication

### Reference behavior / Current implementation
- **Username rules**: 1-10 letters only (`char.IsLetter`) (`DbClient.cs:158-160`). Stored lowercase in `logins.name`; display casing kept in `accounts.name`.
- **Password rules**: length > 8, not whitespace, must not start with `waw-token:` (`DbClient.cs:162-165`).
- **Hash scheme** (`Common/Database/PasswordHasher.cs`):
  - New: `PBKDF2(HMAC-SHA256, password UTF-8, salt UTF-8 bytes, 100_000 iterations, 32 bytes)`, stored as `pbkdf2$100000$<base64>` (`:16-20`).
  - Salt: `MathUtils.GenerateSalt()` per account, stored in `logins.password_salt` (`DbClient.cs:221`) - exact salt length/charset in `Common/Utilities/MathUtils` (UNVERIFIED, not read).
  - Legacy: `Base64(SHA1(UTF8(password + salt)))`, no prefix; still verified (`:35-38`) and **silently upgraded** to PBKDF2 on the next successful password (not token) login (`DbClient.cs:298-301`).
  - Comparison is constant-time (`CryptographicOperations.FixedTimeEquals`).
- **Verify flow** `DbClient.VerifyAccount` (`DbClient.cs:265-333`): lowercase name -> per-name lockout check -> SELECT login -> token or password check -> clear limiter -> optional hash upgrade -> load account JSON by `LOWER(name)` -> if `gameServerGuid != Guid.Empty` acquire Redis lock (see `Redis.md`) and set `logins.last_login_at = now()`.
- **Launcher "remember me" tokens** (`Common/Database/LoginTokens.cs`): token = `waw-token:` + base64url(32 random bytes) (`:21`). Only `SHA-256(token)` hex stored in `login_tokens` (`:23`). Valid if unused < 30 days (`:16,38`); use refreshes `last_used_at` at most once an hour (`:41`). Max 10 per login, oldest pruned on create (`:17,28-31`). Accepted anywhere a password is accepted, including the GameServer `Hello`. A token cannot mint a new token (`DbClient.cs:170-171`). `/account/forget` revokes one token.
- **Sessions**: none. No session id, cookie, or JWT. HTTP requests are stateless re-authentication; the only "session" state is the Redis account lock held by the GameServer connection (see `Redis.md`).
- **Ranks** (`Common/Database/Ranks.cs:14-17,20-31`): Player 0, Moderator 80, Developer 90, Owner 100; legacy `IsAdmin=true` = Owner. Owner can only be set directly in the database (`AdminDb.cs:102-103`).

### New architecture (RECOMMENDATION)
- Keep PBKDF2 compatibility for existing hashes (verify `pbkdf2$` and legacy SHA1 then re-hash), but move new hashes to Argon2id (or PBKDF2-SHA256 >= 600k iterations per current OWASP guidance) with a per-hash random binary salt stored in the hash string.
- Introduce real sessions: login returns a short-lived access token (opaque, stored hashed in PostgreSQL or Redis) + a refresh token (the existing launcher-token model fits). Unity client stores the token, never the password. Every HTTP route authenticates by `Authorization: Bearer`.
- Game join: the API issues a **one-time join ticket** (random, ~60 s TTL, bound to account + character + target server) that the Unity client presents to the C++ GameServer; the GameServer redeems it with the API (or validates an HMAC-signed ticket) instead of receiving a password.
- HTTPS everywhere (desktop included).

### Implementation status
NOT STARTED.

### Differences / Reason
Passwords on every request and on the game TCP port (`Hello.Password`) are the largest security gap; a C++ GameServer should never handle passwords or PBKDF2.

### Tests
`PasswordHasherTests.cs`, `LoginTokensTests.cs` (prefix, uniqueness, hash stability, token-like passwords rejected), `AttemptLimiterTests.cs`, `RanksTests.cs` (Common.Tests). No test exercises `VerifyAccount` against a database.

### Known issues
- `AttemptLimiter` is per-process memory: restart resets lockouts; a multi-instance API would need Redis-backed limits.
- Per-name lockout (10/10 min) lets anyone lock out any known username for 10 minutes (`DbClient.cs:273-274` blocks even a correct password).
- `/account/register` writes account + login + mail without a transaction (`DbClient.cs:251-257`); a failure in the middle leaves an account row without a login (name then permanently taken in `accounts` but not in `logins` -> `UNIQUE(name)` on accounts makes the next register attempt throw). UNVERIFIED runtime effect.
- `/char/list` returns the Guest list for wrong credentials instead of an error (`List.cs:19`).

---

## 5. Characters

### Reference behavior / Current implementation
- Storage: characters are an array inside `accounts.data->'Characters'`, **indexed by CharId** (`CharacterDb.cs:12`, `DbClient.cs:455-458`). Never physically removed.
- **List**: `/char/list` (above). Dead and deleted characters are filtered out (`ModelUtils.cs:36`).
- **Create**: NOT an HTTP route. The game client sends a `Create` packet to the GameServer (`GameServer/Game/Session/Create.cs:25-49`), which calls RPC `CreateCharacter(Account, objectType, skinType, role)` **passing its in-memory Account object** loaded at `Hello`. The AccountServer (`DbClient.CreateCharacterAsync`, `DbClient.cs:336-412`) checks free slots (`CharacterSlots.HasFree`: living, non-deleted), skin (`SkinRules.CanWear`: exists, class, owned or free), role (`Roles.IsValid`), builds the character from `PlayerDescs` start stats + `newCharsConfig`, gives the class starter equipment unless `StarterPending`, increments `NextCharId`, appends, then `FlushAsync(acc)` = **whole-account write of the GameServer's copy through `DbWriter<Account>`**. The updated Account is returned and the GameServer adopts it (`Create.cs:37`).
- **Delete**: `/char/delete` soft-deletes (`IsDeleted = true`) on a freshly loaded account, whole-account write (`DbClient.cs:414-435`). No check that the character is currently online.
- **Role**: chosen at creation (stored directly by `CreateCharacterAsync`, `DbClient.cs:360`); legacy role-less characters use `/char/chooseRole` -> `RoleDb.SetAsync` (only if no role yet).
- **Slots**: `/account/purchaseCharSlot` (fame cost `CharSlotCost`, default 1000).
- **Skins**: `/account/purchaseSkin` (`SkinDb.BuyAsync`, one conditional UPDATE: balance >= price AND not owned) and RPC `UnlockSkin` (item use in game). Free skins are never stored.
- **Load**: RPC `GetCharacter(accountId, charId)` re-reads the account row and returns `Characters[charId]` (`DbClient.cs:449-459`).
- **Save / death**: RPCs `SaveCharacter` and `RecordDeath` (see `Persistence.md`).

### New architecture (RECOMMENDATION)
- Character create/delete/list/slot/skin purchase stay in the Account/API service (out-of-game, transactional, money-like).
- Create must be a single SQL transaction against the current DB row (or a normalized `characters` table) - never a write-back of a client/GameServer copy of the account.
- Delete must refuse (or coordinate) when the account is locked by a live GameServer session.
- Character **load** for play: the C++ GameServer receives the character snapshot as part of join-ticket redemption (API returns it), or reads it directly from PostgreSQL (see section 12).

### Implementation status
NOT STARTED.

### Tests
`CharacterSlotsTests.cs`, `CharacterDbTests.cs` (Clean only), `SkinTests.cs` (GameServer.Tests), `RoleSelectionTests.cs`, `Tools/E2E/e2e_skins.py`, `Tools/E2E/e2e_roles.py` (live-server scripts, not run).

### Known issues
- **Lost update on create** (also audit F56, `Docs/EngineeringAudit.md:699-701`): the whole-account write from the GameServer's copy can overwrite anything changed in the DB since `Hello` (gold from an inbox/daily claim, skins, campsite chests, saved character progress of other characters). Because `DbWriter` is asynchronous, the RPC answers "success" before the row is committed; if the batch fails (DB down), the creation is silently lost while the player plays the new character, and every later `SaveCharacter` for it writes nothing (`CharacterDb.cs:90,95-96`: array too short) -> all progress lost.
- **Lost update on delete / buy slot**: same whole-document overwrite class; `BuyCharSlotAsync` is check-then-write in memory, so two concurrent requests can both pass the fame check (`DbClient.cs:437-447`).
- **Delete while online is undone**: `CharacterDb.Clean` copies `IsDeleted` from the GameServer's live copy (`CharacterDb.cs:55`), so the next autosave writes `IsDeleted=false` back.
- `Characters[charId]` assumes array index == CharId; any manual DB edit that breaks this corrupts addressing.

---

## 6. Rewards (daily gift, spin, inbox, bounties, starter)

### Reference behavior / Current implementation
- **Daily gift**: 7-day cycle (`RewardRules.cs:34`), 24 h real cooldown from the last claim (`DailyCooldown`, `RewardRules.cs:13-25`), streak in `daily_rewards`. Gold/fame added by one atomic jsonb UPDATE in the same transaction that records the claim; items of the day are mailed to the Inbox in the same transaction (`RewardsDb.cs:121-141`). Rewards table is the owner setting `daily-rewards.txt` (`DailyRewardsFile.cs`, hot-reloaded).
- **Daily spin**: server-side weighted roll with `RandomNumberGenerator.GetInt32` (`RewardsDb.cs:154`), same transaction pattern.
- **Inbox**: list (50), read, claim (gold/fame into account + items into `gift_items` queue, one transaction), delete (only if nothing to claim). Inbox capped at 100 claimed/empty messages per account (`RewardRules.cs:119`, `RewardsDb.cs:65-67`). Sources: welcome mail at registration, `/dev/mail`, `/mail` chat command (RPC `SendMail`), daily-gift items.
- **Gift chest**: queue -> chest via RPC `FillGiftChest` (see `Persistence.md`).
- **Bounties**: tracked on the GameServer in `Character.BountyState`; payout via RPC `ClaimBounty` -> `RewardsDb.GrantAsync` (`RewardsDb.cs:193-199`). The AccountServer does **no** validation of the amount: it trusts the GameServer (`AccServerRpcHandler.cs:130-133`).
- **Starter**: `Account.StarterPending` set at registration (`DbClient.cs:226`); cleared only by RPC `ClaimStarter` -> `StarterDb.ClaimAsync` (conditional jsonb_set, `StarterDb.cs:13-22`).
- **Storage rows**: RPC `BuyStorage` -> `CampsiteDb.BuyRowAsync` (one conditional UPDATE: gold >= cost, VaultCount == expected, < 64) (`CampsiteDb.cs:40-62`).

### New architecture (RECOMMENDATION)
- All currency-granting operations stay in the Account/API service (or a shared "economy" module in it) as single SQL transactions with **idempotency keys** (e.g. `bounty:{accountId}:{day}`) so a retried RPC cannot double-pay.
- Promote gold/fame balances to real columns (or a ledger table) instead of jsonb paths (see `PostgreSQL.md`).

### Implementation status
NOT STARTED.

### Tests
`RewardRulesTests.cs`, `DailyRewardsFileTests.cs`, `GiftChestRulesTests.cs`, `CampsiteDbTests.cs` (pure functions only), `StarterRewardTests.cs`, `GiftChestTests.cs` (GameServer.Tests, in-memory store).

### Known issues
- `ClaimBounty` is not idempotent: if the RPC commits but the answer is lost, the GameServer gives the claim back (`GameServer/Game/Systems/Rewards/BountyBoard.cs:88-100`) and the player can claim again -> double payout. The `BountyState` "claimed" flag is persisted only by the next character save, so a crash between payout and save also allows a second claim.
- `GrantAsync` reads the balance after commit on a second connection (not atomic with the grant; cosmetic).

---

## 7. Guilds

### Reference behavior / Current implementation
- Table `guilds` exists (`Schema.sql:27-35`); `accounts.guild_id` column + index; `Account.GuildName/GuildRank/GuildId` in JSON.
- **No code creates, joins or edits guilds**: `DbWriter<Guild>` is initialised (`DbClient.cs:41`) but nothing enqueues a Guild; the three HTTP routes are commented out (`Systems/Guild/*.cs`). Read-only uses: `/public/guild`, `/public/leaderboard?kind=guilds` (`PublicDb.cs:238-246,251-266`), `ModelUtils.cs:108-126` (Guild.ToXml, unused by live routes), GameServer `GuildHall` world (`GameServer/Game/Worlds/Logic/GuildHall.cs`, testing hall id -1).
- Whether any guild rows exist in a live DB: UNVERIFIED (DB not touched).

### New architecture (RECOMMENDATION)
Treat guilds as **not implemented**; design fresh in the Account/API service (normalized `guilds`, `guild_members` tables) when the feature is scheduled. Do not port the jsonb guild fields.

### Implementation status
NOT STARTED.

---

## 8. Music, news, bug board

- **Music** (`Systems/Music/MusicHandlers.cs`): one global shared playlist held **in AccountServer memory** (`MusicService`, monotonic stopwatch); restarts reshuffle. Library from `musicConfig.xml`. Skip/set require credentials and cooldowns (10 s/account, 2 s global).
- **News**: `/news/feed` reads `Resources/News/News.txt`; patch notes = `patch_notes` table + shipped `PatchNotes.txt`, merged and cached 15 s (`News/NewsFeed.cs:13-48`); exposed only at `/public/releases`.
- **Bug board**: `bug_posts` table; text cleaned to <= 200 chars, 10 posts/hour/account (`BugBoardRules.cs:12-15`), 50 newest listed.

### New architecture (RECOMMENDATION)
Keep in the Account/API service. Music state: if more than one API instance is ever run, move the music director state to Redis (or the GameServer, since it is a gameplay presentation feature); for a single instance, porting as-is is fine.

### Implementation status
NOT STARTED.

### Tests
`MusicDirectorTests.cs`, `MusicLibraryTests.cs`, `BugBoardRulesTests.cs`, `PatchNotesTests.cs`.

---

## 9. Public Portal API (`/public/*`)

Read-only, no login, JSON, `Access-Control-Allow-Origin: *`, 240 requests/min/IP, answers cached in a process-local `ConcurrentDictionary` for 60 s (`PublicDb.cs:19,154-165`; cache trimmed only when > 5000 entries). Profiles are public for every account; nothing private (no ids, IPs, gold, inbox) per `PublicDb.cs:14-17` - but note `Profile` does expose fame balances, last seen and online world. Presence (`online`, `world`) comes from asking every connected GameServer via RPC `GetUserInfo` (`PublicHandlers.cs:27-37`), sequentially, swallowing errors.

RECOMMENDATION: keep in the Account/API service; replace the in-process cache with HTTP caching headers + a small Redis/Memory cache; precompute leaderboards (materialized view refreshed every minute) because `chars`/`level` boards scan every account's jsonb array (`PublicDb.cs:225-236`). Status: NOT STARTED. Tests: `PublicProfileTests.cs`.

---

## 10. Developer API (`/dev/*`)

Used by `DeveloperDashboard.exe`. Each request signs in with username + launcher token (`DevHandlers.cs:19-21,32-50`); Developer (90+) required, `/dev/mail` Owner only; every change goes through `AdminDb` (rank rules: actor must outrank target, cannot act on self, Owner cannot be changed by commands - `AdminDb.cs:131-145`, `90-111`) and is logged with `[DEV]`. Moderation also pushed live to GameServers (`ApplyModeration`), weather pushed to all GameServers.

RECOMMENDATION: keep in the Account/API service behind proper auth (bearer token + role claim), add an audit-log table instead of log lines only. Status: NOT STARTED. Tests: `DeveloperDashboardTests.cs`, `ModerationRulesTests.cs`.

---

## 11. RPC hub (AccountServer side of GameServer <-> AccountServer)

### Transport (`Common/Messaging/IpcServer.cs`, `IpcClient.cs`, `RpcHandshake.cs`, `RpcCertificateHelper.cs`)
- TCP listener on `ListenAddress:ListenPort` (8081) -> TLS with a self-signed RSA-2048 cert auto-generated on first start (`RpcCertificateHelper.cs:20-42`, 10-year validity); the GameServer pins the exact certificate thumbprint (`:56-59`).
- Then a length-prefixed shared-secret handshake (max 4096 bytes), compared with plain `==` (not constant-time) (`RpcHandshake.cs:27-45`).
- Then StreamJsonRpc (bidirectional JSON-RPC) with `IAccountServerRpc` served by a new `AccServerRpcHandler` per connection, and an `IGameServerRpc` proxy back to that GameServer.
- On connection end (clean or exception) `handler.Close()` removes the GameServer from `IpcServer.Clients` and releases **all its Redis account locks** (`IpcServer.cs:71-84`, `AccServerRpcHandler.cs:24-27`).

### Methods served (`IAccountServerRpc`, `Common/Messaging/Proxies.cs:43-95`; implementation `AccountServer/Messaging/AccServerRpcHandler.cs`)
| RPC | Impl line | Does | GameServer caller |
|---|---|---|---|
| `GameServerConnected(Guid)` | 29-36 | registers the GameServer in `IpcServer.Clients` | `GameServer/Program.cs:49,100` |
| `GetUserInfo(name, accId)` | 38-40 | forwards to the GameServer's own `GetUserInfo` (odd loop-back) | none found |
| `VerifyAccount(user, pass, serverGuid)` | 42-45 | password/token check + Redis lock acquire (with dead-owner takeover) | `Session/Hello.cs:53` |
| `GetActiveBans(accId)` | 47-49 | SELECT bans | `Hello.cs:76` |
| `FlushAccount(Account)` | 51-53 | **whole-account write via DbWriter** | `Hello.cs:87` (expired ban -> `IsBanned=false`) |
| `GetCharacter(accId, charId)` | 55-57 | read one character | `Session/Load.cs:32` |
| `CreateCharacter(Account, type, skin, role)` | 139-142 | create + whole-account write | `Session/Create.cs:26` |
| `FindAccount(name)` | 59-62 | brief lookup | chat commands (UNVERIFIED exact caller) |
| `Moderate(req)` | 64-77 | ban/unban/mute/unmute/setrank via AdminDb | `Chat/Commands/ModCommands.cs:128,152,168,193`, `OwnerCommands.cs:63` |
| `GetMuteState(accId)` | 79-84 | longest active mute end | `Hello.cs:137`, `ModCommands.cs:21` |
| `SendMail(req)` | 135-137 | inbox mail (+items) | `OwnerCommands.cs:106` |
| `SaveCampsiteChests(accId, chests)` | 86-88 | jsonb_set `VaultChests` | `Worlds/Logic/Campsite.cs:362` |
| `LoadGiftChest` / `SaveGiftChest` / `FillGiftChest` | 90-103 | `gift_chests` / `gift_items` | `Worlds/Logic/GiftChestStore.cs:18-20` |
| `SaveCharacter(accId, chr)` | 105-107 | targeted jsonb_set of one character | `Systems/Persistence/CharacterSaver.cs:114` |
| `RecordDeath(accId, chr)` | 109-111 | transactional death + fame payout | `Systems/Combat/PlayerDeath.cs:105` |
| `FlagSuspect(...)` | 113-115 | INSERT anticheat_flags | `Systems/Combat/AntiCheat.cs:38` |
| `ClaimStarter(accId)` | 118-120 | clear StarterPending | `Session/ClaimStarter.cs:35` |
| `BuyStorage(accId, expectedRows)` | 122-124 | conditional storage purchase | `Session/StorageShop.cs:65` |
| `UnlockSkin(accId, skin)` | 126-128 | add skin | `Systems/Inventory/SkinUnlock.cs:70` |
| `ClaimBounty(accId, gold, fame)` | 130-133 | grant gold/fame (no validation) | `Systems/Rewards/BountyBoard.cs:88` |

Methods the AccountServer calls on GameServers (`IGameServerRpc`, `Proxies.cs:12-26`): `GlobalAnnouncement`, `GetGameServer` (player count), `GetUserInfo` (presence), `GetStatus`, `ApplyModeration`, `GetWeather`, `SetWeather`.

### Known issues
- The RPC trusts the GameServer completely (any amounts, any account id); the only gate is the shared secret + pinned cert.
- `GetUserInfo` on the hub just calls back into the same GameServer (`AccServerRpcHandler.cs:38-40`).
- Presence and status fan-out are sequential over every GameServer.

---

## 12. RECOMMENDATION - should the AccountServer remain a separate service?

**Yes. Keep a separate Account/API service, written in C# on ASP.NET Core (port and modernize the existing code, do not rewrite in C++).**

Justification from the responsibilities found:
1. **~45 HTTP routes** used by five different consumers (game client, browser client, launcher, Developer Dashboard, Portal/website) - none of them need the realtime simulation. Registration, login, tokens, character list/create/delete, purchases, inbox, daily rewards, bug board, patch notes, leaderboards, moderation tools are request/response CRUD with PostgreSQL transactions. ASP.NET Core does this with far less code and risk than C++ (HTTP, JSON, auth, rate limiting, PBKDF2/Argon2, Npgsql/Dapper are all mature in .NET).
2. **The existing C# code is reusable**: the targeted SQL (`CharacterDb`, `DeathDb`, `RewardsDb`, `SkinDb`, `CampsiteDb`, `GiftsDb`, `RoleDb`, `StarterDb`, `AdminDb`, `PublicDb`, `DevDb`) and pure rule classes (`RewardRules`, `GiftChestRules`, `Ranks`, `ModerationRules`, `PasswordHasher`, `LoginTokens`) can be lifted almost unchanged, keeping their unit tests.
3. **Blast-radius / availability separation**: a GameServer crash must not take down login, the launcher or the website, and an API deploy must not disconnect players. The reference already relies on this split (locks released per GameServer, GameServer reconnect loop).
4. **Security**: passwords and password hashing stay out of the game process; the C++ GameServer only handles join tickets.

What should move to (or be owned by) the C++ GameServer:
- Live-session state and in-game authority (inventory, stats, death detection, bounty progress, campsite chest contents while loaded, gift-chest take-only rule) - already the case.
- **Option A (recommended for first port):** keep the reference split - GameServer persists through an internal API (gRPC or HTTP/JSON over mTLS instead of StreamJsonRpc, which has no C++ implementation). Every write remains a targeted, idempotent call (SaveCharacter, RecordDeath, SaveCampsite, Gift chest, BuyStorage, UnlockSkin, ClaimBounty with idempotency key).
- **Option B (later, if RPC latency/throughput matters):** let the C++ GameServer write its own session data (character snapshot, campsite chests, gift chest) directly to PostgreSQL with the same targeted SQL (libpqxx), while the API keeps sole ownership of accounts, credentials, currencies, purchases, mail and moderation. If chosen, both services must share one migration-managed schema and the same row-level rules (e.g. "dead stays dead").
- Account lock acquire/release for play sessions can be done by the GameServer directly in Redis (it is the session owner), with TTL + heartbeat (see `Redis.md`).

What must NOT move to the GameServer:
- Registration, password/token verification and hashing, token issuance.
- Anything that grants or spends currency outside a live action (daily gift/spin, inbox claim, skin/slot purchase, storage purchase validation), moderation/ranks, Developer and Public APIs, patch notes, bug board.
- Character create/delete (needs slot/skin/currency checks and must be transactional).

Status: NOT STARTED.

---

## 13. Security observations (summary)

1. Desktop client uses plain HTTP to :8080 and plain TCP to the game port with the password (or long-lived token) in every request and in `Hello` (`Settings.cs:41`, `Hello.cs:33-40`).
2. No sessions; PBKDF2 (100k) on every authenticated request is a CPU DoS lever (64 concurrent handlers).
3. In-memory rate limiters only; per-username lockout is a griefing vector.
4. `X-Forwarded-For` is trusted from any address of the host (`Program.cs:256-259`), correct only while nginx is the only local proxy.
5. RPC: self-signed pinned TLS + shared secret compared non-constant-time; the local `rpcServerConfig.xml` binds `0.0.0.0` (firewall must block 8081, `VPS_SETUP.md:4`).
6. Whole-account overwrites (`FlushAccount`, create, delete, buy slot) are integrity risks (lost updates; possible item/currency rollback).
7. Bounty payout amounts are trusted from the GameServer; no idempotency.
8. `/public/*` exposes every account's profile (by design, opt-out planned).
9. Secrets live in git-ignored XML files (`postgresConfig.xml`, `rpcServerConfig.xml`, `rpcClientConfig.xml`, `.pfx`); they are present on disk in REF - not reproduced here.
10. Default new-account balances in source are 100000 gold/fame (`newAccountsConfig.xml:2-3`) - test values; the VPS owner file may differ (UNVERIFIED).

---

## 14. Discrepancies (docs vs code)

| # | Doc says | Code does |
|---|---|---|
| D1 | `Docs/EngineeringAudit.md:862`: "AccountServer no longer sets `ThreadPool.SetMinThreads(1000, 1000)`" | `AccountServer/Program.cs:35` still calls `ThreadPool.SetMinThreads(1000, 1000)` |
| D2 | `Docs/EngineeringAudit.md:102-103`: game server persists only via FlushAccount + vault; "no character save anywhere" | Stale (the audit's own change log, `:662-701`, records the fix). `SaveCharacter` RPC exists (`Proxies.cs:72`), called from `CharacterSaver.cs:114` |
| D3 | `Docs/EngineeringAudit.md:109-112`: 25 endpoints, SHA1, no rate limiting, no tokens, MaxAccountsPerIp 3000 | 45 live routes; PBKDF2 (`PasswordHasher.cs`); limiters (`LoginGuards.cs`, `DbClient.cs:23`); launcher tokens (`LoginTokens.cs`); `MaxAccountsPerIp = 10` (`DbClient.cs:19`). Audit section 1.5 is stale |
| D4 | `Docs/EngineeringAudit.md:947` ("Characters are now written ... never the whole account from the game server") and `WaW-Server/AGENTS.md:43-44` ("Known leftover: `Create.Handle` still does") | Two GameServer whole-account writes remain: `Create.cs:26` (via `CreateCharacterAsync` -> `FlushAsync`) **and** `Hello.cs:87` (`FlushAccount` when a ban has lapsed) |
| D5 | `Common/Messaging/Proxies.cs:47-49` comment: AccountServer "is the sole process allowed to open alloy.db (Direct connection mode ...)" | LiteDB/alloy.db is gone; PostgreSQL via Npgsql (`DbClient.cs:27-35`) |
| D6 | `WaW-Server/README.md:39`: "The first time you run the server an `alloy.db` file is created" | No LiteDB; PostgreSQL + Redis required (`DbClient.Load`) |
| D7 | `Common/Database/RoleDb.cs:7`: "the ONLY place that does [write a role]" | `DbClient.CreateCharacterAsync` writes `Role` on creation (`DbClient.cs:360`) |
| D8 | `AccountServer/Program.cs:146-149` comment: GameServers "reconnect and reassert their locks" | The GameServer only calls `GameServerConnected` on reconnect (`GameServer/Program.cs:100`); it never re-acquires locks. Locks survive only because they are keyed by its unchanged GUID in Redis |
| D9 | `AccountServer.csproj:43-45` copies `Handlers\Crossdomain\crossdomain.xml`; handler reads `Handlers/Crossdomain/crossdomain.xml` (`Crossdomain.cs:13`) | The file lives at `AccountServer/Systems/Crossdomain/crossdomain.xml`; the copy item matches no file, so `/crossdomain.xml` likely throws -> `<Error>Internal error.</Error>` (UNVERIFIED at runtime) |
| D10 | `GameServer/Game/Session/Hello.cs:63-66` handles `VerifyStatus.AccountInUse` with `Failure.ACCOUNT_IN_USE` | Unreachable: `VerifyAccount` returns `Acc = null` with AccountInUse, so the `acc == null` branch at `:58-61` sends `Failure.DEFAULT` first |
| D11 | `Docs/EngineeringAudit.md:1.5` example `rpcServerConfig` `0.0.0.0` / `CLAUDE` "RPC bound to loopback by default" | Example file uses `127.0.0.1`; the local real `rpcServerConfig.xml` uses `0.0.0.0` (values other than the address not reproduced) |
| D12 | `Systems/Char/List.cs:13` class name `ListMembers` | Serves `/char/list` (copy-paste name from the guild handler) |
